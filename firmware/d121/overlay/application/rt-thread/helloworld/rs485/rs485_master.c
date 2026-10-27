/*
 * D12x 侧 RS485 主站（uart3 / 板载 U11 收发器）
 *
 * 硬件前提（已在 target/d12x/demo68-nor 里配好，应用层不要重复设置）：
 *   - pinmux: PB10/PB11 = function 5  →  UART3_TX / UART3_RX
 *   - CONFIG_AIC_DEV_UART3_MODE = 2   →  USART_FUNC_RS485_COMACT_IO
 *     驱动在 aic_drv_uart.c 的 uart_dev_paras[] 里把该编译期常量直接填进
 *     serial_configure.function，open 时即生效，DE/RE 由硬件换向。
 *
 * ⚠ 因此这里绝对不要用 RT_SERIAL_CONFIG_DEFAULT 去 rt_device_control()：
 *   那个宏会把 function 字段清成 0(RS232)，等于把 compact-io 换向关掉，
 *   现象是"突然又发不出数据了"。波特率等参数改 defconfig，不改代码。
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <aic_core.h>

#include "rs485_proto.h"
#include "modbus_rtu.h"

#define RS485_DEV_NAME      "uart3"
#define RS485_POLL_STACK    4096
#define RS485_POLL_PRIO     18      /* 高于 LVGL/tshell 的 20，避免同优先级时间片轮转抬高往返时延 */
#define RS485_RX_CHUNK      64
#define RS485_DEF_TIMEOUT   200      /* ms */
#define RS485_DEF_INTERVAL  1000     /* ms */

/* ---------------- 协议实现 ---------------- */

rt_uint16_t rs485_crc16(const rt_uint8_t *data, rt_size_t len)
{
    rt_uint16_t crc = 0xFFFF;

    for (rt_size_t i = 0; i < len; i++)
    {
        crc ^= (rt_uint16_t)data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x0001) ? (rt_uint16_t)((crc >> 1) ^ 0xA001) : (rt_uint16_t)(crc >> 1);
    }

    return crc;
}

rt_size_t rs485_pack(rt_uint8_t *out, rt_uint8_t addr, rt_uint8_t cmd,
                     const rt_uint8_t *payload, rt_uint8_t payload_len)
{
    rt_uint8_t scratch[3 + RS485_PAYLOAD_MAX];
    rt_uint16_t crc;
    rt_size_t idx = 0;

    if (!out || payload_len > RS485_PAYLOAD_MAX || (payload_len && !payload))
        return 0;

    scratch[0] = addr;
    scratch[1] = cmd;
    scratch[2] = payload_len;
    if (payload && payload_len)
        memcpy(&scratch[3], payload, payload_len);

    crc = rs485_crc16(scratch, 3 + payload_len);

    out[idx++] = RS485_HDR0;
    out[idx++] = RS485_HDR1;
    out[idx++] = addr;
    out[idx++] = cmd;
    out[idx++] = payload_len;
    if (payload && payload_len)
    {
        memcpy(&out[idx], payload, payload_len);
        idx += payload_len;
    }
    out[idx++] = (rt_uint8_t)(crc & 0xFF);
    out[idx++] = (rt_uint8_t)(crc >> 8);
    out[idx++] = RS485_TAIL0;
    out[idx++] = RS485_TAIL1;

    return idx;
}

/* ---------------- 接收状态机 ---------------- */

struct rx_ctx { rt_uint8_t bytes[DMB_ADU_MAX]; unsigned used; };
struct response {
    rt_uint32_t rtt_ms;
    rt_uint8_t addr, cmd, len;
    rt_uint8_t payload[250];
    rt_uint8_t wire[DMB_ADU_MAX];
    unsigned wire_len;
};

struct stats
{
    rt_uint32_t ok;
    rt_uint32_t timeout;
    rt_uint32_t crc_err;
    rt_uint32_t frame_err;      /* 帧头/帧尾/长度非法，即垃圾字节被丢弃的次数 */
    rt_uint32_t bytes_rx;
    rt_uint32_t last_ms;        /* 最近一次成功事务的往返时间 */
    rt_uint32_t max_ms;
    rt_uint32_t tx_err, open_err, busy, drain_limit;
};

static struct rx_ctx   g_rx;
static struct stats    g_st;
void rs485_bus_stats(rs485_bus_stats_t *out)
{
    if (!out) return;
    rt_enter_critical();
    out->ok = g_st.ok; out->timeouts = g_st.timeout; out->crc_errors = g_st.crc_err;
    rt_exit_critical();
}
static rt_device_t     g_dev;
static struct rt_semaphore g_rx_sem;
static struct rt_mutex g_bus_mutex;   /* 序列化所有事务：轮询线程与黑匣子可并发调用 */
static rt_bool_t       g_ready = RT_FALSE;
static rt_bool_t       g_ipc_ready;

static int rs485_ipc_init(void)
{
    int result = RT_EOK;
    rt_enter_critical();
    if (!g_ipc_ready) {
        result = rt_sem_init(&g_rx_sem, "rs485rx", 0, RT_IPC_FLAG_FIFO);
        if (result == RT_EOK) {
            result = rt_mutex_init(&g_bus_mutex, "rs485bus", RT_IPC_FLAG_PRIO);
            if (result != RT_EOK) rt_sem_detach(&g_rx_sem);
            else g_ipc_ready = RT_TRUE;
        }
    }
    rt_exit_critical();
    return result;
}

static void rx_reset(void) { memset(&g_rx, 0, sizeof(g_rx)); }
static int rx_feed(rt_uint8_t byte, struct response *out)
{
    if (g_rx.used == sizeof(g_rx.bytes)) {
        memmove(g_rx.bytes, g_rx.bytes + 1, --g_rx.used); g_st.frame_err++;
    }
    g_rx.bytes[g_rx.used++] = byte;
    while (g_rx.used >= 2) {
        const rt_uint8_t *b = g_rx.bytes; unsigned length = 0;
        if (b[0] && b[0] <= 247) {
            if (b[1] & 0x80) length = 5;
            else if (b[1] == 6) length = 8;
            else if ((b[1] == 3 || b[1] == 4) && g_rx.used >= 3 && b[2] <= 250 && !(b[2] & 1)) length = b[2] + 5;
            else if ((b[1] == 3 || b[1] == 4) && g_rx.used < 3) return 0;
        }
        if (length && g_rx.used < length) return 0;
        if (length && dmb_valid(b, length)) {
            memcpy(out->wire, b, length); out->wire_len = length;
            out->addr = b[0]; out->cmd = b[1];
            out->len = b[1] & 0x80 ? 1 : b[1] == 6 ? 4 : b[2];
            memcpy(out->payload, b + (((b[1] & 0x80) || b[1] == 6) ? 2 : 3), out->len);
            g_rx.used -= length; memmove(g_rx.bytes, g_rx.bytes + length, g_rx.used); return 1;
        }
        if (length) g_st.crc_err++; else g_st.frame_err++;
        memmove(g_rx.bytes, g_rx.bytes + 1, --g_rx.used);
    }
    return 0;
}

static rt_err_t rx_indicate(rt_device_t dev, rt_size_t size)
{
    rt_sem_release(&g_rx_sem);
    return RT_EOK;
}

static rt_err_t rs485_open(void)
{
    if (g_ready)
        return RT_EOK;

    g_dev = rt_device_find(RS485_DEV_NAME);
    if (!g_dev)
    {
        rt_kprintf("[rs485] %s not found, check AIC_USING_UART3\n", RS485_DEV_NAME);
        return -RT_ERROR;
    }

    struct serial_configure config;
    if (rt_device_control(g_dev, RT_SERIAL_GET_CONFIG, &config) != RT_EOK) return -RT_ERROR;
    /* Preserve compact-io direction and every board-specific field. */
    config.baud_rate = 115200; config.data_bits = DATA_BITS_8;
    config.parity = PARITY_NONE; config.stop_bits = STOP_BITS_2;
    if (rt_device_control(g_dev, RT_DEVICE_CTRL_CONFIG, &config) != RT_EOK) return -RT_ERROR;
    rx_reset();

    if (rt_device_open(g_dev, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        rt_kprintf("[rs485] open %s failed\n", RS485_DEV_NAME);
        g_dev = RT_NULL;
        return -RT_ERROR;
    }

    rt_device_set_rx_indicate(g_dev, rx_indicate);
    g_ready = RT_TRUE;

    return RT_EOK;
}

/*
 * 发一帧并等对应 ACK。总线上若有空闲噪声字节（端接/偏置不匹配时的典型现象），
 * 会被状态机直接丢掉，因此这里不需要额外清缓冲。
 */
static rt_err_t rs485_raw_transact(const rt_uint8_t *tx, rt_size_t tx_len,
                                  struct response *resp, rt_uint32_t timeout_ms)
{
    rt_uint8_t buf[RS485_RX_CHUNK];
    rt_size_t n;
    rt_tick_t t0 = rt_tick_get(), budget;
    int result = -RT_ETIMEOUT;
    if (!resp || !tx || tx_len < 4 || tx_len > DMB_ADU_MAX || tx[0] > 247 ||
        !timeout_ms || timeout_ms > 60000) return -RT_EINVAL;
    rt_uint8_t addr = tx[0], cmd = tx[1];
    memset(resp, 0, sizeof(*resp));
    budget = rt_tick_from_millisecond(timeout_ms);
    if (rs485_ipc_init() != RT_EOK) return -RT_ERROR;

    /* 半双工总线：一次只允许一笔事务(含等应答)，多线程调用安全 */
    if (rt_mutex_take(&g_bus_mutex, budget) != RT_EOK)
    {
        g_st.busy++;
        return -RT_EBUSY;
    }
    /* Opening and parser state are protected by the same bus mutex as transactions. */
    if (rs485_open() != RT_EOK) { g_st.open_err++; result = -RT_ERROR; goto done; }

    rx_reset();
    /* 丢掉上一笔事务残留的半帧 */
    unsigned drained = 0;
    while ((n = rt_device_read(g_dev, -1, buf, sizeof(buf))) > 0) {
        drained += n;
        if (drained >= 512 || (rt_tick_t)(rt_tick_get() - t0) >= budget) {
            g_st.drain_limit++; result = -RT_EBUSY; goto done;
        }
    }
    rt_sem_control(&g_rx_sem, RT_IPC_CMD_RESET, RT_NULL);
    if ((rt_tick_t)(rt_tick_get() - t0) >= budget) goto timed_out;

    /* Never transmit into the tail of the previous RTU frame. */
    rt_thread_mdelay(2);
    if ((rt_tick_t)(rt_tick_get() - t0) >= budget) goto timed_out;
    if (rt_device_write(g_dev, 0, tx, tx_len) != tx_len)
    {
        g_st.tx_err++; result = -RT_ERROR; goto done;
    }
    if (!addr) { rt_thread_mdelay(4); result = RT_EOK; goto done; }

    while (1)
    {
        if ((rt_tick_t)(rt_tick_get() - t0) >= budget) break;
        n = rt_device_read(g_dev, -1, buf, sizeof(buf));
        if (n > 0)
        {
            g_st.bytes_rx += n;
            for (rt_size_t i = 0; i < n; i++)
            {
                if (!rx_feed(buf[i], resp))
                    continue;
                if (resp->addr != addr || (resp->cmd != cmd && resp->cmd != (cmd | 0x80)))
                    continue;   /* 不是回给我的，继续找 */

                {
                    rt_uint32_t ms = (rt_uint64_t)(rt_tick_get() - t0) * 1000 / RT_TICK_PER_SECOND;
                    resp->rtt_ms = ms;
                    g_st.ok++;
                    g_st.last_ms = ms;
                    if (ms > g_st.max_ms) g_st.max_ms = ms;
                    result = RT_EOK; goto done;
                }
            }
        }
        else {
            rt_tick_t elapsed = rt_tick_get() - t0;
            if (elapsed >= budget || rt_sem_take(&g_rx_sem, budget - elapsed) != RT_EOK) break;
        }
    }

timed_out:
    g_st.timeout++;
done:
    rt_mutex_release(&g_bus_mutex);
    return result;
}
static rt_err_t rs485_transact(rt_uint8_t addr, rt_uint8_t cmd,
                               const rt_uint8_t *payload, rt_uint8_t plen,
                               struct response *resp, rt_uint32_t timeout_ms)
{
    if (!addr || addr > 247 || !payload || plen != 4) return -RT_EINVAL;
    rt_uint8_t tx[8]; rt_size_t n = dmb_request(tx, addr, cmd, dmb_get16(payload), dmb_get16(payload + 2));
    if (!n) return -RT_EINVAL;
    int result = rs485_raw_transact(tx, n, resp, timeout_ms);
    if (!result && (resp->cmd & 0x80)) { g_st.frame_err++; return -RT_ERROR; }
    return result;
}

/* 最近一次成功事务的往返时间(ms) */
rt_uint32_t rs485_last_rtt_ms(void)
{
    return g_st.last_ms;
}

/* ---------------- 上层接口（后续采集线程直接调） ---------------- */

rt_err_t rs485_query(rt_uint8_t addr, rs485_telemetry_t *tele, rt_uint32_t timeout_ms)
{
    return rs485_query_ex(addr, tele, timeout_ms, RT_NULL);
}
rt_err_t rs485_query_ex(rt_uint8_t addr, rs485_telemetry_t *tele, rt_uint32_t timeout_ms, rt_uint32_t *rtt_ms)
{
    struct response response; const rt_uint8_t request[] = {0, 0, 0, DMB_STAR_REGS};
    if (!tele) return -RT_EINVAL;
    memset(tele, 0, sizeof(*tele)); if (rtt_ms) *rtt_ms = 0;
    int result = rs485_transact(addr, 4, request, sizeof(request), &response, timeout_ms);
    if (result) return result;
    const rt_uint8_t *p = response.payload;
    if (response.len != DMB_STAR_REGS * 2 || dmb_get16(p) != DMB_MAP_VERSION ||
        dmb_get16(p + 2) != 1 || dmb_get16(p + 4) != addr ||
        (dmb_get16(p + 6) & ~3) || dmb_get16(p + 22) > 10000 || dmb_get16(p + 24) > 1) {
        g_st.frame_err++; return -RT_ERROR;
    }
    tele->uptime_s = dmb_get32(p + 12); tele->quality = dmb_get16(p + 6);
    tele->quality_tag = RS485_QUALITY_TAG;
    tele->temp_centi = (rt_int16_t)dmb_get16(p + 20); tele->humi_centi = dmb_get16(p + 22);
    tele->led_state = dmb_get16(p + 24);
    tele->ax = (rt_int16_t)dmb_get16(p + 26); tele->ay = (rt_int16_t)dmb_get16(p + 28);
    tele->az = (rt_int16_t)dmb_get16(p + 30); tele->gx = (rt_int16_t)dmb_get16(p + 32);
    tele->gy = (rt_int16_t)dmb_get16(p + 34); tele->gz = (rt_int16_t)dmb_get16(p + 36);
    if (rtt_ms) *rtt_ms = response.rtt_ms;
    return RT_EOK;
}
rt_err_t rs485_query_system(rt_uint8_t addr, rs485_system_t *out, rt_uint32_t timeout_ms, rt_uint32_t *rtt_ms)
{
    struct response response; const rt_uint8_t request[] = {0, 0, 0, DMB_SYSTEM_REGS};
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out)); if (rtt_ms) *rtt_ms = 0;
    int result = rs485_transact(addr, 4, request, sizeof(request), &response, timeout_ms);
    if (result) return result;
    const rt_uint8_t *p = response.payload;
    if (response.len != DMB_SYSTEM_REGS * 2 || dmb_get16(p) != DMB_MAP_VERSION ||
        dmb_get16(p + 2) != 2 || dmb_get16(p + 4) != addr || (dmb_get16(p + 6) & ~15) ||
        ((dmb_get16(p + 6) & RS485_SYS_CPU) && dmb_get16(p + 20) > 10000) ||
        ((dmb_get16(p + 6) & RS485_SYS_MEMORY) && (!dmb_get32(p + 24) || dmb_get32(p + 28) > dmb_get32(p + 24)))) {
        g_st.frame_err++; return -RT_ERROR;
    }
    out->valid = dmb_get16(p + 6); out->sequence = dmb_get32(p + 8);
    out->uptime_s = dmb_get32(p + 12); out->age_ms = dmb_get32(p + 16);
    out->cpu_centi = dmb_get16(p + 20); out->load_milli = dmb_get16(p + 22);
    out->mem_total_kib = dmb_get32(p + 24); out->mem_available_kib = dmb_get32(p + 28);
    out->process_uptime_s = dmb_get32(p + 32); out->requests = dmb_get32(p + 36);
    out->responses = dmb_get32(p + 40); out->crc_errors = dmb_get32(p + 44);
    out->frame_errors = dmb_get32(p + 48); out->tx_errors = dmb_get32(p + 52);
    out->rx_bytes = dmb_get32(p + 56); out->tx_bytes = dmb_get32(p + 60); out->rejected = dmb_get32(p + 64);
    if (rtt_ms) *rtt_ms = response.rtt_ms;
    return RT_EOK;
}
rt_err_t rs485_set_led(rt_uint8_t addr, rt_uint8_t on, rt_uint32_t timeout_ms)
{
    if (on > 1) return -RT_EINVAL;
    rt_uint8_t request[] = {0, 0, 0, on}; struct response response;
    int result = rs485_transact(addr, 6, request, sizeof(request), &response, timeout_ms);
    if (!result && (response.len != sizeof(request) || memcmp(response.payload, request, sizeof(request)))) {
        g_st.frame_err++; return -RT_ERROR;
    }
    return result;
}

static void print_telemetry(rt_uint8_t addr, const rs485_telemetry_t *t)
{
    int temp = t->temp_centi;
    int abs_temp = (temp < 0) ? -temp : temp;

    rt_kprintf("[rs485] slave 0x%02X  temp %s%d.%02d C  humi %d.%02d %%  up %us  led %s  rtt %ums\n",
               addr, (temp < 0) ? "-" : "", abs_temp / 100, abs_temp % 100,
               t->humi_centi / 100, t->humi_centi % 100,
               (unsigned)t->uptime_s, t->led_state ? "ON" : "OFF", g_st.last_ms);
    rt_kprintf("        accel %6d %6d %6d   gyro %6d %6d %6d\n",
               t->ax, t->ay, t->az, t->gx, t->gy, t->gz);
}

/* ---------------- 周期轮询线程 ---------------- */

static rt_thread_t   g_poller = RT_NULL;
static rt_uint8_t    g_poll_addr = RS485_ADDR_DEFAULT;
static rt_uint32_t   g_poll_ms = RS485_DEF_INTERVAL;
static volatile rt_bool_t g_poll_run = RT_FALSE;
static volatile rt_bool_t g_poll_starting;

static void poller_entry(void *arg)
{
    rs485_telemetry_t tele;

    while (g_poll_run)
    {
        if (rs485_query(g_poll_addr, &tele, RS485_DEF_TIMEOUT) == RT_EOK)
            print_telemetry(g_poll_addr, &tele);
        else
        {
            rt_kprintf("[rs485] slave 0x%02X TIMEOUT (ok %u / to %u / crc %u / junk %u)\n",
                       g_poll_addr, g_st.ok, g_st.timeout, g_st.crc_err, g_st.frame_err);
        }

        for (rt_uint32_t slept = 0; g_poll_run && slept < g_poll_ms;) {
            rt_uint32_t step = g_poll_ms - slept > 20 ? 20 : g_poll_ms - slept;
            rt_thread_mdelay(step); slept += step;
        }
    }

    rt_enter_critical(); g_poller = RT_NULL; rt_exit_critical();
    rt_thread_delete(rt_thread_self());
}
int rs485_poller_stop(void)
{
    if (g_poll_starting) return -RT_EBUSY;
    g_poll_run = RT_FALSE;
    rt_tick_t started = rt_tick_get();
    while (g_poller) {
        if ((rt_tick_t)(rt_tick_get() - started) >= rt_tick_from_millisecond(2000)) return -RT_ETIMEOUT;
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}
static int parse_number(const char *s, unsigned min, unsigned max, unsigned *out)
{
    if (!s || !*s || *s == '-' || *s == '+') return -RT_EINVAL;
    char *end; unsigned long value = strtoul(s, &end, 0);
    if (*end || value < min || value > max) return -RT_EINVAL;
    *out = value; return RT_EOK;
}

/* ---------------- msh 命令 ---------------- */

static void usage(void)
{
    rt_kprintf("Modbus RTU master on %s (115200/8N2, hardware direction control)\n", RS485_DEV_NAME);
    rt_kprintf("  mbq query [addr]            单次查询遥测\n");
    rt_kprintf("  mbq led   <addr> <0|1>      远程点从站 LED\n");
    rt_kprintf("  mbq start [addr] [ms]       启动周期轮询 (默认 %d ms)\n", RS485_DEF_INTERVAL);
    rt_kprintf("  mbq stop                    停止周期轮询\n");
    rt_kprintf("  mbq stat [reset]            统计 / 清零\n");
}

int mbq(int argc, char **argv)
{
    rt_uint8_t addr = g_poll_addr;
    rs485_telemetry_t tele;

    if (argc == 3 && !strcmp(argv[1], "raw")) {
        size_t count = strlen(argv[2]); rt_uint8_t request[32]; struct response response;
        if (count < 8 || count > sizeof(request) * 2 || count % 2) return -RT_EINVAL;
        for (size_t i = 0; i < count / 2; i++) {
            char byte[3] = {argv[2][i * 2], argv[2][i * 2 + 1], 0}, *end;
            if (strspn(byte, "0123456789abcdefABCDEF") != 2) return -RT_EINVAL;
            request[i] = strtoul(byte, &end, 16);
        }
        int rc = rs485_raw_transact(request, count / 2, &response, 500);
        rt_kprintf("RTU_RAW rc=%d bytes=", rc);
        if (!rc) for (unsigned i = 0; i < response.wire_len; i++) rt_kprintf("%02X", response.wire[i]);
        rt_kprintf("\r\n"); return rc;
    }

    if (argc < 2)
    {
        usage();
        return 0;
    }

    if (!strcmp(argv[1], "query"))
    {
        unsigned parsed = addr;
        if (argc >= 3 && parse_number(argv[2], 1, 254, &parsed)) return -RT_EINVAL;
        addr = parsed;

        if (rs485_query(addr, &tele, RS485_DEF_TIMEOUT) == RT_EOK)
            print_telemetry(addr, &tele);
        else
            rt_kprintf("[rs485] slave 0x%02X no response (to %u crc %u junk %u)\n",
                       addr, g_st.timeout, g_st.crc_err, g_st.frame_err);
        return 0;
    }

    if (!strcmp(argv[1], "system")) {
        unsigned parsed = 2; rs485_system_t sys; rt_uint32_t rtt;
        if (argc > 3 || (argc == 3 && parse_number(argv[2], 1, 254, &parsed))) return -RT_EINVAL;
        int rc = rs485_query_system(parsed, &sys, RS485_DEF_TIMEOUT, &rtt);
        if (rc) { rt_kprintf("[system] addr=%u rc=%d\n", parsed, rc); return rc; }
        rt_kprintf("[system] addr=%u valid=%u seq=%u age=%ums cpu=%u.%02u%% load=%u.%03u up=%us rtt=%ums\n",
            parsed, (unsigned)sys.valid, (unsigned)sys.sequence, (unsigned)sys.age_ms, (unsigned)sys.cpu_centi / 100, (unsigned)sys.cpu_centi % 100,
            (unsigned)sys.load_milli / 1000, (unsigned)sys.load_milli % 1000, (unsigned)sys.uptime_s, rtt);
        rt_kprintf("memory total=%u available=%u KiB | requests=%u responses=%u crc=%u frame=%u tx_err=%u\n",
            (unsigned)sys.mem_total_kib, (unsigned)sys.mem_available_kib, (unsigned)sys.requests, (unsigned)sys.responses,
            (unsigned)sys.crc_errors, (unsigned)sys.frame_errors, (unsigned)sys.tx_errors);
        return RT_EOK;
    }

    if (!strcmp(argv[1], "led"))
    {
        if (argc < 4) { rt_kprintf("usage: mbq led <addr> <0|1>\n"); return -1; }
        unsigned parsed, on;
        if (parse_number(argv[2], 1, 254, &parsed) || parse_number(argv[3], 0, 1, &on)) return -RT_EINVAL;
        return rs485_set_led(parsed, on, RS485_DEF_TIMEOUT);
    }

    if (!strcmp(argv[1], "start"))
    {
        unsigned parsed_addr = addr, parsed_ms = g_poll_ms;
        if ((argc >= 3 && parse_number(argv[2], 1, 254, &parsed_addr)) ||
            (argc >= 4 && parse_number(argv[3], 100, 3600000, &parsed_ms))) return -RT_EINVAL;
        rt_enter_critical();
        if (g_poller || g_poll_starting)
        {
            rt_exit_critical();
            rt_kprintf("[rs485] poller already running (mbq stop first)\n");
            return 0;
        }
        g_poll_starting = RT_TRUE; rt_exit_critical();
        g_poll_ms = parsed_ms; g_poll_addr = parsed_addr;
        g_poll_run  = RT_TRUE;
        g_poller = rt_thread_create("rs485p", poller_entry, RT_NULL,
                                    RS485_POLL_STACK, RS485_POLL_PRIO, 10);
        if (!g_poller)
        {
            g_poll_run = RT_FALSE;
            g_poll_starting = RT_FALSE;
            rt_kprintf("[rs485] poller create failed\n");
            return -RT_ENOMEM;
        }
        if (rt_thread_startup(g_poller) != RT_EOK) {
            rt_thread_delete(g_poller); g_poller = RT_NULL;
            g_poll_run = g_poll_starting = RT_FALSE; return -RT_ERROR;
        }
        g_poll_starting = RT_FALSE;
        rt_kprintf("[rs485] polling slave 0x%02X every %u ms\n", g_poll_addr, g_poll_ms);
        return 0;
    }

    if (!strcmp(argv[1], "stop"))
    {
        int result = rs485_poller_stop();
        rt_kprintf("[rs485] poller stop rc=%d\n", result);
        return result;
    }

    if (!strcmp(argv[1], "stat"))
    {
        rt_enter_critical();
        if (argc >= 3 && !strcmp(argv[2], "reset"))
            memset(&g_st, 0, sizeof(g_st));
        struct stats snapshot = g_st;
        rt_exit_critical();

        rt_kprintf("ok %u | timeout %u | crc_err %u | junk_dropped %u | bytes %u | last %ums max %ums\n",
                   snapshot.ok, snapshot.timeout, snapshot.crc_err, snapshot.frame_err,
                   snapshot.bytes_rx, snapshot.last_ms, snapshot.max_ms);
        rt_kprintf("tx_err %u | open_err %u | busy %u | drain_limit %u | poller %d\n",
                   snapshot.tx_err, snapshot.open_err, snapshot.busy, snapshot.drain_limit, !!g_poller);
        return 0;
    }

    usage();
    return 0;
}
MSH_CMD_EXPORT(mbq, RS485 master query on uart3);
