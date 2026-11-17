// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: cube_model.c —— 魔方状态引擎实现
//
// 数学来源: csTimer mathlib 的 CubieCube（角块/棱块置换 + 朝向）。
// 复合顺序极其重要: 在状态 c 上施加转动 m 的正确写法是
//     c' = c ∘ m        （cornMult(c, m)）
// 写成 m ∘ c 时单步看起来也对（从复原态出发两者等价），但连续转动会把顺序
// 颠倒，表现为"越转越乱"。上一版项目在这里翻过车，所以本工程用群论恒等式
// (R U R' U')^6 = I、T-perm^2 = I 在 selftest 里把这条钉死。
#include "cube_model.h"

#include <string.h>
#include <stdio.h>

const char CUBE_FACE_LETTER[7] = "URFDLB";

// 6 个基本 90° 转动（CW），索引 0..5 = U R F D L B
static const uint8_t BASE_CA[6][8] = {
    {3, 0, 1, 2, 4, 5, 6, 7},          // U
    {20, 1, 2, 8, 15, 5, 6, 19},       // R
    {9, 21, 2, 3, 16, 12, 6, 7},       // F
    {0, 1, 2, 3, 5, 6, 7, 4},          // D
    {0, 10, 22, 3, 4, 17, 13, 7},      // L
    {0, 1, 11, 23, 4, 5, 18, 14},      // B
};
static const uint8_t BASE_EA[6][12] = {
    {6, 0, 2, 4, 8, 10, 12, 14, 16, 18, 20, 22},   // U
    {16, 2, 4, 6, 22, 10, 12, 14, 8, 18, 20, 0},   // R
    {0, 19, 4, 6, 8, 17, 12, 14, 3, 11, 20, 22},   // F
    {0, 2, 4, 6, 10, 12, 14, 8, 16, 18, 20, 22},   // D
    {0, 2, 20, 6, 8, 10, 18, 14, 16, 4, 12, 22},   // L
    {0, 2, 4, 23, 8, 10, 12, 21, 16, 18, 7, 15},   // B
};

// 预计算的 18 种转动: index = face*3 + turn  (turn 0=CW 1=180 2=CCW)
static cube_t s_moves[18];
static bool s_moves_ready;

static void corner_mult(const cube_t *a, const cube_t *b, cube_t *out) {
    for (int i = 0; i < 8; i++) {
        int ori = ((a->ca[b->ca[i] & 7] >> 3) + (b->ca[i] >> 3)) % 3;
        out->ca[i] = (uint8_t)((a->ca[b->ca[i] & 7] & 7) | (ori << 3));
    }
}

static void edge_mult(const cube_t *a, const cube_t *b, cube_t *out) {
    for (int i = 0; i < 12; i++) {
        out->ea[i] = (uint8_t)(a->ea[b->ea[i] >> 1] ^ (b->ea[i] & 1));
    }
}

static void cube_mult(const cube_t *a, const cube_t *b, cube_t *out) {
    corner_mult(a, b, out);
    edge_mult(a, b, out);
}

static void moves_init(void) {
    if (s_moves_ready) return;
    for (int f = 0; f < 6; f++) {
        cube_t base;
        memcpy(base.ca, BASE_CA[f], 8);
        memcpy(base.ea, BASE_EA[f], 12);
        s_moves[f * 3 + 0] = base;                                          // CW
        cube_mult(&base, &base, &s_moves[f * 3 + 1]);                       // 180
        cube_mult(&s_moves[f * 3 + 1], &base, &s_moves[f * 3 + 2]);         // CCW
    }
    s_moves_ready = true;
}

void cube_identity(cube_t *c) {
    for (int i = 0; i < 8; i++) c->ca[i] = (uint8_t)i;
    for (int i = 0; i < 12; i++) c->ea[i] = (uint8_t)(i * 2);
}

void cube_copy(cube_t *dst, const cube_t *src) { memcpy(dst, src, sizeof(*dst)); }

bool cube_equal(const cube_t *a, const cube_t *b) {
    return memcmp(a->ca, b->ca, 8) == 0 && memcmp(a->ea, b->ea, 12) == 0;
}

void cube_turn(cube_t *c, int face, int dir) {
    if (face < 0 || face > 5 || dir < 0 || dir > 1) return;
    moves_init();
    cube_t tmp;
    cube_mult(c, &s_moves[face * 3 + (dir ? 2 : 0)], &tmp);
    *c = tmp;
}

void cube_turn_half(cube_t *c, int face) {
    if (face < 0 || face > 5) return;
    moves_init();
    cube_t tmp;
    cube_mult(c, &s_moves[face * 3 + 1], &tmp);
    *c = tmp;
}

// 贴纸映射（csTimer 标准布局）
static const int8_t C_FACELET[8][3] = {
    {8, 9, 20}, {6, 18, 38}, {0, 36, 47}, {2, 45, 11},
    {29, 26, 15}, {27, 44, 24}, {33, 53, 42}, {35, 17, 51},
};
static const int8_t E_FACELET[12][2] = {
    {5, 10}, {7, 19}, {3, 37}, {1, 46},
    {32, 16}, {28, 25}, {30, 43}, {34, 52},
    {23, 12}, {21, 41}, {50, 39}, {48, 14},
};

void cube_facelet_string(const cube_t *c, char out[55]) {
    int perm[54];
    for (int i = 0; i < 54; i++) perm[i] = i;

    for (int i = 0; i < 8; i++) {
        int j = c->ca[i] & 0x7;
        int ori = c->ca[i] >> 3;
        for (int n = 0; n < 3; n++) {
            perm[C_FACELET[i][(n + ori) % 3]] = C_FACELET[j][n];
        }
    }
    for (int i = 0; i < 12; i++) {
        int j = c->ea[i] >> 1;
        int ori = c->ea[i] & 1;
        for (int n = 0; n < 2; n++) {
            perm[E_FACELET[i][(n + ori) % 2]] = E_FACELET[j][n];
        }
    }
    for (int i = 0; i < 54; i++) out[i] = CUBE_FACE_LETTER[perm[i] / 9];
    out[54] = 0;
}

bool cube_from_facelet_string(cube_t *c, const char in[55]) {
    int8_t f[54];
    const char centers[6] = {in[4], in[13], in[22], in[31], in[40], in[49]};
    uint8_t counts[6] = {0};

    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < i; j++) {
            if (centers[j] == centers[i]) return false;   // 中心色重复
        }
    }
    for (int i = 0; i < 54; i++) {
        int v = -1;
        for (int j = 0; j < 6; j++) {
            if (in[i] == centers[j]) { v = j; break; }
        }
        if (v < 0) return false;
        f[i] = (int8_t)v;
        counts[v]++;
    }
    for (int i = 0; i < 6; i++) {
        if (counts[i] != 9) return false;
    }

    // 角块
    uint8_t ca[8];
    for (int i = 0; i < 8; i++) {
        int ori;
        for (ori = 0; ori < 3; ori++) {
            int cv = f[C_FACELET[i][ori]];
            if (cv == 0 || cv == 3) break;               // U 或 D 色
        }
        if (ori == 3) return false;
        int col1 = f[C_FACELET[i][(ori + 1) % 3]];
        int col2 = f[C_FACELET[i][(ori + 2) % 3]];
        ca[i] = 0xff;
        for (int j = 0; j < 8; j++) {
            if (col1 == C_FACELET[j][1] / 9 && col2 == C_FACELET[j][2] / 9) {
                ca[i] = (uint8_t)(j | ((ori % 3) << 3));
                break;
            }
        }
        if (ca[i] == 0xff) return false;
    }

    // 棱块
    uint8_t ea[12];
    for (int i = 0; i < 12; i++) {
        ea[i] = 0xff;
        for (int j = 0; j < 12; j++) {
            if (f[E_FACELET[i][0]] == E_FACELET[j][0] / 9 &&
                f[E_FACELET[i][1]] == E_FACELET[j][1] / 9) {
                ea[i] = (uint8_t)(j << 1);
                break;
            }
            if (f[E_FACELET[i][0]] == E_FACELET[j][1] / 9 &&
                f[E_FACELET[i][1]] == E_FACELET[j][0] / 9) {
                ea[i] = (uint8_t)((j << 1) | 1);
                break;
            }
        }
        if (ea[i] == 0xff) return false;
    }

    // 合法性: 每个块唯一、角朝向和 %3==0、棱朝向和==0、置换奇偶一致
    unsigned seen_c = 0, seen_e = 0, twists = 0, flips = 0, cp = 0, ep = 0;
    for (int i = 0; i < 8; i++) {
        unsigned piece = ca[i] & 7;
        if (seen_c & (1u << piece)) return false;
        seen_c |= 1u << piece;
        twists += ca[i] >> 3;
        for (int j = 0; j < i; j++) cp ^= (ca[j] & 7) > piece;
    }
    for (int i = 0; i < 12; i++) {
        unsigned piece = ea[i] >> 1;
        if (seen_e & (1u << piece)) return false;
        seen_e |= 1u << piece;
        flips ^= ea[i] & 1;
        for (int j = 0; j < i; j++) ep ^= (ea[j] >> 1) > piece;
    }
    if (twists % 3 || flips || cp != ep) return false;

    // 回环: 解出来的 cubie 必须能还原出同一张贴纸
    cube_t decoded;
    memcpy(decoded.ca, ca, sizeof(ca));
    memcpy(decoded.ea, ea, sizeof(ea));
    char canonical[55];
    cube_facelet_string(&decoded, canonical);
    for (int i = 0; i < 54; i++) {
        if (canonical[i] != CUBE_FACE_LETTER[f[i]]) return false;
    }

    memcpy(c->ca, ca, 8);
    memcpy(c->ea, ea, 12);
    return true;
}

bool cube_is_solved(const cube_t *c) {
    char s[55];
    cube_facelet_string(c, s);
    for (int i = 0; i < 54; i++) {
        if (s[i] != s[(i / 9) * 9]) return false;
    }
    return true;
}

// --- 奇艺 27 字节贴纸块 ---
// 内部色码 URFDLB(0..5) -> 奇艺色码(L=0 R=1 D=2 U=3 F=4 B=5)
static const uint8_t QY_COLOR_FROM_FACE[6] = {3, 1, 4, 2, 0, 5};
// 奇艺色码 -> 内部面索引，用于反解
static const int8_t QY_FACE_FROM_COLOR[6] = {4, 1, 3, 0, 2, 5};

void cube_pack_qy27(const cube_t *c, uint8_t out[27]) {
    char s[55];
    cube_facelet_string(c, s);
    memset(out, 0, 27);
    for (int i = 0; i < 54; i++) {
        int face = 0;
        while (face < 6 && CUBE_FACE_LETTER[face] != s[i]) face++;
        uint8_t nib = QY_COLOR_FROM_FACE[face];
        if (i & 1) out[i >> 1] |= (uint8_t)(nib << 4);
        else       out[i >> 1] |= nib;
    }
}

bool cube_unpack_qy27(cube_t *c, const uint8_t in[27]) {
    char s[55];
    for (int i = 0; i < 54; i++) {
        uint8_t byte = in[i >> 1];
        uint8_t nib = (i & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
        if (nib > 5) return false;
        s[i] = CUBE_FACE_LETTER[QY_FACE_FROM_COLOR[nib]];
    }
    s[54] = 0;
    cube_t tmp = *c;
    if (!cube_from_facelet_string(&tmp, s)) return false;
    *c = tmp;
    return true;
}

size_t cube_alg_to_string(const uint8_t *faces, const uint8_t *dirs, size_t n,
                          char *out, size_t capacity) {
    size_t used = 0;
    if (!capacity) return 0;
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(out + used, capacity - used, "%s%c%s",
                         used ? " " : "", CUBE_FACE_LETTER[faces[i]],
                         dirs[i] ? "'" : "");
        if (w < 0 || (size_t)w >= capacity - used) break;
        used += (size_t)w;
    }
    return used;
}
