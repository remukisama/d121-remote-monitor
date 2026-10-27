/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-08-25     RSOC2026     RS485 half-duplex driver header for RT-Thread Spark-1
 */

#ifndef APPLICATIONS_RS485_DRV_H_
#define APPLICATIONS_RS485_DRV_H_

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RS485 默认串口设备名称与控制引脚 */
#ifndef BSP_RS485_UART_NAME
#define BSP_RS485_UART_NAME    "uart6"
#endif

#ifndef BSP_RS485_RE_PIN
#define BSP_RS485_RE_PIN       GET_PIN(G, 8)
#endif

/* 
 * SP3485 半双工方向控制宏：
 * RE(低电平有效) 与 DE(高电平有效) 在星火1号硬件上共接至 PG8
 * - PG8 输出低电平(0)：使能接收(~RE=0, DE=0) -> 处于监听/接收状态
 * - PG8 输出高电平(1)：使能发送(~RE=1, DE=1) -> 处于驱动/发送状态
 */
#define RS485_RX_MODE()        rt_pin_write(BSP_RS485_RE_PIN, PIN_LOW)
#define RS485_TX_MODE()        rt_pin_write(BSP_RS485_RE_PIN, PIN_HIGH)

/* RS485 驱动统计数据结构 */
typedef struct
{
    rt_uint32_t tx_bytes;      /* 发送字节总数 */
    rt_uint32_t rx_bytes;      /* 接收字节总数 */
    rt_uint32_t tx_packets;    /* 发送数据包数 */
    rt_uint32_t rx_packets;    /* 接收数据包数 */
    rt_uint32_t rx_crc_err;    /* CRC校验错误数 */
    rt_uint32_t rx_timeout;    /* 接收超时次数 */
    rt_uint32_t tx_errors;
} rs485_stat_t;

/* 驱动核心接口 */
rt_err_t      rs485_drv_init(void);
rt_size_t     rs485_drv_send(const void *buf, rt_size_t size);
rt_size_t     rs485_drv_recv(void *buf, rt_size_t max_size, rt_int32_t timeout_ms);
rt_err_t      rs485_drv_set_baudrate(rt_uint32_t baudrate);
void          rs485_drv_flush(void);
rs485_stat_t* rs485_drv_get_stat(void);
void          rs485_drv_reset_stat(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATIONS_RS485_DRV_H_ */
