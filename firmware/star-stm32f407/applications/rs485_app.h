/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-08-25     RSOC2026     RS485 Master/Slave Protocol and Application Header
 */

#ifndef APPLICATIONS_RS485_APP_H_
#define APPLICATIONS_RS485_APP_H_

#include <rtthread.h>
#include <stddef.h>
#include "rs485_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 协议常量定义 */
#define RS485_FRAME_HEADER1       0xAA
#define RS485_FRAME_HEADER2       0x55
#define RS485_FRAME_TAIL1         0x0D
#define RS485_FRAME_TAIL2         0x0A

#define RS485_ADDR_BROADCAST      0xFF
#define RS485_DEFAULT_SLAVE_ADDR  0x01

/* 功能码定义 (Function Codes) */
#define CMD_QUERY_STATUS          0x01  /* 主机查询从机状态与遥测数据 */
#define CMD_CTRL_LED              0x02  /* 主机控制从机 LED 状态 */
#define CMD_ECHO                  0x03  /* 回环测试 */
#define CMD_ACK_FLAG              0x80  /* 响应应答标志位 (Response ACK) */
#define CMD_NACK_ERROR            0xFF  /* 错误应答 (NACK) */

/* 协议帧最大长度 */
#define RS485_MAX_PAYLOAD_LEN     64
#define RS485_MAX_FRAME_LEN       (RS485_MAX_PAYLOAD_LEN + 9)
#define RS485_QUALITY_TAG         0xA1
#define RS485_Q_ENV_VALID         0x01
#define RS485_Q_IMU_VALID         0x02

/* 从机遥测状态数据结构
 * 布局与 D12x 侧 rs485_proto.h 的显式偏移逐字节对应，改动后两边必须同步重烧。
 * v2 (2026-09-15): +ICM20608 六轴(原始 LSB)，temp/humi 从假数据换成板载 AHT21。 */
typedef struct
{
    rt_uint32_t uptime_s;      /* 0:  系统运行时间 (秒) */
    rt_int16_t  temp_centi;    /* 4:  温度 0.01℃ (AHT21) */
    rt_uint16_t humi_centi;    /* 6:  湿度 0.01 %RH (AHT21) */
    rt_uint8_t  led_state;     /* 8:  LED 状态 */
    rt_uint8_t  pad0;          /* 9:  对齐填充 */
    rt_int16_t  accel_x;       /* 10: 原始 LSB, ±2g=16384 LSB/g (ICM20608) */
    rt_int16_t  accel_y;       /* 12 */
    rt_int16_t  accel_z;       /* 14 */
    rt_int16_t  gyro_x;        /* 16: 原始 LSB, ±250dps=131 LSB/dps */
    rt_int16_t  gyro_y;        /* 18 */
    rt_int16_t  gyro_z;        /* 20 */
    rt_uint8_t  quality;       /* 22: sensor validity (old padding) */
    rt_uint8_t  quality_tag;   /* 23: 0xA1 = validity is supplied */
} rs485_telemetry_t;           /* sizeof = 22 -> 对齐补到 24，两边同构同编译器族 */
_Static_assert(sizeof(rs485_telemetry_t) == 24, "telemetry wire length");
_Static_assert(offsetof(rs485_telemetry_t, accel_x) == 10, "acceleration wire offset");
_Static_assert(offsetof(rs485_telemetry_t, quality) == 22, "validity wire offset");

/* 协议编解码与应用接口 */
rt_uint16_t rs485_calc_crc16(const rt_uint8_t *data, rt_size_t len);
rt_size_t   rs485_pack_frame(rt_uint8_t *out_frame, rt_uint8_t slave_addr, rt_uint8_t cmd, const rt_uint8_t *payload, rt_uint8_t payload_len);
rt_bool_t   rs485_unpack_frame(const rt_uint8_t *in_frame, rt_size_t frame_len, rt_uint8_t *slave_addr, rt_uint8_t *cmd, rt_uint8_t *payload, rt_uint8_t *payload_len);

/* 主从任务控制 */
rt_err_t    rs485_app_init(void);
rt_err_t    rs485_master_start(rt_uint8_t target_slave, rt_uint32_t interval_ms);
rt_err_t    rs485_master_stop(void);
rt_err_t    rs485_master_query_once(rt_uint8_t target_slave);
rt_err_t    rs485_master_set_led_once(rt_uint8_t target_slave, rt_uint8_t led_state);

rt_err_t    rs485_slave_start(rt_uint8_t my_addr);
rt_err_t    rs485_slave_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATIONS_RS485_APP_H_ */
