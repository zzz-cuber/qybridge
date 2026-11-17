# 第三方来源与致谢

本项目（qybridge）的协议实现、魔方转动表和 LED 驱动思路，参考了以下公开项目。
**本仓库不包含上述项目的代码副本**，只在必要处沿用了公开的协议常量、位布局
与数学表；凡属代码级借鉴之处，均在下文注明。

---

## 1. cubebridge —— **GPL-3.0**（本项目继承其许可证）

* 用途：本项目的直接前身；`main/cube_model.c` 的转动表（`BASE_CA` / `BASE_EA`）、
  面贴映射表（`C_FACELET` / `E_FACELET`）、`main/move_led.c` 的 WS2812 RMT 参数
  与新旧两层实现思路，均来自该项目。
* 许可证：GNU General Public License v3.0
* 因此 **qybridge 整体以 GPL-3.0 发布**，见根目录 `LICENSE`。

## 2. GAN-main（Alex Beng）—— **MIT**

* 用途：多协议网关（MoYu / QiYi / GAN）的参考实现。本项目用它交叉验证了
  奇艺的转动码面序（L R D U F B）与方向位约定、魔域 v5 的帧结构。
* 未复制其 Rust 代码；仅比对协议细节。
* https://github.com/ 内搜索仓库名 `GAN-main`（作者仓库随发行版分叉，此处不固定链接）

## 3. btcube-web（Simon Kelly 等）—— **MIT**

* 用途：魔域（MoYu）v5 协议位布局的主要参考：状态帧 6 面 × 8 × 3bit、
  转动帧 5 × 16bit 间隔 + 步数计数 + 5 × 5bit 转动码。
* 未复制其 TypeScript 代码。

## 4. qiyi_smartcube_protocol（Simon Schwartz）—— **MIT**

* 用途：奇艺 QYSC 协议文档与 Wireshark 解析器。本项目的帧格式、
  27 字节状态块（色码 `L=0 R=1 D=2 U=3 F=4 B=5`）、历史区布局、
  ack 语义均以该文档的真实抓包为准。
* 未复制其代码。

## 5. csTimer

* 用途：`cube_model.c` 中角块/棱块置换表的数学形式与其 `mathlib` 一致
  （这是公开的魔方群论表示法，不构成代码复制）。

## 6. 协议常量说明

以下内容属于**公开的逆向工程事实**，散见于多个开源项目，不属于任何单一作者的
独创表达：

* 奇艺 AES-128-ECB 固定密钥、服务/特征 UUID
* 魔域 WCU 的服务/特征 UUID、根密钥与 IV 的 MAC 派生方案（`%255`）
* 各厂商的贴纸色码、转动码编号

如果你认为本项目侵犯了你的权利，请开 Issue，我们会立即处理。
