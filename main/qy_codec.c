// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: qy_codec.c —— 奇艺 QYSC 编解码实现
#include "qy_codec.h"

#include <string.h>
#include "aes/esp_aes.h"

// 奇艺固定密钥（公开逆向事实）
static const uint8_t QY_AES_KEY[16] = {
    0x57, 0xB1, 0xF9, 0xAB, 0xCD, 0x5A, 0xE8, 0xA7,
    0x9C, 0xB9, 0x8C, 0xE7, 0x57, 0x8C, 0x51, 0x08,
};

// 内部面 URFDLB -> 奇艺转动码里的面序号（顺序为 L R D U F B）
static const uint8_t QY_MOVE_FACE_INDEX[6] = {3, 1, 4, 2, 0, 5};

uint16_t qy_crc16_modbus(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

uint8_t qy_move_code(int face, int dir) {
    if (face < 0 || face > 5) return 0;
    uint8_t idx = QY_MOVE_FACE_INDEX[face];
    // dir: 0=CW -> 奇艺 dir 位 1；1=CCW -> dir 位 0
    uint8_t bit = dir ? 0u : 1u;
    return (uint8_t)(((idx << 1) | bit) + 1);
}

bool qy_move_decode(uint8_t code, int *face, int *dir) {
    if (code < 1 || code > 12) return false;
    uint8_t idx = (uint8_t)((code - 1) >> 1);
    uint8_t bit = (uint8_t)((code - 1) & 1u);
    // 反查 QY_MOVE_FACE_INDEX
    int f = -1;
    for (int i = 0; i < 6; i++) {
        if (QY_MOVE_FACE_INDEX[i] == idx) { f = i; break; }
    }
    if (f < 0) return false;
    if (face) *face = f;
    if (dir) *dir = bit ? 0 : 1;   // 奇艺 dir 位 1 = CW
    return true;
}

const char *qy_move_name(uint8_t code) {
    static const char *NAMES[13] = {
        "??", "L'", "L", "R'", "R", "D'", "D", "U'", "U", "F'", "F", "B'", "B",
    };
    return (code <= 12) ? NAMES[code] : "??";
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// 写 "opcode 之后" 的公共部分: ts + facelets + 偏移34/35
static void write_head(uint8_t *out, size_t total, uint8_t opcode, uint32_t ticks,
                       const uint8_t face27[27], uint8_t battery) {
    memset(out, 0, total);
    out[0] = 0xFE;
    out[2] = opcode;
    put_be32(out + 3, ticks);
    memcpy(out + 7, face27, 27);
    out[34] = 0;          // 真机抓包里这里是 0
    out[35] = battery > 100 ? 100 : battery;
}

// 计算 CRC 并填到 len-2 处
static void finish_plain(uint8_t *frame, size_t total) {
    frame[1] = (uint8_t)total;
    uint16_t crc = qy_crc16_modbus(frame, total - 2);
    frame[total - 2] = (uint8_t)(crc & 0xFF);
    frame[total - 1] = (uint8_t)(crc >> 8);
}

size_t qy_build_hello(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery) {
    write_head(out, QY_LEN_SHORT, QY_OP_HELLO, ticks, face27, battery);
    finish_plain(out, QY_LEN_SHORT);
    return QY_LEN_SHORT;
}

size_t qy_build_sync_confirm(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery) {
    write_head(out, QY_LEN_SHORT, QY_OP_SYNC_CONFIRM, ticks, face27, battery);
    finish_plain(out, QY_LEN_SHORT);
    return QY_LEN_SHORT;
}

size_t qy_build_current_state(uint8_t *out, uint32_t ticks, const uint8_t face27[27], uint8_t battery) {
    write_head(out, QY_LEN_SHORT, QY_OP_CURRENT, ticks, face27, battery);
    finish_plain(out, QY_LEN_SHORT);
    return QY_LEN_SHORT;
}

size_t qy_build_state_change(uint8_t *out, uint32_t ticks, const uint8_t face27[27],
                             uint8_t move_code, uint8_t battery,
                             const qy_hist_t *hist, size_t hist_n, bool solved_ack) {
    write_head(out, QY_LEN_STATE, QY_OP_STATE_CHANGE, ticks, face27, battery);
    out[QY_MOVE_OFF] = move_code;

    // ★ 实测(2026-11-01, 真机+真 App): 把历史区填上会让 App 把这些动作**重放一遍**,
    //   表现为"拧一下虚拟魔方动好几步、最终颜色还错"; 全部填 0xFF(App 无事可做)
    //   就完全正常。所以默认 hist_n = 0。
    //   真机抓包里历史区是有内容的, 说明这个 App 对历史区的用法和我们理解的不一样
    //   (方向或时间戳窗口), 在没搞清之前发空历史是最安全的。
    memset(out + QY_HISTORY_FIRST_OFF, 0xFF, QY_HISTORY_SLOTS * 5);
    for (size_t i = 0; i < hist_n && i < QY_HISTORY_SLOTS; i++) {
        size_t slot = QY_HISTORY_SLOTS - 1 - i;              // i=0 -> 槽 10
        uint8_t *p = out + QY_HISTORY_FIRST_OFF + slot * 5;
        put_be32(p, hist[i].ticks);
        p[4] = hist[i].move;
    }
    out[QY_ACK_OFF] = solved_ack ? 1 : 0;

    finish_plain(out, QY_LEN_STATE);
    return QY_LEN_STATE;
}

static void aes_block(const uint8_t *in, uint8_t *out, bool encrypt) {
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, QY_AES_KEY, 128);
    esp_aes_crypt_ecb(&ctx, encrypt ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT, in, out);
    esp_aes_free(&ctx);
}

size_t qy_seal(uint8_t *frame, size_t plain_len) {
    size_t wire = (plain_len + 15u) & ~(size_t)15u;
    memset(frame + plain_len, 0, wire - plain_len);
    for (size_t off = 0; off < wire; off += 16) {
        uint8_t tmp[16];
        memcpy(tmp, frame + off, 16);
        aes_block(tmp, frame + off, true);
    }
    return wire;
}

bool qy_unseal(uint8_t *frame, size_t wire_len) {
    if (wire_len < 16 || (wire_len % 16) != 0) return false;
    for (size_t off = 0; off < wire_len; off += 16) {
        uint8_t tmp[16];
        memcpy(tmp, frame + off, 16);
        aes_block(tmp, frame + off, false);
    }
    return true;
}

bool qy_parse_app_frame(const uint8_t *plain, size_t len, qy_app_msg_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (len < 4) return false;

    // 主路径: 标准 0xFE 帧
    if (plain[0] == 0xFE) {
        size_t flen = plain[1];
        if (flen < 4 || flen > len) return false;
        uint16_t want = (uint16_t)(plain[flen - 2] | (plain[flen - 1] << 8));
        if (qy_crc16_modbus(plain, flen - 2) != want) return false;

        if (flen == 9) {
            // 9 字节有两种: RequestState(05 05 05 05 05) 和 ACK(回显我们帧的 3..7 字节)
            static const uint8_t REQ[5] = {0x05, 0x05, 0x05, 0x05, 0x05};
            bool is_req = memcmp(plain + 2, REQ, 5) == 0;
            out->kind = is_req ? QY_APP_REQUEST_STATE : QY_APP_ACK;
            return true;
        }
        if (flen == 21) {
            out->kind = QY_APP_HELLO;
            return true;
        }
        if (flen == QY_LEN_SHORT && plain[2] == QY_OP_SYNC_CONFIRM) {
            out->kind = QY_APP_SYNC_STATE;
            memcpy(out->face27, plain + 7, 27);
            return true;
        }
        return false;
    }

    // 兼容路径: 部分 App 版本用 FC 4C 08 下达"设状态/重置"
    // 布局: FC 4C 08 <ts:4 LE> <facelets:27>
    if (len >= 8 + 27 && plain[0] == 0xFC && plain[1] == 0x4C && plain[2] == 0x08) {
        out->kind = QY_APP_SYNC_STATE;
        memcpy(out->face27, plain + 8, 27);
        return true;
    }
    return false;
}
