// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: move_led.h —— 步数指示灯（每转译一步闪一下）
//
// 用途: 比赛现场没有电脑时，靠它一眼确认"板子还活着、动作确实进来了"。
//       转一步 → 绿灯闪一下；不闪 = 没收到动作（魔方没连上/没唤醒/被别的 App 占用）。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化板载 WS2812（ESP32-S3 常见为 GPIO48）。失败只是不亮，不影响其它功能。
void move_led_init(void);

// 请求闪一下。非阻塞，可从任意任务调用；连续调用会合并（不会堆积）。
void move_led_blink(void);

#ifdef __cplusplus
}
#endif
