#ifndef BB_SYSTEM_H
#define BB_SYSTEM_H
#include "rs485_proto.h"
#define BB_SYSTEM_RING_LEN 360
#define BB_SYSTEM_EVENTS 16
typedef struct {
    rt_uint32_t ts_ms;
    rs485_system_t metrics;
    rt_uint16_t rtt_ms;
    rt_uint8_t query_ok;
} bb_system_frame_t;
/* 1=communication recovered, 2=lost, 3=process restarted, 4=stale, 5=fresh again. */
typedef struct {
    rt_uint32_t id, session_id, run_id;
    rt_uint8_t type, disk_source;
    bb_system_frame_t frame;
} bb_system_event_t;
typedef struct {
    int online, stale, has_last;
    rt_uint32_t frames, ok, timeouts, errors, events, written, dropped, write_errors;
    bb_system_frame_t last;
} bb_system_status_t;
/* Owned exclusively by the existing collector worker, including all filesystem work. */
void bb_system_begin(void);
void bb_system_step(const char *backend, rt_uint32_t session, rt_uint32_t run);
void bb_system_end(void);
void bb_system_status(bb_system_status_t *out);
int bb_system_recent(bb_system_frame_t *out, int capacity);
int bb_system_events(bb_system_event_t *out, int capacity);
/* Filesystem calls run exclusively in workers/shells. */
int bb_system_history_events(bb_system_event_t *out, int capacity);
int bb_system_history_window(const bb_system_event_t *event, bb_system_frame_t *out, int capacity);
int bb_system_file_lock(void);
void bb_system_file_unlock(void);
#endif
