// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: moyu_codec.c —— 魔域位流编解码实现
#include "moyu_codec.h"

#include <string.h>
#include "aes/esp_aes.h"

// 根密钥/IV（公开逆向事实；与 GAN 同派生方案）
static const uint8_t MOYU_KEY_BASE[16] = {
    21, 119, 58, 92, 103, 14, 45, 31, 23, 103, 42, 19, 155, 103, 82, 87};
static const uint8_t MOYU_IV_BASE[16] = {
    17, 35, 38, 37, 134, 42, 44, 59, 85, 6, 127, 49, 126, 103, 33, 87};

static const char MOYU_FACE_ALPHABET[7] = "FBUDLR";   // 0=F 1=B 2=U 3=D 4=L 5=R
// 贴纸块的面顺序: [2,5,0,3,4,1] -> U R F D L B
static const uint8_t MOYU_FACE_ORDER[6] = {2, 5, 0, 3, 4, 1};

// 字母 -> 工程内部面索引(URFDLB)
static int face_from_letter(char ch) {
    switch (ch) {
        case 'U': return 0;
        case 'R': return 1;
        case 'F': return 2;
        case 'D': return 3;
        case 'L': return 4;
        case 'B': return 5;
        default: return -1;
    }
}

// ---------- 位读取 ----------
typedef struct {
    const uint8_t *buf;
    size_t bits;
} bitreader_t;

static uint32_t br_read(bitreader_t *r, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r->bits >= MOYU_PKT_LEN * 8u) return v;   // 越界: 返回已读到的部分
        size_t byte = r->bits >> 3;
        unsigned bit = 7u - (unsigned)(r->bits & 7u);
        v = (v << 1) | ((r->buf[byte] >> bit) & 1u);
        r->bits++;
    }
    return v;
}

// ---------- 位写入 ----------
typedef struct {
    uint8_t *buf;
    size_t bits;
} bitwriter_t;

static void bw_write(bitwriter_t *w, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; i++) {
        if (w->bits >= MOYU_PKT_LEN * 8u) return;
        size_t byte = w->bits >> 3;
        unsigned bit = 7u - (unsigned)(w->bits & 7u);
        uint32_t b = (v >> (n - 1 - i)) & 1u;
        if (b) w->buf[byte] |= (uint8_t)(1u << bit);
        else   w->buf[byte] &= (uint8_t)~(1u << bit);
        w->bits++;
    }
}

// ---------- 加解密 ----------
void moyu_crypto_setup(moyu_crypto_t *c, const uint8_t mac[6]) {
    memcpy(c->key, MOYU_KEY_BASE, 16);
    memcpy(c->iv, MOYU_IV_BASE, 16);
    for (int i = 0; i < 6; i++) {
        c->key[i] = (uint8_t)((c->key[i] + mac[5 - i]) % 255);
        c->iv[i] = (uint8_t)((c->iv[i] + mac[5 - i]) % 255);
    }
    c->ready = true;
}

static void aes_ecb_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16], bool enc) {
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, key, 128);
    esp_aes_crypt_ecb(&ctx, enc ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT, in, out);
    esp_aes_free(&ctx);
}

// 20 字节帧: 首 16 字节与末 16 字节各自 AES，再与 IV 异或（首末块重叠属正常）
void moyu_crypto_decode(const moyu_crypto_t *c, uint8_t buf[MOYU_PKT_LEN]) {
    uint8_t blk[16], out[16];
    size_t off = MOYU_PKT_LEN - 16;
    memcpy(blk, buf + off, 16);
    aes_ecb_block(c->key, blk, out, false);
    for (int i = 0; i < 16; i++) buf[off + i] = (uint8_t)(out[i] ^ c->iv[i]);

    memcpy(blk, buf, 16);
    aes_ecb_block(c->key, blk, out, false);
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)(out[i] ^ c->iv[i]);
}

void moyu_crypto_encode(const moyu_crypto_t *c, uint8_t buf[MOYU_PKT_LEN]) {
    uint8_t blk[16], out[16];
    blk[0] = 0;
    for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(buf[i] ^ c->iv[i]);
    aes_ecb_block(c->key, blk, out, true);
    memcpy(buf, out, 16);

    size_t off = MOYU_PKT_LEN - 16;
    memcpy(blk, buf + off, 16);
    for (int i = 0; i < 16; i++) blk[i] ^= c->iv[i];
    aes_ecb_block(c->key, blk, out, true);
    memcpy(buf + off, out, 16);
}

// ---------- 解析 ----------
// 48 个 3bit 贴纸 -> 54 字母 URFDLB
//
// ★ 实测修正（2026-11，真机 WCU_MY32_CD35）:
//   位流里的 6 个面块就是自然顺序 F B U D L R，色码也是 FBUDLR 的索引
//   （复原时：F 面全 0、B 面全 1、U 面全 2、D 全 3、L 全 4、R 全 5）。
//   DCTimer 里的 [2,5,0,3,4,1] 置换是另一批型号的写法，用在这台机器上会把
//   U/R/F/D/L/B 整个错位，表现为"状态非法"。
//   所以：流里第 s 块 = 物理面 s -> 写到输出字符串的 MOYU_FACE_ORDER[s] 那一段。
static bool parse_facelets(bitreader_t *r, char out[55]) {
    out[54] = 0;
    for (int s = 0; s < 6; s++) {
        int out_face = MOYU_FACE_ORDER[s];
        if (out_face > 5) return false;
        int base = out_face * 9;
        for (int j = 0; j < 8; j++) {
            uint32_t v = br_read(r, 3);
            if (v > 5) return false;
            out[base + (j < 4 ? j : j + 1)] = MOYU_FACE_ALPHABET[v];
        }
        out[base + 4] = MOYU_FACE_ALPHABET[s];   // 中心块 = 该面自身颜色
    }
    return true;
}

static void parse_move_code(moyu_move_t *m, uint32_t code, uint32_t dt) {
    m->raw_code = (uint8_t)code;
    m->dt_ms = (uint16_t)(dt > 0xFFFF ? 0xFFFF : dt);
    m->slice = code > 11;
    m->dir = (uint8_t)(code & 1u);           // 1 = 逆时针(')
    if (code <= 11) {
        // "FBUDLR"[code>>1] 即面字母，再换算成内部面索引(URFDLB)
        int fi = face_from_letter(MOYU_FACE_ALPHABET[code >> 1]);
        m->face = (uint8_t)(fi < 0 ? 0 : fi);
    } else {
        m->face = 0xFF;                      // 中层转 M/E/S: 奇艺 12 码无法表达
    }
}

bool moyu_parse(moyu_msg_t *out, uint8_t buf[MOYU_PKT_LEN]) {
    memset(out, 0, sizeof(*out));
    bitreader_t r = {buf, 0};
    uint8_t opcode = (uint8_t)br_read(&r, 8);
    out->opcode = opcode;

    switch (opcode) {
        case MOYU_OP_INFO: {
            out->kind = MOYU_MSG_INFO;
            for (int i = 0; i < 8; i++) {
                uint8_t ch = (uint8_t)br_read(&r, 8);
                out->name[i] = (char)ch;
            }
            out->name[8] = 0;
            break;
        }
        case MOYU_OP_STATUS:
        case MOYU_OP_RESET: {   // 0xA2 重置后魔方会回一帧带状态的 0xA2
            out->kind = MOYU_MSG_STATUS;
            if (!parse_facelets(&r, out->facelet)) return false;
            out->step = (uint8_t)br_read(&r, 8);
            break;
        }
        case MOYU_OP_POWER: {
            out->kind = MOYU_MSG_POWER;
            out->battery = (uint8_t)br_read(&r, 8);
            out->charging = br_read(&r, 8) == 1;
            if (out->battery > 100) out->battery = 0;
            break;
        }
        case MOYU_OP_MOVE: {
            out->kind = MOYU_MSG_MOVE;
            uint32_t dur[5];
            for (int i = 0; i < 5; i++) dur[i] = br_read(&r, 16);
            out->move_step = (uint8_t)br_read(&r, 8);
            uint32_t codes[5];
            for (int i = 0; i < 5; i++) codes[i] = br_read(&r, 5);
            out->move_count = 5;
            for (int i = 0; i < 5; i++) {
                parse_move_code(&out->moves[i], codes[i], dur[i]);
            }
            break;
        }
        default:
            out->kind = MOYU_MSG_OTHER;
            break;
    }
    return true;
}

void moyu_build_simple(uint8_t out[MOYU_PKT_LEN], uint8_t opcode) {
    memset(out, 0, MOYU_PKT_LEN);
    out[0] = opcode;
}

void moyu_build_gyro(uint8_t out[MOYU_PKT_LEN], bool enable) {
    memset(out, 0, MOYU_PKT_LEN);
    out[0] = MOYU_OP_GYRO;
    out[2] = enable ? 1 : 0;
}

void moyu_build_reset(uint8_t out[MOYU_PKT_LEN]) {
    memset(out, 0, MOYU_PKT_LEN);
    bitwriter_t w = {out, 0};
    bw_write(&w, 8, MOYU_OP_RESET);
    for (uint32_t f = 0; f < 6; f++) {
        for (int i = 0; i < 8; i++) bw_write(&w, 3, f);
    }
}
