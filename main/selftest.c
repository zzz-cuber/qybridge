// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: selftest.c —— 上电自检
//
// 为什么要做: 这个项目最贵的错误(丢步、帧布局错、色码错)在电脑上没法一眼看出来，
// 而上电自检能在连手机之前就把它们挡住。串口会打印 PASS/FAIL，一目了然。
#include "selftest.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"

#include "bridge.h"
#include "cube_model.h"
#include "moyu_codec.h"
#include "qy_codec.h"

static const char *TAG = "selftest";
static int s_fail;
static bool s_verbose;

#define CHECK(cond, fmt, ...)                                        \
    do {                                                             \
        if (cond) {                                                  \
            if (s_verbose) ESP_LOGI(TAG, "  PASS: " fmt, ##__VA_ARGS__); \
        } else {                                                     \
            s_fail++;                                                \
            ESP_LOGE(TAG, "  FAIL: " fmt, ##__VA_ARGS__);            \
        }                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// 1. CRC / AES
// ---------------------------------------------------------------------------
static void test_crc_and_aes(void) {
    // 协议文档给了标准例子: fe 09 02 00 02 45 2c -> crc 0x1bef
    static const uint8_t v[] = {0xfe, 0x09, 0x02, 0x00, 0x02, 0x45, 0x2c};
    CHECK(qy_crc16_modbus(v, sizeof(v)) == 0x1bef, "CRC16-MODBUS 已知向量");

    uint8_t buf[QY_WIRE_STATE];
    for (int i = 0; i < 96; i++) buf[i] = (uint8_t)(i * 7 + 3);
    uint8_t orig[96];
    memcpy(orig, buf, 96);
    qy_seal(buf, 94);
    bool changed = memcmp(orig, buf, 96) != 0;
    qy_unseal(buf, 96);
    CHECK(changed && memcmp(orig, buf, 94) == 0, "AES-128-ECB 往返一致");
}

// ---------------------------------------------------------------------------
// 2. 贴纸打包（协议文档里给了复原态标准字节）
// ---------------------------------------------------------------------------
static void test_facelet_pack(void) {
    static const uint8_t SOLVED27[27] = {
        0x33, 0x33, 0x33, 0x33, 0x13, 0x11, 0x11, 0x11, 0x11,
        0x44, 0x44, 0x44, 0x44, 0x24, 0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x00, 0x50, 0x55, 0x55, 0x55, 0x55,
    };
    cube_t c;
    cube_identity(&c);
    uint8_t out[27];
    cube_pack_qy27(&c, out);
    CHECK(memcmp(out, SOLVED27, 27) == 0, "复原态 27 字节与文档逐字节一致");

    cube_t back;
    CHECK(cube_unpack_qy27(&back, SOLVED27) && cube_equal(&back, &c),
          "27 字节 -> cubie 往返一致");

    // 非法状态必须被拒（同一个色色块重复，奇偶不合法）
    uint8_t bad[27];
    memcpy(bad, SOLVED27, 27);
    bad[0] = 0x11;
    cube_t tmp;
    CHECK(!cube_unpack_qy27(&tmp, bad), "非法贴纸被拒绝");
}

// ---------------------------------------------------------------------------
// 3. 转动码映射（协议文档的表: 1=L' 2=L 3=R' 4=R 5=D' 6=D 7=U' 8=U 9=F' 10=F 11=B' 12=B）
// ---------------------------------------------------------------------------
static void test_move_codes(void) {
    struct { int face, dir; uint8_t code; const char *name; } cases[] = {
        {4, 1, 1, "L'"}, {4, 0, 2, "L"},   // face L=4
        {1, 1, 3, "R'"}, {1, 0, 4, "R"},
        {3, 1, 5, "D'"}, {3, 0, 6, "D"},
        {0, 1, 7, "U'"}, {0, 0, 8, "U"},
        {2, 1, 9, "F'"}, {2, 0, 10, "F"},
        {5, 1, 11, "B'"}, {5, 0, 12, "B"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code = qy_move_code(cases[i].face, cases[i].dir);
        int f = -1, d = -1;
        bool dec = qy_move_decode(code, &f, &d);
        if (code != cases[i].code || !dec || f != cases[i].face || d != cases[i].dir) {
            ESP_LOGE(TAG, "    映射错: face=%d dir=%d 期望 code=%u 实得 %u",
                     cases[i].face, cases[i].dir, cases[i].code, code);
            ok = false;
        }
        if (strcmp(qy_move_name(code), cases[i].name) != 0) ok = false;
    }
    CHECK(ok, "12 个转动码与文档表一致");
}

// ---------------------------------------------------------------------------
// 4. StateChange 帧布局
// ---------------------------------------------------------------------------
static void test_state_frame_layout(void) {
    cube_t c;
    cube_identity(&c);
    uint8_t f27[27];
    cube_pack_qy27(&c, f27);

    qy_hist_t hist[3] = {
        {.move = qy_move_code(0, 0), .ticks = 1000},   // 上一步 U
        {.move = qy_move_code(1, 1), .ticks = 900},    // 再上一步 R'
        {.move = qy_move_code(2, 0), .ticks = 800},    // 再上一步 F
    };
    uint8_t frame[QY_WIRE_STATE];
    size_t n = qy_build_state_change(frame, 1500, f27, qy_move_code(5, 0), 88, hist, 3, true);

    CHECK(n == 94, "StateChange 明文长度 = 94 (文档 0x5e)");
    CHECK(frame[0] == 0xFE && frame[1] == 94 && frame[2] == 0x03, "帧头/长度/opcode");
    CHECK(frame[3] == 0x00 && frame[4] == 0x00, "时间戳大端");
    CHECK((frame[3] << 24 | frame[4] << 16 | frame[5] << 8 | frame[6]) == 1500, "时间戳值");
    CHECK(memcmp(frame + 7, f27, 27) == 0, "贴纸块位置 = 7");
    CHECK(frame[QY_MOVE_OFF] == qy_move_code(5, 0), "本次转动码位置 = 34");
    CHECK(frame[35] == 88, "电量位置 = 35");
    CHECK(frame[QY_ACK_OFF] == 1, "ack 位置 = 91");

    // 历史: 最新一步在偏移 86，越旧越往 36 排，空槽 0xFF
    const uint8_t *slot10 = frame + 86;
    CHECK((slot10[0] << 24 | slot10[1] << 16 | slot10[2] << 8 | slot10[3]) == 1000 &&
          slot10[4] == hist[0].move, "最新历史在偏移 86");
    const uint8_t *slot9 = frame + 81;
    CHECK((slot9[0] << 24 | slot9[1] << 16 | slot9[2] << 8 | slot9[3]) == 900 &&
          slot9[4] == hist[1].move, "次新历史在偏移 81");
    const uint8_t *slot8 = frame + 76;
    CHECK((slot8[0] << 24 | slot8[1] << 16 | slot8[2] << 8 | slot8[3]) == 800 &&
          slot8[4] == hist[2].move, "第三新历史在偏移 76");
    bool ff_ok = true;
    for (int i = 0; i < 7 * 5; i++) {
        if (frame[36 + i] != 0xFF) ff_ok = false;
    }
    CHECK(ff_ok, "未使用的历史槽 = 0xFF");

    // CRC: 整帧(含 CRC)的余数必须为 0
    CHECK(qy_crc16_modbus(frame, 94) == 0, "整帧 CRC 余数为 0");
}

// ---------------------------------------------------------------------------
// 5. 群论恒等式（转动表 / 复合顺序）
// ---------------------------------------------------------------------------
static void test_group(void) {
    cube_t c, id;
    cube_identity(&id);

    cube_identity(&c);
    for (int i = 0; i < 6; i++) {
        cube_turn(&c, 1, 0); cube_turn(&c, 0, 0);
        cube_turn(&c, 1, 1); cube_turn(&c, 0, 1);
    }
    CHECK(cube_equal(&c, &id), "(R U R' U')^6 = 恒等");

    // T-perm = R U R' U' R' F R2 U' R' U' R U R' F'
    cube_identity(&c);
    for (int rep = 0; rep < 2; rep++) {
        cube_turn(&c, 1, 0); cube_turn(&c, 0, 0); cube_turn(&c, 1, 1); cube_turn(&c, 0, 1);
        cube_turn(&c, 1, 1); cube_turn(&c, 2, 0); cube_turn_half(&c, 1);
        cube_turn(&c, 0, 1); cube_turn(&c, 1, 1); cube_turn(&c, 0, 1);
        cube_turn(&c, 1, 0); cube_turn(&c, 0, 0); cube_turn(&c, 1, 1); cube_turn(&c, 2, 1);
    }
    CHECK(cube_equal(&c, &id), "T-perm^2 = 恒等");

    // 单步四次 = 恒等
    bool ok = true;
    for (int f = 0; f < 6; f++) {
        cube_identity(&c);
        for (int i = 0; i < 4; i++) cube_turn(&c, f, 0);
        if (!cube_equal(&c, &id)) ok = false;
    }
    CHECK(ok, "每面转四次 = 恒等");
}

// ---------------------------------------------------------------------------
// 6. 贴纸解析往返
// ---------------------------------------------------------------------------
static void test_facelet_roundtrip(void) {
    cube_t c;
    cube_identity(&c);
    const int seq[][2] = {{1, 0}, {0, 0}, {1, 1}, {0, 1}, {2, 0}, {4, 1}, {5, 1}, {3, 0}, {2, 1}, {1, 0}};
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        cube_turn(&c, seq[i][0], seq[i][1]);
    }
    char s[55];
    cube_facelet_string(&c, s);
    cube_t back;
    CHECK(cube_from_facelet_string(&back, s) && cube_equal(&back, &c), "贴纸串往返一致");
    CHECK(!cube_is_solved(&c), "打乱后不是复原态");
}

// ---------------------------------------------------------------------------
// 7. 魔域转动帧解析
// ---------------------------------------------------------------------------
static void bw(uint8_t *buf, size_t *bitpos, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; i++) {
        size_t b = *bitpos >> 3;
        unsigned bit = 7u - (unsigned)(*bitpos & 7u);
        uint32_t val = (v >> (n - 1 - i)) & 1u;
        if (val) buf[b] |= (uint8_t)(1u << bit);
        else buf[b] &= (uint8_t)~(1u << bit);
        (*bitpos)++;
    }
}

static void test_moyu_move_parse(void) {
    // 一帧 0xA5 的骨架: opcode + 5×(16bit 间隔) + 步数 + 5×(5bit 转动码)
    uint8_t pkt[MOYU_PKT_LEN];
    moyu_msg_t msg;

    // 1) 12 个转动码逐个穷举: code>>1 取 "FBUDLR", code&1 = 逆时针
    //    0=F 1=F' 2=B 3=B' 4=U 5=U' 6=D 7=D' 8=L 9=L' 10=R 11=R'
    static const int WANT_FACE[12] = {2, 2, 5, 5, 0, 0, 3, 3, 4, 4, 1, 1};
    static const int WANT_DIR[12] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    static const char *WANT_NAME[12] = {"F", "F'", "B", "B'", "U", "U'",
                                        "D", "D'", "L", "L'", "R", "R'"};
    bool ok = true;
    for (int code = 0; code < 12; code++) {
        memset(pkt, 0, sizeof(pkt));
        size_t pos = 0;
        bw(pkt, &pos, 8, MOYU_OP_MOVE);
        for (int i = 0; i < 5; i++) bw(pkt, &pos, 16, 100);
        bw(pkt, &pos, 8, 1);
        bw(pkt, &pos, 5, (uint32_t)code);
        for (int i = 0; i < 4; i++) bw(pkt, &pos, 5, 0);
        if (!moyu_parse(&msg, pkt) || msg.moves[0].face != WANT_FACE[code] ||
            msg.moves[0].dir != WANT_DIR[code] || msg.moves[0].slice) {
            ESP_LOGE(TAG, "    码 %d 期望 %s (face=%d dir=%d), 实得 face=%d dir=%d",
                     code, WANT_NAME[code], WANT_FACE[code], WANT_DIR[code],
                     msg.moves[0].face, msg.moves[0].dir);
            ok = false;
        }
    }
    CHECK(ok, "12 个魔域转动码映射正确");

    // 2) 一帧多步 + 间隔 + 步数计数
    memset(pkt, 0, sizeof(pkt));
    size_t pos = 0;
    bw(pkt, &pos, 8, MOYU_OP_MOVE);
    const uint16_t dur[5] = {120, 130, 140, 150, 160};
    for (int i = 0; i < 5; i++) bw(pkt, &pos, 16, dur[i]);
    bw(pkt, &pos, 8, 42);                        // 步数计数
    const uint32_t codes[5] = {4, 11, 0, 6, 2};  // U, R', F, D, B
    for (int i = 0; i < 5; i++) bw(pkt, &pos, 5, codes[i]);

    CHECK(moyu_parse(&msg, pkt) && msg.kind == MOYU_MSG_MOVE, "0xA5 解析成功");
    CHECK(msg.move_step == 42, "步数计数 = 42");
    CHECK(msg.moves[0].face == 0 && msg.moves[0].dir == 0, "moves[0] = U");
    CHECK(msg.moves[1].face == 1 && msg.moves[1].dir == 1, "moves[1] = R'");
    CHECK(msg.moves[4].face == 5 && msg.moves[4].dir == 0, "moves[4] = B");
    CHECK(msg.moves[0].dt_ms == 120, "间隔 120ms");

    // 3) 中层转必须被识别为 slice
    memset(pkt, 0, sizeof(pkt));
    pos = 0;
    bw(pkt, &pos, 8, MOYU_OP_MOVE);
    for (int i = 0; i < 5; i++) bw(pkt, &pos, 16, 100);
    bw(pkt, &pos, 8, 7);
    bw(pkt, &pos, 5, 12);   // M
    for (int i = 0; i < 4; i++) bw(pkt, &pos, 5, 0);
    CHECK(moyu_parse(&msg, pkt) && msg.moves[0].slice, "M(12) 被识别为中层转");
}

// ---------------------------------------------------------------------------
// 8. 端到端: 动作流 -> 帧序列
// ---------------------------------------------------------------------------
static uint8_t s_cap[16][QY_WIRE_STATE];
static size_t s_cap_len[16];
static int s_cap_n;

static void test_sink(const uint8_t *plain, size_t len) {
    if (s_cap_n < 16) {
        memcpy(s_cap[s_cap_n], plain, len);
        s_cap_len[s_cap_n] = len;
        s_cap_n++;
    }
}
static void test_cmd(const uint8_t plain[MOYU_PKT_LEN]) { (void)plain; }
static uint32_t s_fake_ms;
static uint32_t fake_clock(void) { return s_fake_ms; }

static void feed_status(uint8_t step, const char *facelet) {
    moyu_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = MOYU_MSG_STATUS;
    m.step = step;
    if (facelet) snprintf(m.facelet, sizeof(m.facelet), "%s", facelet);
    bridge_on_moyu(&m);
}

static void feed_moves(uint8_t step, const uint8_t *faces, const uint8_t *dirs, int n) {
    moyu_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = MOYU_MSG_MOVE;
    m.move_step = step;
    m.move_count = (uint8_t)n;
    // faces[0] 是这一批里最早的一步；帧内 moves[0] 才是最新，所以要倒着填
    for (int i = 0; i < n; i++) {
        m.moves[n - 1 - i].face = faces[i];
        m.moves[n - 1 - i].dir = dirs[i];
        m.moves[n - 1 - i].raw_code = 0;
        m.moves[n - 1 - i].dt_ms = 200;
        m.moves[n - 1 - i].slice = false;
    }
    bridge_on_moyu(&m);
}

static void test_end_to_end(void) {
    static const char SOLVED[] =
        "UUUUUUUUURRRRRRRRRFFFFFFFFFDDDDDDDDDLLLLLLLLLBBBBBBBBB";

    s_fake_ms = 1000;
    bridge_init(test_sink, test_cmd);
    bridge_set_clock(fake_clock);
    s_cap_n = 0;

    feed_status(0, SOLVED);
    CHECK(bridge_model_valid(), "拿到快照后状态有效");
    // 首次快照会额外补一帧 CurrentState 给 App（让 App 从 Hello 的旧值纠正过来）
    CHECK(s_cap_n == 1 && s_cap_len[0] == 38 && s_cap[0][2] == QY_OP_CURRENT,
          "首次快照后补发 CurrentState");
    s_cap_n = 0;

    // 打乱: R U F'
    uint8_t f1[3] = {1, 0, 2};
    uint8_t d1[3] = {0, 0, 1};
    feed_moves(3, f1, d1, 3);
    CHECK(s_cap_n == 3, "3 步 -> 3 帧 (不合并/不丢弃)");
    CHECK(s_cap_len[0] == 94 && s_cap[0][QY_MOVE_OFF] == qy_move_code(1, 0), "第 1 帧 = R");
    CHECK(s_cap[2][QY_ACK_OFF] == 0, "打乱态 ack = 0");

    // 时间戳必须严格递增
    bool mono = true;
    for (int i = 1; i < s_cap_n; i++) {
        uint32_t a = (uint32_t)s_cap[i - 1][3] << 24 | s_cap[i - 1][4] << 16 |
                     s_cap[i - 1][5] << 8 | s_cap[i - 1][6];
        uint32_t b = (uint32_t)s_cap[i][3] << 24 | s_cap[i][4] << 16 |
                     s_cap[i][5] << 8 | s_cap[i][6];
        if (b <= a) mono = false;
    }
    CHECK(mono, "时间戳严格递增");

    // 还原: 打乱 (R U F') 的逆 = F U' R'
    uint8_t f2[3] = {2, 0, 1};
    uint8_t d2[3] = {0, 1, 1};
    feed_moves(6, f2, d2, 3);
    CHECK(s_cap_n == 6, "还原 3 步 -> 再 3 帧");
    CHECK(s_cap[5][QY_ACK_OFF] == 1, "最后一步复原 -> ack = 1");

    // 复原态的 27 字节必须回到标准复原块
    static const uint8_t SOLVED27[27] = {
        0x33, 0x33, 0x33, 0x33, 0x13, 0x11, 0x11, 0x11, 0x11,
        0x44, 0x44, 0x44, 0x44, 0x24, 0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x00, 0x50, 0x55, 0x55, 0x55, 0x55,
    };
    CHECK(memcmp(s_cap[5] + 7, SOLVED27, 27) == 0, "复原后贴纸回到标准复原块");

    // 历史区: 默认模式(0) 必须全 0xFF。
    // 原因见 docs/02-DNF根因分析.md 第 8 节: 这个 App 会把填进去的历史当动作重放,
    // 空历史才正常。这里断言"默认不发历史"。
    bool ff_ok = true;
    for (int i = 0; i < QY_HISTORY_SLOTS * 5; i++) {
        if (s_cap[5][QY_HISTORY_FIRST_OFF + i] != 0xFF) ff_ok = false;
    }
    CHECK(ff_ok, "默认历史区全 0xFF (App 不会重放动作)");

    // 再单独验一次编解码器的"历史写入"能力(模式 1), 保证那部分代码没坏
    bridge_set_hist_mode(1);
    uint8_t f7[1] = {1};      // R
    uint8_t d7[1] = {1};      // R'
    s_cap_n = 0;
    feed_moves(7, f7, d7, 1);
    CHECK(s_cap_n == 1, "模式1: 再转一步产生 1 帧");
    const uint8_t *slot10 = s_cap[0] + 86;
    CHECK(slot10[4] == qy_move_code(1, 1), "模式1: 最新历史槽 = 上一步 R'");
    CHECK(slot10[0] || slot10[1] || slot10[2] || slot10[3], "模式1: 历史槽带时间戳");
    bridge_set_hist_mode(0);

    // 一步丢失(计数跳变超过本帧能回补的 5 步)必须被判定为丢步而不是硬转
    s_fake_ms = 5000;
    bridge_init(test_sink, test_cmd);
    bridge_set_clock(fake_clock);
    s_cap_n = 0;
    feed_status(10, SOLVED);
    s_cap_n = 0;
    moyu_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = MOYU_MSG_MOVE;
    m.move_step = 20;      // 从 10 跳到 20: 中间丢了 10 步, 本帧只带 5 步
    m.move_count = 5;
    bridge_on_moyu(&m);
    CHECK(s_cap_n == 0 && !bridge_model_valid(), "计数跳变 -> 停机+告警, 不产生错误帧");
}

// ---------------------------------------------------------------------------
int selftest_run(bool verbose) {
    s_verbose = verbose;
    s_fail = 0;
    ESP_LOGI(TAG, "===== 自检开始 =====");
    test_crc_and_aes();
    test_facelet_pack();
    test_move_codes();
    test_state_frame_layout();
    test_group();
    test_facelet_roundtrip();
    test_moyu_move_parse();
    test_end_to_end();
    if (s_fail == 0) ESP_LOGI(TAG, "===== 自检全部通过 =====");
    else ESP_LOGE(TAG, "===== 自检失败 %d 项 =====", s_fail);
    return s_fail;
}
