/*
 * Copyright (c) 2022-2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: weilin.peng@artinchip.com
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "blackbox/bb.h"
#include "aic_drv_wdt.h"
#ifdef RT_USING_ULOG
#include <ulog.h>
#endif

static volatile rt_tick_t gui_heartbeat_tick;
static volatile int gui_heartbeat_seen, health_active, health_tripped, health_test_fault;
static rt_uint32_t health_feeds;
void bb_gui_heartbeat(void) { gui_heartbeat_tick = rt_tick_get(); gui_heartbeat_seen = 1; }
void bb_gui_heartbeat_clear(void) { gui_heartbeat_seen = 0; }
static void health_worker(void *arg)
{
    (void)arg;
    rt_device_t device = rt_device_find("wdt");
    rt_uint16_t timeout = 10, clear_threshold = 0;
    if (!device || rt_device_init(device) != RT_EOK ||
        rt_device_control(device, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout) != RT_EOK ||
        rt_device_control(device, RT_DEVICE_CTRL_WDT_SET_CLR_THD, &clear_threshold) != RT_EOK ||
        rt_device_control(device, RT_DEVICE_CTRL_WDT_START, RT_NULL) != RT_EOK) {
        rt_kprintf("[health] watchdog start failed\r\n"); return;
    }
    health_active = 1;
    for (;;) {
        int gui_ok = !gui_heartbeat_seen || (rt_tick_t)(rt_tick_get() - gui_heartbeat_tick) < rt_tick_from_millisecond(10000);
        if (!health_test_fault && gui_ok && bb_health_ok(10000) && !health_tripped) {
            if (rt_device_control(device, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL) == RT_EOK) health_feeds++;
            else health_tripped = 1;
        } else if (!health_tripped) {
            health_tripped = 1;
            rt_kprintf("[health] progress stalled; watchdog will recover system\r\n");
        }
        rt_thread_mdelay(1000);
    }
}
static int cmd_apphealth(int argc, char **argv)
{
    if (argc == 2 && !rt_strcmp(argv[1], "test")) {
        if (!health_active || bb_is_running()) return -RT_EBUSY;
        health_test_fault = 1;
    }
    rt_kprintf("health active=%d feeds=%u tripped=%d gui_age_ms=%u bb_ok=%d\r\n", health_active,
        (unsigned)health_feeds, health_tripped,
        (unsigned)((rt_uint64_t)(rt_tick_get() - gui_heartbeat_tick) * 1000 / RT_TICK_PER_SECOND), bb_health_ok(10000));
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_apphealth, apphealth, terminal progress and watchdog diagnostics);

int main(void)
{
#ifdef ULOG_USING_FILTER
    ulog_global_filter_lvl_set(ULOG_OUTPUT_LVL);
#endif
    rt_thread_t worker = rt_thread_create("health", health_worker, RT_NULL, 4096, 10, 10);
    if (!worker) return -RT_ENOMEM;
    if (rt_thread_startup(worker) != RT_EOK) { rt_thread_delete(worker); return -RT_ERROR; }
    return RT_EOK;
}
