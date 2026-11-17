// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: qy_server.h —— 虚拟奇艺魔方（BLE 外设）
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在 nimble_port_init() 之前调用: 生成 QY-QYSC-S-XXXX 名称和对应 BLE 地址
void qy_server_prepare_identity(void);
// nimble_port_init() 之后调用: 注册 GATT 服务
void qy_server_register(void);
// host 同步完成回调里调用: 设置随机地址并开始广播
void qy_server_on_host_sync(void);

// 发一帧明文奇艺帧（内部补零 + AES + 入队）。可从任意任务调用，不阻塞。
bool qy_server_send(const uint8_t *plain, size_t len);
bool qy_server_subscribed(void);
const char *qy_server_name(void);

#ifdef __cplusplus
}
#endif
