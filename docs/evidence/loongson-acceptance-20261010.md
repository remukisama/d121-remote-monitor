> 2026-10-10 历史记录：当时使用 AA55，当前三端已迁移 Modbus RTU。

# 正式龙芯系统指标从站：实现与验收

日期：2026-10-10。源码组件：`D:/xuantie/loongson_slave2/`。实际板卡：久久派WiFi版LS2K0300、LoongOS v0.1、Linux4.19.190+。用户确认本轮只采系统运行指标。

## 实现

地址2，UART1=/dev/ttyS1、115200/8N1，MAX3485模块EN由TIM2_CH2/GPIO88控制。每秒独立采集系统开机时间、CPU忙碌比例、内存与1分钟负载；为主站查询生成一致快照，提供有效位、序号、样本年龄、进程运行时间和通信统计。

兼容01查询只提供真实系统运行时间，环境/IMU有效位为0。专用04返回64字节系统指标，05返回身份，03支持最多64字节二进制回显。协议仍是AA55自定义协议，非Modbus RTU。完整字段表见`loongson_slave2/PROTOCOL.md`。

正式程序已安装到`/usr/local/sbin/loongson-slave2`，服务`loongson-slave2.service`已启用且实际运行。状态文件位于`/run/loongson-slave2/status.json`；正常退出恢复UART设置并置EN低，异常退出后ExecStopPost再次置低，systemd失败重启与进展看门狗已实测。

## 源码、构建与主机验证

使用WSL现有LoongArch GCC8.3工具链，`-Wall -Wextra -Werror -Wformat=2 -Wshadow`构建，未新增板端依赖。没有修改用户WSL竞赛工程或内核。

- ASan/UBSan：CRC标准向量、最大载荷、分片、连续帧、AA重叠、坏CRC/尾标记/长度恢复、残帧失效、10万字节随机噪声与内存边界。
- 采集解析：CPU差值与首次/回绕/无进展样本、guest不重复计算、内存缺字段/超量/零总量、负载非有限值/负数/饱和。
- 实际程序PTY测试：04系统指标、05身份、01兼容、03二进制回显；广播/其他地址/ACK/非法请求不应答；坏帧及碎片恢复；原子JSON、systemd抽象socket READY/看门狗通知、SIGTERM正常退出和CLI边界。

生产程序不编入测试串口选项；主机PTY模拟不会访问GPIO，也不会作为板上RS485测试成绩。

## 实机验证

| 检查 | 结果 |
|---|---|
| 程序/服务哈希 | 主机与板端一致 |
| 实际采集 | CPU、内存、负载、运行时间，valid=15，采样序号递增 |
| CPU受控负载 | 初始cpu_centi_pct=100（1%），3秒受控忙循环期间达到10000（100%），结束后0；进程一直active |
| 正式程序双节点通信 | D121交替查询星火地址1与龙芯地址2各100次全部成功；星火持续采集 |
| 地址1响应时间 | 最小8ms、最大131ms、平均11.81ms |
| 地址2响应时间 | 最小4ms、最大55ms、平均7.35ms |
| 通信与记录 | 100对查询结束时CRC/发送/打开错误均0；星火TF written=frames、dropped=0、write_errors=0；GUI健康 |
| 看门狗 | 确认EN低且没有地址2事务时SIGSTOP进程；systemd报告10秒Watchdog timeout，PID2652→2665、NRestarts=1；主站恢复查询2成功 |
| 真实开机启动 | 软件重启崩溃后用户两次重新上电；无需手动启动服务，地址2分别在系统运行52秒、19秒时应答；COM7核对服务enabled/active、PID1547、有效指标，验证了冷启动自动运行 |
| 正常服务停止 | 更新服务配置时，旧进程日志STOP exit=0，requests=responses=101，tx_errors=0；退出后重新启动正常 |

04/05的完整数据报文已在实际程序PTY测试中验证，真实龙芯采集值由COM7状态文件和snapshot核对；本轮D121只通过现有01查询验证物理RS485应答。未将“主站收到了完整04系统指标”列为已实测能力，也未更改主站固件或UI。

最初三板联调前的timeout=1、busy=0；主动停止临时地址2测试后timeout=2、busy=1。正式程序100对查询并未新增这两个计数。随后第二次系统重启失败期间手动查询2如预期无应答，timeout变3。不能把这些有意故障注入或历史计数说成0。

## LoongOS软件重启问题

第一次普通软件重启在`es8323_i2c_shutdown → snd_soc_component_write`处空指针崩溃，系统未能重启。WSL同版ES8323源码关机回调无条件使用全局组件指针，实机没有注册声卡，日志与源码支持未初始化组件指针的判断。

临时解绑音频后再次测试，音频关机回调已绕过，但随后在WiFi模块`aicbsp_sdio_remove`处崩溃。这说明仅绕过音频回调不能提供可靠软件重启。已停止该实验，重启脚本已替换成禁用提示并退出2，板上实际执行确认不会再触发重启或解绑。

没有刷内核、持久禁用音频/WiFi或使用强制重启。用户物理重新上电后原驱动正常加载、从站自动启动。现有内核普通软件重启仍未修复；驱动修复需单独核对运行内核、厂商源码、模块ABI并做平台验收。当前CPU/内存采集与地址2应答的完成，不代表这些系统驱动缺陷已解决。

## 交付资产与恢复

组件`README.md`包含WSL来源、构建、维护与当前限制；`Makefile`提供`make test`、`make package`。产物`build/loongson-slave2.tar.gz`包含程序、服务、安装脚本、禁用的旧重启入口和SHA256SUMS。安装脚本在板端更新前备份旧文件，启动失败尽可能还原，源码语法检查已通过。

实际已部署的三个文件SHA256：

| 文件 | SHA256 |
|---|---|
| loongson-slave2 | ee0ea031e92483ea72b5db9b13619613d8bc1232dab14cd58148cc6854fbdeec |
| loongson-slave2.service | a983a156b19d5e23e430e61f9d1f2b76e76b9a6c14c4b2cf3c5006315b7b1139 |
| 禁用的loongson-slave2-reboot | 18fc96b5729b46dacb0d097dbff98156e70c954b30d91771963483f20696ad16 |

整理末期旧CH340接口接触不良，随后虽然能枚举COM7，但.NET与Windows原生WriteFile都返回错误87；驱动重启需要管理员权限，当前进程未能执行。用户换用另一个TTL转串口模块后，COM7写入与登录恢复。没有更改或重新安装Windows驱动。

最终完整包已重新传输，包哈希与四个文件哈希均验证通过；实际运行`sh install.sh`成功，返回PACKAGE_INSTALL_EXIT=0。上版备份位于板端`/var/lib/loongson-slave2/backup-fX4rFy`。最终包SHA256：`c678ab98874245b1bae6bc398233b492c4e32a372fd9c957ebb65566d0f1c3d7`。

换转接模块后的额外10对交替查询全部成功：星火最小9ms/最大10ms/平均9.5ms，龙芯最小4ms/最大5ms/平均4.9ms。最终服务enabled/active、PID1964、NRestarts=0；有效位15、样本序号71，requests=responses=10、CRC/格式/发送错误均0，RSS约1600KiB。程序已保持运行，所有电脑端串口释放。

末期主站检查发现黑匣子已处于停止状态，session185、frames=written=16522、events22，显示的是停采前的离线/质量状态；手动查询地址1/2已验证当前通信正常。本轮保留主站停止采集状态，没有擅自改变GUI控制状态。其累计timeout=8657为当时读到的历史累计值，不能用前面短测数据将它覆盖或说成0；当前仅主站手动查询与龙芯独立采样运行，不宣称主站黑匣子正在持续记录两个节点。

## 原始日志

- `com7-formal-transfer.txt`、`com7-formal-install.txt`、`com7-formal-unit-final.txt`：构建包校验、程序采样、服务启动与最终单位配置。
- `d121-formal-first-query.txt`、`d121-formal-dual-100-pairs.txt`：正式程序物理链路验证。
- `com7-formal-watchdog.txt`、`d121-formal-watchdog-recovered.txt`：看门狗故障与恢复。
- `com7-formal-cpu-test.txt`：真实CPU指标变化。
- `com7-formal-reboot.txt`：ES8323崩溃；`com7-formal-safe-reboot.txt`：绕过后AIC8800崩溃。第二个文件名沿用试验名，不代表重启成功。
- `d121-after-user-reset.txt`、`com7-formal-after-user-reset.txt`、`d121-after-second-user-reset.txt`：两次物理恢复与自启动。
- `com7-formal-reboot-helper-retired.txt`：禁用失败实验的确认。
- `com7-formal-final-package.txt`、`com7-formal-package-installed.txt`：最终完整包与安装脚本验证、旧版本备份。
- `d121-formal-final-10-pairs.txt`、`d121-formal-final-state.txt`、`com7-formal-final-state.txt`：换转接模块后的最终通信与服务状态。

后续工作：主站读取04/05、每节点独立调度/状态/记录，完成双节点正式验收，再迁移标准Modbus RTU。采集用户明确要求的系统指标已实现；没有额外承诺未连接的传感器或实体控制。
