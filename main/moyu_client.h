// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: moyu_client.h —— 连接实体魔域魔方（BLE 主机）
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "moyu_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在 nimble_port_init() 之后调用: 读配对记录、准备扫描
void moyu_client_init(void);
// host 同步完成后调用
void moyu_client_on_host_sync(void);

// 向实体魔方写一帧（会自动加密）。可从任意任务调用。
bool moyu_client_write(const uint8_t plain[MOYU_PKT_LEN]);

// 控制台用
void moyu_client_rescan(void);
void moyu_client_clear_pairing(void);
bool moyu_client_connected(void);
const char *moyu_client_state_name(void);

#ifdef __cplusplus
}
#endif
