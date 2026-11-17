// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: app_main.c —— 启动、NimBLE 初始化、串口控制台
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "bridge.h"
#include "cube_model.h"
#include "move_led.h"
#include "moyu_client.h"
#include "qy_server.h"
#include "runtime.h"
#include "selftest.h"

static const char *TAG = "app";

#define FIRMWARE_TAG "qybridge 1.0.0 (MoYu WCU -> QiYi QYSC)"

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    ESP_LOGI(TAG, "NimBLE host 已同步");
    qy_server_on_host_sync();
    moyu_client_on_host_sync();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset reason=%d", reason);
}

// ---------------------------------------------------------------------------
// 串口控制台
// ---------------------------------------------------------------------------
static void print_help(void) {
    printf("\n命令:\n"
           "  state     打印本地虚拟魔方状态\n"
           "  stats     打印统计\n"
           "  test      跑一次完整自检\n"
           "  rescan    重新扫描实体魔方\n"
           "  clear     清除配对记录\n"
           "  reset     给实体魔方下发 0xA2 复原\n"
           "  resync    强制重新同步（丢步后用）\n"
           "  policy    查看/切换 \"App 复原重置是否带动实体魔方\"\n"
           "  hist N    历史区模式 0..3 (排查\"一转就乱\"用)\n"
           "  move N    偏移34 转动码 1=发 0=不发 (排查用)\n"
           "  log       查看/切换日志级别 (info/debug)\n"
           "  help      本帮助\n\n");
}

static void cmd_state(void) {
    cube_t c = {0};
    (void)c;
    bridge_stats_t st;
    bridge_get_stats(&st);
    printf("本地状态: %s  (快照有效=%s, 已连接=%s)\n",
           st.model_valid ? "有效" : "未知",
           st.model_valid ? "是" : "否",
           moyu_client_connected() ? "是" : "否");
    printf("实体魔方: %s  App 订阅: %s\n",
           moyu_client_state_name(), qy_server_subscribed() ? "是" : "否");
}

static void cmd_stats(void) {
    bridge_stats_t st;
    bridge_get_stats(&st);
    printf("转译步数=%lu 丢步=%lu 中层转=%lu App强制状态=%lu 实体重置=%lu 状态请求=%lu\n",
           (unsigned long)st.moves_out, (unsigned long)st.gaps,
           (unsigned long)st.slice_hits, (unsigned long)st.sync_adopt,
           (unsigned long)st.phys_resets, (unsigned long)st.status_req);
    printf("排查旋钮: 历史区模式=%d (0=全FF 1=最新在86 2=最新在36 3=含当前步), "
           "转动码=%s\n",
           bridge_get_hist_mode(), bridge_get_send_move() ? "发" : "不发");
    // 最关键的两个数: 与实体魔方快照比对的一致/不一致次数
    printf("模型核对: 一致=%lu 不一致=%lu  %s\n",
           (unsigned long)st.verify_ok, (unsigned long)st.verify_bad,
           st.verify_bad == 0 ? "(本地模型可信)" : "(本地模型已经漂了!)");
}

static void console_task(void *arg) {
    (void)arg;
    char line[128];
    print_help();
    while (fgets(line, sizeof(line), stdin)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strcmp(line, "help") || !strcmp(line, "?")) {
            print_help();
        } else if (!strcmp(line, "state")) {
            cmd_state();
        } else if (!strcmp(line, "stats")) {
            cmd_stats();
        } else if (!strcmp(line, "test")) {
            selftest_run(true);
        } else if (!strcmp(line, "rescan")) {
            moyu_client_rescan();
            printf("已请求重新扫描\n");
        } else if (!strcmp(line, "clear")) {
            moyu_client_clear_pairing();
            moyu_client_rescan();
        } else if (!strcmp(line, "reset")) {
            runtime_post_command(RT_CMD_RESET_CUBE);
            printf("已请求下发 0xA2\n");
        } else if (!strcmp(line, "resync")) {
            runtime_post_command(RT_CMD_RESYNC);
            printf("已请求重新同步\n");
        } else if (!strcmp(line, "policy")) {
            runtime_post_command(RT_CMD_TOGGLE_POLICY);
            printf("已切换策略（结果见日志）\n");
        } else if (!strncmp(line, "hist", 4)) {
            int m = (line[4] == ' ') ? (line[5] - '0') : -1;
            if (m < 0 || m > 3) {
                printf("用法: hist 0|1|2|3\n  0=历史区全 0xFF  1=最新在偏移86(默认)\n"
                       "  2=最新在偏移36  3=当前步也放进最新槽\n");
            } else {
                runtime_post_command((rt_cmd_t)(RT_CMD_HIST_0 + m));
                printf("已切换历史区模式 -> %d\n", m);
            }
        } else if (!strncmp(line, "move", 4)) {
            int on = (line[5] - '0');
            runtime_post_command(on ? RT_CMD_MOVE_ON : RT_CMD_MOVE_OFF);
            printf("偏移34 转动码: %s\n", on ? "发送" : "发送 0");
        } else if (!strcmp(line, "log")) {
            static bool dbg;
            dbg = !dbg;
            esp_log_level_set("bridge", dbg ? ESP_LOG_DEBUG : ESP_LOG_INFO);
            printf("bridge 日志: %s\n", dbg ? "DEBUG" : "INFO");
        } else if (line[0]) {
            printf("未知命令: %s (输入 help)\n", line);
        }
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "固件: %s", FIRMWARE_TAG);
    qy_server_prepare_identity();
    move_led_init();     // 板载 WS2812: 每转译一步闪一下(失败只是不亮)

    // 先自检: 帧格式/色码/转动表任何一项错了都不该开始连魔方
    if (selftest_run(false) != 0) {
        ESP_LOGE(TAG, "自检未通过, 功能可能异常(仍继续启动, 便于抓日志)");
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_att_set_preferred_mtu(247);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    qy_server_register();
    moyu_client_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);

    if (!runtime_start()) {
        ESP_LOGE(TAG, "runtime 启动失败");
    }
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
