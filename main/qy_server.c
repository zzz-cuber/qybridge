// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: qy_server.c —— 虚拟奇艺魔方（NimBLE 外设 + GATT + 通知队列）
//
// 线程模型:
//   * GATT 回调 / 广播 / 通知发送 全部在 NimBLE host 任务里
//   * 想动 bridge 状态时，只往 runtime 队列投一条命令，绝不在这里读改状态
//   * 发送队列是"只进不出、绝不跳帧"的：链路再挤也按顺序等，因为跳一帧就少一步，
//     App 的动作模型立刻和贴纸对不上，最后就是"本次复原步骤不完整"
#include "qy_server.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "qy_codec.h"
#include "bridge.h"
#include "runtime.h"

static const char *TAG = "qy";

#define QY_SVC_UUID   0xFFF0
#define QY_CHR_FFF4   0xFFF4
#define QY_CHR_FFF5   0xFFF5
#define QY_CHR_DATA   0xFFF6
#define QY_CHR_FFF7   0xFFF7

#define QY_MIN_MTU_STATE 99          // 96 字节状态帧 + 3 字节 ATT 头
#define QY_TX_QUEUE_LEN 48

static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(QY_SVC_UUID);
static const ble_uuid16_t s_chr_fff4 = BLE_UUID16_INIT(QY_CHR_FFF4);
static const ble_uuid16_t s_chr_fff5 = BLE_UUID16_INIT(QY_CHR_FFF5);
static const ble_uuid16_t s_chr_data = BLE_UUID16_INIT(QY_CHR_DATA);
static const ble_uuid16_t s_chr_fff7 = BLE_UUID16_INIT(QY_CHR_FFF7);

static uint8_t s_phys_mac[6];
static uint8_t s_addr_le[6];
static char s_name[16];
static const uint8_t s_own_addr_type = BLE_OWN_ADDR_RANDOM;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_data_handle;
static uint16_t s_mtu = 23;
static bool s_subscribed;
static bool s_app_hello;
static bool s_auto_hello;
static uint32_t s_sub_ms;

typedef struct {
    uint16_t len;
    uint8_t data[QY_WIRE_STATE];
} tx_item_t;

static QueueHandle_t s_tx_q;
static struct ble_npl_callout s_tick;
static bool s_tick_ready;
static uint32_t s_drop_count;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static void hex_dump(char *out, size_t cap, const uint8_t *data, int len) {
    size_t pos = 0;
    out[0] = 0;
    for (int i = 0; i < len && pos + 4 <= cap; i++) {
        pos += (size_t)snprintf(out + pos, cap - pos, "%02X ", data[i]);
    }
}

// ---------------------------------------------------------------------------
// 身份
// ---------------------------------------------------------------------------
void qy_server_prepare_identity(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    memcpy(s_phys_mac, mac, 6);

    // 名称后缀 4 位十六进制取自 BT MAC 后两字节，并以它构造
    // CC:A3:00:00:XX:YY：App 会用"名称后缀 -> 地址"的规则校验，两者必须一致。
    snprintf(s_name, sizeof(s_name), "QY-QYSC-S-%02X%02X", mac[4], mac[5]);
    s_addr_le[0] = mac[5];
    s_addr_le[1] = mac[4];
    s_addr_le[2] = 0x00;
    s_addr_le[3] = 0x00;
    s_addr_le[4] = 0xA3;
    s_addr_le[5] = 0xCC;

    ESP_LOGI(TAG, "虚拟奇艺魔方 %s 地址 CC:A3:00:00:%02X:%02X", s_name, mac[4], mac[5]);
}

const char *qy_server_name(void) { return s_name; }

// ---------------------------------------------------------------------------
// 发送队列
// ---------------------------------------------------------------------------
bool qy_server_send(const uint8_t *plain, size_t len) {
    if (!plain || len == 0 || len > QY_WIRE_STATE) return false;
    // 没订阅就不要攒帧: 订阅后 App 会重新拿 Hello 里的状态，
    // 这里若把订阅前的帧攒下来，订阅后一次性补放反而会把 App 的模型带歪。
    if (!s_subscribed) return false;
    tx_item_t item;
    memcpy(item.data, plain, len);
    item.len = (uint16_t)len;
    if (xQueueSend(s_tx_q, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        // 队列满 = 链路已经跟不上动作。这里绝不静默丢帧:
        // 交给上层判定为致命错误并重新同步。
        s_drop_count++;
        ESP_LOGE(TAG, "发送队列溢出(%u 次丢弃) — 链路吞吐不足, 请重连", (unsigned)s_drop_count);
        return false;
    }
    return true;
}

bool qy_server_subscribed(void) { return s_subscribed; }

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static int stub_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    static const uint8_t zero[2] = {0, 0};
    return os_mbuf_append(ctxt->om, zero, sizeof(zero)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int stub_rw(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return stub_read(conn, attr, ctxt, arg);
    return 0;
}

// App -> 魔方
static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t zero[16] = {0};
        return os_mbuf_append(ctxt->om, zero, sizeof(zero)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > QY_WIRE_STATE || (len % 16) != 0) {
        ESP_LOGW(TAG, "[app] 写入长度非法 len=%u (奇艺帧必须是 16 的倍数)", (unsigned)len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t frame[QY_WIRE_STATE];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &copied) != 0 || copied != len) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    qy_app_msg_t msg;
    if (!qy_unseal(frame, len) || !qy_parse_app_frame(frame, len, &msg)) {
        char hex[3 * 24 + 1];
        hex_dump(hex, sizeof(hex), frame, len < 24 ? len : 24);
        ESP_LOGW(TAG, "[app] 丢弃无法解析的帧 len=%u: %s", (unsigned)len, hex);
        return 0;   // 不回错误，避免 App 断连
    }

    switch (msg.kind) {
        case QY_APP_HELLO:
            ESP_LOGI(TAG, "[app] AppHello");
            s_app_hello = true;
            runtime_post_hello();
            break;
        case QY_APP_REQUEST_STATE:
            ESP_LOGI(TAG, "[app] RequestState");
            runtime_post_current();
            break;
        case QY_APP_SYNC_STATE:
            ESP_LOGW(TAG, "[app] SyncState (复原重置/重新同步)");
            runtime_post_app(&msg);
            break;
        case QY_APP_ACK:
            break;
        default:
            ESP_LOGW(TAG, "[app] 未识别帧 len=%u", (unsigned)len);
            break;
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&s_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]){
            // 真机 profile 有 fff4/fff5/fff6/fff7，协议只用 fff6，其余留作占位
            { .uuid = (const ble_uuid_t *)&s_chr_fff4, .flags = BLE_GATT_CHR_F_READ,
              .access_cb = stub_read },
            { .uuid = (const ble_uuid_t *)&s_chr_fff5,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .access_cb = stub_rw },
            { .uuid = (const ble_uuid_t *)&s_chr_data,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                       BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
              .access_cb = data_access, .val_handle = &s_data_handle },
            { .uuid = (const ble_uuid_t *)&s_chr_fff7, .flags = BLE_GATT_CHR_F_READ,
              .access_cb = stub_read },
            { 0 },
        },
    },
    { 0 },
};

void qy_server_register(void) {
    s_tx_q = xQueueCreate(QY_TX_QUEUE_LEN, sizeof(tx_item_t));
    configASSERT(s_tx_q);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(s_name));
}

// ---------------------------------------------------------------------------
// 广播
// ---------------------------------------------------------------------------
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &s_svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (const uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(TAG, "广播字段设置失败");
        return;
    }

    uint8_t mfg[8] = {0xE5, 0x02, 0, 0, 0, 0, 0, 0};
    memcpy(mfg + 2, s_phys_mac, 6);
    struct ble_hs_adv_fields sr;
    memset(&sr, 0, sizeof(sr));
    sr.mfg_data = mfg;
    sr.mfg_data_len = sizeof(mfg);
    ble_gap_adv_rsp_set_fields(&sr);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event_handler, NULL);
    if (rc != 0) ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
    else ESP_LOGI(TAG, "广播中: %s", s_name);
}

static void request_fast_conn(void) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    // 96 字节帧在默认 30~50ms 间隔下吞吐不够，做公式时队列会积压，
    // App 就会"延迟补放"(表现为做一步却转好几下)。这里要更快的间隔。
    struct ble_gap_upd_params p = {
        .itvl_min = 12, .itvl_max = 24, .latency = 0,
        .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(s_conn, &p);
    if (rc == 0) ESP_LOGI(TAG, "已请求快速连接间隔 (15~30ms)");
}

static void new_session(void) {
    s_subscribed = false;
    s_app_hello = false;
    s_auto_hello = false;
    s_mtu = 23;
}

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn = event->connect.conn_handle;
                new_session();
                s_mtu = ble_att_mtu(s_conn);
                ESP_LOGI(TAG, "App 已连接 conn=%u MTU=%u", (unsigned)s_conn, (unsigned)s_mtu);
                request_fast_conn();
            } else {
                ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle == s_conn) {
                ESP_LOGI(TAG, "App 断开 reason=0x%02x", event->disconnect.reason);
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                new_session();
                // 清空积压: 旧会话的帧发给新会话会造成"补放"
                tx_item_t discard;
                while (xQueueReceive(s_tx_q, &discard, 0) == pdTRUE) {}
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_MTU:
            if (event->mtu.conn_handle == s_conn) {
                s_mtu = event->mtu.value;
                ESP_LOGI(TAG, "MTU = %u", (unsigned)s_mtu);
                if (s_mtu < QY_MIN_MTU_STATE) {
                    ESP_LOGE(TAG, "MTU %u < %d: 96 字节状态帧发不出去(奇艺不支持分片)",
                             (unsigned)s_mtu, QY_MIN_MTU_STATE);
                }
                request_fast_conn();
            }
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.conn_handle != s_conn || event->subscribe.attr_handle != s_data_handle)
                break;
            if (event->subscribe.cur_notify) {
                // 订阅瞬间清空旧帧: 订阅前的任何帧对 App 都是"没有上下文的历史"
                tx_item_t discard;
                while (xQueueReceive(s_tx_q, &discard, 0) == pdTRUE) {}
                s_subscribed = true;
                s_sub_ms = now_ms();
                s_auto_hello = false;
                ESP_LOGI(TAG, "App 已订阅 fff6");
            } else {
                s_subscribed = false;
                ESP_LOGW(TAG, "App 取消订阅");
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_TX:
            if (event->notify_tx.conn_handle == s_conn &&
                event->notify_tx.attr_handle == s_data_handle &&
                event->notify_tx.status != 0) {
                ESP_LOGE(TAG, "通知发送失败 status=%d", event->notify_tx.status);
            }
            break;
        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 发送泵 (host 任务)
// ---------------------------------------------------------------------------
static void tx_tick(struct ble_npl_event *ev) {
    (void)ev;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) start_advertising();

    // 官方 App 有时反过来等魔方先开口: 订阅后 800ms 没有 AppHello 就主动推 Hello。
    // 但如果此刻还没拿到实体魔方的快照，Hello 里的状态就是旧的（App 会先显示错的状态），
    // 所以最多等到 2.5s，等不到再按当前模型发。
    if (s_subscribed && !s_app_hello && !s_auto_hello && now_ms() - s_sub_ms > 800) {
        if (!bridge_model_valid() && now_ms() - s_sub_ms < 2500) {
            // 再等等快照
        } else {
            s_auto_hello = true;
            if (!bridge_model_valid()) {
                ESP_LOGW(TAG, "2.5s 未拿到实体魔方状态, 按当前模型推 CubeHello");
            } else {
                ESP_LOGW(TAG, "800ms 未收到 AppHello, 主动推 CubeHello");
            }
            runtime_post_hello();
        }
    }

    if (s_subscribed && s_mtu >= QY_MIN_MTU_STATE) {
        for (int i = 0; i < 8; i++) {
            tx_item_t item;
            if (xQueuePeek(s_tx_q, &item, 0) != pdTRUE) break;
            uint8_t wire[QY_WIRE_STATE];
            memcpy(wire, item.data, item.len);
            size_t wlen = qy_seal(wire, item.len);
            struct os_mbuf *om = ble_hs_mbuf_from_flat(wire, (uint16_t)wlen);
            if (!om) break;
            int rc = ble_gatts_notify_custom(s_conn, s_data_handle, om);
            if (rc == 0) {
                tx_item_t drop;
                xQueueReceive(s_tx_q, &drop, 0);      // 成功提交才出队
                continue;
            }
            // 提交失败就保持队首不动，下一 tick 原序重试 —— 绝不跳帧
            break;
        }
    }
    ble_npl_callout_reset(&s_tick, pdMS_TO_TICKS(8));
}

void qy_server_on_host_sync(void) {
    int rc = ble_hs_id_set_rnd(s_addr_le);
    if (rc != 0) ESP_LOGE(TAG, "ble_hs_id_set_rnd rc=%d", rc);
    ESP_LOGI(TAG, "  BLE 地址: CC:A3:00:00:%02X:%02X (随机静态)", s_phys_mac[4], s_phys_mac[5]);
    if (!s_tick_ready) {
        ble_npl_callout_init(&s_tick, nimble_port_get_dflt_eventq(), tx_tick, NULL);
        s_tick_ready = true;
    }
    ble_npl_callout_reset(&s_tick, pdMS_TO_TICKS(10));
    start_advertising();
}
