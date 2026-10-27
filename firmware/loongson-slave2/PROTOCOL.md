# 龙芯系统指标协议 v1

当前仍为项目AA55自定义协议，不能称为Modbus RTU。RS485半双工、115200/8N1；默认地址2，星火地址1。仅收到发给本地址的合法请求才应答；广播、其他地址和ACK报文不应答。

外层：`AA 55 | address:u8 | command:u8 | length:u8 | payload | CRC16低 高 | 0D 0A`。CRC初值FFFF、多项式A001，范围address至payload，最大载荷64、帧长73字节。应答command=request_command|80。没有原样复制C结构体，所有多字节字段显式小端。

| 请求command | 请求载荷 | 应答载荷 | 用途 |
|---|---|---|---|
| 01 | 长度0 | 原有24字节布局，uptime为系统开机运行时间，22=0、23=A1 | 兼容现有主站查询；环境/IMU无效，零值不是传感测量 |
| 03 | 0..64字节 | 原样返回 | 二进制链路回显 |
| 04 | 长度0 | 以下64字节系统指标 | 正式系统采集数据 |
| 05 | 长度0 | 32字节身份 | 识别节点与版本 |

未知命令、非法请求长度不应答，计入rejected。命令02的LED控制未实现，不能拿应答当实际GPIO或LED动作。

## 04系统指标应答（command=84，length=64）

| 字节偏移 | 类型 | 名称、单位与含义 |
|---|---|---|
| 0 | u8 | schema_version=1 |
| 1 | u8 | node_type=2：Linux系统指标节点 |
| 2 | u8 | valid：CPU=bit0、内存=bit1、负载=bit2、系统运行时间=bit3 |
| 3 | u8 | station_address，默认2 |
| 4 | u32 | sequence，采集次数；进程重新启动后重新计数 |
| 8 | u32 | uptime_s，CLOCK_BOOTTIME，包含系统挂起，单位秒 |
| 12 | u32 | sample_age_ms，样本年龄；无样本时FFFFFFFF |
| 16 | u16 | cpu_centi_pct：整个CPU忙碌百分比×100，0..10000；不是该进程占用率 |
| 18 | u16 | load1_milli：1分钟系统负载×1000，最大65535；负载不是百分比 |
| 20 | u32 | mem_total_kib，/proc/meminfo MemTotal，单位KiB |
| 24 | u32 | mem_available_kib，MemAvailable，单位KiB |
| 28 | u32 | process_uptime_s，当前从站进程运行秒数，不含系统挂起 |
| 32 | u32 | requests，发给本地址的非ACK合法外层报文次数，包括随后被拒绝的命令 |
| 36 | u32 | responses，已发送完成次数，不包括当前应答 |
| 40 | u32 | crc_errors |
| 44 | u32 | frame_errors，非法长度或结束标记 |
| 48 | u32 | tx_errors，写入或等待发送完成等失败 |
| 52 | u32 | rx_bytes，收到的全部串口字节，包含其他节点通信 |
| 56 | u32 | tx_bytes，已成功发送完整帧的字节数，不包括当前应答 |
| 60 | u32 | rejected，发给本地址但命令/载荷不支持 |

采集默认1秒一次，独立线程读proc，短临界区复制快照。CPU按相邻两次/proc/stat的差值，忙碌部分排除idle和iowait；guest时间不重复相加。首个样本无CPU差值，CPU有效位为0。字段读取失败时清除对应有效位。样本超过采样周期3倍时valid全部清零，不能把旧数字继续当有效新数据。

计数和sequence均为u32，进程重启后清零，长期运行按u32回绕。CPU字段不能用于证明真实主频。没有thermal接口时不伪造芯片温度。

## 05身份应答（command=85，length=32）

0=schema1，1=node_type2，2=station_address，3=capabilities(01系统指标、02回显、04兼容查询)，4/5/6=程序版本major/minor/patch（1/0/0），7保留0，8..31为NUL填充的ASCII `LS2K0300-LoongOS`。

主站当前`mbq query 2`仅解码01兼容应答，不能直接显示04的CPU/内存等指标。正式双节点数据模型、记录及标准Modbus映射属于主站后续接入；诊断应使用04/05并检查version、node_type、有效位、sample_age和字段范围。
