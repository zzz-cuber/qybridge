// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: bridge.c —— 转译核心实现
#include "bridge.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bridge";

static struct {
    bridge_sink_t sink;
    bridge_cmd_t cmd;
    uint32_t (*now_ms_fn)(void);

    cube_t cube;
    bool valid;

    uint32_t ms_clock;        // 虚拟毫秒时钟
    uint32_t last_ticks;
    bool ticks_valid;

    qy_hist_t hist[QY_HISTORY_SLOTS];   // hist[0] = 上一步
    size_t hist_n;
    int hist_mode;      // 现场可调, 见 bridge_set_hist_mode
    bool send_move;     // 是否在偏移 34 填转动码

    uint8_t last_step;
    bool step_valid;
    uint32_t verify_at;      // 到点向魔方要一次快照做比对（只核对，不覆盖）
    uint8_t battery;
    bool physical_reset;
    uint32_t status_req_ms;
    bridge_stats_t stats;
} s;

static uint32_t default_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t now_ms(void) { return s.now_ms_fn ? s.now_ms_fn() : default_now_ms(); }

void bridge_init(bridge_sink_t sink, bridge_cmd_t cmd) {
    memset(&s, 0, sizeof(s));
    s.sink = sink;
    s.cmd = cmd;
    s.now_ms_fn = default_now_ms;
    s.physical_reset = true;
    // 历史区默认不发: 实测这个 App 会把历史区里的动作重放一遍, 导致"拧一下动好几步"。
    // 详见 docs/02-DNF根因分析.md 第 8 节。串口 hist 1/2/3 可现场切回做对比实验。
    s.hist_mode = 0;
    s.send_move = true;
    // 在拿到实体魔方的电量帧之前不能报 0: 有些 App 会因为"电量不足"直接拒绝开局,
    // 而且 0% 会让用户以为板子/魔方坏了。先报 100, 收到 0xA4 后立刻用真实值覆盖。
    s.battery = 100;
    cube_identity(&s.cube);
    s.valid = false;

    // 自检: 状态引擎必须满足群论恒等式，否则后面全是错的
    cube_t c, id;
    cube_identity(&c);
    cube_identity(&id);
    for (int rep = 0; rep < 6; rep++) {
        cube_turn(&c, 1, 0);   // R
        cube_turn(&c, 0, 0);   // U
        cube_turn(&c, 1, 1);   // R'
        cube_turn(&c, 0, 1);   // U'
    }
    if (!cube_equal(&c, &id)) {
        ESP_LOGE(TAG, "状态引擎自检失败 (R U R' U')^6 != 恒等");
    } else {
        ESP_LOGI(TAG, "状态引擎自检通过 ((R U R' U')^6 = 恒等)");
    }
}

void bridge_set_clock(uint32_t (*fn)(void)) { s.now_ms_fn = fn ? fn : default_now_ms; }

void bridge_set_physical_reset(bool enable) { s.physical_reset = enable; }
bool bridge_get_physical_reset(void) { return s.physical_reset; }

// 现场排查用: 历史区/转动码的行为可以现场切换，不用重烧固件
void bridge_set_hist_mode(int mode) {
    if (mode < 0 || mode > 3) return;
    s.hist_mode = mode;
    ESP_LOGW(TAG, "历史区模式 = %d (%s)", mode,
             mode == 0 ? "不填, 全 0xFF" :
             mode == 1 ? "最新在偏移86" :
             mode == 2 ? "最新在偏移36" : "当前步也放进最新槽");
}

int bridge_get_hist_mode(void) { return s.hist_mode; }

void bridge_set_send_move(bool on) {
    s.send_move = on;
    ESP_LOGW(TAG, "偏移34 转动码: %s", on ? "发送" : "发送 0");
}

bool bridge_get_send_move(void) { return s.send_move; }

static void request_status(int why) {
    if (!s.cmd) return;
    uint32_t t = now_ms();
    // 节流: 状态未知期间每来一帧转动都会走到这里, 不能把 0xA3 刷屏
    if (s.status_req_ms && (uint32_t)(t - s.status_req_ms) < 300) return;
    uint8_t pkt[MOYU_PKT_LEN];
    moyu_build_simple(pkt, MOYU_OP_STATUS);
    s.cmd(pkt);
    s.status_req_ms = t ? t : 1;
    s.stats.status_req++;
    ESP_LOGD(TAG, "请求实体魔方状态 (reason=%d)", why);
}

void bridge_force_resync(void) {
    s.valid = false;
    s.step_valid = false;
    s.stats.model_valid = false;
    request_status(1);
}

void bridge_force_reset_cube(void) {
    if (!s.cmd) return;
    uint8_t pkt[MOYU_PKT_LEN];
    moyu_build_reset(pkt);
    s.cmd(pkt);
    s.stats.phys_resets++;
    // 魔方收到 0xA2 后会回一帧 0xA2(带新的状态与步数)，以那一帧为新的基准
    s.step_valid = false;
    request_status(6);
    ESP_LOGW(TAG, "已向实体魔方下发 0xA2 重置(复原态)");
}

static uint32_t next_ticks(void) {
    uint32_t t = qy_ms_to_ticks(s.ms_clock);
    if (s.ticks_valid && (int32_t)(t - s.last_ticks) <= 0) {
        t = s.last_ticks + 2;   // 至少 +2 tick(1.25ms)，保证严格递增
    }
    s.last_ticks = t;
    s.ticks_valid = true;
    return t;
}

static void hist_push(uint32_t ticks, uint8_t move) {
    size_t n = s.hist_n < QY_HISTORY_SLOTS ? s.hist_n + 1 : QY_HISTORY_SLOTS;
    for (size_t i = n - 1; i > 0; i--) s.hist[i] = s.hist[i - 1];
    s.hist[0].ticks = ticks;
    s.hist[0].move = move;
    s.hist_n = n;
}

void bridge_on_cube_connected(void) {
    s.valid = false;
    s.step_valid = false;
    s.hist_n = 0;
    s.ticks_valid = false;
    s.ms_clock = now_ms();
    s.stats.model_valid = false;
    ESP_LOGI(TAG, "实体魔方已连接: 状态置为未知, 请求快照");
    request_status(2);
}

void bridge_on_cube_disconnected(void) {
    s.valid = false;
    s.step_valid = false;
    s.stats.model_valid = false;
}

// 把实体魔方报来的 54 字符贴纸串(URFDLB) 解成 cubie
static bool cube_from_moyu_facelet(cube_t *c, const char facelet[55]) {
    return cube_from_facelet_string(c, facelet);
}

static void adopt_state(const cube_t *c, uint8_t step, bool have_step, const char *why) {
    cube_copy(&s.cube, c);
    s.valid = true;
    s.stats.model_valid = true;
    // 换了基准之后, 旧的历史步序对 App 已经没有意义, 清空更安全
    s.hist_n = 0;
    if (have_step) {
        s.last_step = step;
        s.step_valid = true;
    }
    char s54[55];
    cube_facelet_string(&s.cube, s54);
    ESP_LOGI(TAG, "采纳状态 (%s): %s%s", why, s54, cube_is_solved(&s.cube) ? "  [已复原]" : "");
}

static void emit_move(int face, int dir, uint16_t dt_ms) {    // 时间戳: 优先用魔方上报的间隔（同一报文里的多步才有真实的时间间隔，
    // 否则会被压成同一毫秒，App 会把这些步一次性播完，看起来"一下动好几步"）。
    // 只有间隔不可信时(0 或 0xFFFF 空槽标记)才退回真实时钟。
    if (dt_ms >= 1 && dt_ms <= 2000) {
        s.ms_clock += dt_ms;
        uint32_t wall = now_ms();
        if ((int32_t)(wall - s.ms_clock) > 0) s.ms_clock = wall;
    } else {
        uint32_t wall = now_ms();
        if ((int32_t)(wall - s.ms_clock) > 0) s.ms_clock = wall;
    }

    cube_turn(&s.cube, face, dir);
    uint32_t ticks = next_ticks();
    uint8_t code = qy_move_code(face, dir);

    uint8_t face27[27];
    cube_pack_qy27(&s.cube, face27);
    bool solved = cube_is_solved(&s.cube);

    uint8_t frame[QY_WIRE_STATE];
    // 历史区按当前模式组装（见 bridge_set_hist_mode）
    qy_hist_t h[QY_HISTORY_SLOTS];
    size_t hn = 0;
    if (s.hist_mode != 0) {
        if (s.hist_mode == 3) {          // 当前这一步也放进最新槽（GAN-main 的写法）
            h[hn].move = code;
            h[hn].ticks = ticks;
            hn++;
        }
        for (size_t i = 0; i < s.hist_n && hn < QY_HISTORY_SLOTS; i++) h[hn++] = s.hist[i];
        if (s.hist_mode == 2) {          // 改成"最新在偏移 36"（上一版固件的写法）
            for (size_t i = 0; i < hn / 2; i++) {
                qy_hist_t t = h[i];
                h[i] = h[hn - 1 - i];
                h[hn - 1 - i] = t;
            }
        }
    }
    size_t len = qy_build_state_change(frame, ticks, face27,
                                       s.send_move ? code : 0, s.battery,
                                       h, hn, solved);
    hist_push(ticks, code);
    s.stats.moves_out++;
    s.verify_at = now_ms() + 400;   // 一批动作停下来后核对一次

    if (s.sink) s.sink(frame, len);

    ESP_LOGI(TAG, "[mv %lu] %-2s ts=%lu dt=%ums%s", (unsigned long)s.stats.moves_out,
             qy_move_name(code), (unsigned long)ticks, (unsigned)dt_ms,
             solved ? "  => ACK=1 (复原)" : "");
}

static void handle_status(const moyu_msg_t *msg) {
    cube_t c;
    if (!cube_from_moyu_facelet(&c, msg->facelet)) {
        ESP_LOGW(TAG, "实体魔方状态非法, 忽略: %s", msg->facelet);
        return;
    }
    if (!s.valid) {
        adopt_state(&c, msg->step, true, "首次快照");
        // App 可能在我们拿到快照之前就连上并从 Hello 里读到了旧值，
        // 这里补一帧 CurrentState 把真实状态交给它（未订阅时会被发送层直接丢掉）。
        if (s.sink) {
            uint8_t frame[QY_WIRE_STATE];
            uint8_t f27[27];
            cube_pack_qy27(&s.cube, f27);
            size_t len = qy_build_current_state(frame, next_ticks(), f27, s.battery);
            s.sink(frame, len);
            ESP_LOGI(TAG, "已向 App 补发当前状态");
        }
        return;
    }
    if (!s.step_valid) {
        // 状态已经由别的途径(例如 App 的 SyncState)建立，这里只补步数基准
        s.last_step = msg->step;
        s.step_valid = true;
        ESP_LOGI(TAG, "以快照步数作为动作基准 step=%u", msg->step);
        return;
    }
    // 已经同步: 只比对, 不覆盖 —— 快照会过期，覆盖它就是上一版"虚晃弹回"的根因
    if (!cube_equal(&c, &s.cube)) {
        char a[55], b[55];
        cube_facelet_string(&s.cube, a);
        cube_facelet_string(&c, b);
        s.stats.verify_bad++;
        if (s.stats.verify_bad <= 5) {   // 前几次打全, 之后只计数, 免得刷屏
            ESP_LOGE(TAG, "[verify] 不一致(仅告警, 不覆盖) 第%lu次\n  本地: %s\n  魔方: %s",
                     (unsigned long)s.stats.verify_bad, a, b);
        }
    } else {
        s.stats.verify_ok++;
        // 前 3 次打出来, 证明验证回路真的在跑
        if (s.stats.verify_ok <= 3) {
            ESP_LOGI(TAG, "[verify] 与实体魔方一致 (#%lu)", (unsigned long)s.stats.verify_ok);
        }
    }
}

static void handle_move(const moyu_msg_t *msg) {
    if (!s.valid || !s.step_valid) {
        // 还没拿到快照: 不动本地状态, 等快照回来再用 delta 补齐(魔方每帧都带最近 5 步)
        request_status(3);
        return;
    }
    uint8_t delta = (uint8_t)(msg->move_step - s.last_step);
    if (delta == 0) return;                      // 重复帧

    if (delta > msg->move_count) {
        s.stats.gaps++;
        ESP_LOGE(TAG, "丢步: 计数跳变 %u > 本帧可回补 %u, 重新同步", delta, msg->move_count);
        s.valid = false;
        s.step_valid = false;
        s.stats.model_valid = false;
        request_status(4);
        return;
    }

    for (int k = delta - 1; k >= 0; k--) {
        const moyu_move_t *m = &msg->moves[k];
        if (delta >= 2) {
            // 一次收到多步: 把整帧的 5 个码和步数打出来, 便于判断是"魔方虚报"还是"用户真转了"
            ESP_LOGW(TAG, "本帧 %u 步 step=%u 码=[%u %u %u %u %u] 间隔=[%u %u %u %u %u]",
                     delta, msg->move_step,
                     msg->moves[0].raw_code, msg->moves[1].raw_code,
                     msg->moves[2].raw_code, msg->moves[3].raw_code, msg->moves[4].raw_code,
                     msg->moves[0].dt_ms, msg->moves[1].dt_ms, msg->moves[2].dt_ms,
                     msg->moves[3].dt_ms, msg->moves[4].dt_ms);
        }
        if (m->slice) {
            // 奇艺只有 12 个面转码, M/E/S 数学上无法表达。
            // 绝不静默丢弃(丢弃 = App 动作模型缺一步 = 必然 DNF), 而是停机告警,
            // 由用户按 App 的"复原重置"触发一次完整重同步。
            s.stats.slice_hits++;
            ESP_LOGE(TAG, "收到中层转 raw=%u, 奇艺协议无法表达 -> 状态置为未知, 请按 App 复原重置",
                     m->raw_code);
            s.valid = false;
            s.step_valid = false;
            s.stats.model_valid = false;
            request_status(5);
            return;
        }
        emit_move(m->face, m->dir, m->dt_ms);
    }
    s.last_step = msg->move_step;
}

void bridge_on_moyu(const moyu_msg_t *msg) {
    switch (msg->kind) {
        case MOYU_MSG_STATUS:
            handle_status(msg);
            break;
        case MOYU_MSG_MOVE:
            handle_move(msg);
            break;
        case MOYU_MSG_POWER:
            s.battery = msg->battery;
            ESP_LOGI(TAG, "电量 %u%%%s", msg->battery, msg->charging ? " (充电中)" : "");
            break;
        case MOYU_MSG_INFO:
            ESP_LOGI(TAG, "魔方信息: %s", msg->name);
            break;
        default:
            break;
    }
}

size_t bridge_build_hello(uint8_t *plain_out) {
    uint8_t face27[27];
    cube_pack_qy27(&s.cube, face27);
    uint32_t ticks = next_ticks();
    if (!s.valid) {
        ESP_LOGW(TAG, "发送 Hello 时本地状态未知(先按当前模型发送)");
    }
    return qy_build_hello(plain_out, ticks, face27, s.battery);
}

size_t bridge_build_current(uint8_t *plain_out) {
    uint8_t face27[27];
    cube_pack_qy27(&s.cube, face27);
    return qy_build_current_state(plain_out, next_ticks(), face27, s.battery);
}

void bridge_on_app_hello(void) { ESP_LOGI(TAG, "收到 App Hello"); }

void bridge_on_app_sync(const uint8_t face27[27]) {
    cube_t app;
    if (!cube_unpack_qy27(&app, face27)) {
        ESP_LOGW(TAG, "App 下发的状态不合法, 仅回确认");
    } else {
        bool app_solved = cube_is_solved(&app);
        bool was_solved = s.valid && cube_is_solved(&s.cube);
        if (!s.valid) {
            adopt_state(&app, 0, false, "App 握手初始化");
        } else if (cube_equal(&app, &s.cube)) {
            ESP_LOGI(TAG, "App 同步: 与本地一致%s", app_solved ? " (复原态)" : "");
        } else {
            s.stats.sync_adopt++;
            adopt_state(&app, 0, false, "App 强制(复原重置/重新同步)");
        }
        // App 说"复原"而实体魔方不是复原态 -> 这正是"复原重置"按钮的语义:
        // 不仅虚拟魔方复原, 也要让实体魔方内核回到复原态。
        if (app_solved && !was_solved && s.physical_reset && s.cmd) {
            bridge_force_reset_cube();
        }
    }

    // 无论如何都要回 SyncConfirm, 否则 App 会一直等
    if (s.sink) {
        uint8_t frame[QY_WIRE_STATE];
        uint8_t f27[27];
        cube_pack_qy27(&s.cube, f27);
        size_t len = qy_build_sync_confirm(frame, next_ticks(), f27, s.battery);
        s.sink(frame, len);
    }
}

bool bridge_model_valid(void) { return s.valid; }

// 周期性调用: 动作停下来 400ms 后向实体魔方要一次快照, 只比对不覆盖。
// 这是本项目"我们到底对不对"的唯一实测判据:
//   verify_bad == 0  -> 本地模型与实体魔方始终一致, 乱的是 App 那侧
//   verify_bad >  0  -> 转译链路自己就漂了, 先修这里
void bridge_tick(void) {
    if (s.verify_at && (int32_t)(now_ms() - s.verify_at) >= 0) {
        s.verify_at = 0;
        if (s.valid) request_status(7);
    }
}

void bridge_get_stats(bridge_stats_t *out) {
    if (!out) return;
    *out = s.stats;
    out->model_valid = s.valid;
}
