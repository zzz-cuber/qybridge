// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: qy_codec.h —— 奇艺 QYSC 线上格式编解码（纯函数，无 BLE 依赖）
//
// 事实来源（三份独立材料互相印证，勿凭印象修改）:
//   1) qiyi_smartcube_protocol-master/README.md（真实抓包 + Wireshark 解析器）
//   2) GAN-main/shared/src/protocol.rs 的 QiYi 编码器（作者用真机 App 校准过面序）
//   3) 上一版固件能连上 App 的部分（GATT/AES/CRC 已实证可用）
//
// 明文帧（AES 之前）布局：
//   偏移 0        0xFE
//   偏移 1        帧长（含 0xFE、长度、内容、CRC；不含补零）
//   偏移 2        opcode: 0x02 Hello / 0x03 StateChange / 0x04 SyncConfirm / 0x05 CurrentState
//   偏移 3..6     时间戳 u32 大端，单位 1.6 tick/ms（即 ms*8/5）
//   偏移 7..33    魔方状态 27 字节（低半字节在前，面序 URFDLB）
//   偏移 34       StateChange: 本次转动码；其它帧: 0
//   偏移 35       电量 0..100
//   偏移 36..90   StateChange: 历史 11 槽（每槽 时间戳u32BE + 转动码），空槽 0xFF×5
//   偏移 91       StateChange: ack（1 = 本帧状态为复原态）
//   末尾 2 字节   CRC16-MODBUS（小端），覆盖 [0, len-2)
//   之后补 0 到 16 的倍数，再整段 AES-128-ECB 加密
//
// ★ 历史区的方向是最容易做错的一处：
//   真实魔方把"最新的一步"放在偏移 86（第 11 槽），越旧越往偏移 36 方向排，
//   未用到的槽是 0xFF。上一版固件把最新一步放在了偏移 36（方向反了），
//   App 拿这段历史去补/核对步数时就会错位 —— 这是"本次复原步骤不完整"的
//   主要嫌疑之一。见 docs/02-DNF根因分析.md。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QY_OP_HELLO        0x02
#define QY_OP_STATE_CHANGE 0x03
#define QY_OP_SYNC_CONFIRM 0x04
#define QY_OP_CURRENT      0x05

#define QY_LEN_SHORT 38      // Hello / SyncConfirm / CurrentState 的明文帧长
#define QY_LEN_STATE 94      // StateChange 的明文帧长
#define QY_WIRE_SHORT 48     // 补零并 AES 之后的线上长度
#define QY_WIRE_STATE 96     // 补零并 AES 之后的线上长度（要求 ATT MTU >= 99）

#define QY_HISTORY_SLOTS 11
#define QY_HISTORY_FIRST_OFF 36
#define QY_HISTORY_LAST_OFF  (QY_HISTORY_FIRST_OFF + (QY_HISTORY_SLOTS - 1) * 5)  // 86
#define QY_ACK_OFF 91
#define QY_MOVE_OFF 34

// 一次转动在历史区里的记录
typedef struct {
    uint8_t move;     // 奇艺转动码 1..12
    uint32_t ticks;   // 绝对时间戳（1.6 tick/ms）
} qy_hist_t;

// ---------- 基础工具 ----------
// ms -> 奇艺 tick（App 侧按 tick/1.6 还原成毫秒）
static inline uint32_t qy_ms_to_ticks(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * 8u) / 5u);
}
uint16_t qy_crc16_modbus(const uint8_t *data, size_t len);

// ---------- 转动码 ----------
// 1..12 = L' L R' R D' D U' U F' F B' B
// App 解码: face_idx = (code-1)>>1 依次 L R D U F B；dir_bit = (code-1)&1，0=逆时针
uint8_t qy_move_code(int face, int dir);           // face 0..5 = URFDLB, dir 0=CW 1=CCW
bool qy_move_decode(uint8_t code, int *face, int *dir);
const char *qy_move_name(uint8_t code);            // "U" / "U'" / "??"

// ---------- 组帧（明文）----------
// 所有 out 缓冲区请一律给到 QY_WIRE_STATE(96) 字节，省得逐处核对。
// 返回值 = 明文帧长（38 或 94）
size_t qy_build_hello(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery);
size_t qy_build_sync_confirm(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery);
size_t qy_build_current_state(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery);
// hist: 历史记录，hist[0] 是"上一步"（不含本次），可为 NULL
size_t qy_build_state_change(uint8_t *out, uint32_t ticks, const uint8_t face27[27],
                             uint8_t move_code, uint8_t battery,
                             const qy_hist_t *hist, size_t hist_n, bool solved_ack);

// 明文帧 -> 线上密文（补零到 16 的倍数 + AES-128-ECB 原地加密）
// 返回线上长度（48 / 96）；buffer 需至少 96 字节
size_t qy_seal(uint8_t *frame, size_t plain_len);

// 线上密文 -> 明文（原地解密）。返回解密后的可用长度（不校验 CRC）。
bool qy_unseal(uint8_t *frame, size_t wire_len);

// ---------- App -> 魔方 的帧 ----------
typedef enum {
    QY_APP_NONE = 0,
    QY_APP_HELLO,          // 21 字节，App 首次握手
    QY_APP_ACK,            // 9 字节，App 对我们某帧的确认
    QY_APP_REQUEST_STATE,  // 9 字节 FE 09 05 05 05 05 05，请求当前状态
    QY_APP_SYNC_STATE,     // 38 字节，App 下达"把状态设成这样"（连接 / 解完 / 复原重置）
} qy_app_kind_t;

typedef struct {
    qy_app_kind_t kind;
    uint8_t face27[27];    // 仅 SYNC_STATE 有效
} qy_app_msg_t;

// 解析明文形态的 App 帧（调用前先 qy_unseal）
bool qy_parse_app_frame(const uint8_t *plain, size_t len, qy_app_msg_t *out);

#ifdef __cplusplus
}
#endif
