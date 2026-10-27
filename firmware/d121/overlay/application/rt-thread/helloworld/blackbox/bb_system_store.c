/* Snapshot one bounded CSV under the file mutex, then parse outside it. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "bb.h"
#include "bb_system.h"
#ifdef BB_STORE_HOST
#include <pthread.h>
static pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
int bb_system_file_lock(void) { return pthread_mutex_lock(&files_mutex); }
void bb_system_file_unlock(void) { pthread_mutex_unlock(&files_mutex); }
#else
static struct rt_mutex files_mutex;
static int files_ready;
int bb_system_file_lock(void)
{
    rt_enter_critical();
    if (!files_ready) { rt_mutex_init(&files_mutex, "sysdisk", RT_IPC_FLAG_PRIO); files_ready = 1; }
    rt_exit_critical();
    return rt_mutex_take(&files_mutex, rt_tick_from_millisecond(300)) == RT_EOK ? 0 : -1;
}
void bb_system_file_unlock(void) { rt_mutex_release(&files_mutex); }
#endif
static int load_file(unsigned slot, char **out)
{
    char path[160];
    int path_len = rt_snprintf(path, sizeof(path), "%s/s%02u.csv", BB_HISTORY_ROOT, slot);
    *out = NULL;
    if (path_len < 0 || path_len >= sizeof(path)) return -RT_EINVAL;
    if (bb_system_file_lock()) return -RT_EBUSY;
    int fd = open(path, O_RDONLY), result = 0; struct stat st;
    /* This RT-Thread DFS stores negative errno; host POSIX stores positive errno. */
    if (fd < 0) { result = errno == ENOENT || errno == -ENOENT ? 0 : -RT_ERROR; goto done; }
    if (fstat(fd, &st)) { result = -RT_ERROR; goto done; }
    if (st.st_size < 1 || st.st_size > 200 * 1024 + 384) goto done;
    *out = rt_malloc(st.st_size + 1);
    if (!*out) { result = -RT_ENOMEM; goto done; }
    int used = 0;
    while (used < st.st_size) {
        int n = read(fd, *out + used, st.st_size - used);
        if (n <= 0) { rt_free(*out); *out = NULL; result = -RT_ERROR; goto done; }
        used += n;
    }
    (*out)[used] = 0; result = used;
done:
    if (fd >= 0 && close(fd)) { if (*out) rt_free(*out); *out = NULL; result = -RT_ERROR; }
    bb_system_file_unlock(); return result;
}
static int parse_frame(char *line, int version, bb_system_frame_t *f, unsigned *bits, rt_uint32_t *id)
{
    rt_uint32_t v[22]; char *p = line;
    for (int i = 0; i < (version == 2 ? 22 : 21); i++) {
        if (*p < '0' || *p > '9') return -1;
        char *end; errno = 0; unsigned long n = strtoul(p, &end, 10);
        if (errno || n > UINT32_MAX) return -1;
        v[i] = n;
        if (i + 1 < (version == 2 ? 22 : 21)) { if (*end != ',') return -1; p = end + 1; }
        else if (*end && strcmp(end, "\r")) return -1;
    }
    if (v[1] > 1 || v[2] > 31 || v[3] > 15 || v[7] > 65535 || v[8] > 65535 || v[12] > 65535 ||
        ((v[3] & 1) && v[7] > 10000) || ((v[3] & 2) && (!v[9] || v[10] > v[9]))) return -1;
    memset(f, 0, sizeof(*f)); f->ts_ms = v[0]; f->query_ok = v[1]; *bits = v[2]; *id = version == 2 ? v[21] : 0;
    rs485_system_t *m = &f->metrics; m->valid = v[3]; m->sequence = v[4]; m->uptime_s = v[5]; m->age_ms = v[6];
    m->cpu_centi = v[7]; m->load_milli = v[8]; m->mem_total_kib = v[9]; m->mem_available_kib = v[10];
    m->process_uptime_s = v[11]; f->rtt_ms = v[12]; m->requests = v[13]; m->responses = v[14];
    m->crc_errors = v[15]; m->frame_errors = v[16]; m->tx_errors = v[17]; m->rx_bytes = v[18];
    m->tx_bytes = v[19]; m->rejected = v[20]; return 0;
}
static int newer(const bb_system_event_t *a, const bb_system_event_t *b)
{
    if (a->session_id != b->session_id) return a->session_id > b->session_id;
    if (a->frame.ts_ms != b->frame.ts_ms) return (rt_int32_t)(a->frame.ts_ms - b->frame.ts_ms) > 0;
    return a->id > b->id;
}
static void keep_event(bb_system_event_t *out, int capacity, int *count, const bb_system_event_t *e)
{
    int pos = *count;
    if (pos == capacity) { if (!newer(e, &out[0])) return; memmove(out, out + 1, (capacity - 1) * sizeof(*out)); pos--; }
    else (*count)++;
    while (pos > 0 && newer(&out[pos - 1], e)) { out[pos] = out[pos - 1]; pos--; }
    out[pos] = *e;
}
static int scan(bb_system_event_t *out_events, const bb_system_event_t *selected,
                bb_system_frame_t *out_frames, int capacity)
{
    if (capacity < 1 || (!out_events && (!selected || !out_frames))) return -RT_EINVAL;
    struct stat st; if (stat(BB_HISTORY_ROOT, &st) || !S_ISDIR(st.st_mode)) return -RT_ERROR;
    int count = 0;
    for (unsigned slot = 0; slot < 16; slot++) {
        char *data; int size = load_file(slot, &data);
        if (size < 0) return size;
        if (!size) continue;
        char *line = data, *end = strchr(line, '\n');
        unsigned version = 0, session = 0, run = 0, addr = 0;
        if (!end) { rt_free(data); continue; } *end = 0;
        if (sscanf(line, "# linux-system-v2 session=%u run=%u addr=%u", &session, &run, &addr) == 3) version = 2;
        else if (sscanf(line, "# linux-system-v1 session=%u addr=%u", &session, &addr) == 2) { version = 1; run = session; }
        if (!version || addr != 2 || !session || session % 16 != slot || !run) { rt_free(data); continue; }
        bb_system_frame_t last_good; memset(&last_good, 0, sizeof(last_good)); rt_uint32_t legacy_id = 0;
        line = end + 1;
        while ((end = strchr(line, '\n')) != NULL) {
            *end = 0; bb_system_frame_t f; unsigned bits; rt_uint32_t id;
            if (end - line < 384 && !parse_frame(line, version, &f, &bits, &id)) {
                if (out_frames && (version == 1 ? session == selected->session_id : run == selected->run_id)) {
                    rt_int32_t delta = f.ts_ms - selected->frame.ts_ms;
                    if (delta >= -30000 && delta <= 30000 && count < capacity) out_frames[count++] = f;
                }
                if (out_events) for (unsigned type = 1; type <= 5; type++) if (bits & (1U << (type - 1))) {
                    bb_system_event_t e; memset(&e, 0, sizeof(e)); e.type = type;
                    e.id = version == 2 ? id++ : ++legacy_id; e.session_id = session; e.run_id = run; e.disk_source = 1;
                    e.frame = type == 2 && last_good.query_ok ? last_good : f; e.frame.ts_ms = f.ts_ms;
                    keep_event(out_events, capacity, &count, &e);
                }
                if (f.query_ok) last_good = f;
            }
            line = end + 1;
        }
        rt_free(data);
    }
    if (out_frames) for (int i = 1; i < count; i++) {
        bb_system_frame_t f = out_frames[i]; int j = i;
        while (j && (rt_int32_t)(out_frames[j - 1].ts_ms - f.ts_ms) > 0) { out_frames[j] = out_frames[j - 1]; j--; }
        out_frames[j] = f;
    }
    return count;
}
int bb_system_history_events(bb_system_event_t *out, int capacity)
{
    if (!out) return -RT_EINVAL;
    return scan(out, NULL, NULL, capacity);
}
int bb_system_history_window(const bb_system_event_t *event, bb_system_frame_t *out, int capacity)
{
    if (!event || !out || event->disk_source != 1) return -RT_EINVAL;
    return scan(NULL, event, out, capacity);
}
