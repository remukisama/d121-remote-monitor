/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-08-25     RSOC2026     RS485 Half-duplex Communication & Master/Slave Control Main Entry
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "rs485_app.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

int main(void)
{
    rt_kprintf("\n==================================================================\n");
    rt_kprintf("   RT-Thread Spark-1 RS485 Half-duplex Master/Slave Demo           \n");
    rt_kprintf("   Hardware: SP3485 on UART6 (TX: PG14, RX: PG9, RE/DE: PG8)      \n");
    rt_kprintf("   Available MSH Commands:                                        \n");
    rt_kprintf("     - rs485_send <text>              : Send raw ASCII text       \n");
    rt_kprintf("     - rs485_master query [slave_id]  : Query slave telemetry once\n");
    rt_kprintf("     - rs485_master set_led <id> <0|1>: Remote control slave LED  \n");
    rt_kprintf("     - rs485_master start [id] [ms]   : Periodic polling master   \n");
    rt_kprintf("     - rs485_master stop              : Stop master polling       \n");
    rt_kprintf("     - rs485_slave start [slave_addr] : Start slave listener      \n");
    rt_kprintf("     - rs485_slave stop               : Stop slave listener       \n");
    rt_kprintf("     - rs485_stat [reset]             : Show statistics & errors  \n");
    rt_kprintf("     - rs485_baud <9600|115200>       : Set RS485 baudrate        \n");
    rt_kprintf("==================================================================\n\n");

    /* 自动初始化底层 RS485 驱动 */
    if (rs485_app_init() != RT_EOK)
    {
        LOG_E("RS485 application initialization failed!");
        return -RT_ERROR;
    }

    rt_kprintf("RS485 Driver ready! Type commands above in MSH terminal.\n\n");

    /* 主线程挂起出让 CPU，让 MSH 终端保持清爽 */
    while (1)
    {
        rt_thread_mdelay(5000);
    }

    return RT_EOK;
}
