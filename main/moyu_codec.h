// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: moyu_codec.h —— 魔域 WCU_MY32 / MY3 线上格式编解码（纯函数）
//
// 事实来源:
//   1) btcube-web-main/src/moyu/protocol.ts（v5 位布局，逐位移位写得很清楚）
//   2) GAN-main/scripts/proxy/moyu32.py（DCTimer 血统，真机验证过）
//   3) 上一版固件 cube_client.c 的 MY3 分支（能连上真机）
//
// 帧固定 20 字节。位序为大端（每字节高位在前）。
//   0xA1 设备信息
//   0xA3 状态:   bit8..152  6 面 × 8 个 3bit 贴纸；bit152..160 步数计数
//   0xA4 电量:   bit8..16 电量；bit16..24 充电状态
//   0xA5 转动:   bit8..88   5 × 16bit 时间间隔(ms)
//                bit88..96  步数计数（mod 256）
//                bit96..121 5 × 5bit 转动码（[0] 是最近一步）
//                bit121..127 相邻面；bit127..136 角度；bit136..145 相对角度
//
// 转动码: code>>1 取 "FBUDLRMES"，code&1 = 1 表示逆时针(')。
//   0..11 = F B U D L R 六面；12..17 = M E S 中层（奇艺 12 码无法表达）
//
// 贴纸色码同样是 "FBUDLR" 的索引；面块顺序 [2,5,0,3,4,1] 即 U R F D L B。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOYU_PKT_LEN 20
#define MOYU_OP_INFO   0xA1
#define MOYU_OP_STATUS 0xA3
#define MOYU_OP_POWER  0xA4
#define MOYU_OP_MOVE   0xA5
#define MOYU_OP_RESET  0xA2
#define MOYU_OP_GYRO   0xAC

// 转动码 > 11 表示中层转（M/E/S），奇艺协议无法表达
#define MOYU_CODE_MAX_FACE 11

// 密钥/IV 派生器（与 GAN 同方案，%255 不是 %256）
typedef struct {
    uint8_t key[16];
    uint8_t iv[16];
    bool ready;
} moyu_crypto_t;

void moyu_crypto_setup(moyu_crypto_t *c, const uint8_t mac[6]);
void moyu_crypto_encode(const moyu_crypto_t *c, uint8_t buf[MOYU_PKT_LEN]);
void moyu_crypto_decode(const moyu_crypto_t *c, uint8_t buf[MOYU_PKT_LEN]);

typedef enum {
    MOYU_MSG_UNKNOWN = 0,
    MOYU_MSG_INFO,
    MOYU_MSG_STATUS,
    MOYU_MSG_POWER,
    MOYU_MSG_MOVE,
    MOYU_MSG_OTHER,
} moyu_msg_kind_t;

// 一步转动（已换算成工程内部约定）
typedef struct {
    uint8_t face;      // 0..5 = URFDLB
    uint8_t dir;       // 0=CW 1=CCW
    uint8_t raw_code;  // 原始 5bit 码
    uint16_t dt_ms;    // 与"上一帧里更旧的一步"的间隔
    bool slice;        // true = M/E/S，奇艺无法表达
} moyu_move_t;

typedef struct {
    moyu_msg_kind_t kind;
    uint8_t opcode;
    // STATUS
    char facelet[55];      // 54 字符 URFDLB（字母即面名）
    uint8_t step;          // 步数计数
    // POWER
    uint8_t battery;
    bool charging;
    // MOVE
    uint8_t move_step;
    uint8_t move_count;              // 本帧携带的有效步数（通常 5）
    moyu_move_t moves[5];            // moves[0] 是最新一步
    // INFO
    char name[20];
} moyu_msg_t;

// 解密 + 解析一帧 20 字节。buf 会被原地解密（调用方需自行留副本时请先拷贝）。
bool moyu_parse(moyu_msg_t *out, uint8_t buf[MOYU_PKT_LEN]);

// 组一帧请求/命令（明文 20 字节）
void moyu_build_simple(uint8_t out[MOYU_PKT_LEN], uint8_t opcode);
void moyu_build_gyro(uint8_t out[MOYU_PKT_LEN], bool enable);
// 重置: 0xA2 + 48 个 3bit 贴纸（每面 8 个，填本面颜色 = 复原）
void moyu_build_reset(uint8_t out[MOYU_PKT_LEN]);

#ifdef __cplusplus
}
#endif
