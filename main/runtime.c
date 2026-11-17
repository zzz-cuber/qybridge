// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: runtime.c —— bridge 任务与事件队列
#include "runtime.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "bridge.h"
#include "move_led.h"
#include "moyu_client.h"
#include "qy_server.h"

static const char *TAG = "runtime";

typedef enum {
    EV_MOYU = 0,
    EV_CUBE_UP,
    EV_CUBE_DOWN,
    EV_HELLO,
    EV_CURRENT,
    EV_APP,
    EV_CMD,
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    union {
        moyu_msg_t moyu;
        qy_app_msg_t app;
        rt_cmd_t cmd;
    } u;
} ev_t;

static QueueHandle_t s_q;

// bridge 的出口: 一帧明文奇艺帧 -> 外设发送队列
static void on_bridge_sink(const uint8_t *plain, size_t len) {
    qy_server_send(plain, len);
    // 只有"状态变化帧"(= 一步转动)才闪灯；Hello / SyncConfirm 这类控制帧不闪。
    if (len >= 3 && plain[2] == QY_OP_STATE_CHANGE) move_led_blink();
}

// bridge 的出口: 一帧命令 -> 实体魔方
static void on_bridge_cmd(const uint8_t plain[MOYU_PKT_LEN]) {
    moyu_client_write(plain);
}

static void handle_ev(const ev_t *ev) {
    switch (ev->kind) {
        case EV_MOYU:
            bridge_on_moyu(&ev->u.moyu);
            break;
        case EV_CUBE_UP:
            bridge_on_cube_connected();
            break;
        case EV_CUBE_DOWN:
            bridge_on_cube_disconnected();
            break;
        case EV_HELLO: {
            uint8_t frame[QY_WIRE_STATE];
            size_t len = bridge_build_hello(frame);
            qy_server_send(frame, len);
            ESP_LOGI(TAG, "已推送 CubeHello");
            break;
        }
        case EV_CURRENT: {
            uint8_t frame[QY_WIRE_STATE];
            size_t len = bridge_build_current(frame);
            qy_server_send(frame, len);
            ESP_LOGI(TAG, "已回复 CurrentState");
            break;
        }
        case EV_APP:
            bridge_on_app_sync(ev->u.app.face27);
            break;
        case EV_CMD:
            switch (ev->u.cmd) {
                case RT_CMD_RESYNC:
                    bridge_force_resync();
                    ESP_LOGW(TAG, "已强制重新同步");
                    break;
                case RT_CMD_RESET_CUBE:
                    bridge_force_reset_cube();
                    break;
                case RT_CMD_TOGGLE_POLICY:
                    bridge_set_physical_reset(!bridge_get_physical_reset());
                    ESP_LOGW(TAG, "App 复原重置 -> 实体魔方重置: %s",
                             bridge_get_physical_reset() ? "开" : "关");
                    break;
                case RT_CMD_HIST_0:
                    bridge_set_hist_mode(0);
                    break;
                case RT_CMD_HIST_1:
                    bridge_set_hist_mode(1);
                    break;
                case RT_CMD_HIST_2:
                    bridge_set_hist_mode(2);
                    break;
                case RT_CMD_HIST_3:
                    bridge_set_hist_mode(3);
                    break;
                case RT_CMD_MOVE_ON:
                    bridge_set_send_move(true);
                    break;
                case RT_CMD_MOVE_OFF:
                    bridge_set_send_move(false);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void runtime_task(void *arg) {
    (void)arg;
    ev_t ev;
    for (;;) {
        // 50ms 超时不是为了处理事件，而是给 bridge 一个周期去发起"只核对不覆盖"的验证
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_ev(&ev);
        }
        bridge_tick();
    }
}

static bool post(const ev_t *ev) {
    if (!s_q) return false;
    // 队列满说明链路/处理已经严重落后；宁可报错也不丢事件
    if (xQueueSend(s_q, ev, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "runtime 事件队列溢出");
        return false;
    }
    return true;
}

bool runtime_post_moyu(const moyu_msg_t *msg) {
    ev_t ev = { .kind = EV_MOYU };
    ev.u.moyu = *msg;
    return post(&ev);
}

bool runtime_post_cube(bool connected) {
    ev_t ev = { .kind = connected ? EV_CUBE_UP : EV_CUBE_DOWN };
    return post(&ev);
}

bool runtime_post_hello(void) {
    ev_t ev = { .kind = EV_HELLO };
    return post(&ev);
}

bool runtime_post_current(void) {
    ev_t ev = { .kind = EV_CURRENT };
    return post(&ev);
}

bool runtime_post_app(const qy_app_msg_t *msg) {
    ev_t ev = { .kind = EV_APP };
    ev.u.app = *msg;
    return post(&ev);
}

bool runtime_post_command(rt_cmd_t cmd) {
    ev_t ev = { .kind = EV_CMD };
    ev.u.cmd = cmd;
    return post(&ev);
}

bool runtime_start(void) {
    s_q = xQueueCreate(64, sizeof(ev_t));
    if (!s_q) return false;

    bridge_init(on_bridge_sink, on_bridge_cmd);

    if (xTaskCreate(runtime_task, "bridge", 4096, NULL, 6, NULL) != pdPASS) {
        return false;
    }
    ESP_LOGI(TAG, "bridge 任务已启动");
    return true;
}
