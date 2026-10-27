/* Typed Linux metrics; independent validity, counters, events and bounded logs. */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "bb.h"
#include "bb_system.h"
static bb_system_status_t state;
static bb_system_frame_t ring[BB_SYSTEM_RING_LEN];
static bb_system_frame_t last_good;
static bb_system_event_t events[BB_SYSTEM_EVENTS];
static unsigned head, count, event_head, event_count, lost_streak;
static rt_uint32_t due, sequence_at, last_sequence, last_up, last_process;
static int had_sequence, fd = -1, disk_kind = -1;
static rt_uint32_t disk_session, bytes;
static rt_uint8_t pending_event;
static int close_log(void)
{
    int result = 0;
    if (fd >= 0) { if (fsync(fd) < 0) result = -1; if (close(fd) < 0) result = -1; fd = -1; }
    return result;
}
void bb_system_begin(void)
{
    rt_enter_critical();
    memset(&state, 0, sizeof(state)); head = count = event_head = event_count = lost_streak = 0;
    rt_exit_critical();
    due = bb_now_ms(); had_sequence = 0; disk_kind = -1; pending_event = 0;
    memset(&last_good, 0, sizeof(last_good));
}
void bb_system_end(void) { if (close_log()) { rt_enter_critical(); state.write_errors++; rt_exit_critical(); } }
static void add_event(unsigned type, const bb_system_frame_t *frame)
{
    bb_system_event_t e; memset(&e, 0, sizeof(e)); e.id = ++state.events; e.type = type; e.frame = *frame;
    events[event_head] = e; event_head = (event_head + 1) % BB_SYSTEM_EVENTS;
    if (event_count < BB_SYSTEM_EVENTS) event_count++;
    pending_event |= 1U << (type - 1);
}
static void log_frame(const char *backend, rt_uint32_t session, rt_uint32_t run, const bb_system_frame_t *f)
{
    int kind = !strcmp(backend, "sdcard") ? 1 : !strcmp(backend, "nor") ? 2 : 0;
    if (kind != disk_kind || session != disk_session) {
        if (close_log()) state.write_errors++;
        disk_kind = kind; disk_session = session; bytes = 0;
        if (kind) {
            char path[80];
            /* 16 fixed TF slots and one 64 KiB NOR slot bound storage independently of Star files. */
            if (kind == 1) rt_snprintf(path, sizeof(path), "/sdcard/blackbox/s%02u.csv", (unsigned)(session % 16));
            else strcpy(path, "/data/blackbox/bb_system.csv");
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) state.write_errors++;
        }
    }
    if (fd < 0) { state.dropped++; return; }
    unsigned limit = kind == 1 ? 200 * 1024 : 64 * 1024;
    if (bytes >= limit) {
        if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0) {
            state.write_errors++; state.dropped++; close_log(); return;
        }
        bytes = 0;
    }
    char line[384]; int n;
    if (!bytes) {
        n = rt_snprintf(line, sizeof(line), "# linux-system-v2 session=%u run=%u addr=2; event bits 1=online 2=offline 4=restart 8=stale 16=fresh\n"
            "ts_ms,query_ok,event_bits,valid,seq,up_s,age_ms,cpu_centi,load_milli,total_kib,available_kib,process_s,rtt_ms,requests,responses,crc,frame,tx_err,rx_bytes,tx_bytes,rejected,event_id\n", (unsigned)session, (unsigned)run);
        if (n <= 0 || n >= sizeof(line) || write(fd, line, n) != n) goto failed;
        bytes += n;
    }
    const rs485_system_t *m = &f->metrics;
    unsigned event_n = 0; for (unsigned bits = pending_event; bits; bits >>= 1) event_n += bits & 1;
    n = rt_snprintf(line, sizeof(line), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        (unsigned)f->ts_ms, f->query_ok, pending_event, m->valid, (unsigned)m->sequence, (unsigned)m->uptime_s, (unsigned)m->age_ms,
        m->cpu_centi, m->load_milli, (unsigned)m->mem_total_kib, (unsigned)m->mem_available_kib, (unsigned)m->process_uptime_s,
        f->rtt_ms, (unsigned)m->requests, (unsigned)m->responses, (unsigned)m->crc_errors, (unsigned)m->frame_errors, (unsigned)m->tx_errors,
        (unsigned)m->rx_bytes, (unsigned)m->tx_bytes, (unsigned)m->rejected,
        event_n ? (unsigned)(state.events - event_n + 1) : 0);
    if (n <= 0 || n >= sizeof(line) || write(fd, line, n) != n || fsync(fd) < 0) goto failed;
    bytes += n; state.written++;
    rt_enter_critical();
    for (unsigned i = 0; i < event_n; i++) {
        bb_system_event_t *e = &events[(event_head + BB_SYSTEM_EVENTS - event_n + i) % BB_SYSTEM_EVENTS];
        e->session_id = session; e->run_id = run; e->disk_source = kind == 1 ? 1 : 2;
    }
    rt_exit_critical(); return;
failed:
    state.write_errors++; state.dropped++; close_log();
}
void bb_system_step(const char *backend, rt_uint32_t session, rt_uint32_t run)
{
    rt_uint32_t now = bb_now_ms();
    if ((rt_int32_t)(now - due) < 0) return;
    due = now + 1000; /* No catch-up bursts after a filesystem/bus delay. */
    bb_system_frame_t f; memset(&f, 0, sizeof(f)); rt_uint32_t rtt = 0;
    int rc = rs485_query_system(2, &f.metrics, 100, &rtt);
    f.ts_ms = bb_now_ms(); f.rtt_ms = rtt; f.query_ok = rc == RT_EOK;
    rt_enter_critical(); pending_event = 0;
    if (!rc) {
        state.ok++; lost_streak = 0;
        if (!state.online) { state.online = 1; add_event(1, &f); }
        int restarted = had_sequence && (f.metrics.uptime_s < last_up || f.metrics.process_uptime_s < last_process);
        if (!had_sequence || restarted || f.metrics.sequence != last_sequence) sequence_at = f.ts_ms;
        if (restarted) add_event(3, &f);
        last_sequence = f.metrics.sequence; last_up = f.metrics.uptime_s;
        last_process = f.metrics.process_uptime_s; had_sequence = 1;
        int stale = f.metrics.age_ms > 3000 || (rt_uint32_t)(f.ts_ms - sequence_at) > 3000;
        if (stale != state.stale) { state.stale = stale; add_event(stale ? 4 : 5, &f); }
        if (stale) f.metrics.valid = 0;
        last_good = f;
    } else {
        if (rc == -RT_ETIMEOUT) {
            state.timeouts++;
            if (++lost_streak >= 3 && state.online) {
                state.online = 0;
                bb_system_frame_t context = last_good; context.ts_ms = f.ts_ms;
                add_event(2, &context);
            }
        } else { state.errors++; lost_streak = 0; }
    }
    ring[head] = f; head = (head + 1) % BB_SYSTEM_RING_LEN;
    if (count < BB_SYSTEM_RING_LEN) count++;
    state.last = f; state.has_last = 1; state.frames++;
    rt_exit_critical();
    /* I/O must never run with scheduling locked. Counters are single-writer. */
    if (!bb_system_file_lock()) { log_frame(backend, session, run, &f); bb_system_file_unlock(); }
    else { state.dropped++; state.write_errors++; }
}
void bb_system_status(bb_system_status_t *out)
{
    if (!out) return;
    rt_enter_critical(); *out = state; rt_exit_critical();
}
int bb_system_recent(bb_system_frame_t *out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    rt_enter_critical(); int n = capacity < count ? capacity : count;
    for (int i = 0; i < n; i++) out[i] = ring[(head + BB_SYSTEM_RING_LEN - n + i) % BB_SYSTEM_RING_LEN];
    rt_exit_critical(); return n;
}
int bb_system_events(bb_system_event_t *out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    rt_enter_critical(); int n = capacity < event_count ? capacity : event_count;
    for (int i = 0; i < n; i++) out[i] = events[(event_head + BB_SYSTEM_EVENTS - n + i) % BB_SYSTEM_EVENTS];
    rt_exit_critical(); return n;
}
