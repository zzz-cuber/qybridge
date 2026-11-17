// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: cube_model.h —— 纯逻辑的魔方状态引擎（不依赖任何 ESP-IDF 头文件）
//
// 约定（全工程统一，务必牢记）:
//   面索引  face = 0..5  对应 U R F D L B
//   转动方向 dir = 0 顺时针(CW)  1 逆时针(CCW)，都是"面对该面看"的方向
//
// 本文件不包含任何 BLE / 协议 / 时间相关内容，因此可以在电脑上单独编译，
// 也可以被 selftest 直接调用做群论恒等式验证。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 面名，索引即 face
extern const char CUBE_FACE_LETTER[7];      // "URFDLB"

// CubieCube 模型（与 csTimer mathlib 的 ca/ea 布局一致）
//   ca[i] = 朝向 << 3 | 位置    （8 个角块）
//   ea[i] = 位置 << 1 | 朝向    （12 个棱块）
typedef struct {
    uint8_t ca[8];
    uint8_t ea[12];
} cube_t;

void cube_identity(cube_t *c);
void cube_copy(cube_t *dst, const cube_t *src);
bool cube_equal(const cube_t *a, const cube_t *b);

// 施加一次 90° 转动。face 0..5 = URFDLB，dir 0=CW 1=CCW。
void cube_turn(cube_t *c, int face, int dir);

// 施加一次 180°（等价于同一方向转两次，保留是为了公式表达清楚）
void cube_turn_half(cube_t *c, int face);

// 54 字符贴纸串，面序 URFDLB，每个字符是该贴纸所属的"面"（'U'..'B'）。
// 复原态 = "UUUUUUUUURRRRRRRRRFFFFFFFFFDDDDDDDDDLLLLLLLLLBBBBBBBBB"
void cube_facelet_string(const cube_t *c, char out[55]);

// 从 54 字符贴纸串反解 cubie（允许整体配色不同，以中心块为准）。
// 中心块重复 / 颜色数量不对 / 奇偶不合法 / 无法回环校验时返回 false 且不改动 c。
bool cube_from_facelet_string(cube_t *c, const char in[55]);

// 是否处于复原态（6 面各自同色）
bool cube_is_solved(const cube_t *c);

// --- 奇艺 27 字节贴纸块（已按奇艺色码打包）---
// 低半字节在前；面序 URFDLB；色码 L=0 R=1 D=2 U=3 F=4 B=5
void cube_pack_qy27(const cube_t *c, uint8_t out[27]);
// 反向解析。返回 false 表示这 27 字节不是一个合法状态。
bool cube_unpack_qy27(cube_t *c, const uint8_t in[27]);

// 调试用: 打印 "U R' F2 ..." 形式的解（capacity 为字符数上限）
size_t cube_alg_to_string(const uint8_t *faces, const uint8_t *dirs, size_t n,
                          char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
