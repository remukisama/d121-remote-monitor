# D121 ESP8266 TLS 固件

来源为 Espressif `esp-at` 的 `release/v2.2.0.0_esp8266` 分支，初始提交 `5249c31d1df78c68622b33ffa9fd82b9991c28be`；SDK 固定为 `ff16a35c362047b47c0b3d29c02ef9755b623447`。这是 D121 设备网络模块的构建依赖，生产后台工程不在此仓库。

## 改动

- 启用 `CONFIG_MBEDTLS_HAVE_TIME_DATE`，通过 SNTP 自动取得当前时间；未取得有效日期时，TLS 证书校验会拒绝尚未生效的证书。
- 保持 26 MHz 晶振、115200 串口，Flash 使用 40 MHz；UART0 最终使用 TX=GPIO15/D8、RX=GPIO13/D7，禁用 CTS/RTS 和额外收发控制引脚。USB 的 ROM 烧录引脚仍为 GPIO1/GPIO3。
- `client_ca` 索引 0 是 Bemfa 所需的 Certum 根；索引 1 和 2 只用于错误 CA 与过期证书的对照。D121 主站正常连接始终指定 CA 索引 0。
- 修复中文目录下 CMake 按非 UTF-8 读取参数文件的问题，并保存 SDK 链接脚本工具的 Unicode 路径补丁。修改只影响构建工具，不替代 SDK 的证书验证。

## 构建

源码保留在 `D:\codex相关项目\ESP8266-AT-TLS`，通过 WSL 构建。SDK 未作为大目录提交；在 `esp-idf` 中检出上述提交并初始化子模块后，应用 `patches/sdk-ff16a35-unicode-ldgen.patch`。

依赖工具链来自 SDK `tools/tools.json` 指定的 `xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz`，SDK 安装器会校验 SHA256。工具和 Python 虚拟环境位于被忽略的 `.codex-build/`，未修改 Windows 全局 Python 包。

本轮 Python 3.10 环境使用 setuptools 65.5.1、click 7.1.2、pyserial 3.5、future 0.18.3、cryptography 3.4.8、pyparsing 2.3.1、pyelftools 0.29、PyYAML 6.0.2、xlrd 2.0.1、packaging 23.2、bitstring 3.1.9。先执行虚拟环境 Python 的 `tools/d121_host_compat.py`，再运行 `bash tools/d121_build.sh`。首次配置自动执行，必要时设 `D121_RECONFIGURE=1`；Ninja 并发限制为 8。

`build/module_info.json` 由脚本生成，防止交互配置与构建参数不一致。输出在 `build/`，生成的配置必须确认包含日期校验、26 MHz 晶振和预期引脚；不能只以构建返回成功判定可烧录。

## 实机验证及恢复

2026-10-25 已在独立 USB 模式下对比：标准固件接受的同一过期证书，被启用日期校验并自动校时后的固件拒绝；正确 CA/域名连接、错误域名、错误 CA 和恢复正确配置也通过。测试过程中曾临时使用 GPIO1/GPIO3 作为命令口，最终部署改用 GPIO15/GPIO13；应用程序与 TLS 配置相同，差别仅为独立工厂参数分区。

跨工程证据目录为 `D:\xuantie\work\network-hardening-20261022\`。原 AT 1.7.4 的完整 4 MiB 备份在 `D:\xuantie\work\backups\network-hardening-20261022\nodemcu-at174-verified.bin`：全部分段经过两次读取比较并通过设备整片 MD5；SHA256 为 `5f139363d02183623a1488755c546d285bf59a683469269a78ed13b515cfb7f8`。该文件含原模块配置，不提交仓库。

最终接线：D121 PA2/TX → D7/GPIO13/RX，PA3/RX ← D8/GPIO15/TX，GND 共地；NodeMCU 独立 USB 供电，两板 3.3V 不相连。最终模式中 COM10 可用于 ROM 烧录，日常 AT 通信由 D121 执行。

主站加密 MQTT、云端保存确认和短时故障回归以跨工程的最终实机记录为准；独立 TLS 握手不等于上述链路均已通过。长期观测按用户要求暂停。

2026-10-25 最终接回主站后已实测 `tls=1 auth_verified=1`、原有 10 条积压清空；打开 USB 串口及两次重连、星火停止/恢复期间产生的 2 条事件也确认清空。真实微信云端 15 项通过。随后找不到 AP 的原因由用户确认热点曾关闭或手机离开过；恢复后自动重连和云端 15 项复验通过。不能由上述短时通过推断 Wi-Fi 长期稳定。详见跨工程 `README.md`、`short-fault-result.json` 和 `hotspot-recovery.json`。
