/*
 * Local application data types. Active collection uses Modbus RTU register map V1.
 * AA55 constants/packing remain legacy diagnostic helpers, not the live wire format.
 * 对端源码: D:\RT-ThreadStudio\workspace\RS485\applications\rs485_app.h
 *
 * 帧结构: [0xAA 0x55] [ADDR] [CMD] [LEN] [PAYLOAD...] [CRC_L CRC_H] [0x0D 0x0A]
 * CRC   : CRC16-MODBUS (多项式 0xA001, 初值 0xFFFF)，计算范围 ADDR..PAYLOAD
 * 响应  : 从站把 CMD 或上 0x80 作为 ACK
 *
 * 改动本文件时，星火侧的 rs485_app.h 必须同步修改。
 */

#ifndef __RS485_PROTO_H__
#define __RS485_PROTO_H__

#include <rtthread.h>
#include <stddef.h>

#define RS485_HDR0              0xAA
#define RS485_HDR1              0x55
#define RS485_TAIL0             0x0D
#define RS485_TAIL1             0x0A

#define RS485_ADDR_BROADCAST    0xFF
#define RS485_ADDR_DEFAULT      0x01

#define RS485_CMD_QUERY         0x01
#define RS485_CMD_CTRL_LED      0x02
#define RS485_CMD_ECHO          0x03
#define RS485_CMD_SYSTEM        0x04
#define RS485_SYS_CPU           0x01
#define RS485_SYS_MEMORY        0x02
#define RS485_SYS_LOAD          0x04
#define RS485_SYS_UPTIME        0x08

/* Linux system payload is explicitly decoded; this struct is never sent raw. */
typedef struct {
    rt_uint8_t valid;
    rt_uint16_t cpu_centi, load_milli;
    rt_uint32_t sequence, uptime_s, age_ms, mem_total_kib, mem_available_kib;
    rt_uint32_t process_uptime_s, requests, responses, crc_errors, frame_errors;
    rt_uint32_t tx_errors, rx_bytes, tx_bytes, rejected;
} rs485_system_t;
rt_err_t rs485_query_system(rt_uint8_t addr, rs485_system_t *out,
                            rt_uint32_t timeout_ms, rt_uint32_t *rtt_ms);
#define RS485_CMD_ACK_FLAG      0x80

#define RS485_PAYLOAD_MAX       64
#define RS485_FRAME_MAX         (RS485_PAYLOAD_MAX + 9)

/*
 * 遥测载荷。从站发送的是 sizeof(rs485_telemetry_t)=24 字节（含对齐填充）。
 * 按显式偏移取值而不是整体 memcpy，避免两端编译器差异导致字段错位。
 * v2 (2026-09-15): 与星火侧 rs485_app.h 同步 —— +ICM20608 六轴，temp/humi 为板载 AHT21。
 */
#define RS485_TELEMETRY_LEN     24
#define RS485_OFF_UPTIME        0   /* u32, 秒          */
#define RS485_OFF_TEMP          4   /* i16, 0.01 摄氏度 */
#define RS485_OFF_HUMI          6   /* u16, 0.01 %RH    */
#define RS485_OFF_LED           8   /* u8               */
#define RS485_OFF_AX            10  /* 以下 6x i16, ICM20608 原始 LSB */
#define RS485_OFF_AY            12
#define RS485_OFF_AZ            14
#define RS485_OFF_GX            16
#define RS485_OFF_GY            18
#define RS485_OFF_GZ            20
#define RS485_OFF_QUALITY       22
#define RS485_OFF_QUALITY_TAG   23
/* Add validity using the two existing padding bytes; old 24-byte peers still work. */
#define RS485_QUALITY_TAG       0xA1
#define RS485_Q_ENV_VALID       0x01
#define RS485_Q_IMU_VALID       0x02

typedef struct
{
    rt_uint32_t uptime_s;
    rt_int16_t  temp_centi;
    rt_uint16_t humi_centi;
    rt_uint8_t  led_state;
    rt_int16_t  ax, ay, az;
    rt_int16_t  gx, gy, gz;
    rt_uint8_t  quality, quality_tag;
} rs485_telemetry_t;
_Static_assert(sizeof(rs485_telemetry_t) == RS485_TELEMETRY_LEN, "telemetry wire layout");
_Static_assert(offsetof(rs485_telemetry_t, ax) == RS485_OFF_AX, "acceleration wire offset");
_Static_assert(offsetof(rs485_telemetry_t, quality) == RS485_OFF_QUALITY, "quality wire offset");

rt_uint16_t rs485_crc16(const rt_uint8_t *data, rt_size_t len);
rt_size_t   rs485_pack(rt_uint8_t *out, rt_uint8_t addr, rt_uint8_t cmd,
                       const rt_uint8_t *payload, rt_uint8_t payload_len);

/* 最近一次成功事务的往返时间(ms)，供黑匣子/UI 读取 */
rt_uint32_t rs485_last_rtt_ms(void);
typedef struct { rt_uint32_t ok, timeouts, crc_errors; } rs485_bus_stats_t;
void rs485_bus_stats(rs485_bus_stats_t *out);

/* 遥测查询(带总线序列化)，见 rs485_master.c */
rt_err_t rs485_query(rt_uint8_t addr, rs485_telemetry_t *tele, rt_uint32_t timeout_ms);
rt_err_t rs485_query_ex(rt_uint8_t addr, rs485_telemetry_t *tele, rt_uint32_t timeout_ms, rt_uint32_t *rtt_ms);
rt_err_t rs485_set_led(rt_uint8_t addr, rt_uint8_t on, rt_uint32_t timeout_ms);
int rs485_poller_stop(void);

#endif /* __RS485_PROTO_H__ */
