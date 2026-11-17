// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: runtime.h —— bridge 状态的唯一持有任务
//
// 为什么要有它: bridge 的 cubie 状态必须只有一个写者。
//   * BLE host 任务(收发回调) 只投递事件
//   * 控制台任务也只投递事件
//   * 本任务串行处理，推进状态、产出奇艺帧
// 这样既不用锁，也不会出现"NimBLE 回调里阻塞导致协议栈卡死"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "moyu_codec.h"
#include "qy_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RT_CMD_NONE = 0,
    RT_CMD_RESYNC,      // 强制重新同步
    RT_CMD_RESET_CUBE,  // 给实体魔方下发 0xA2
    RT_CMD_TOGGLE_POLICY,
    RT_CMD_HIST_0,      // 历史区模式切换（现场排查）
    RT_CMD_HIST_1,
    RT_CMD_HIST_2,
    RT_CMD_HIST_3,
    RT_CMD_MOVE_ON,     // 偏移34 转动码开关
    RT_CMD_MOVE_OFF,
} rt_cmd_t;

// 创建队列、初始化 bridge、启动任务
bool runtime_start(void);

// ---- 实体魔方(BLE Central 的 host 任务回调) ----
bool runtime_post_moyu(const moyu_msg_t *msg);
bool runtime_post_cube(bool connected);

// ---- 奇艺 App(BLE Peripheral 的 host 任务回调) ----
bool runtime_post_hello(void);
bool runtime_post_current(void);
bool runtime_post_app(const qy_app_msg_t *msg);

// ---- 控制台 ----
bool runtime_post_command(rt_cmd_t cmd);

#ifdef __cplusplus
}
#endif
