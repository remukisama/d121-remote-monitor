# Modbus RTU 迁移记录（2026-10-20）

当前状态：三端已部署；独立 MinimalModbus 经真实 RS485 总线的互通、双站 TF 采集、受控单站中断恢复、真实界面回放/启停检查通过。ESP8266 重新插拔 USB、恢复三线后 AT 和联网恢复，17 条积压事件清空；独立 HTTPS 读到本轮双站数据。采集和云任务均运行。本轮完成 Modbus 迁移验收与连接恢复，不代表 TLS、生产后台更新等完整交付已经完成。

## 范围

- 总线从 AA55 切换到 115200/8N2 的 Modbus RTU，星火地址 1，龙芯地址 2。
- 04 读取缓存测量；星火另支持 03/06 读写 LED，广播写入不应答；标准异常、CRC、数量和映射边界检查。
- 主站转换到既有 UI、TF、MQTT 数据结构。新诊断命令 `mbq raw HEX` 转发实际请求并打印实际应答，供独立工具校验。
- 龙芯默认 RTU，旧协议测试显式使用 `--legacy`。星火 IMU 快照包含采样序号、年龄和有效性，查询路径不重新探测传感器。

映射及计量单位见[寄存器表](../../docs/Modbus寄存器表V1.md)。公共实现为 `protocol/modbus_rtu.h`，通过 `sync_protocol.py` 同步到三个工程。

## 已完成验证

| 层级 | 结果与证据 |
|---|---|
| 公共协议 | 已知外部 CRC 向量、最大 125 寄存器、非法数量/地址/长度、广播/外站/坏 CRC 静默、100000 次模糊输入，ASan/UBSan；`rtu-tests.txt` |
| 主站生产源码 | 实际 8 字节请求、寄存器解码、UART 保留板级功能且设置 8N2、超时预算/噪声/错误/计时回绕；`master-final-tests.txt`、`regression-first.txt` |
| 星火生产处理器 | 04/03/06、LED、异常及广播/CRC 静默；完整原回归 `regression-star-rtu.txt` |
| 龙芯生产程序 | 单元、运行边界及显式 legacy PTY 回归；`loongson-build-first.txt` |
| 独立工具 | 未改动的 MinimalModbus 2.1.1 对实际龙芯主机程序默认 RTU 模式，经 socat PTY 读取 20 轮并检查异常/恢复；`independent-pty.txt`。GPIO 模拟，不代表物理总线 |
| 构建 | D121、STM32F407、LoongArch64 构建及打包通过；`build-master.txt`、`build-star-final.txt`、`package-loongson.txt` |
| 龙芯实机 | 串口传输包及校验和通过，服务 active，版本 1.1.0-modbus，状态 protocol=modbus-rtu、valid=15；`loongson-deployment.txt/json` |
| 星火/主站实机部署 | 星火 ST-Link 读回校验成功并复位；主站 OS/rodata 烧录成功且应用启动确认；`flash-star.txt`、`flash-master.txt`、`flash/` |
| 独立工具跨物理总线 | MinimalModbus 2.1.1 每站 20 次交替读取，星火 LED 03/06 写入并恢复，5 项异常检查通过；实际请求/应答字节在 `independent-hardware.json`，结果 `independent-hardware.txt` |
| 双站与存储 | `capture-start.txt`、`tf-progress.txt`：双站在线且有效，3 次检查写入持续增长，dropped/write_errors/switches=0；`network-recheck.txt`：watchdog active=1、tripped=0、bb_ok=1 |
| 受控中断 | `hardware-isolation.txt`：星火停应答时龙芯仍在线且 TF 正常，恢复后温湿度/IMU 有效。`isolation-after.txt` 保留因此产生的累计 timeout=14，CRC/TX/open 错误为 0 |
| 真实界面 | `hardware-ui.txt`：592 点 TF 回放、跨节点清空窗口与 slide 钩子、3 轮真实 UI 启停回调通过；结束恢复采集，最终 TF 会话 24 |
| 网络与补传 | `esp-restored-wires.txt`：D121 识别原 ESP AT 1.7.4；`recovered-health.txt`：MQTT online=1、events=17、queued=0、app_ack=56、persist_errors=0，双站有效、TF 正常。app_ack 为累计确认条数，不等于 17 个唯一事件 |
| 独立云端读回 | `cloud-readback-proxy.txt`：2026-10-20 17:22:13，boot=20/collection=4 双站在线且新鲜，星火有效、龙芯 valid=15；诊断 write_errors/dropped_frames/bus_crc_errors=0。PC 直连 URLError，经已有本机代理读回成功，未改系统代理 |

龙芯旧程序保存在板上 `/var/lib/loongson-slave2/modbus-backup-T5y8I1/loongson-slave2`。本机迁移前源码/镜像位于 `work/backups/modbus-rtu-20261020/`。

固件冻结于 `images/master-modbus.img` 与 `images/star-modbus.bin`，SHA256 见 `firmware-hashes.txt`。主站镜像检查仅含 OS/rodata，已写入硬件；星火以 NORMAL + halt 连接写入并校验成功。首次因未连接 ST-Link 未开始擦写，接回后重试成功。未刷写 ESP8266 固件。

## 独立工具的验收边界

`independent_hardware.py` 已执行成功：独立 MinimalModbus 通过 D121 原始 UART 桥跨真实 RS485 总线，交替读取每站 20 次、LED 写入与恢复、标准异常。请求构造与应答 CRC 验证使用未修改的第三方协议库，桥只转发实际字节。此桥接不等同独立 USB-RS485 适配器，也不证明字符间隔、电气波形或一致性认证。广播、坏 CRC 静默及模糊输入目前以主机测试为证，未新增相应物理故障注入。

## 网络中断与恢复

USB 重新分配后 COM10 曾出现，但主站 `cloud probe`、电脑 USB AT 和只读 ROM 芯片识别均未收到串口数据。自动复位未捕获启动输出，用户表示手按 RST 后无灯光变化。用户再次插拔 USB 后，`esp-after-usb-replug.txt` 得到 AT→OK；随后确认板间三线接回，主站取得 ESP AT 版本并恢复联网。没有测量供电电压或波形，不能据此断言确切电气根因，也未判定模块损坏或改写其 Flash。

`network-after-rewire.txt` 的 queued=11、app_ack=20 是旧固件停止前状态。新固件测试期间队列增至 17，恢复后清空并取得应用确认和独立云端新数据。Wi-Fi 配置未修改，仍为本日已验证过的用户指定网络。恢复时 attempts=5/reconnects=4、errors=3/timeouts=11 保留此前无应答探测累计值；不能声称全程零网络错误。总线 timeout=14 为受控星火停应答产生，后续未清零。

## 源码与复现

三个仓均在本地分支 `codex/modbus-rtu-20261020`，未推送远端：主站 SDK `27196f8d0`，星火 `4ba33bc`，根工程提交见 Git 日志（含龙芯、公共协议、测试、寄存器表与本记录）。根工程既有未提交文件、SDK 其他历史修改未纳入本轮提交。

复测：主站 `build.ps1`；星火原 Debug Makefile；龙芯 `make test package`；公共核心 `test_rtu.c` 在 WSL 用 ASan/UBSan；独立 PTY 用 `independent_pty.py`。独立工具依赖位于本目录被忽略的 `python-libs/`，MinimalModbus 2.1.1 与 pyserial 3.5，未修改协议库代码。硬件独立测试前必须先 `bb stop`，之后恢复 `bb start 1`；TF/UI 脚本会新增会话，沿用现有文件轮转上限。

后台可靠性修复仍为本机完成、生产未部署，见[上轮记录](../云端使用说明.md)。TLS、数据库恢复演练、移动网络验收和长时间耐久仍需后续工作。
