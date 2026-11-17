// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: bridge.h —— 转译核心（唯一真相源 = 动作流）
//
// 设计原则（上一版项目 DNF 的根源就是违反了这几条）:
//   1. 只有一个真相源: 本地 cubie 状态只由"动作流"推进。
//      绝不因为"实体魔方快照看起来不一样"就中途覆盖本地状态 —— 那会造成
//      App 侧"虚晃一下又弹回去"，以及 App 的动作模型与贴纸对不上。
//   2. 一个物理动作 <=> 恰好一帧 StateChange。不丢、不合并、不重排、不补发。
//      App 收到 ack=1 时结束本次复原并按它自己的动作模型做 CFOP 复盘；
//      只要少一个动作，复盘就走不到复原态，于是弹"本次复原步骤不完整"。
//   3. 时间戳只增不减，且尽量还原魔方上报的真实间隔。
//   4. 状态只在三种时刻被"外部强制": 连接后的首次状态快照、App 下达 SyncState
//      (复原重置)、丢步后主动重新同步。除此以外一律由动作推导。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "cube_model.h"
#include "moyu_codec.h"
#include "qy_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t moves_out;      // 已转译的步数
    uint32_t gaps;           // 丢步(计数跳变过大)次数
    uint32_t slice_hits;     // 收到 M/E/S 中层转的次数
    uint32_t sync_adopt;     // App 强制状态次数
    uint32_t phys_resets;    // 下发给实体魔方的重置次数
    uint32_t status_req;     // 向实体魔方请求状态次数
    uint32_t verify_ok;      // 与实体魔方快照比对一致次数
    uint32_t verify_bad;     // 与实体魔方快照比对不一致次数
    bool model_valid;
    bool app_subscribed;
} bridge_stats_t;

// 输出: 一帧明文奇艺帧（38 或 94 字节）。由上层补零+AES+通知。
typedef void (*bridge_sink_t)(const uint8_t *plain, size_t len);
// 输出: 一帧要写给实体魔方的命令（已由调用方负责加密）
typedef void (*bridge_cmd_t)(const uint8_t plain[MOYU_PKT_LEN]);

void bridge_init(bridge_sink_t sink, bridge_cmd_t cmd);
// 可注入时钟（毫秒），默认使用 esp_timer
void bridge_set_clock(uint32_t (*now_ms_fn)(void));

// ---- 实体魔方侧 ----
void bridge_on_cube_connected(void);
void bridge_on_cube_disconnected(void);
void bridge_on_moyu(const moyu_msg_t *msg);
// 周期性回调（runtime 任务里每 50ms 调一次）：负责"只核对不覆盖"的验证请求
void bridge_tick(void);

// ---- 奇艺 App 侧 ----
void bridge_on_app_hello(void);
void bridge_on_app_sync(const uint8_t face27[27]);
size_t bridge_build_hello(uint8_t *plain_out);
size_t bridge_build_current(uint8_t *plain_out);
bool bridge_model_valid(void);
void bridge_get_stats(bridge_stats_t *out);

// 控制
void bridge_set_physical_reset(bool enable);
bool bridge_get_physical_reset(void);
// 现场排查用的两个旋钮（改动立即生效，不用重烧）
//   hist: 0=历史区全 0xFF  1=最新在偏移86(默认, 与真机一致)
//         2=最新在偏移36(上一版固件写法)  3=当前步也放进最新槽
void bridge_set_hist_mode(int mode);
int bridge_get_hist_mode(void);
//   send_move: false 时偏移 34 一律填 0，用来判断 App 是不是自己从历史区推步
void bridge_set_send_move(bool on);
bool bridge_get_send_move(void);
void bridge_force_resync(void);          // 标记未知 + 向魔方请求状态
void bridge_force_reset_cube(void);      // 直接下发 0xA2

#ifdef __cplusplus
}
#endif
