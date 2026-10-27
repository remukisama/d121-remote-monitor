/*
 * blackbox: 采集线程 + 事件判定 + 三档后端落盘 + PSRAM 环形缓冲
 *
 * 数据源:
 *   - RS485 从站遥测: 每 BB_SAMPLE_MS 调 rs485_query()（与 mbq 轮询线程共
 *     用 uart3，rs485_master.c 内的总线 mutex 自动排队）
 *   - 片内结温: temp_tsen_cpu，每 10 个采样周期读一次（1Hz）
 *
 * 事件（TF 分会话 CSV；NOR 兜底写 bb_event.log）:
 *   - 从站连续 3 次超时 → SLAVE_LOST；恢复应答 → SLAVE_ON
 *   - 结温 >= hot_on(默认 85.0C) → CHIP_HOT；回落 <= hot_off(80.0C) → CHIP_OK（迟滞）
 *   - 从站加速度幅值 sqrt(ax²+ay²+az²) 越限 → VIB_HI / 回落 → VIB_OK
 *
 * 落盘后端启动时按序探测；写入失败时依次降级:
 *   1. /sdcard/blackbox/   —— 但若根目录存在 bootcfg.txt 判定为"升级卡"，跳过
 *   2. /data/blackbox/     —— NOR littlefs 1MB 兜底
 *   3. 内存环形缓冲 only   —— 无介质，事件只进 RAM，bbstat 可见
 *
 * Copyright (c) 2026   SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BB_H__
#define __BB_H__

#include <rtthread.h>

#define BB_SAMPLE_MS   100    /* 10Hz 总线采样 */
#define BB_RING_LEN    1024   /* ~102s: room for a 30s/30s event window */
#define BB_EVENT_RING_LEN 16
#define BB_HISTORY_PRE_MS 30000U
#define BB_HISTORY_POST_MS 30000U
#ifndef BB_HISTORY_ROOT
#define BB_HISTORY_ROOT "/sdcard/blackbox"
#endif

#define BB_F_SLAVE_ON  0x0001
#define BB_F_CHIP_HOT  0x0002
#define BB_F_VIB_HI    0x0004
#define BB_F_ENV_INVALID 0x0008
#define BB_F_IMU_INVALID 0x0010
#define BB_F_QUALITY_KNOWN 0x0020

/* 振动越限阈值: accel 合成矢量的平方（LSB²），±2g 量程 1g=16384LSB。
 * 静止重力≈1g(2.68e8)，阈值取 sqrt 后 ≈1.3g 的平方；轻敲板子可瞬时越线。 */
#define BB_VIB_ON_SQ   (21000L * 21000L)
#define BB_VIB_OFF_SQ  (18500L * 18500L)

/* 帧文件 bb_frame.log: CSV, 10Hz (v2: +从站六轴):
 *   ts_ms,slave_on,slave_temp01,slave_humi01,slave_up,chip_temp1,rtt_ms,flags,ax,ay,az,gx,gy,gz
 *   accel/gyro 为从站 ICM20608 原始 LSB（±2g=16384/g, ±250dps=131/dps）
 *   slave_temp 单位 0.01C（AHT21）；chip_temp 单位 0.1C（片内 tsen） */
typedef struct {
    rt_uint32_t ts_ms;
    rt_int16_t  slave_temp;   /* 0.01 C, 从站 AHT21 */
    rt_uint16_t slave_humi;   /* 0.01 %RH */
    rt_uint32_t slave_up;     /* 从站 uptime_s */
    rt_int16_t  chip_temp;    /* 0.1 C, 片内 tsen */
    rt_uint16_t rtt_ms;
    rt_uint16_t flags;
    rt_int16_t  ax, ay, az;   /* 从站 ICM20608 原始 LSB */
    rt_int16_t  gx, gy, gz;
} bb_frame_t;

typedef enum {
    BB_EVT_SLAVE_ON, BB_EVT_SLAVE_LOST,
    BB_EVT_CHIP_HOT, BB_EVT_CHIP_OK,
    BB_EVT_VIB_HI, BB_EVT_VIB_OK
} bb_event_type_t;

/* UI and log share the same event; frame contains the trigger context. */
typedef struct {
    rt_uint32_t id;
    rt_uint32_t ts_ms;
    bb_event_type_t type;
    bb_frame_t frame;
    rt_uint32_t session_id;
    rt_uint32_t run_id;
    rt_uint32_t frame_offset;
    rt_uint8_t disk_source; /* 0=RAM, 1=TF session, 2=NOR legacy fallback */
} bb_event_t;

int bb_events_recent(bb_event_t *out, int n);
/* Milliseconds since blackbox component initialization; kernel tick may start near wrap. */
rt_uint32_t bb_now_ms(void);
int bb_health_ok(rt_uint32_t budget_ms);
void bb_gui_heartbeat(void);
void bb_gui_heartbeat_clear(void);
/* Blocking filesystem APIs: call only from a worker/shell, never LVGL callbacks. */
int bb_history_events(bb_event_t *out, int n);
int bb_history_window(const bb_event_t *event, bb_frame_t *out, int capacity);

/* 取最近 n 帧，按时间升序写入 out，返回实际帧数。供回放/UI 使用。 */
int bb_ring_recent(bb_frame_t *out, int n);

/* 当前后端: "sdcard" / "nor" / "ram" */
const char *bb_backend_name(void);

/* ---- 公开控制接口 (2026-09-16, 供 M3 监控页/上层调用) ---- */

/* 状态快照（停止后保留计数，启动新采集时清零） */
typedef struct {
    int             running;      /* 1=采集中 */
    const char     *backend;      /* "sdcard"/"nor"/"ram" */
    rt_uint32_t     frames;       /* 累计采集帧数，包含未成功写盘帧 */
    rt_uint32_t     events;       /* 累计事件数 */
    rt_bool_t       slave_online;
    rt_bool_t       chip_hot;
    rt_bool_t       vib_hi;
    rt_bool_t       has_last;     /* last 是否有效 */
    bb_frame_t      last;         /* 最新一帧 */
    rt_uint8_t      addr;
    rt_uint32_t     write_errors; /* sticky for this collection session */
    rt_uint32_t     written_frames;
    rt_uint32_t     written_events, dropped_events;
    rt_uint32_t     dropped_frames;
    rt_uint32_t     backend_switches;
    rt_uint32_t     session_id;
    rt_uint32_t     collection_generation; /* Changes on each start, including RAM/NOR sessions. */
} bb_status_t;

int  bb_is_running(void);
int  bb_start_addr(rt_uint8_t addr);   /* Same active addr: 0; different addr/transition: -RT_EBUSY. */
int  bb_stop(void);                    /* 幂等：未运行返回 0 */
int  bb_get_status(bb_status_t *st);

#endif /* __BB_H__ */
