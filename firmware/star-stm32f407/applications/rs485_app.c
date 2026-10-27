/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-08-25     RSOC2026     RS485 Master/Slave Protocol and Application Implementation
 */

#include "rs485_app.h"
#include "spark_imu.h"
#include "modbus_rtu.h"
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static rt_err_t parse_argument(const char *text, unsigned long min, unsigned long max, unsigned long *out)
{
    if (!text || !*text || *text == '-' || *text == '+') return -RT_EINVAL;
    char *end; errno = 0; unsigned long n = strtoul(text, &end, 0);
    if (errno || *end || n < min || n > max) return -RT_EINVAL;
    *out = n; return RT_EOK;
}

#define DBG_TAG "rs485.app"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 板载状态指示 RGB-LED (星火1号用户手册核验：PF12为红灯，PF11为蓝灯，低电平点亮) */
#define PIN_LED_R    GET_PIN(F, 12)
#define PIN_LED_B    GET_PIN(F, 11)

/* 从站每帧调试打印开关：这些 rt_kprintf 是同步阻塞在 115200 控制台上的，
 * 主机 100ms 轮询时每帧会被调 3 次，直接串进应答延时里。
 * 平时置 0 编译剔除；要抓总线原始字节时改 1 重编。 */
#define RS485_SLAVE_DEBUG    0

/* 主机与从机线程控制块 */
static rt_thread_t g_master_thread = RT_NULL;
static rt_thread_t g_slave_thread  = RT_NULL;
static volatile rt_bool_t g_master_running = RT_FALSE;
static volatile rt_bool_t g_slave_running  = RT_FALSE;
static volatile rt_bool_t g_role_starting;

static rt_uint8_t  g_target_slave_addr = RS485_DEFAULT_SLAVE_ADDR;
static rt_uint32_t g_master_interval_ms = 1000;
static rt_uint8_t  g_my_slave_addr = RS485_DEFAULT_SLAVE_ADDR;
static int g_modbus_mode = 1;

static int modbus_read(void *context, uint8_t function, uint16_t start, uint16_t count, uint16_t *out)
{
    rt_uint8_t led = *(rt_uint8_t *)context;
    if (function == 3) {
        if (start || count != 1) return 2;
        out[0] = led; return 0;
    }
    if ((unsigned)start + count > DMB_STAR_REGS) return 2;
    uint16_t regs[DMB_STAR_REGS] = {0};
    rt_int16_t temp = 0; rt_uint16_t humidity = 0; spark_imu_t imu;
    rt_uint32_t sequence, age;
    int env_valid = spark_env_read(&temp, &humidity);
    int imu_valid = spark_imu_snapshot(&imu, &sequence, &age) == RT_EOK;
    regs[0] = DMB_MAP_VERSION; regs[1] = 1; regs[2] = g_my_slave_addr;
    regs[3] = (env_valid ? 1 : 0) | (imu_valid ? 2 : 0);
    dmb_regs32(regs + 4, sequence); dmb_regs32(regs + 6, spark_uptime_ms() / 1000);
    dmb_regs32(regs + 8, age); regs[10] = (uint16_t)temp; regs[11] = humidity; regs[12] = led;
    regs[13] = (uint16_t)imu.ax; regs[14] = (uint16_t)imu.ay; regs[15] = (uint16_t)imu.az;
    regs[16] = (uint16_t)imu.gx; regs[17] = (uint16_t)imu.gy; regs[18] = (uint16_t)imu.gz;
    memcpy(out, regs + start, count * sizeof(*out)); return 0;
}
static int modbus_write(void *context, uint16_t reg, uint16_t value)
{
    if (reg) return 2;
    if (value > 1) return 3;
    *(rt_uint8_t *)context = value;
    rt_pin_write(PIN_LED_R, value ? PIN_LOW : PIN_HIGH);
    rt_pin_write(PIN_LED_B, value ? PIN_LOW : PIN_HIGH);
    return 0;
}
static void modbus_dispatch(const rt_uint8_t *request, rt_size_t length, rt_uint8_t *led)
{
    uint8_t reply[DMB_ADU_MAX];
    if (length >= 4 && !dmb_valid(request, length)) rs485_drv_get_stat()->rx_crc_err++;
    size_t n = dmb_server(request, length, g_my_slave_addr, led, modbus_read, modbus_write, reply, sizeof(reply));
    if (n && rs485_drv_send(reply, n) != n) LOG_W("Modbus reply send failed");
}

/**
 * @brief CRC16-MODBUS 校验算法 (多项式 0xA001，初始值 0xFFFF)
 */
rt_uint16_t rs485_calc_crc16(const rt_uint8_t *data, rt_size_t len)
{
    rt_uint16_t crc = 0xFFFF;
    for (rt_size_t i = 0; i < len; i++)
    {
        crc ^= (rt_uint16_t)data[i];
        for (rt_uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

/**
 * @brief 组装 RS485 自定义协议数据帧
 * 帧结构: [AA 55] [ADDR] [CMD] [LEN] [PAYLOAD...] [CRC_L CRC_H] [0D 0A]
 */
rt_size_t rs485_pack_frame(rt_uint8_t *out_frame, rt_uint8_t slave_addr, rt_uint8_t cmd, const rt_uint8_t *payload, rt_uint8_t payload_len)
{
    if (out_frame == RT_NULL || payload_len > RS485_MAX_PAYLOAD_LEN || (payload_len && !payload))
    {
        return 0;
    }

    out_frame[0] = RS485_FRAME_HEADER1;
    out_frame[1] = RS485_FRAME_HEADER2;
    out_frame[2] = slave_addr;
    out_frame[3] = cmd;
    out_frame[4] = payload_len;

    if (payload != RT_NULL && payload_len > 0)
    {
        rt_memcpy(&out_frame[5], payload, payload_len);
    }

    /* 对 [ADDR] 到 [PAYLOAD 末尾] 进行 CRC16 校验 */
    rt_uint16_t crc = rs485_calc_crc16(&out_frame[2], 3 + payload_len);
    rt_size_t idx = 5 + payload_len;

    out_frame[idx++] = (rt_uint8_t)(crc & 0xFF);        /* CRC 低字节在前 */
    out_frame[idx++] = (rt_uint8_t)((crc >> 8) & 0xFF); /* CRC 高字节在后 */
    out_frame[idx++] = RS485_FRAME_TAIL1;
    out_frame[idx++] = RS485_FRAME_TAIL2;

    return idx;
}

/**
 * @brief 解析 RS485 自定义协议数据帧（支持前导噪声过滤与尾部容错）
 */
rt_bool_t rs485_unpack_frame(const rt_uint8_t *in_frame, rt_size_t frame_len, rt_uint8_t *slave_addr, rt_uint8_t *cmd, rt_uint8_t *payload, rt_uint8_t *payload_len)
{
    if (in_frame == RT_NULL || frame_len < 9)
    {
        return RT_FALSE;
    }

    /* 1. 查找帧头 0xAA 0x55 (过滤前导噪声) */
    rt_size_t start_idx = 0;
    while (start_idx + 1 < frame_len)
    {
        if (in_frame[start_idx] == RS485_FRAME_HEADER1 && in_frame[start_idx + 1] == RS485_FRAME_HEADER2)
        {
            break;
        }
        start_idx++;
    }

    if (start_idx + 9 > frame_len)
    {
        return RT_FALSE; /* 未找到完整帧头 */
    }

    const rt_uint8_t *pframe = &in_frame[start_idx];
    rt_size_t rem_len = frame_len - start_idx;

    rt_uint8_t plen = pframe[4];
    if (plen > RS485_MAX_PAYLOAD_LEN || rem_len < (rt_size_t)(plen + 9) ||
        pframe[7 + plen] != RS485_FRAME_TAIL1 || pframe[8 + plen] != RS485_FRAME_TAIL2)
    {
        return RT_FALSE;
    }

    /* 2. 校验 CRC16: 从 ADDR(索引2) 到 PAYLOAD 结束 */
    rt_uint16_t calc_crc = rs485_calc_crc16(&pframe[2], 3 + plen);
    rt_uint16_t recv_crc = (rt_uint16_t)pframe[5 + plen] | ((rt_uint16_t)pframe[5 + plen + 1] << 8);

    if (calc_crc != recv_crc)
    {
        rs485_drv_get_stat()->rx_crc_err++;
        LOG_W("RS485 Frame CRC mismatch! Calc=0x%04X, Recv=0x%04X", calc_crc, recv_crc);
        return RT_FALSE;
    }

    /* 3. 提取字段 */
    if (slave_addr) *slave_addr = pframe[2];
    if (cmd)        *cmd        = pframe[3];
    if (payload_len)*payload_len= plen;
    if (payload && plen > 0)
    {
        rt_memcpy(payload, &pframe[5], plen);
    }

    return RT_TRUE;
}

/* =========================== 主机模式 (Master) =========================== */

/**
 * @brief 主机单次查询指定从机状态与遥测数据
 */
rt_err_t rs485_master_query_once(rt_uint8_t target_slave)
{
    if (!target_slave || target_slave > 254) return -RT_EINVAL;
    rt_uint8_t tx_buf[RS485_MAX_FRAME_LEN];
    rt_uint8_t rx_buf[RS485_MAX_FRAME_LEN];

    /* 组装查询数据帧 */
    rt_size_t tx_len = rs485_pack_frame(tx_buf, target_slave, CMD_QUERY_STATUS, RT_NULL, 0);

    /* 清空历史残留数据并发送 */
    rs485_drv_flush();
    if (rs485_drv_send(tx_buf, tx_len) != tx_len) return -RT_ERROR;

    rt_kprintf("[Master -> Slave 0x%02X] Query Status sent...\n", target_slave);

    /* 等待从机响应 (半双工应答超时设为 300ms) */
    rt_size_t rx_len = rs485_drv_recv(rx_buf, sizeof(rx_buf), 300);
    if (rx_len == 0)
    {
        rt_kprintf("[Master] Slave 0x%02X response timeout (no ACK received)!\n", target_slave);
        return -RT_ETIMEOUT;
    }

    /* 解包响应帧 */
    rt_uint8_t resp_addr, resp_cmd, resp_plen;
    rt_uint8_t resp_payload[RS485_MAX_PAYLOAD_LEN];

    if (rs485_unpack_frame(rx_buf, rx_len, &resp_addr, &resp_cmd, resp_payload, &resp_plen))
    {
        if (resp_addr == target_slave && resp_cmd == (CMD_QUERY_STATUS | CMD_ACK_FLAG) && resp_plen >= sizeof(rs485_telemetry_t))
        {
            rs485_telemetry_t tele;
            rt_memcpy(&tele, resp_payload, sizeof(tele));

            rt_kprintf("\n[Master <- Slave 0x%02X] ACK Received Successfully:\n", resp_addr);
            rt_kprintf("  - Slave Uptime   : %u s\n", (unsigned int)tele.uptime_s);
            rt_kprintf("  - Temperature    : %d.%02d C\n", tele.temp_centi / 100, tele.temp_centi % 100);
            rt_kprintf("  - Humidity       : %u.%02u %%\n", tele.humi_centi / 100, tele.humi_centi % 100);
            rt_kprintf("  - Slave LED State: %s\n\n", tele.led_state ? "ON" : "OFF");
            return RT_EOK;
        }
        else
        {
            rt_kprintf("[Master] Invalid ACK cmd=0x%02X or addr=0x%02X\n", resp_cmd, resp_addr);
            return -RT_ERROR;
        }
    }

    rt_kprintf("[Master] Frame parse failed (corrupted frame)!\n");
    return -RT_ERROR;
}

/**
 * @brief 主机单次控制指定从机 LED 状态
 */
rt_err_t rs485_master_set_led_once(rt_uint8_t target_slave, rt_uint8_t led_state)
{
    if (!target_slave || target_slave > 254 || led_state > 1) return -RT_EINVAL;
    rt_uint8_t tx_buf[RS485_MAX_FRAME_LEN];
    rt_uint8_t rx_buf[RS485_MAX_FRAME_LEN];

    rt_uint8_t payload[1] = { led_state ? 1 : 0 };
    rt_size_t tx_len = rs485_pack_frame(tx_buf, target_slave, CMD_CTRL_LED, payload, 1);

    rs485_drv_flush();
    if (rs485_drv_send(tx_buf, tx_len) != tx_len) return -RT_ERROR;

    rt_kprintf("[Master -> Slave 0x%02X] Set LED = %s command sent...\n", target_slave, led_state ? "ON" : "OFF");

    rt_size_t rx_len = rs485_drv_recv(rx_buf, sizeof(rx_buf), 300);
    if (rx_len == 0)
    {
        rt_kprintf("[Master] Slave 0x%02X set LED timeout!\n", target_slave);
        return -RT_ETIMEOUT;
    }

    rt_uint8_t resp_addr, resp_cmd, resp_plen;
    rt_uint8_t resp_payload[RS485_MAX_PAYLOAD_LEN];
    if (rs485_unpack_frame(rx_buf, rx_len, &resp_addr, &resp_cmd, resp_payload, &resp_plen))
    {
        if (resp_addr == target_slave && resp_cmd == (CMD_CTRL_LED | CMD_ACK_FLAG) &&
            resp_plen == 1 && resp_payload[0] == led_state)
        {
            rt_kprintf("[Master <- Slave 0x%02X] LED Control ACK Success! Current State: %s\n",
                       resp_addr, resp_payload[0] ? "ON" : "OFF");
            return RT_EOK;
        }
    }

    rt_kprintf("[Master] Set LED response error!\n");
    return -RT_ERROR;
}

/**
 * @brief 主机轮询线程入口
 */
static void rs485_master_thread_entry(void *parameter)
{
    rt_kprintf("[Master Thread] Started. Polling Slave 0x%02X every %u ms...\n",
               g_target_slave_addr, (unsigned int)g_master_interval_ms);

    while (g_master_running)
    {
        rs485_master_query_once(g_target_slave_addr);
        for (rt_uint32_t slept = 0; g_master_running && slept < g_master_interval_ms;) {
            unsigned step = g_master_interval_ms - slept > 20 ? 20 : g_master_interval_ms - slept;
            rt_thread_mdelay(step); slept += step;
        }
    }

    rt_kprintf("[Master Thread] Stopped.\n");
    rt_enter_critical(); g_master_thread = RT_NULL; rt_exit_critical();
}

rt_err_t rs485_master_start(rt_uint8_t target_slave, rt_uint32_t interval_ms)
{
    if (!target_slave || target_slave == RS485_ADDR_BROADCAST || interval_ms < 100 || interval_ms > 3600000) return -RT_EINVAL;
    rt_enter_critical();
    if (g_slave_thread || g_role_starting)
    {
        rt_exit_critical();
        rt_kprintf("Error: Slave mode is currently running! Please stop slave first.\n");
        return -RT_EBUSY;
    }

    if (g_master_thread)
    {
        int result = g_master_running ? RT_EOK : -RT_EBUSY;
        rt_exit_critical();
        rt_kprintf("Master mode is already running.\n");
        return result;
    }

    g_role_starting = RT_TRUE; rt_exit_critical();
    g_target_slave_addr = target_slave;
    g_master_interval_ms = (interval_ms < 100) ? 1000 : interval_ms;
    g_master_running = RT_TRUE;

    g_master_thread = rt_thread_create("rs485_m", rs485_master_thread_entry, RT_NULL, 4096, 15, 10);
    if (g_master_thread != RT_NULL)
    {
        if (rt_thread_startup(g_master_thread) != RT_EOK) {
            rt_thread_delete(g_master_thread); g_master_thread = RT_NULL;
            g_master_running = g_role_starting = RT_FALSE; return -RT_ERROR;
        }
        g_role_starting = RT_FALSE;
        return RT_EOK;
    }

    g_master_running = g_role_starting = RT_FALSE;
    return -RT_ERROR;
}

rt_err_t rs485_master_stop(void)
{
    if (g_role_starting) return -RT_EBUSY;
    g_master_running = RT_FALSE;
    rt_tick_t start = rt_tick_get();
    while (g_master_thread) {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(2000)) return -RT_ETIMEOUT;
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

/* =========================== 从机模式 (Slave) =========================== */

static int str_case_cmp(const char *s1, const char *s2)
{
    while (*s1 && *s2)
    {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (*s2 + 32) : *s2;
        if (c1 != c2)
        {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}
/* Preserve partial packets and process buffered packets before waiting for new bytes. */
static rt_uint8_t g_slave_pending[2 * RS485_MAX_FRAME_LEN];
static rt_size_t g_slave_pending_len;
static rt_tick_t g_slave_pending_at;
static rt_size_t pop_slave_packet(rt_uint8_t *out)
{
    while (g_slave_pending_len >= 2) {
        if (g_slave_pending[0] != RS485_FRAME_HEADER1 || g_slave_pending[1] != RS485_FRAME_HEADER2) {
            memmove(g_slave_pending, g_slave_pending + 1, --g_slave_pending_len); continue;
        }
        if (g_slave_pending_len < 5) return 0;
        unsigned length = g_slave_pending[4] + 9;
        if (length <= RS485_MAX_FRAME_LEN && g_slave_pending_len < length) return 0;
        if (length <= RS485_MAX_FRAME_LEN && rs485_unpack_frame(g_slave_pending, length, RT_NULL, RT_NULL, RT_NULL, RT_NULL)) {
            memcpy(out, g_slave_pending, length);
            g_slave_pending_len -= length;
            memmove(g_slave_pending, g_slave_pending + length, g_slave_pending_len); return length;
        }
        memmove(g_slave_pending, g_slave_pending + 1, --g_slave_pending_len);
    }
    return 0;
}
static rt_size_t receive_slave_packet(rt_uint8_t *out)
{
    rt_size_t ready = pop_slave_packet(out);
    if (ready) return ready;
    if (g_slave_pending_len && (rt_tick_t)(rt_tick_get() - g_slave_pending_at) > rt_tick_from_millisecond(100)) g_slave_pending_len = 0;
    rt_uint8_t chunk[RS485_MAX_FRAME_LEN];
    rt_size_t n = rs485_drv_recv(chunk, sizeof(chunk), 100);
    if (!n) return 0;
    int ascii = !g_slave_pending_len;
    for (rt_size_t i = 0; i < n; i++) if ((chunk[i] < 32 && chunk[i] != '\r' && chunk[i] != '\n') || chunk[i] > 126) ascii = 0;
    if (ascii) { memcpy(out, chunk, n); return n; }
    if (g_slave_pending_len + n > sizeof(g_slave_pending)) g_slave_pending_len = 0;
    memcpy(g_slave_pending + g_slave_pending_len, chunk, n); g_slave_pending_len += n;
    g_slave_pending_at = rt_tick_get(); return pop_slave_packet(out);
}

/**
 * @brief 从机监听与应答线程入口
 */
static void rs485_slave_thread_entry(void *parameter)
{
    rt_uint8_t rx_buf[DMB_ADU_MAX];
    rt_uint8_t tx_buf[RS485_MAX_FRAME_LEN];
    rt_uint8_t current_led_state = 0;
    g_slave_pending_len = 0;

    rt_pin_mode(PIN_LED_R, PIN_MODE_OUTPUT);
    rt_pin_mode(PIN_LED_B, PIN_MODE_OUTPUT);
    rt_pin_write(PIN_LED_R, PIN_HIGH); /* 默认熄灭 (共阳极，高电平熄灭，低电平点亮) */
    rt_pin_write(PIN_LED_B, PIN_HIGH);

    rt_kprintf("[Slave Thread] Listening as Slave ID 0x%02X (%s, 115200/8N2)\n", g_my_slave_addr, g_modbus_mode ? "Modbus RTU" : "AA55 legacy");

    while (g_slave_running)
    {
        /* 阻塞等待接收一帧数据 (超时 500ms 检查线程运行标志) */
        rt_size_t rx_len = g_modbus_mode ? rs485_drv_recv(rx_buf, sizeof(rx_buf), 100) : receive_slave_packet(rx_buf);
        if (rx_len == 0)
        {
            continue;
        }

        if (g_modbus_mode) { modbus_dispatch(rx_buf, rx_len, &current_led_state); continue; }

#if RS485_SLAVE_DEBUG
        /* 调试打印：显示总线上接收到的原始十六进制字节 */
        rt_kprintf("[Slave RX Raw %u bytes]: ", (unsigned int)rx_len);
        for (rt_size_t i = 0; i < rx_len; i++)
        {
            rt_kprintf("%02X ", rx_buf[i]);
        }
        rt_kprintf("\n");
#endif

        rt_uint8_t recv_addr, cmd, plen;
        rt_uint8_t payload[RS485_MAX_PAYLOAD_LEN];

        if (rs485_unpack_frame(rx_buf, rx_len, &recv_addr, &cmd, payload, &plen))
        {
            /* ---------- 模式 A：二进制 HEX 协议帧处理 ---------- */
            if (recv_addr != g_my_slave_addr && recv_addr != RS485_ADDR_BROADCAST)
            {
                continue; /* 非发给本机的报文，忽略 */
            }

#if RS485_SLAVE_DEBUG
            rt_kprintf("[Slave 0x%02X] Received HEX CMD 0x%02X from Master\n", g_my_slave_addr, cmd);
#endif

            /* 指令 1: 主机查询状态与遥测 */
            if (recv_addr == RS485_ADDR_BROADCAST) {
                /* A broadcast never receives ACKs from multiple slaves. */
                if (cmd == CMD_CTRL_LED && plen == 1 && payload[0] <= 1) {
                    current_led_state = payload[0];
                    rt_pin_write(PIN_LED_R, current_led_state ? PIN_LOW : PIN_HIGH);
                    rt_pin_write(PIN_LED_B, current_led_state ? PIN_LOW : PIN_HIGH);
                }
                continue;
            }
            if (cmd == CMD_QUERY_STATUS && plen == 0)
            {
                rs485_telemetry_t tele;
                spark_imu_t imu;

                rt_memset(&tele, 0, sizeof(tele));
                tele.uptime_s = spark_uptime_ms() / 1000;
                tele.quality_tag = RS485_QUALITY_TAG;

                /* 温湿度: 板载 AHT21 (1Hz 缓存线程, 无数据时保持 0, 主站可辨识) */
                if (spark_env_read(&tele.temp_centi, &tele.humi_centi)) tele.quality |= RS485_Q_ENV_VALID;

                /* Sensor retries run independently; replies only copy a fresh snapshot. */
                if (spark_imu_cached(&imu) == RT_EOK)
                {
                    tele.accel_x = imu.ax; tele.accel_y = imu.ay; tele.accel_z = imu.az;
                    tele.gyro_x  = imu.gx; tele.gyro_y  = imu.gy; tele.gyro_z  = imu.gz;
                    tele.quality |= RS485_Q_IMU_VALID;
                }
                tele.led_state  = current_led_state;

                rt_size_t tx_len = rs485_pack_frame(tx_buf, g_my_slave_addr, (CMD_QUERY_STATUS | CMD_ACK_FLAG),
                                                    (rt_uint8_t *)&tele, sizeof(tele));
                
                rt_thread_mdelay(2);
                rs485_drv_send(tx_buf, tx_len);
#if RS485_SLAVE_DEBUG
                rt_kprintf("[Slave 0x%02X] Telemetry ACK sent.\n", g_my_slave_addr);
#endif
            }
            /* 指令 2: 主机控制从机 LED */
            else if (cmd == CMD_CTRL_LED)
            {
                if (plen == 1 && payload[0] <= 1)
                {
                    current_led_state = payload[0] ? 1 : 0;
                    rt_pin_write(PIN_LED_R, current_led_state ? PIN_LOW : PIN_HIGH);
                    rt_pin_write(PIN_LED_B, current_led_state ? PIN_LOW : PIN_HIGH);
                    rt_kprintf("[Slave 0x%02X] LED switched to %s\n", g_my_slave_addr, current_led_state ? "ON" : "OFF");

                    rt_uint8_t ack_payload[1] = { current_led_state };
                    rt_size_t tx_len = rs485_pack_frame(tx_buf, g_my_slave_addr, (CMD_CTRL_LED | CMD_ACK_FLAG),
                                                        ack_payload, 1);
                    rt_thread_mdelay(2);
                    rs485_drv_send(tx_buf, tx_len);
                }
            }
            /* 指令 3: 回环测试 */
            else if (cmd == CMD_ECHO)
            {
                rt_size_t tx_len = rs485_pack_frame(tx_buf, g_my_slave_addr, (CMD_ECHO | CMD_ACK_FLAG),
                                                    payload, plen);
                rt_thread_mdelay(2);
                rs485_drv_send(tx_buf, tx_len);
            }
        }
        else
        {
            int printable = 1;
            for (rt_size_t i = 0; i < rx_len; i++)
                if ((rx_buf[i] < 32 && rx_buf[i] != '\r' && rx_buf[i] != '\n') || rx_buf[i] > 126) printable = 0;
            if (!printable) continue; /* Corrupt binary traffic cannot trigger text LED commands. */
            /* ---------- 模式 B：纯文本 ASCII 字符串指令处理 (如电脑串口直接发送 "LED ON") ---------- */
            char str_cmd[64];
            rt_size_t copy_len = (rx_len < sizeof(str_cmd) - 1) ? rx_len : (sizeof(str_cmd) - 1);
            rt_memcpy(str_cmd, rx_buf, copy_len);
            str_cmd[copy_len] = '\0';

            /* 过滤末尾的回车换行、空格与不可见字符 */
            while (copy_len > 0 && (str_cmd[copy_len - 1] == '\r' || str_cmd[copy_len - 1] == '\n' || 
                                    str_cmd[copy_len - 1] == ' ' || (unsigned char)str_cmd[copy_len - 1] < 32))
            {
                str_cmd[--copy_len] = '\0';
            }

            char reply_str[160];

            /* 指令: "LED ON" 或 "ON" 或 "1" -> 点亮板载 LED 并字符串回复 */
            if (str_case_cmp(str_cmd, "LED ON") == 0 || str_case_cmp(str_cmd, "LED_ON") == 0 || 
                str_case_cmp(str_cmd, "ON") == 0 || strcmp(str_cmd, "1") == 0)
            {
                current_led_state = 1;
                rt_pin_write(PIN_LED_R, PIN_LOW);
                rt_pin_write(PIN_LED_B, PIN_LOW);
                rt_kprintf("[Slave 0x%02X] String CMD: '%s' -> [LED ON]\n", g_my_slave_addr, str_cmd);

                rt_snprintf(reply_str, sizeof(reply_str), "[Slave %02X] OK: LED turned ON!\r\n", g_my_slave_addr);
                rt_thread_mdelay(2);
                rs485_drv_send(reply_str, rt_strlen(reply_str));
            }
            /* 指令: "LED OFF" 或 "OFF" 或 "0" -> 熄灭板载 LED 并字符串回复 */
            else if (str_case_cmp(str_cmd, "LED OFF") == 0 || str_case_cmp(str_cmd, "LED_OFF") == 0 || 
                     str_case_cmp(str_cmd, "OFF") == 0 || strcmp(str_cmd, "0") == 0)
            {
                current_led_state = 0;
                rt_pin_write(PIN_LED_R, PIN_HIGH);
                rt_pin_write(PIN_LED_B, PIN_HIGH);
                rt_kprintf("[Slave 0x%02X] String CMD: '%s' -> [LED OFF]\n", g_my_slave_addr, str_cmd);

                rt_snprintf(reply_str, sizeof(reply_str), "[Slave %02X] OK: LED turned OFF!\r\n", g_my_slave_addr);
                rt_thread_mdelay(2);
                rs485_drv_send(reply_str, rt_strlen(reply_str));
            }
            /* 指令: "QUERY" 或 "TEMP" 或 "STATUS" -> 回传温湿度与状态可读字符串 */
            else if (str_case_cmp(str_cmd, "QUERY") == 0 || str_case_cmp(str_cmd, "TEMP") == 0 || 
                     str_case_cmp(str_cmd, "STATUS") == 0 || str_case_cmp(str_cmd, "GET") == 0)
            {
                rt_uint32_t uptime = spark_uptime_ms() / 1000;
                rt_int16_t  temp_c = 0;
                rt_uint16_t humi_c = 0;

                /* 2026-09-16: 去掉模拟温湿度（原来是 2650+(uptime%30)*10 /
                 * 5500+(uptime%20)*20 的假数据），改为读板载 AHT21 真值；
                 * 拿不到就如实回 N/A，绝不捏造数值。 */
                if (spark_env_read(&temp_c, &humi_c))
                {
                    rt_kprintf("[Slave 0x%02X] String CMD: '%s' -> Telemetry string replied (%d.%02dC/%u.%02u%%)\n",
                               g_my_slave_addr, str_cmd, temp_c / 100,
                               (temp_c < 0 ? -temp_c : temp_c) % 100, humi_c / 100, humi_c % 100);
                    rt_snprintf(reply_str, sizeof(reply_str),
                                "[Slave %02X] Telemetry: Temp=%d.%02dC, Humi=%u.%02u%%, Uptime=%us, LED=%s\r\n",
                                g_my_slave_addr, temp_c / 100,
                                (temp_c < 0 ? -temp_c : temp_c) % 100, humi_c / 100, humi_c % 100,
                                (unsigned int)uptime, current_led_state ? "ON" : "OFF");
                }
                else
                {
                    rt_kprintf("[Slave 0x%02X] String CMD: '%s' -> Telemetry string replied (AHT21 N/A)\n",
                               g_my_slave_addr, str_cmd);
                    rt_snprintf(reply_str, sizeof(reply_str),
                                "[Slave %02X] Telemetry: Temp=N/A, Humi=N/A, Uptime=%us, LED=%s\r\n",
                                g_my_slave_addr, (unsigned int)uptime,
                                current_led_state ? "ON" : "OFF");
                }
                rt_thread_mdelay(2);
                rs485_drv_send(reply_str, rt_strlen(reply_str));
            }
            /* 指令: "PING" -> 回传 "PONG" */
            else if (str_case_cmp(str_cmd, "PING") == 0)
            {
                rt_kprintf("[Slave 0x%02X] String CMD: 'PING' -> PONG\n", g_my_slave_addr);
                rt_snprintf(reply_str, sizeof(reply_str), "[Slave %02X] PONG!\r\n", g_my_slave_addr);
                rt_thread_mdelay(2);
                rs485_drv_send(reply_str, rt_strlen(reply_str));
            }
            /* 任意普通文本 -> 自动回显 Echo */
            else if (copy_len > 0)
            {
                rt_kprintf("[Slave 0x%02X] String Received: '%s'\n", g_my_slave_addr, str_cmd);
                rt_snprintf(reply_str, sizeof(reply_str), "[Slave %02X] Echo: %s\r\n", g_my_slave_addr, str_cmd);
                rt_thread_mdelay(2);
                rs485_drv_send(reply_str, rt_strlen(reply_str));
            }
        }
    }

    rt_kprintf("[Slave Thread] Stopped.\n");
    rt_enter_critical(); g_slave_thread = RT_NULL; rt_exit_critical();
}

rt_err_t rs485_slave_start(rt_uint8_t my_addr)
{
    if (!my_addr || my_addr == RS485_ADDR_BROADCAST) return -RT_EINVAL;
    rt_enter_critical();
    if (g_master_thread || g_role_starting)
    {
        rt_exit_critical();
        rt_kprintf("Error: Master mode is currently running! Please stop master first.\n");
        return -RT_EBUSY;
    }

    if (g_slave_thread)
    {
        int result = g_slave_running ? RT_EOK : -RT_EBUSY;
        rt_exit_critical();
        rt_kprintf("Slave mode is already running.\n");
        return result;
    }

    g_role_starting = RT_TRUE; rt_exit_critical();
    g_my_slave_addr = my_addr;
    g_slave_running = RT_TRUE;

    g_slave_thread = rt_thread_create("rs485_s", rs485_slave_thread_entry, RT_NULL, 4096, 16, 10);
    if (g_slave_thread != RT_NULL)
    {
        if (rt_thread_startup(g_slave_thread) != RT_EOK) {
            rt_thread_delete(g_slave_thread); g_slave_thread = RT_NULL;
            g_slave_running = g_role_starting = RT_FALSE; return -RT_ERROR;
        }
        g_role_starting = RT_FALSE;
        return RT_EOK;
    }

    g_slave_running = g_role_starting = RT_FALSE;
    return -RT_ERROR;
}

rt_err_t rs485_slave_stop(void)
{
    if (g_role_starting) return -RT_EBUSY;
    g_slave_running = RT_FALSE;
    rt_tick_t start = rt_tick_get();
    while (g_slave_thread) {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(2000)) return -RT_ETIMEOUT;
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

/**
 * @brief 初始化 RS485 应用层
 */
rt_err_t rs485_app_init(void)
{
    return rs485_drv_init();
}

/* =========================== MSH 命令行交互接口 =========================== */

static int rs485_send(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: rs485_send <text_message>\n");
        rt_kprintf("Example: rs485_send Hello_Spark1\n");
        return -1;
    }

    rs485_drv_init();
    rt_size_t len = strlen(argv[1]);
    rt_size_t sent = rs485_drv_send(argv[1], len);

    rt_kprintf("RS485 Sent %u bytes: \"%s\"\n", (unsigned int)sent, argv[1]);
    return 0;
}
MSH_CMD_EXPORT(rs485_send, Send raw text string via RS485);

static int rs485_master(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage:\n");
        rt_kprintf("  rs485_master start [slave_id] [interval_ms] - Start periodic polling\n");
        rt_kprintf("  rs485_master stop                          - Stop master polling\n");
        rt_kprintf("  rs485_master query [slave_id]              - Query slave status once\n");
        rt_kprintf("  rs485_master set_led <slave_id> <0|1>      - Control remote slave LED\n");
        return -1;
    }

    rs485_drv_init();

    if (strcmp(argv[1], "start") == 0)
    {
        unsigned long slave = RS485_DEFAULT_SLAVE_ADDR, interval = 1000;
        if (argc > 4 || (argc >= 3 && parse_argument(argv[2],1,254,&slave)) ||
            (argc == 4 && parse_argument(argv[3],100,3600000,&interval))) return -RT_EINVAL;
        return rs485_master_start(slave, interval);
    }
    else if (strcmp(argv[1], "stop") == 0)
    {
        return rs485_master_stop();
    }
    else if (strcmp(argv[1], "query") == 0)
    {
        unsigned long slave = RS485_DEFAULT_SLAVE_ADDR;
        if (argc > 3 || (argc == 3 && parse_argument(argv[2],1,254,&slave))) return -RT_EINVAL;
        return rs485_master_query_once(slave);
    }
    else if (strcmp(argv[1], "set_led") == 0)
    {
        if (argc < 4)
        {
            rt_kprintf("Usage: rs485_master set_led <slave_id> <0|1>\n");
            return -1;
        }
        unsigned long slave, state;
        if (argc != 4 || parse_argument(argv[2],1,254,&slave) || parse_argument(argv[3],0,1,&state)) return -RT_EINVAL;
        return rs485_master_set_led_once(slave, state);
    }

    rt_kprintf("Unknown subcommand '%s'\n", argv[1]);
    return -1;
}
MSH_CMD_EXPORT(rs485_master, RS485 Master control commands);

static int rs485_slave(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage:\n");
        rt_kprintf("  rs485_slave start [my_slave_addr] - Start slave listening daemon\n");
        rt_kprintf("  rs485_slave stop                  - Stop slave daemon\n");
        return -1;
    }

    rs485_drv_init();

    if (strcmp(argv[1], "start") == 0)
    {
        unsigned long addr = RS485_DEFAULT_SLAVE_ADDR;
        if (argc > 3 || (argc == 3 && parse_argument(argv[2],1,254,&addr))) return -RT_EINVAL;
        return rs485_slave_start(addr);
    }
    else if (strcmp(argv[1], "stop") == 0)
    {
        return rs485_slave_stop();
    }

    rt_kprintf("Unknown subcommand '%s'\n", argv[1]);
    return -1;
}
MSH_CMD_EXPORT(rs485_slave, RS485 Slave control commands);

static int rs485_stat(int argc, char **argv)
{
    rs485_stat_t *st = rs485_drv_get_stat();

    rt_kprintf("\n================ RS485 Statistics ================\n");
    rt_kprintf("  TX Packets   : %u\n", (unsigned int)st->tx_packets);
    rt_kprintf("  TX Bytes     : %u bytes\n", (unsigned int)st->tx_bytes);
    rt_kprintf("  RX Packets   : %u\n", (unsigned int)st->rx_packets);
    rt_kprintf("  RX Bytes     : %u bytes\n", (unsigned int)st->rx_bytes);
    rt_kprintf("  RX CRC Errors: %u\n", (unsigned int)st->rx_crc_err);
    rt_kprintf("  RX Timeouts  : %u\n", (unsigned int)st->rx_timeout);
    rt_kprintf("  Master State : %s\n", g_master_running ? "RUNNING" : "IDLE");
    rt_kprintf("  Slave State  : %s\n", g_slave_running ? "RUNNING" : "IDLE");
    rt_kprintf("==================================================\n\n");

    if (argc >= 2 && strcmp(argv[1], "reset") == 0)
    {
        rs485_drv_reset_stat();
        rt_kprintf("Statistics reset successfully.\n");
    }

    return 0;
}
MSH_CMD_EXPORT(rs485_stat, Display RS485 communication statistics);

static int rs485_baud(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: rs485_baud <9600|19200|38400|57600|115200>\n");
        return -1;
    }

    unsigned long baud;
    if (g_slave_running || g_master_running) return -RT_EBUSY;
    if (argc != 2 || parse_argument(argv[1],1200,3000000,&baud))
    {
        rt_kprintf("Invalid baudrate!\n");
        return -1;
    }

    rs485_drv_init();
    if (rs485_drv_set_baudrate(baud) == RT_EOK)
    {
        rt_kprintf("RS485 baudrate set to %u bps successfully.\n", (unsigned int)baud);
        return 0;
    }
    else
    {
        rt_kprintf("Failed to set RS485 baudrate!\n");
        return -1;
    }
}
MSH_CMD_EXPORT(rs485_baud, Configure RS485 UART baudrate);

/* 2026-09-16: 从站开机自启 —— 本板在本系统中固定扮演 Slave 0x01，被 D12x
 * 主站轮询。以前每次上电/重烧后都要手敲 rs485_slave start 1，演示日是
 * 隐患（拔电重启就"从站失踪"）。如需临时当主站用：先 `rs485_slave stop`
 * 再 rs485_master start（主/从互斥检查本来就在 rs485_*_start 里）。 */
static int rs485_slave_autostart(void)
{
    rs485_drv_init();
    return rs485_slave_start(RS485_DEFAULT_SLAVE_ADDR) == RT_EOK ? 0 : -1;
}
INIT_APP_EXPORT(rs485_slave_autostart);
