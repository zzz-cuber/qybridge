// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: move_led.c —— 步数指示灯实现
//
// 只做一件事: 收到"闪一下"的请求 -> 绿灯亮 40ms -> 熄灭。
// 时间应参照旧项目验证过的 WS2812 参数（GPIO48 / RMT 10MHz / GRB / 高位先发），
// 这套参数在本机的 ESP32-S3 N16R8 上确实能亮绿色。
#include "move_led.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"

static const char *TAG = "led";

#define MOVE_LED_GPIO 48      // ESP32-S3 N16R8 开发板板载 WS2812 常见引脚
#define MOVE_LED_ON_MS 40     // 亮 40ms，和 cubebridge 的"绿色微光短闪"一致
#define MOVE_LED_GREEN 64     // 0..255，够醒目又不刺眼

static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;
static uint8_t s_grb[3];          // RMT 异步发送期间必须保持有效
static TaskHandle_t s_task;
static bool s_ready;

static bool send_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_ready) return false;
    if (rmt_tx_wait_all_done(s_ch, 100) != ESP_OK) return false;
    esp_rom_delay_us(300);        // 确保上一帧已锁存
    s_grb[0] = g;                 // WS2812 是 GRB 顺序
    s_grb[1] = r;
    s_grb[2] = b;
    rmt_transmit_config_t tx = {
        .loop_count = 0,
        .flags.eot_level = 0,     // 发完保持低电平（>50us 即复位码，锁存颜色）
    };
    esp_err_t err = rmt_transmit(s_ch, s_enc, s_grb, sizeof(s_grb), &tx);
    if (err != ESP_OK) ESP_LOGD(TAG, "WS2812 transmit: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void led_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (send_rgb(0, MOVE_LED_GREEN, 0)) {
            vTaskDelay(pdMS_TO_TICKS(MOVE_LED_ON_MS));
            send_rgb(0, 0, 0);
            rmt_tx_wait_all_done(s_ch, 100);
        }
        // 快拧时会有多个请求积压，这里一次性清掉，只保留"闪一下"的语义
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

void move_led_blink(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

void move_led_init(void) {
    rmt_tx_channel_config_t ch_cfg = {
        .gpio_num = MOVE_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10MHz: 1 tick = 100ns
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
        .flags.init_level = 0,
    };
    rmt_symbol_word_t bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 };
    rmt_symbol_word_t bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 };
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = bit0,
        .bit1 = bit1,
        .flags.msb_first = 1,     // WS2812 高位先发，否则亮度会被读错
    };

    if (rmt_new_tx_channel(&ch_cfg, &s_ch) != ESP_OK ||
        rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK ||
        rmt_enable(s_ch) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812(GPIO%d) 初始化失败, 步数指示灯不可用(不影响其它功能)",
                 MOVE_LED_GPIO);
        return;
    }
    s_ready = true;
    send_rgb(0, 0, 0);            // 上电先熄灭：WS2812 会记住上次颜色
    rmt_tx_wait_all_done(s_ch, 100);

    if (xTaskCreate(led_task, "move_led", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "LED 任务创建失败");
        s_task = NULL;
    } else {
        ESP_LOGI(TAG, "步数指示灯就绪 (GPIO%d, 每转译一步闪一下)", MOVE_LED_GPIO);
    }
}
