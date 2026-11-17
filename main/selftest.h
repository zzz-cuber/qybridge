// qybridge —— 魔域智能魔方 -> 奇艺 Smart Player Pro 协议网关
// 文件: selftest.h —— 上电自检（不需要手机和魔方就能跑）
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 返回 0 表示全部通过；否则返回失败项数
int selftest_run(bool verbose);

#ifdef __cplusplus
}
#endif
