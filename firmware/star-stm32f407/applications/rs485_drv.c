/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-08-25     RSOC2026     RS485 half-duplex driver implementation for RT-Thread Spark-1
 */

#include "rs485_drv.h"
#include <stm32f4xx.h>

#define DBG_TAG "rs485.drv"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* RTU at >19200bps uses at least 1.75ms inter-frame silence.
 * Use 2ms at the platform's 1kHz tick rate.
 * RT_TICK_PER_SECOND=1000，2ms 正好是 2 个 tick，不会被取整放大。 */
#define RS485_FRAME_GAP_MS   2

/* 内部设备与同步对象 */
static rt_device_t     g_rs485_serial = RT_NULL;
static struct rt_semaphore g_rs485_rx_sem;
static rs485_stat_t    g_rs485_stat = {0};
static rt_bool_t       g_is_inited = RT_FALSE;
static rt_bool_t g_objects_ready;
static struct rt_mutex g_tx_mutex;
static int driver_ipc_init(void)
{
    int result = RT_EOK;
    rt_enter_critical();
    if (!g_objects_ready) {
        result = rt_sem_init(&g_rs485_rx_sem, "485_rx", 0, RT_IPC_FLAG_FIFO);
        if (result == RT_EOK) {
            result = rt_mutex_init(&g_tx_mutex, "485_tx", RT_IPC_FLAG_PRIO);
            if (result != RT_EOK) rt_sem_detach(&g_rs485_rx_sem);
            else g_objects_ready = RT_TRUE;
        }
    }
    rt_exit_critical(); return result;
}

/**
 * @brief 串口中断接收回调函数
 */
static rt_err_t rs485_rx_indicate(rt_device_t dev, rt_size_t size)
{
    if (size > 0)
    {
        rt_sem_release(&g_rs485_rx_sem);
    }
    return RT_EOK;
}

/**
 * @brief 微秒级硬件延时辅助函数
 */
static void rs485_delay_us(rt_uint32_t us)
{
    rt_uint32_t ticks;
    rt_uint32_t told, tnow, tcnt = 0;
    rt_uint32_t reload = SysTick->LOAD;

    ticks = us * (SystemCoreClock / 1000000);
    told = SysTick->VAL;
    while (1)
    {
        tnow = SysTick->VAL;
        if (tnow != told)
        {
            if (tnow < told)
            {
                tcnt += told - tnow;
            }
            else
            {
                tcnt += reload - tnow + told;
            }
            told = tnow;
            if (tcnt >= ticks)
            {
                break;
            }
        }
    }
}

/**
 * @brief 初始化 RS485 硬件与驱动
 */
rt_err_t rs485_drv_init(void)
{
    if (driver_ipc_init() != RT_EOK) return -RT_ERROR;
    if (rt_mutex_take(&g_tx_mutex, rt_tick_from_millisecond(2000)) != RT_EOK) return -RT_ETIMEOUT;
    if (g_is_inited)
    {
        rt_mutex_release(&g_tx_mutex);
        return RT_EOK;
    }

    /* 1. 初始化半双工方向控制引脚 PG8 为推挽输出，默认置低使能接收 */
    rt_pin_mode(BSP_RS485_RE_PIN, PIN_MODE_OUTPUT);
    RS485_RX_MODE();

    /* 2. 查找底层串口设备 (uart6) */
    g_rs485_serial = rt_device_find(BSP_RS485_UART_NAME);
    if (!g_rs485_serial)
    {
        LOG_E("Cannot find RS485 serial device %s!", BSP_RS485_UART_NAME);
        rt_mutex_release(&g_tx_mutex);
        return -RT_ERROR;
    }

    /* 3. 初始化接收信号量 */

    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    config.baud_rate = 115200; config.data_bits = DATA_BITS_8;
    config.parity = PARITY_NONE; config.stop_bits = STOP_BITS_2;
    rt_err_t configured = rt_device_control(g_rs485_serial, RT_DEVICE_CTRL_CONFIG, &config);
    if (configured != RT_EOK) { rt_mutex_release(&g_tx_mutex); return configured; }

    /* 4. 以中断接收模式打开串口 */
    rt_err_t result = rt_device_open(g_rs485_serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX);
    if (result != RT_EOK)
    {
        LOG_E("Failed to open %s with err=%d", BSP_RS485_UART_NAME, result);
        rt_mutex_release(&g_tx_mutex);
        return result;
    }

    /* 5. 注册接收回调函数 */
    rt_device_set_rx_indicate(g_rs485_serial, rs485_rx_indicate);

    g_is_inited = RT_TRUE;
    rt_mutex_release(&g_tx_mutex);
    LOG_I("RS485 driver initialized successfully on %s (RE Pin: PG8)", BSP_RS485_UART_NAME);

    return RT_EOK;
}

/**
 * @brief 发送 RS485 半双工数据（自动处理 TX/RX 切换时序与硬件移位完成等待）
 */
rt_size_t rs485_drv_send(const void *buf, rt_size_t size)
{
    if (!g_is_inited || !g_rs485_serial || buf == RT_NULL || size == 0)
    {
        return 0;
    }
    if (rt_mutex_take(&g_tx_mutex, rt_tick_from_millisecond(100)) != RT_EOK) { g_rs485_stat.tx_errors++; return 0; }

    /* 1. 切换至发送模式 (拉高 PG8) */
    RS485_TX_MODE();
    rs485_delay_us(20); /* 收发器建立建立时间 (典型 10~20us) */

    /* 2. 调用串口写入接口 */
    rt_size_t written = rt_device_write(g_rs485_serial, 0, buf, size);

    /* 3. 关键硬件等待：等待 USART6 移位寄存器最后一个停止位完全发送完毕 (TC=1)
     * 若直接切换接收模式，将导致最后一个字节在空中被收发器直接截断！
     */
    rt_tick_t started = rt_tick_get();
    while (!(USART6->SR & USART_SR_TC) &&
           (rt_tick_t)(rt_tick_get() - started) < rt_tick_from_millisecond(20))
    {
        /* 硬件等待 TC 标志置位 */
    }

    /* 4. 保护间隔后立即切回接收监听状态 (拉低 PG8) */
    rs485_delay_us(10);
    RS485_RX_MODE();
    if (!(USART6->SR & USART_SR_TC) || written != size) {
        g_rs485_stat.tx_errors++; rt_mutex_release(&g_tx_mutex); return 0;
    }

    /* 5. 统计计数 */
    g_rs485_stat.tx_bytes += written;
    g_rs485_stat.tx_packets++;
    rt_mutex_release(&g_tx_mutex);

    return written;
}

/**
 * @brief 接收 RS485 数据（支持超时机制与分包接收）
 */
rt_size_t rs485_drv_recv(void *buf, rt_size_t max_size, rt_int32_t timeout_ms)
{
    if (!g_is_inited || !g_rs485_serial || buf == RT_NULL || max_size == 0)
    {
        return 0;
    }

    rt_uint8_t *pbuf = (rt_uint8_t *)buf;
    rt_size_t total_read = 0;
    rt_tick_t start_tick = rt_tick_get();
    rt_tick_t wait_ticks = (timeout_ms < 0) ? RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout_ms);

    while (total_read < max_size)
    {
        /* 尝试从串口读取可用数据 */
        rt_size_t ch_read = rt_device_read(g_rs485_serial, 0, pbuf + total_read, max_size - total_read);
        if (ch_read > 0)
        {
            total_read += ch_read;
            /* 收到首字节后，改用较短的字符间静默窗口判定单帧接收完毕 */
            wait_ticks = rt_tick_from_millisecond(RS485_FRAME_GAP_MS);
            start_tick = rt_tick_get();
            continue;
        }

        /* 无数据可读，等待信号量唤醒 */
        rt_tick_t now_tick = rt_tick_get();
        if (timeout_ms == 0 || ((timeout_ms >= 0 || total_read) && (now_tick - start_tick) >= wait_ticks))
        {
            break; /* 超时退出 */
        }

        rt_tick_t remain_ticks = wait_ticks - (now_tick - start_tick);
        if (remain_ticks == 0)
        {
            break;
        }

        if (rt_sem_take(&g_rs485_rx_sem, remain_ticks) != RT_EOK)
        {
            break; /* 等待信号量超时 */
        }
    }

    if (total_read > 0)
    {
        g_rs485_stat.rx_bytes += total_read;
        g_rs485_stat.rx_packets++;
    }
    else if (timeout_ms > 0)
    {
        g_rs485_stat.rx_timeout++;
    }

    return total_read;
}

/**
 * @brief 动态配置 RS485 串口波特率
 */
rt_err_t rs485_drv_set_baudrate(rt_uint32_t baudrate)
{
    if (!g_is_inited || !g_rs485_serial)
    {
        return -RT_ERROR;
    }

    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    config.baud_rate = baudrate;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_2;
    config.parity    = PARITY_NONE;

    rt_err_t result = rt_mutex_take(&g_tx_mutex, rt_tick_from_millisecond(1000));
    if (result != RT_EOK) return result;
    result = rt_device_control(g_rs485_serial, RT_DEVICE_CTRL_CONFIG, &config);
    rt_mutex_release(&g_tx_mutex);
    return result;
}

/**
 * @brief 清空串口接收缓冲区
 */
void rs485_drv_flush(void)
{
    if (!g_is_inited || !g_rs485_serial)
    {
        return;
    }

    rt_uint8_t dummy[32];
    unsigned drained = 0; rt_size_t got;
    while (drained < 512 && (got = rt_device_read(g_rs485_serial, 0, dummy, sizeof(dummy))) > 0) drained += got;
    rt_sem_control(&g_rs485_rx_sem, RT_IPC_CMD_RESET, RT_NULL);
}

/**
 * @brief 获取统计信息
 */
rs485_stat_t* rs485_drv_get_stat(void)
{
    return &g_rs485_stat;
}

/**
 * @brief 重置统计信息
 */
void rs485_drv_reset_stat(void)
{
    rt_enter_critical(); rt_memset(&g_rs485_stat, 0, sizeof(g_rs485_stat)); rt_exit_critical();
}
