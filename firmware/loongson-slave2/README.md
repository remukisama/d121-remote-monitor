# 龙芯 LS2K0300 系统指标从站

该从站运行于久久派 LoongOS / Linux，默认 Modbus RTU 地址 2、115200 / 8N2。独立采样线程采集 CPU、内存、负载和运行时间，主站查询时读取缓存快照；有效位和样本年龄区分通信在线与指标新鲜。

UART1 与 MAX3485 模块相连，GPIO88/TIM2_CH2 控制发送使能；实际接线应以板卡手册和已验证现场接线为准。详细寄存器和单位见[项目寄存器表](../../docs/Modbus寄存器表V1.md)。保留的 `--legacy` 和 `PROTOCOL.md` 是历史 AA55 协议，当前主站轮询使用 Modbus RTU。

在 Linux / WSL 主机：

```sh
cd firmware/loongson-slave2
make test
make package TOOLCHAIN=/path/to/loongarch64-linux-gnu-
```

`make test` 使用主机 GCC、ASan/UBSan 与 PTY；运行路径测试模拟 GPIO，不宣称验证物理电平。`make package` 需要 LoongArch64 交叉 GCC，生成包含程序、systemd 服务、安装脚本与 SHA256SUMS 的压缩包。安装脚本校验文件并备份旧程序后再更新。

此前独立 MinimalModbus 经真实 RS485 总线对每站交替读 20 次，龙芯进程异常恢复和冷启动自启动也有实机记录。普通系统软件重启在该板曾触发 ES8323 / AIC8800 驱动崩溃，尚未修复；不要将其描述为已验证的可靠重启功能。原阶段记录保留在[历史说明](../../docs/龙芯从站历史说明-20261010.md)，其中较早的 AA55 状态不代表当前协议。
