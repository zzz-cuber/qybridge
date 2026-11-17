// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: moyu_client.c —— 连接实体魔域魔方（NimBLE 主机）
//
// 流程: 扫描(名称 WCU_*) -> 连接 -> 协商 MTU -> 发现服务
//       -> 发现特征 -> 订阅通知 -> 发 0xA1/0xA3/0xA4/0xAC 初始化
//       -> 收 0xA5 转动帧 / 0xA3 状态帧 -> 交给 runtime
//
// 加密: AES-128，密钥/IV 由魔方 MAC 派生（%255）。
// 若用 BLE 地址派生的密钥解不开（部分型号地址与名称不符），
// 会自动改用名称后缀推出的 "CF:30:16:00:XX:YY" 再试一次。
#include "moyu_client.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "moyu_codec.h"
#include "runtime.h"
#include "bridge.h"

static const char *TAG = "moyu";

// 魔域 WCU 服务/特征
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0xB0, 0x5C, 0x79, 0xD2, 0x05, 0xA3, 0x60, 0x17,
    0xA0, 0xB5, 0x35, 0x77, 0x3E, 0xB0, 0x83, 0x07);
static const ble_uuid128_t CHR_READ_UUID = BLE_UUID128_INIT(
    0xB1, 0x5C, 0x79, 0xD2, 0x05, 0xA3, 0x60, 0x17,
    0xA0, 0xB5, 0x35, 0x77, 0x3E, 0xB0, 0x83, 0x07);
static const ble_uuid128_t CHR_WRITE_UUID = BLE_UUID128_INIT(
    0xB2, 0x5C, 0x79, 0xD2, 0x05, 0xA3, 0x60, 0x17,
    0xA0, 0xB5, 0x35, 0x77, 0x3E, 0xB0, 0x83, 0x07);

#define NVS_NAMESPACE "qybridge"

typedef enum {
    PH_IDLE = 0,
    PH_SCAN,
    PH_CONNECT,
    PH_MTU,
    PH_SVC,
    PH_CHR,
    PH_DSC,
    PH_READY,
} phase_t;

static phase_t s_ph;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_read_val, s_write_val, s_cccd;
static bool s_have_read, s_have_write;
static bool s_subscribed;

static ble_addr_t s_peer;
static bool s_have_peer;
static uint8_t s_mac[6];              // 显示序
static char s_name[32];

static moyu_crypto_t s_crypto;
static bool s_fallback_tried;
static uint8_t s_key_tries;      // 换钥匙的重试次数(0~2)
static uint8_t s_fallback_mac[6];
static bool s_have_fallback;

static struct ble_npl_callout s_tick;
static bool s_tick_ready;
static uint32_t s_phase_ms;
static uint32_t s_ready_ms;
static uint32_t s_init_at;      // 非 0 表示到点要发初始化指令
static uint32_t s_rx_ok;
static uint8_t s_retry;
static bool s_scan_wanted;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

const char *moyu_client_state_name(void) {
    switch (s_ph) {
        case PH_SCAN: return "扫描";
        case PH_CONNECT: return "连接";
        case PH_MTU: return "协商MTU";
        case PH_SVC: return "发现服务";
        case PH_CHR: return "发现特征";
        case PH_DSC: return "订阅";
        case PH_READY: return "已连接";
        default: return "空闲";
    }
}

bool moyu_client_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_ph == PH_READY; }

// ---------------------------------------------------------------------------
// NVS 配对记录
// ---------------------------------------------------------------------------
static void save_peer(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "peer", &s_peer, sizeof(s_peer));
    nvs_set_blob(h, "mac", s_mac, 6);
    nvs_commit(h);
    nvs_close(h);
}

static void load_peer(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_peer);
    if (nvs_get_blob(h, "peer", &s_peer, &len) == ESP_OK && len == sizeof(s_peer)) {
        size_t mlen = 6;
        if (nvs_get_blob(h, "mac", s_mac, &mlen) == ESP_OK && mlen == 6) {
            s_have_peer = true;
            ESP_LOGI(TAG, "已保存的魔方 %02X:%02X:%02X:%02X:%02X:%02X",
                     s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
        }
    }
    nvs_close(h);
}

void moyu_client_clear_pairing(void) {
    s_have_peer = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "已清除配对记录");
}

// ---------------------------------------------------------------------------
// 加密与钥匙
// ---------------------------------------------------------------------------
static void setup_crypto_from(const uint8_t mac[6]) {
    moyu_crypto_setup(&s_crypto, mac);
    s_rx_ok = 0;
    ESP_LOGI(TAG, "由 MAC %02X:%02X:%02X:%02X:%02X:%02X 派生密钥",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 名称 WCU_MY32_1A2B -> CF:30:16:00:1A:2B（DCTimer 的兜底规则）
static void fallback_mac_from_name(const char *name) {
    s_have_fallback = false;
    if (!name) return;
    size_t n = strlen(name);
    if (n < 4) return;
    int v[4];
    for (int i = 0; i < 4; i++) {
        char c = name[n - 4 + i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        if (d < 0) return;
        v[i] = d;
    }
    s_fallback_mac[0] = 0xCF;
    s_fallback_mac[1] = 0x30;
    s_fallback_mac[2] = 0x16;
    s_fallback_mac[3] = 0x00;
    s_fallback_mac[4] = (uint8_t)((v[0] << 4) | v[1]);
    s_fallback_mac[5] = (uint8_t)((v[2] << 4) | v[3]);
    s_have_fallback = true;
}

// NimBLE 同一时刻只允许一个 GATT 过程。初始化指令一口气发 4 帧、
// bridge 又可能同时请求快照，必然撞车(rc=6 BLE_HS_EBUSY)。
// 这里用一个小的待发队列 + tick 重试，保证一帧都不丢。
#define MOYU_PEND_MAX 6
static uint8_t s_pend[MOYU_PEND_MAX][MOYU_PKT_LEN];
static int s_pend_n;

static bool write_now(const uint8_t plain[MOYU_PKT_LEN]) {
    uint8_t enc[MOYU_PKT_LEN];
    memcpy(enc, plain, MOYU_PKT_LEN);
    moyu_crypto_encode(&s_crypto, enc);
    return ble_gattc_write_flat(s_conn, s_write_val, enc, MOYU_PKT_LEN, NULL, NULL) == 0;
}

bool moyu_client_write(const uint8_t plain[MOYU_PKT_LEN]) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_have_write || !s_subscribed) return false;
    if (write_now(plain)) return true;
    if (s_pend_n < MOYU_PEND_MAX) {
        memcpy(s_pend[s_pend_n], plain, MOYU_PKT_LEN);
        s_pend_n++;
        ESP_LOGD(TAG, "GATT 忙, 帧已排队 (待发 %d)", s_pend_n);
        return true;
    }
    ESP_LOGW(TAG, "待发队列满, 丢弃一帧");
    return false;
}

// tick 里把排队的帧补发出去
static void flush_pending(void) {
    while (s_pend_n > 0) {
        if (!write_now(s_pend[0])) return;   // 还忙, 下个 tick 再来
        for (int i = 1; i < s_pend_n; i++) memcpy(s_pend[i - 1], s_pend[i], MOYU_PKT_LEN);
        s_pend_n--;
    }
}

static void send_init_sequence(void) {
    uint8_t pkt[MOYU_PKT_LEN];
    moyu_build_simple(pkt, MOYU_OP_INFO);   // 0xA1: 连接后必须先发一次
    moyu_client_write(pkt);
    moyu_build_simple(pkt, MOYU_OP_STATUS); // 0xA3: 请求快照
    moyu_client_write(pkt);
    moyu_build_simple(pkt, MOYU_OP_POWER);  // 0xA4: 电量
    moyu_client_write(pkt);
    moyu_build_gyro(pkt, false);            // 0xAC: 关掉陀螺仪上报，省带宽
    moyu_client_write(pkt);
}

// ---------------------------------------------------------------------------
// 通知处理
// ---------------------------------------------------------------------------
static void on_notify(const uint8_t *data, int len) {
    if (len != MOYU_PKT_LEN) return;
    uint8_t buf[MOYU_PKT_LEN];
    memcpy(buf, data, MOYU_PKT_LEN);

    moyu_crypto_decode(&s_crypto, buf);
    moyu_msg_t msg;
    if (!moyu_parse(&msg, buf)) {
        ESP_LOGW(TAG, "解析失败 (钥匙可能不对)");
        return;
    }
    s_rx_ok++;
    // 只在调试级别打印，避免高频日志拖慢链路
    if (msg.kind == MOYU_MSG_MOVE) {
        ESP_LOGD(TAG, "转动帧 step=%u", msg.move_step);
    }
    runtime_post_moyu(&msg);
}

// ---------------------------------------------------------------------------
// GATT 发现
// ---------------------------------------------------------------------------
static void start_disc_svc(void);
static void start_scan(void);
static void begin_subscribe(void);
static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg);

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    if (conn != s_conn) return 0;
    if (err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (s_svc_start == 0) {
            ESP_LOGE(TAG, "未找到魔域服务");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        s_ph = PH_CHR;
        ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, chr_cb, NULL);
        return 0;
    }
    ESP_LOGE(TAG, "服务发现错误 status=%d", err->status);
    return 0;
}

// ---------------------------------------------------------------------------
// 特征表 / 订阅
//
// 踩过的坑: 一开始直接往"特征值句柄 + 1"写 CCCD，结果写到了 0 号句柄
// (ATT 0x101 Invalid Handle)。原因是没真正把发现出来的表打出来核对。
// 现在: 发现完打印整张表 -> 选 notify 特征 -> CCCD 候选逐个试。
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t def;
    uint16_t val;
    ble_uuid_any_t uuid;   // 原样存下来, 用 ble_uuid_cmp 精确比较
    uint8_t props;
} chr_rec_t;

static chr_rec_t s_chrs[12];
static int s_chr_n;

// IDF 6.0 的 NimBLE: ble_uuid_t 只有 type, 128 位要看 ble_uuid128_t.value[]
static uint16_t uuid_low16(const ble_uuid_t *u) {
    if (u->type == BLE_UUID_TYPE_16) return ble_uuid_u16(u);
    if (u->type == BLE_UUID_TYPE_128) {
        const ble_uuid128_t *x = (const ble_uuid128_t *)u;
        return (uint16_t)(x->value[0] | (x->value[1] << 8));   // 小端, 低 16 位
    }
    return 0;
}

static void finish_subscribe(void);
static int cccd_cb(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg);

// CCCD 候选句柄: 先试用描述符发现拿到的，再试 val+1 / val+2
static uint16_t s_cccd_try[3];
static int s_cccd_n;
static int s_cccd_i;

static void cccd_try_next(void) {
    if (s_cccd_i >= s_cccd_n) {
        ESP_LOGE(TAG, "所有 CCCD 候选都失败, 断开重试");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    s_cccd = s_cccd_try[s_cccd_i++];
    uint16_t val = 0x0001;
    int rc = ble_gattc_write_flat(s_conn, s_cccd, &val, 2, cccd_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "CCCD 写入启动失败 handle=%u rc=%d", s_cccd, rc);
        cccd_try_next();
    } else {
        ESP_LOGI(TAG, "订阅 CCCD: handle=%u (候选 %d/%d)", s_cccd, s_cccd_i, s_cccd_n);
    }
}

static int cccd_cb(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg) {
    (void)attr; (void)arg;
    if (conn != s_conn) return 0;
    if (err->status == 0 || err->status == BLE_HS_EDONE) {
        finish_subscribe();
        return 0;
    }
    ESP_LOGW(TAG, "CCCD 写入失败 handle=%u status=%d, 换下一个候选", s_cccd, err->status);
    cccd_try_next();
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val; (void)arg;
    if (conn != s_conn) return 0;
    if (err->status == 0 && dsc) {
        uint16_t u = uuid_low16((const ble_uuid_t *)&dsc->uuid);
        ESP_LOGI(TAG, "  dsc: handle=%u uuid=0x%04X", dsc->handle, u);
        if (u == BLE_GATT_DSC_CLT_CFG_UUID16 && s_cccd == 0) {
            s_cccd = dsc->handle;
        }
        return 0;
    }
    // 发现结束(或出错): 组装候选并开始写
    if (s_cccd) {
        s_cccd_try[s_cccd_n++] = s_cccd;
    }
    uint16_t a = (uint16_t)(s_read_val + 1);
    uint16_t b = (uint16_t)(s_read_val + 2);
    if (s_cccd != a && s_cccd_n < 3) s_cccd_try[s_cccd_n++] = a;
    if (s_cccd != b && s_cccd_n < 3) s_cccd_try[s_cccd_n++] = b;
    s_cccd_i = 0;
    if (s_cccd_n == 0) {
        ESP_LOGE(TAG, "没有可用的 CCCD 候选");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    cccd_try_next();
    return 0;
}

static void begin_subscribe(void) {
    s_cccd = 0;
    s_cccd_n = 0;
    s_cccd_i = 0;
    // 在整个服务范围里找描述符，别只盯着 val+1（真机上就吃过这个亏）
    uint16_t start = (uint16_t)(s_read_val + 1);
    uint16_t end = s_svc_end;
    if (start > end) start = s_svc_start;
    ESP_LOGI(TAG, "搜索描述符: [%u..%u]", start, end);
    int rc = ble_gattc_disc_all_dscs(s_conn, start, end, dsc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "描述符发现启动失败 rc=%d, 直接试 val+1", rc);
        s_cccd_n = 0;
        s_cccd_try[s_cccd_n++] = (uint16_t)(s_read_val + 1);
        s_cccd_try[s_cccd_n++] = (uint16_t)(s_read_val + 2);
        s_cccd_i = 0;
        cccd_try_next();
    }
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    if (conn != s_conn) return 0;
    if (err->status == 0 && chr) {
        if (s_chr_n < (int)(sizeof(s_chrs) / sizeof(s_chrs[0]))) {
            s_chrs[s_chr_n].def = chr->def_handle;
            s_chrs[s_chr_n].val = chr->val_handle;
            s_chrs[s_chr_n].props = chr->properties;
            memcpy(&s_chrs[s_chr_n].uuid, &chr->uuid, sizeof(ble_uuid_any_t));
            s_chr_n++;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "发现 %d 个特征:", s_chr_n);
        for (int i = 0; i < s_chr_n; i++) {
            const ble_uuid_t *u = (const ble_uuid_t *)&s_chrs[i].uuid;
            ESP_LOGI(TAG, "  chr[%d] uuid=%s%04X def=%u val=%u props=0x%02X",
                     i, u->type == BLE_UUID_TYPE_128 ? ".." : "0x",
                     uuid_low16(u), s_chrs[i].def, s_chrs[i].val, s_chrs[i].props);
        }

        // 1) 优先按 UUID 精确匹配两个已知特征
        const chr_rec_t *rd = NULL, *wr = NULL;
        for (int i = 0; i < s_chr_n; i++) {
            const ble_uuid_t *u = (const ble_uuid_t *)&s_chrs[i].uuid;
            if (ble_uuid_cmp(u, (const ble_uuid_t *)&CHR_READ_UUID) == 0) rd = &s_chrs[i];
            else if (ble_uuid_cmp(u, (const ble_uuid_t *)&CHR_WRITE_UUID) == 0) wr = &s_chrs[i];
        }
        // 2) 退化为按属性挑: 能通知的 / 能写的
        if (!rd) {
            for (int i = 0; i < s_chr_n; i++) {
                if (s_chrs[i].props & BLE_GATT_CHR_PROP_NOTIFY) { rd = &s_chrs[i]; break; }
            }
        }
        if (!wr) {
            for (int i = 0; i < s_chr_n; i++) {
                if (s_chrs[i].props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
                    wr = &s_chrs[i];
                    break;
                }
            }
        }
        if (!rd || !wr) {
            ESP_LOGE(TAG, "特征不全: 通知=%s 写=%s",
                     rd ? "有" : "无", wr ? "有" : "无");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        s_read_val = rd->val;
        s_write_val = wr->val;
        s_have_read = s_have_write = true;
        ESP_LOGI(TAG, "选定: 通知 val=%u, 写 val=%u", s_read_val, s_write_val);

        s_ph = PH_DSC;
        begin_subscribe();
        return 0;
    }
    ESP_LOGE(TAG, "特征发现错误 status=%d", err->status);
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

// 订阅完成: 此刻才能给魔方发指令（写特征要订阅后才生效）
// 注意: CCCD 写回调返回时 GATT 过程可能还没完全收尾，立刻写会拿到 BLE_HS_EBUSY(6)，
// 所以初始化指令延后 100ms 由 tick 发。
static void finish_subscribe(void) {
    if (s_subscribed) return;
    s_subscribed = true;
    ESP_LOGI(TAG, "已订阅实体魔方通知, 100ms 后发送初始化指令");
    s_init_at = now_ms() + 100;
    s_ready_ms = now_ms();     // "钥匙解不开就换备用 MAC"的计时起点
    s_ph = PH_READY;
}

// ---------------------------------------------------------------------------
// 连接流程
// ---------------------------------------------------------------------------
static int gap_cb(struct ble_gap_event *event, void *arg);

static void set_phase(phase_t p) {
    s_ph = p;
    s_phase_ms = now_ms();
}

static void start_scan(void) {
    if (s_scan_wanted) ble_gap_disc_cancel();
    s_scan_wanted = true;
    struct ble_gap_disc_params dp;
    memset(&dp, 0, sizeof(dp));
    dp.filter_duplicates = 1;
    dp.passive = 0;
    dp.itvl = 0x0060;
    dp.window = 0x0030;
    s_svc_start = s_svc_end = 0;
    s_have_read = s_have_write = false;
    s_subscribed = false;
    set_phase(PH_SCAN);
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_cb, NULL);
    if (rc != 0) {
        rc = ble_gap_disc(BLE_OWN_ADDR_RANDOM, BLE_HS_FOREVER, &dp, gap_cb, NULL);
    }
    if (rc != 0) ESP_LOGE(TAG, "扫描启动失败 rc=%d", rc);
    else ESP_LOGI(TAG, "开始扫描 WCU_* 魔方");
}

static void connect_to(const ble_addr_t *addr) {
    if (s_scan_wanted) ble_gap_disc_cancel();
    s_scan_wanted = false;
    s_peer = *addr;
    set_phase(PH_CONNECT);
    struct ble_gap_conn_params cp;
    memset(&cp, 0, sizeof(cp));
    cp.scan_itvl = 0x0010;
    cp.scan_window = 0x0010;
    cp.itvl_min = 0x0008;   // 10ms
    cp.itvl_max = 0x0010;   // 20ms
    cp.latency = 0;
    cp.supervision_timeout = 0x0100;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, 8000, &cp, gap_cb, NULL);
    if (rc != 0) {
        rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, addr, 8000, &cp, gap_cb, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "发起连接失败 rc=%d", rc);
        set_phase(PH_IDLE);
    }
}

static void start_disc_svc(void) {
    s_ph = PH_SVC;
    s_svc_start = s_svc_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(s_conn, (const ble_uuid_t *)&SVC_UUID, svc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "服务发现启动失败 rc=%d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool is_target_name(const char *name) {
    return name && strncmp(name, "WCU_", 4) == 0;
}

static void handle_disc(const struct ble_gap_disc_desc *disc) {
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) return;
    if (!fields.name || fields.name_len == 0) return;
    size_t n = fields.name_len < sizeof(s_name) - 1 ? fields.name_len : sizeof(s_name) - 1;
    memcpy(s_name, fields.name, n);
    s_name[n] = 0;
    if (!is_target_name(s_name)) return;

    // MAC 显示序: ble_addr_t 里是小端
    for (int i = 0; i < 6; i++) s_mac[i] = disc->addr.val[5 - i];
    ESP_LOGI(TAG, "发现 %s (%02X:%02X:%02X:%02X:%02X:%02X)", s_name,
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    fallback_mac_from_name(s_name);
    s_fallback_tried = false;
    s_key_tries = 0;
    setup_crypto_from(s_mac);
    s_peer = disc->addr;     // 先记下地址再存盘，否则存进去的是全 0
    save_peer();
    connect_to(&disc->addr);
}

static int gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (s_ph == PH_SCAN) handle_disc(&event->disc);
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (s_ph == PH_SCAN) start_scan();
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
                set_phase(PH_IDLE);
                return 0;
            }
            s_conn = event->connect.conn_handle;
            s_retry = 0;
            s_ph = PH_MTU;
            s_phase_ms = now_ms();
            ESP_LOGI(TAG, "已连接实体魔方 conn=%u", (unsigned)s_conn);
            runtime_post_cube(true);          // 让 bridge 把状态标为未知
            ble_gattc_exchange_mtu(s_conn, NULL, NULL);
            return 0;

        case BLE_GAP_EVENT_MTU:
            if (event->mtu.conn_handle == s_conn && s_ph == PH_MTU) {
                ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
                start_disc_svc();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle != s_conn) return 0;
            ESP_LOGW(TAG, "实体魔方断开 reason=%d", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_have_read = s_have_write = false;
            s_subscribed = false;
            s_pend_n = 0;
            s_init_at = 0;
            runtime_post_cube(false);
            set_phase(PH_IDLE);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            if (event->notify_rx.conn_handle != s_conn) return 0;
            // 兜底: 有些魔方不等 CCCD 回调就推数据，这里也补一次初始化
            if (!s_subscribed) finish_subscribe();
            uint8_t data[MOYU_PKT_LEN];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, data, sizeof(data), &len) == 0) {
                on_notify(data, len);
            }
            return 0;
        }

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// 定时器: 超时重试 / 断线重连 / 钥匙兜底
// ---------------------------------------------------------------------------
static void tick_cb(struct ble_npl_event *ev) {
    (void)ev;
    uint32_t now = now_ms();

    if (s_subscribed) flush_pending();

    // 订阅后延迟发初始化指令（避开 CCCD 写过程的 EBUSY）
    if (s_init_at && (int32_t)(now - s_init_at) >= 0) {
        s_init_at = 0;
        send_init_sequence();
        s_ready_ms = now;
        ESP_LOGI(TAG, "初始化指令已发送 (0xA1/0xA3/0xA4/0xAC)");
        runtime_post_cube(true);   // 让 bridge 请求状态快照
    }

    // 连上但一直解不开 -> 换名称推出的 MAC 试；再不行就退回地址钥匙，然后不再折腾
    if (s_ph == PH_READY && s_rx_ok == 0 && now - s_ready_ms > 4000 && s_key_tries < 2) {
        bool same = s_have_fallback && memcmp(s_fallback_mac, s_mac, 6) == 0;
        s_key_tries++;
        if (s_key_tries == 1 && s_have_fallback && !same) {
            ESP_LOGW(TAG, "用地址派生钥匙解不开, 改用名称推出的 MAC 重试");
            setup_crypto_from(s_fallback_mac);
            memcpy(s_mac, s_fallback_mac, 6);
        } else {
            ESP_LOGW(TAG, "换钥匙后仍解不开, 退回地址派生钥匙");
            for (int i = 0; i < 6; i++) s_mac[i] = s_peer.val[5 - i];
            setup_crypto_from(s_mac);
        }
        send_init_sequence();
        s_ready_ms = now;
    }
    if (s_rx_ok > 0) s_key_tries = 0;

    if (s_ph == PH_MTU || s_ph == PH_SVC || s_ph == PH_CHR || s_ph == PH_DSC) {
        if (now - s_phase_ms > 5000) {
            ESP_LOGW(TAG, "发现流程超时, 断开重来");
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            set_phase(PH_IDLE);
        }
    }

    if (s_ph == PH_IDLE) {
        // 有配对记录就优先直连，连不上退化为扫描
        if (s_have_peer && s_retry < 3) {
            s_retry++;
            ESP_LOGI(TAG, "直连已配对魔方 (第 %u 次)", s_retry);
            connect_to(&s_peer);
        } else {
            if (s_have_peer && s_retry >= 3) {
                ESP_LOGW(TAG, "直连多次失败, 转为扫描");
                s_retry = 0;
                s_have_peer = false;
            }
            if (!s_scan_wanted) start_scan();
        }
    }

    ble_npl_callout_reset(&s_tick, pdMS_TO_TICKS(200));
}

void moyu_client_rescan(void) {
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_have_peer = false;
    s_retry = 0;
    set_phase(PH_IDLE);
}

void moyu_client_on_host_sync(void) {
    load_peer();
    if (!s_tick_ready) {
        ble_npl_callout_init(&s_tick, nimble_port_get_dflt_eventq(), tick_cb, NULL);
        s_tick_ready = true;
    }
    ble_npl_callout_reset(&s_tick, pdMS_TO_TICKS(100));
}

void moyu_client_init(void) {
    s_ph = PH_IDLE;
    s_have_peer = false;
    s_have_read = s_have_write = false;
}
