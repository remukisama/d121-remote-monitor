/*
 * blackbox: 采集线程(10Hz) + 事件判定 + 三档后端落盘 + PSRAM 环形缓冲
 *
 * 数据源:
 *   - RS485 从站遥测: 每 BB_SAMPLE_MS 调 rs485_query()。总线 mutex 已在
 *     rs485_master.c 内序列化，bb 与 mbq 轮询可同时跑。
 *   - 片内结温: "temp_tsen_cpu"，每 10 个采样周期读一次(1Hz)。
 *
 * 落盘后端启动时按序探测，运行时写入失败则向下一档降级:
 *   1. /sdcard/blackbox/ —— 若 /sdcard 根目录存在 bootcfg.txt 判定为升级卡，跳过
 *   2. /data/blackbox/   —— NOR littlefs 兜底(文件写满 200KB 后原地截断)
 *   3. 仅环形缓冲        —— 无介质，事件只进 RAM
 *
 * msh: bb start [addr] / bb stop / bbstat / bb replay [n]
 *
 * Copyright (c) 2026   SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <rtthread.h>
#include <rtdevice.h>
#include "sensor.h"
#include <aic_core.h>

#include "bb.h"
#include "bb_system.h"
#include "bb_store.h"
#include "rs485_proto.h"
#ifndef BB_STORE_HOST
#include <dfs_fs.h>
#endif

#define DBG_TAG "bb"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BB_THREAD_PRIO     19    /* 低于 rs485 轮询(18)，高于 LVGL/tshell(20) */
#define BB_THREAD_STACK    16384 /* Same filesystem call chain as the GUI startup worker. */
#define BB_TSEN_NAME       "temp_tsen_cpu"
#define BB_TSEN_EVERY      10    /* 10Hz * 10 = 1Hz */

#define BB_SD_DIR          BB_HISTORY_ROOT
#define BB_SD_PROBE        "/sdcard/bootcfg.txt"
#define BB_NOR_DIR         "/data/blackbox"
#define BB_FRAME_NAME      "bb_frame.log"
#define BB_EVENT_NAME      "bb_event.log"
#define BB_NOR_MAX         (200 * 1024)
#define BB_NOR_EVENT_MAX   (32 * 1024)

/* 结温告警阈值，0.1 摄氏度，带迟滞 */
#define BB_CHIP_HOT_ON     850
#define BB_CHIP_HOT_OFF    800
/* 连续超时次数达到即判 OFFLINE */
#define BB_LOST_AFTER      3

struct bb_rt {
    rt_thread_t   thread;
    volatile rt_bool_t running;
    rt_uint8_t    addr;
    rt_device_t   tsen;
    /* 落盘 */
    char          dir[32];
    int           fd_frame;
    int           fd_event;
    rt_uint32_t   frame_bytes;
    rt_uint32_t   event_bytes;
    rt_uint32_t   session_id;
    rt_uint32_t   run_id;
    rt_bool_t     storage_fault;
    rt_bool_t     nor_mode;       /* 需要截断策略 */
    /* 环形缓冲 */
    bb_frame_t   *ring;
    rt_uint32_t   ring_head;      /* 下一个写入位置 */
    rt_uint32_t   ring_cnt;
    /* 状态机 */
    rt_uint32_t   timeout_streak;
    rt_bool_t     slave_online;
    rt_bool_t     chip_hot;
    rt_bool_t     vib_hi;
    rt_uint32_t   evt_count;
    rt_uint32_t   frame_count;
    rt_uint32_t   write_errors;
    rt_uint32_t   written_frames;
    rt_uint32_t   written_events, dropped_events;
    rt_uint32_t   dropped_frames;
    rt_uint32_t   backend_switches;
    bb_event_t    events[BB_EVENT_RING_LEN];
    rt_uint32_t   event_head;
    rt_uint32_t   event_count;
};

static struct bb_rt g_bb;
static rt_uint32_t collection_generation;
static volatile rt_bool_t g_bb_starting;
static volatile rt_tick_t g_bb_progress_tick, g_bb_transition_tick;
static rt_tick_t bb_clock_origin;
static rt_bool_t bb_clock_ready;
static int bb_clock_init(void)
{
    bb_clock_origin = rt_tick_get(); bb_clock_ready = RT_TRUE;
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(bb_clock_init);
rt_uint32_t bb_now_ms(void)
{
    rt_enter_critical();
    if (!bb_clock_ready) bb_clock_init();
    rt_tick_t elapsed = rt_tick_get() - bb_clock_origin;
    rt_exit_critical();
    return (rt_uint32_t)((rt_uint64_t)elapsed * 1000 / RT_TICK_PER_SECOND);
}

/* ---------------- 后端 ---------------- */

/* RT-Thread 这套头里没有 F_OK/RT_ENOENT，用 open 探测存在性 */
static int bb_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int bb_try_dir(const char *dir, rt_bool_t nor)
{
    char path[64];
#ifndef BB_STORE_HOST
    const char *mount = nor ? "/data" : "/sdcard";
    struct dfs_filesystem *fs = dfs_filesystem_lookup(mount);
    if (!fs || !fs->path || rt_strcmp(fs->path, mount)) return -RT_ERROR;
#endif

    /* 升级卡保护: TF 根目录有 bootcfg.txt 说明这张卡是刷机的，不往里写 */
    if (!nor && bb_path_exists(BB_SD_PROBE))
    {
        LOG_I("upgrade card detected (%s present), skip sdcard", BB_SD_PROBE);
        return -RT_ERROR;
    }
    if (!nor)
    {
        bb_disk_files_t files;
        int result = bb_store_new(dir, g_bb.run_id, g_bb.addr, &files);
        if (result != RT_EOK) return result;
        g_bb.fd_frame = files.frame_fd; g_bb.fd_event = files.event_fd;
        g_bb.frame_bytes = files.frame_bytes; g_bb.event_bytes = files.event_bytes;
        g_bb.session_id = files.id; g_bb.run_id = files.run_id;
        g_bb.write_errors += files.prune_errors;
        if (files.prune_errors) g_bb.storage_fault = RT_TRUE;
        g_bb.nor_mode = RT_FALSE;
        rt_strncpy(g_bb.dir, dir, sizeof(g_bb.dir) - 1);
        return RT_EOK;
    }

    mkdir(dir, 0755);   /* 已存在返回错，忽略 */

    rt_snprintf(path, sizeof(path), "%s/" BB_FRAME_NAME, dir);
    int fd_f = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd_f < 0)
        return -RT_ERROR;
    rt_snprintf(path, sizeof(path), "%s/" BB_EVENT_NAME, dir);
    int fd_e = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd_e < 0)
    {
        close(fd_f);
        return -RT_ERROR;
    }
    off_t event_end = lseek(fd_e, 0, SEEK_END);
    off_t frame_end = lseek(fd_f, 0, SEEK_END);
    if (event_end < 0 || frame_end < 0) { close(fd_f); close(fd_e); return -RT_ERROR; }

    g_bb.fd_frame = fd_f;
    g_bb.fd_event = fd_e;
    /* 游标定位到文件末尾(追加不覆盖旧数据), frame_bytes 从现状起计 */
    g_bb.frame_bytes = frame_end;
    g_bb.nor_mode = nor;
    g_bb.event_bytes = event_end;
    g_bb.session_id = 0;
    rt_strncpy(g_bb.dir, dir, sizeof(g_bb.dir) - 1);
    return RT_EOK;
}

static void bb_backend_init(void)
{
    g_bb.fd_frame = -1;
    g_bb.fd_event = -1;
    g_bb.dir[0] = '\0';

    /* Only write to an actual mounted medium; a leftover directory is insufficient. */
    if (bb_try_dir(BB_SD_DIR, RT_FALSE) == RT_EOK)
        return;
    if (bb_try_dir(BB_NOR_DIR, RT_TRUE) == RT_EOK)
        return;
    LOG_W("no writable storage, ring buffer only");
}

static int bb_write_line(int fd, const char *buf, rt_size_t len, rt_bool_t urgent)
{
    if (fd < 0 || g_bb.storage_fault)
        return -RT_ERROR;

    if (g_bb.nor_mode && fd == g_bb.fd_frame && g_bb.frame_bytes + len > BB_NOR_MAX)
    {
        /* NOR 兜底空间小: 到上限就从头重写(环形覆盖)，保住最近的数据 */
        char hdr[48];
        if (lseek(fd, 0, SEEK_SET) < 0)
            goto failed;
        int hn = rt_snprintf(hdr, sizeof(hdr), "# wrap at frame %u\r\n", (unsigned)g_bb.frame_count);
        if (write(fd, hdr, hn) != hn || ftruncate(fd, hn) < 0)
            goto failed;
        g_bb.frame_bytes = hn;
    }
    if (g_bb.nor_mode && fd == g_bb.fd_event && g_bb.event_bytes + len > BB_NOR_EVENT_MAX)
    {
        char hdr[48];
        int hn = rt_snprintf(hdr, sizeof(hdr), "# wrap at event %u\r\n", (unsigned)g_bb.evt_count);
        if (lseek(fd, 0, SEEK_SET) < 0 || write(fd, hdr, hn) != hn || ftruncate(fd, hn) < 0)
            goto failed;
        g_bb.event_bytes = hn;
    }
    rt_size_t done = 0;
    while (done < len) {
        int written = write(fd, buf + done, len - done);
        if (written <= 0)
            goto failed;
        done += written;
    }
    if (fd == g_bb.fd_frame)
        g_bb.frame_bytes += done;
    else
        g_bb.event_bytes += done;

    /* FatFs/littlefs 的 write 只进缓冲，目录里的文件大小要到 flush/close 才更新，
     * 定期同步减少掉电损失；仍须检查返回值，不承诺介质掉电零损失。
     * NOR 档按 1Hz 批量 sync 减少 flash GC；TF 档 f_sync 实测开销可忽略；
     * 事件行永远立即 sync（它是取证主体）。 */
    if (urgent || !g_bb.nor_mode || (g_bb.frame_count % 10 == 0))
        if (fsync(fd) < 0)
            goto failed;
    return RT_EOK;
failed:
    g_bb.write_errors++;
    g_bb.storage_fault = RT_TRUE;
    return -RT_ERROR;
}

static void bb_close_storage(void)
{
    if (g_bb.fd_frame >= 0) {
        if (close(g_bb.fd_frame) < 0) g_bb.write_errors++;
        g_bb.fd_frame = -1;
    }
    if (g_bb.fd_event >= 0) {
        if (close(g_bb.fd_event) < 0) g_bb.write_errors++;
        g_bb.fd_event = -1;
    }
}

static void bb_recover_storage(void)
{
    int was_nor = g_bb.nor_mode;
    bb_close_storage();
    g_bb.dir[0] = 0; g_bb.session_id = 0; g_bb.nor_mode = RT_FALSE;
    /* Do not repeatedly probe a failed TF card in the sampling loop. */
    if (!was_nor && bb_try_dir(BB_NOR_DIR, RT_TRUE) == RT_EOK)
        rt_kprintf("[bb] storage degraded to nor\r\n");
    else
        rt_kprintf("[bb] storage degraded to ram\r\n");
    g_bb.storage_fault = RT_FALSE;
    g_bb.backend_switches++;
}

static void bb_roll_session(void)
{
    if (!g_bb.session_id || g_bb.storage_fault ||
        (g_bb.frame_bytes < BB_STORE_FRAME_MAX - 128 && g_bb.event_bytes < BB_STORE_EVENT_MAX - 256)) return;
    bb_disk_files_t files;
    if (bb_store_new(BB_SD_DIR, g_bb.run_id, g_bb.addr, &files) != RT_EOK) {
        g_bb.storage_fault = RT_TRUE; g_bb.write_errors++; return;
    }
    bb_close_storage();
    g_bb.fd_frame = files.frame_fd; g_bb.fd_event = files.event_fd;
    g_bb.session_id = files.id; g_bb.frame_bytes = files.frame_bytes; g_bb.event_bytes = files.event_bytes;
    g_bb.write_errors += files.prune_errors;
    if (files.prune_errors) g_bb.storage_fault = RT_TRUE;
}

/* ctx = 调用点的"当前帧"。事件判定发生在 bb_push() 之前，所以 SLAVE_ON / VIB_HI
 * 这类事件的数据本来就在 ctx 里，直接用；SLAVE_LOST 时当前帧无数据(全 0)，
 * 退回环形缓冲里最后一次成功帧，保留"掉线前最后一帧"的追溯价值。
 * (2026-09-16 修：此前一律读 ring 最后一帧，导致 SLAVE_ON 行恒写成 slave=0,rtt=0) */
static void bb_event(const char *code, bb_event_type_t type, rt_uint32_t ts, const bb_frame_t *ctx)
{
    char buf[256];
    bb_frame_t last = {0};

    /* 附上最近一帧的上下文，便于事后追溯触发瞬间 */
    if (ctx && (ctx->flags & BB_F_SLAVE_ON))
    {
        last = *ctx;
    }
    else if (g_bb.ring_cnt)
    {
        for (rt_uint32_t i = 0; i < g_bb.ring_cnt; i++) {
            rt_uint32_t idx = (g_bb.ring_head + BB_RING_LEN - 1 - i) % BB_RING_LEN;
            if (g_bb.ring[idx].flags & BB_F_SLAVE_ON) { last = g_bb.ring[idx]; break; }
        }
    }
    if (ctx)
        last.chip_temp = ctx->chip_temp;
    last.flags = (last.flags & (BB_F_SLAVE_ON | BB_F_ENV_INVALID | BB_F_IMU_INVALID | BB_F_QUALITY_KNOWN)) | (g_bb.chip_hot ? BB_F_CHIP_HOT : 0)
               | (g_bb.vib_hi ? BB_F_VIB_HI : 0);

    bb_event_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.id = g_bb.evt_count + 1; saved.ts_ms = ts; saved.type = type; saved.frame = last;
    saved.session_id = g_bb.session_id; saved.run_id = g_bb.run_id; saved.frame_offset = g_bb.frame_bytes;
    saved.disk_source = g_bb.session_id ? 1 : g_bb.nor_mode ? 2 : 0;
    int n = bb_store_event_line(buf, sizeof(buf), &saved);
    if (n <= 0 || n >= sizeof(buf) || bb_write_line(g_bb.fd_event, buf, n, RT_TRUE) != RT_EOK) {
        saved.disk_source = 0;
        g_bb.dropped_events++;
    } else g_bb.written_events++;
    rt_enter_critical();
    bb_event_t *event = &g_bb.events[g_bb.event_head];
    *event = saved;
    ++g_bb.evt_count;
    g_bb.event_head = (g_bb.event_head + 1) % BB_EVENT_RING_LEN;
    if (g_bb.event_count < BB_EVENT_RING_LEN)
        g_bb.event_count++;
    rt_exit_critical();
    rt_kprintf("[bb] EVENT %s @%ums\r\n", code, (unsigned)ts);
}

/* ---------------- 采集 ---------------- */

static rt_int16_t bb_read_chip_temp(void)
{
    struct rt_sensor_data data;

    if (!g_bb.tsen)
        return -999;
    if (rt_device_read(g_bb.tsen, 0, &data, 1) == 1 && data.data.temp >= -32768 && data.data.temp <= 32767)
        return (rt_int16_t)data.data.temp;   /* 0.1 C */
    return -998;
}

static void bb_push(const bb_frame_t *f)
{
    rt_enter_critical();
    g_bb.ring[g_bb.ring_head] = *f;
    g_bb.ring_head = (g_bb.ring_head + 1) % BB_RING_LEN;
    if (g_bb.ring_cnt < BB_RING_LEN)
        g_bb.ring_cnt++;
    rt_exit_critical();
}

static void bb_thread_entry(void *arg)
{
    rt_uint32_t cycle = 0;
    rt_int16_t chip = 0;

    while (g_bb.running)
    {
        bb_roll_session();
        if (g_bb.storage_fault) bb_recover_storage();
        rt_tick_t t0 = rt_tick_get();
        rs485_telemetry_t tele;
        bb_frame_t f;
        rt_bool_t ok;
        rt_uint32_t rtt = 0;

        int query_result = rs485_query_ex(g_bb.addr, &tele, 150, &rtt);
        ok = query_result == RT_EOK;

        if (cycle % BB_TSEN_EVERY == 0)
            chip = bb_read_chip_temp();

        f.ts_ms      = bb_now_ms();
        f.slave_temp = ok ? tele.temp_centi : 0;
        f.slave_humi = ok ? tele.humi_centi : 0;
        f.slave_up   = ok ? tele.uptime_s : 0;
        f.chip_temp  = chip;
        f.rtt_ms     = ok ? (rt_uint16_t)rtt : 0;
        f.ax = ok ? tele.ax : 0;  f.ay = ok ? tele.ay : 0;  f.az = ok ? tele.az : 0;
        f.gx = ok ? tele.gx : 0;  f.gy = ok ? tele.gy : 0;  f.gz = ok ? tele.gz : 0;
        f.flags      = (ok ? BB_F_SLAVE_ON : 0) | (g_bb.chip_hot ? BB_F_CHIP_HOT : 0)
                     | (g_bb.vib_hi ? BB_F_VIB_HI : 0);
        rt_uint16_t quality_flags = ok && tele.quality_tag == RS485_QUALITY_TAG ? BB_F_QUALITY_KNOWN |
            (!(tele.quality & RS485_Q_ENV_VALID) ? BB_F_ENV_INVALID : 0) |
            (!(tele.quality & RS485_Q_IMU_VALID) ? BB_F_IMU_INVALID : 0) : 0;
        f.flags |= quality_flags;

        /* --- 事件判定: 从站上下线 --- */
        if (ok)
        {
            g_bb.timeout_streak = 0;
            if (!g_bb.slave_online)
            {
                g_bb.slave_online = RT_TRUE;
                bb_event("SLAVE_ON", BB_EVT_SLAVE_ON, f.ts_ms, &f);
            }
        }
        else if (query_result == -RT_ETIMEOUT && g_bb.slave_online && ++g_bb.timeout_streak >= BB_LOST_AFTER)
        {
            g_bb.slave_online = RT_FALSE;
            bb_event("SLAVE_LOST", BB_EVT_SLAVE_LOST, f.ts_ms, &f);
        }
        else if (query_result != -RT_ETIMEOUT) g_bb.timeout_streak = 0;

        /* --- 事件判定: 结温越限(迟滞) --- */
        if (chip > -900)
        {
            if (!g_bb.chip_hot && f.chip_temp >= BB_CHIP_HOT_ON)
            {
                g_bb.chip_hot = RT_TRUE;
                bb_event("CHIP_HOT", BB_EVT_CHIP_HOT, f.ts_ms, &f);
            }
            else if (g_bb.chip_hot && f.chip_temp <= BB_CHIP_HOT_OFF)
            {
                g_bb.chip_hot = RT_FALSE;
                bb_event("CHIP_OK", BB_EVT_CHIP_OK, f.ts_ms, &f);
            }
        }

        /* --- 事件判定: 从站振动越限(迟滞, 平方避免开方) ---
         * 只统计 accel(含重力)，静止 ~1g 不触发；敲一下从站板瞬时越线 */
        if (ok && !(quality_flags & BB_F_IMU_INVALID))
        {
            rt_int64_t sq = (rt_int64_t)f.ax * f.ax + (rt_int64_t)f.ay * f.ay +
                            (rt_int64_t)f.az * f.az;
            if (!g_bb.vib_hi && sq >= BB_VIB_ON_SQ)
            {
                g_bb.vib_hi = RT_TRUE;
                bb_event("VIB_HI", BB_EVT_VIB_HI, f.ts_ms, &f);
            }
            else if (g_bb.vib_hi && sq <= BB_VIB_OFF_SQ)
            {
                g_bb.vib_hi = RT_FALSE;
                bb_event("VIB_OK", BB_EVT_VIB_OK, f.ts_ms, &f);
            }
        }

        f.flags = (ok ? BB_F_SLAVE_ON : 0) | (g_bb.chip_hot ? BB_F_CHIP_HOT : 0)
                | (g_bb.vib_hi ? BB_F_VIB_HI : 0) | quality_flags;
        bb_push(&f);
        g_bb.frame_count++;

        /* --- 帧落盘 (CSV, 14 列) --- */
        {
            char buf[128];
            int n = rt_snprintf(buf, sizeof(buf),
                                "%u,%u,%d,%u,%u,%d,%u,%u,%d,%d,%d,%d,%d,%d\r\n",
                                (unsigned)f.ts_ms,
                                (unsigned)!!(f.flags & BB_F_SLAVE_ON),
                                f.slave_temp, (unsigned)f.slave_humi,
                                (unsigned)f.slave_up, f.chip_temp,
                                (unsigned)f.rtt_ms, (unsigned)f.flags,
                                f.ax, f.ay, f.az, f.gx, f.gy, f.gz);
            if (n > 0 && n < sizeof(buf) && bb_write_line(g_bb.fd_frame, buf, n, RT_FALSE) == RT_EOK)
                g_bb.written_frames++;
            else
                g_bb.dropped_frames++;
        }
        if (g_bb.storage_fault) bb_recover_storage();

        if (g_bb.running) bb_system_step(bb_backend_name(), g_bb.session_id, g_bb.run_id);
        g_bb_progress_tick = rt_tick_get();
        cycle++;
        {
            rt_uint32_t used_ms = (rt_uint64_t)(rt_tick_get() - t0) * 1000 / RT_TICK_PER_SECOND;
            rt_thread_mdelay(used_ms < BB_SAMPLE_MS ? BB_SAMPLE_MS - used_ms : 1);
        }
    }

    bb_system_end();
    bb_close_storage();
    if (g_bb.tsen) { rt_device_close(g_bb.tsen); g_bb.tsen = RT_NULL; }
    g_bb.thread = RT_NULL;
    rt_thread_delete(rt_thread_self());
}

int bb_ring_recent(bb_frame_t *out, int n)
{
    if (!out || n <= 0)
        return 0;
    rt_enter_critical();
    if (!g_bb.ring) { rt_exit_critical(); return 0; }
    if ((rt_uint32_t)n > g_bb.ring_cnt)
        n = (int)g_bb.ring_cnt;

    rt_uint32_t head = g_bb.ring_head;
    for (int i = 0; i < n; i++)
    {
        rt_uint32_t idx = (head - n + i + BB_RING_LEN) % BB_RING_LEN;
        out[i] = g_bb.ring[idx];
    }
    rt_exit_critical();
    return n;
}

int bb_events_recent(bb_event_t *out, int n)
{
    if (!out || n <= 0) return 0;
    rt_enter_critical();
    if ((rt_uint32_t)n > g_bb.event_count) n = g_bb.event_count;
    for (int i = 0; i < n; i++)
        out[i] = g_bb.events[(g_bb.event_head + BB_EVENT_RING_LEN - n + i) % BB_EVENT_RING_LEN];
    rt_exit_critical();
    return n;
}

const char *bb_backend_name(void)
{
    if (g_bb.dir[0] == '\0')
        return "ram";
    return g_bb.nor_mode ? "nor" : "sdcard";
}

/* ---------------- 公开控制接口 (2026-09-16) ---------------- */

int bb_is_running(void)
{
    return g_bb.thread ? 1 : 0;
}

int bb_start_addr(rt_uint8_t addr)
{
    if (!addr || addr == RS485_ADDR_BROADCAST) return -RT_EINVAL;
    rt_enter_critical();
    if (g_bb.thread || g_bb_starting) {
        int result = g_bb.thread && g_bb.running && g_bb.addr == addr ? RT_EOK : -RT_EBUSY;
        rt_exit_critical();
        return result;
    }
    g_bb_starting = RT_TRUE;
    g_bb_transition_tick = rt_tick_get();
    rt_exit_critical();

    /* Keep the buffer across stop/start; UI can still inspect stopped data. */
    bb_frame_t *ring = g_bb.ring;
    rt_enter_critical();
    memset(&g_bb, 0, sizeof(g_bb));
    g_bb.ring = ring;
    rt_exit_critical();
    g_bb.addr = addr;
    g_bb.fd_frame = g_bb.fd_event = -1;
    g_bb.slave_online = RT_FALSE;

    if (!g_bb.ring)
        g_bb.ring = rt_malloc(BB_RING_LEN * sizeof(bb_frame_t));
    if (!g_bb.ring)
    {
        rt_kprintf("[bb] ring alloc failed\r\n");
        g_bb_starting = RT_FALSE;
        return -RT_ENOMEM;
    }

    bb_backend_init();

    g_bb.tsen = rt_device_find(BB_TSEN_NAME);
    if (g_bb.tsen)
    {
        if (rt_device_open(g_bb.tsen, RT_DEVICE_FLAG_RDONLY) != RT_EOK)
            g_bb.tsen = RT_NULL;
    }
    else
        LOG_W("tsen not found, chip temp will read -999");

    bb_system_begin();
    g_bb.running = RT_TRUE;
    collection_generation++;
    g_bb_progress_tick = rt_tick_get();
    g_bb.thread = rt_thread_create("bb", bb_thread_entry, RT_NULL,
                                   BB_THREAD_STACK, BB_THREAD_PRIO, 10);
    if (!g_bb.thread || rt_thread_startup(g_bb.thread) != RT_EOK)
    {
        g_bb.running = RT_FALSE;
        if (g_bb.thread) { rt_thread_delete(g_bb.thread); g_bb.thread = RT_NULL; }
        if (g_bb.fd_frame >= 0) { close(g_bb.fd_frame); g_bb.fd_frame = -1; }
        if (g_bb.fd_event >= 0) { close(g_bb.fd_event); g_bb.fd_event = -1; }
        if (g_bb.tsen) { rt_device_close(g_bb.tsen); g_bb.tsen = RT_NULL; }
        g_bb_starting = RT_FALSE;
        return -RT_ENOMEM;
    }
    g_bb_starting = RT_FALSE;
    rt_kprintf("[bb] started, addr=0x%02X backend=%s ring=%d frames\r\n",
               g_bb.addr, bb_backend_name(), BB_RING_LEN);
    return 0;
}

int bb_stop(void)
{
    if (g_bb_starting)
        return -RT_ERROR;
    if (!g_bb.thread)                /* 幂等 */
        return 0;
    if (g_bb.running) g_bb_transition_tick = rt_tick_get();
    g_bb.running = RT_FALSE;
    rt_kprintf("[bb] stopping\r\n");
    rt_tick_t started = rt_tick_get();
    while (g_bb.thread) {
        if (rt_tick_get() - started > rt_tick_from_millisecond(2000))
            return -RT_ETIMEOUT;
        rt_thread_mdelay(10);
    }
    return 0;
}

int bb_get_status(bb_status_t *st)
{
    rt_uint32_t head, cnt;

    if (!st)
        return -RT_EINVAL;
    memset(st, 0, sizeof(*st));
    rt_enter_critical();
    st->running      = bb_is_running();
    st->backend      = bb_backend_name();
    st->frames       = g_bb.frame_count;
    st->events       = g_bb.evt_count;
    st->slave_online = g_bb.slave_online;
    st->chip_hot     = g_bb.chip_hot;
    st->vib_hi       = g_bb.vib_hi;
    st->addr         = g_bb.addr;
    st->write_errors = g_bb.write_errors;
    st->written_frames = g_bb.written_frames;
    st->written_events = g_bb.written_events; st->dropped_events = g_bb.dropped_events;
    st->dropped_frames = g_bb.dropped_frames;
    st->backend_switches = g_bb.backend_switches;
    st->session_id = g_bb.session_id;
    st->collection_generation = collection_generation;

    /* 环形缓冲快照：与 cmd_bbstat 同款双读防撕裂 */
    head = g_bb.ring_head;
    cnt  = g_bb.ring_cnt;
    if (cnt && g_bb.ring)
    {
        st->last = g_bb.ring[(head - 1 + BB_RING_LEN) % BB_RING_LEN];
        st->has_last = 1;
    }
    rt_exit_critical();
    return 0;
}
int bb_health_ok(rt_uint32_t budget_ms)
{
    rt_enter_critical();
    rt_tick_t now = rt_tick_get(), budget = rt_tick_from_millisecond(budget_ms);
    int healthy = g_bb_starting ? (rt_tick_t)(now - g_bb_transition_tick) < budget :
        !g_bb.thread ? 1 : (rt_tick_t)(now - (g_bb.running ? g_bb_progress_tick : g_bb_transition_tick)) < budget;
    rt_exit_critical(); return healthy;
}

/* ---------------- msh ---------------- */

static int bb_parse_number(const char *text, unsigned long min, unsigned long max, unsigned long *value)
{
    if (!text || !*text || *text == '-' || *text == '+') return -RT_EINVAL;
    char *end; errno = 0; unsigned long n = strtoul(text, &end, 0);
    if (errno || *end || n < min || n > max) return -RT_EINVAL;
    *value = n; return RT_EOK;
}

static int cmd_bb(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "syshistory") || !strcmp(argv[1], "sysdisk"))) {
        bb_system_event_t *events = rt_malloc(BB_SYSTEM_EVENTS * sizeof(*events));
        if (!events) return -RT_ENOMEM;
        int n = bb_system_history_events(events, BB_SYSTEM_EVENTS), result = n;
        if (!strcmp(argv[1], "syshistory")) {
            rt_kprintf("system history %d\r\n", n);
            for (int i = n - 1; i >= 0; i--) rt_kprintf("session=%u event=%u run=%u ts=%u type=%u\r\n",
                (unsigned)events[i].session_id, (unsigned)events[i].id, (unsigned)events[i].run_id,
                (unsigned)events[i].frame.ts_ms, events[i].type);
        } else {
            unsigned long session, id;
            result = -RT_EINVAL;
            if (argc == 4 && !bb_parse_number(argv[2], 1, UINT32_MAX, &session) && !bb_parse_number(argv[3], 1, UINT32_MAX, &id)) {
                bb_system_frame_t *frames = rt_malloc(BB_SYSTEM_RING_LEN * sizeof(*frames));
                result = frames ? -RT_ERROR : -RT_ENOMEM;
                if (frames) for (int i = 0; i < n; i++) if (events[i].session_id == session && events[i].id == id) {
                    result = bb_system_history_window(&events[i], frames, BB_SYSTEM_RING_LEN); break;
                }
                rt_kprintf("system disk frames=%d", result);
                if (result > 0) rt_kprintf(" first=%u last=%u", (unsigned)frames[0].ts_ms, (unsigned)frames[result - 1].ts_ms);
                rt_kprintf("\r\n"); if (frames) rt_free(frames);
            }
        }
        rt_free(events); return result < 0 ? result : RT_EOK;
    }
    if (argc >= 2 && !strcmp(argv[1], "history")) {
        bb_event_t *events = rt_malloc(BB_EVENT_RING_LEN * sizeof(*events));
        if (!events) return -RT_ENOMEM;
        int n = bb_history_events(events, BB_EVENT_RING_LEN);
        rt_kprintf("history %d\r\n", n);
        for (int i = n - 1; i >= 0; i--)
            rt_kprintf("session=%u event=%u ts=%u type=%u\r\n", (unsigned)events[i].session_id,
                       (unsigned)events[i].id, (unsigned)events[i].ts_ms, (unsigned)events[i].type);
        rt_free(events); return n < 0 ? n : RT_EOK;
    }
    if (argc >= 4 && !strcmp(argv[1], "disk")) {
        bb_event_t *events = rt_malloc(BB_EVENT_RING_LEN * sizeof(*events));
        bb_frame_t *frames = rt_malloc(BB_RING_LEN * sizeof(*frames));
        if (!events || !frames) { if (events) rt_free(events); if (frames) rt_free(frames); return -RT_ENOMEM; }
        unsigned long session, id;
        if (argc != 4 || bb_parse_number(argv[2], 1, UINT32_MAX, &session) ||
            bb_parse_number(argv[3], 1, UINT32_MAX, &id)) { rt_free(events); rt_free(frames); return -RT_EINVAL; }
        int n = bb_history_events(events, BB_EVENT_RING_LEN), got = -RT_ERROR;
        for (int i = 0; i < n; i++) if (events[i].session_id == session && events[i].id == id) {
            got = bb_history_window(&events[i], frames, BB_RING_LEN); break;
        }
        rt_kprintf("disk frames=%d", got);
        if (got > 0) rt_kprintf(" first=%u last=%u", (unsigned)frames[0].ts_ms, (unsigned)frames[got - 1].ts_ms);
        rt_kprintf("\r\n"); rt_free(events); rt_free(frames); return got < 0 ? got : RT_EOK;
    }
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        unsigned long addr = RS485_ADDR_DEFAULT;
        if (argc > 3 || (argc == 3 && bb_parse_number(argv[2], 1, 254, &addr))) return -RT_EINVAL;
        return bb_start_addr(addr);
    }

    if (argc >= 2 && !strcmp(argv[1], "stop"))
        return bb_stop();

    if (argc >= 2 && !strcmp(argv[1], "replay"))
    {
        unsigned long requested = 20;
        if (argc > 3 || (argc == 3 && bb_parse_number(argv[2], 1, 24, &requested))) return -RT_EINVAL;
        int n = requested;
        bb_frame_t frames[24];
        if (n > 24) n = 24;
        int got = bb_ring_recent(frames, n);
        for (int i = 0; i < got; i++)
            rt_kprintf("%u,%u,%d,%u,%u,%d,%u,%u,%d,%d,%d,%d,%d,%d\r\n",
                       (unsigned)frames[i].ts_ms, (unsigned)!!(frames[i].flags & BB_F_SLAVE_ON),
                       frames[i].slave_temp, (unsigned)frames[i].slave_humi,
                       (unsigned)frames[i].slave_up, frames[i].chip_temp,
                       (unsigned)frames[i].rtt_ms, (unsigned)frames[i].flags,
                       frames[i].ax, frames[i].ay, frames[i].az,
                       frames[i].gx, frames[i].gy, frames[i].gz);
        return 0;
    }

    rt_kprintf("usage: bb start [addr] | bb stop | bb replay [n<=24] | bb history | bb disk <session> <event>\r\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_bb, bb, blackbox collector start stop replay);

static int cmd_bbstat(int argc, char **argv)
{
    bb_system_status_t sys; bb_system_status(&sys);
    rt_kprintf("slave2 online=%d stale=%d frames=%u ok=%u timeout=%u errors=%u events=%u written=%u dropped=%u write_errors=%u\r\n",
        (unsigned)sys.online, (unsigned)sys.stale, (unsigned)sys.frames, (unsigned)sys.ok, (unsigned)sys.timeouts, (unsigned)sys.errors, (unsigned)sys.events, (unsigned)sys.written, (unsigned)sys.dropped, (unsigned)sys.write_errors);
    bb_status_t status;
    rt_enter_critical();
    bb_get_status(&status);
    rt_uint32_t bytes = g_bb.frame_bytes, ring_count = g_bb.ring_cnt;
    rt_exit_critical();
    rt_kprintf("running %d | backend %s | frames %u | events %u | bytes %u | ring %u/%u | slave %s | chip %s\r\n",
               status.running, status.backend,
               (unsigned)status.frames, (unsigned)status.events,
               (unsigned)bytes,
               (unsigned)ring_count, (unsigned)BB_RING_LEN,
               status.slave_online ? "ON" : "OFF",
               status.chip_hot ? "HOT" : "OK");
    rt_kprintf("session %u | written %u | dropped %u | write_errors %u | switches %u\r\n",
               (unsigned)status.session_id, (unsigned)status.written_frames, (unsigned)status.dropped_frames,
               (unsigned)status.write_errors, (unsigned)status.backend_switches);
    rt_kprintf("events_written %u | events_dropped %u | quality_known %u | env_valid %u | imu_valid %u\r\n",
        (unsigned)status.written_events, (unsigned)status.dropped_events,
        !!(status.last.flags & BB_F_QUALITY_KNOWN),
        !!(status.last.flags & BB_F_SLAVE_ON) && !(status.last.flags & BB_F_ENV_INVALID),
        !!(status.last.flags & BB_F_SLAVE_ON) && !(status.last.flags & BB_F_IMU_INVALID));
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_bbstat, bbstat, blackbox status);
