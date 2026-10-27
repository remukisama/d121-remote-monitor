/* Bounded, session-separated CSV history. Legacy bb_*.log is never modified here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "bb_store.h"

#ifdef BB_STORE_HOST
#include <pthread.h>
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
static int store_lock(void) { return pthread_mutex_lock(&history_mutex) ? -1 : 0; }
static void store_unlock(void) { pthread_mutex_unlock(&history_mutex); }
#else
static struct rt_mutex history_mutex;
static int history_mutex_ready;
static int store_lock(void)
{
    rt_enter_critical();
    if (!history_mutex_ready) {
        rt_mutex_init(&history_mutex, "bbhist", RT_IPC_FLAG_PRIO);
        history_mutex_ready = 1;
    }
    rt_exit_critical();
    return rt_mutex_take(&history_mutex, rt_tick_from_millisecond(3000)) == RT_EOK ? 0 : -1;
}
static void store_unlock(void) { rt_mutex_release(&history_mutex); }
#endif

typedef struct {
    int fd;
    off_t remaining;
    unsigned pos, count;
    char block[512];
} line_reader_t;

static int reader_init(line_reader_t *r, int fd, off_t start)
{
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < start || lseek(fd, start, SEEK_SET) < 0) return -1;
    memset(r, 0, sizeof(*r)); r->fd = fd; r->remaining = end - start;
    return 0;
}

/* Only return newline-terminated lines; torn append tails are not valid records. */
static int reader_line(line_reader_t *r, char *line, unsigned capacity)
{
    unsigned size = 0; int overflow = 0;
    for (;;) {
        if (r->pos == r->count) {
            if (!r->remaining) return 0;
            unsigned amount = r->remaining > sizeof(r->block) ? sizeof(r->block) : (unsigned)r->remaining;
            int got = read(r->fd, r->block, amount);
            if (got <= 0) return -1;
            r->remaining -= got; r->pos = 0; r->count = got;
        }
        char c = r->block[r->pos++];
        if (c == '\n') { line[size] = 0; return overflow ? 2 : 1; }
        if (c == '\r') continue;
        if (size + 1 < capacity) line[size++] = c;
        else overflow = 1;
    }
}

static int numbers(const char *line, const char *prefix, int64_t *values, int count)
{
    size_t len = strlen(prefix);
    if (strncmp(line, prefix, len)) return -1;
    const char *p = line + len;
    for (int i = 0; i < count; i++) {
        if (!*p || (*p != '-' && (*p < '0' || *p > '9'))) return -1;
        char *end; errno = 0; long long value = strtoll(p, &end, 10);
        if (errno == ERANGE || end == p) return -1;
        values[i] = value;
        if (i + 1 < count) { if (*end != ',') return -1; p = end + 1; }
        else if (*end) return -1;
    }
    return 0;
}

static int make_path(char *path, size_t size, const char *root, char kind, uint32_t id)
{
    int n = snprintf(path, size, "%s/%c%08x.csv", root, kind, (unsigned)id);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static uint32_t file_id(const char *name, char kind)
{
    if (strlen(name) != 13 || name[0] != kind || strcmp(name + 9, ".csv")) return 0;
    uint32_t id = 0;
    for (int i = 1; i < 9; i++) {
        int v = name[i] >= '0' && name[i] <= '9' ? name[i] - '0' :
                name[i] >= 'a' && name[i] <= 'f' ? name[i] - 'a' + 10 : -1;
        if (v < 0) return 0;
        id = (id << 4) | v;
    }
    return id;
}

static int file_header(int fd, char kind, uint32_t expected_id, uint32_t *run, uint32_t *address)
{
    char line[96]; int64_t v[3]; unsigned used = 0;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    /* Headers are short: seeking to EOF walked the entire FAT chain for every scan. */
    while (used + 1 < sizeof(line)) {
        int got = read(fd, line + used, sizeof(line) - 1 - used);
        if (got <= 0) return -1;
        unsigned end = used + got;
        for (unsigned i = used; i < end; i++) if (line[i] == '\n') {
            line[i && line[i - 1] == '\r' ? i - 1 : i] = 0; goto parsed;
        }
        used = end;
    }
    return -1;
parsed:
    if (numbers(line, kind == 'f' ? "#BBF1," : "#BBE1,", v, 3)) return -1;
    if (v[0] <= 0 || v[0] > UINT32_MAX || v[1] != expected_id || v[2] < 1 || v[2] > 254) return -1;
    if (address) *address = v[2];
    *run = v[0]; return 0;
}

static int valid_pair(const char *root, uint32_t id, uint32_t *run)
{
    char path[80]; uint32_t event_run, frame_addr, event_addr;
    if (make_path(path, sizeof(path), root, 'f', id)) return -1;
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    int result = file_header(fd, 'f', id, run, &frame_addr); close(fd); if (result) return -1;
    if (make_path(path, sizeof(path), root, 'e', id)) return -1;
    fd = open(path, O_RDONLY); if (fd < 0) return -1;
    result = file_header(fd, 'e', id, &event_run, &event_addr); close(fd);
    return result || event_run != *run || frame_addr != event_addr ? -1 : 0;
}

/* Keep newest ids in descending order, with a hard scan bound for foreign files. */
static int collect_ids(const char *root, uint32_t *ids, int capacity, uint32_t *maximum)
{
    DIR *dir = opendir(root); if (!dir) return -1;
    int count = 0, visited = 0; struct dirent *entry;
    *maximum = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (++visited > 4096) { closedir(dir); return -1; }
        uint32_t id = file_id(entry->d_name, 'f');
        uint32_t any = id ? id : file_id(entry->d_name, 'e');
        if (any > *maximum) *maximum = any;
        uint32_t run;
        if (!id || valid_pair(root, id, &run)) continue;
        int at = 0; while (at < count && ids[at] > id) at++;
        if (at >= capacity) continue;
        if (count < capacity) count++;
        for (int i = count - 1; i > at; i--) ids[i] = ids[i - 1];
        ids[at] = id;
    }
    closedir(dir); return count;
}

static int write_all(int fd, const char *data, int size)
{
    int done = 0;
    while (done < size) { int n = write(fd, data + done, size - done); if (n <= 0) return -1; done += n; }
    return fsync(fd) < 0 ? -1 : 0;
}

int bb_store_new(const char *root, rt_uint32_t run_id, rt_uint8_t addr, bb_disk_files_t *files)
{
    if (!root || !files || !addr || addr > 254 || store_lock()) return -RT_ERROR;
    uint32_t ids[BB_STORE_KEEP + 1], maximum;
    mkdir(root, 0755);
    int count = collect_ids(root, ids, BB_STORE_KEEP + 1, &maximum);
    int result = -RT_ERROR, fd_f = -1, fd_e = -1;
    char frame_path[80], event_path[80], header[96];
    if (count < 0 || maximum == UINT32_MAX) goto done;
    uint32_t id = maximum + 1, run = run_id ? run_id : id;
    if (make_path(frame_path, sizeof(frame_path), root, 'f', id) ||
        make_path(event_path, sizeof(event_path), root, 'e', id)) goto done;
    fd_f = open(frame_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd_f < 0) goto done;
    fd_e = open(event_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd_e < 0) goto rollback;
    int nf = snprintf(header, sizeof(header), "#BBF1,%u,%u,%u\r\n", (unsigned)run, (unsigned)id, addr);
    if (write_all(fd_f, header, nf)) goto rollback;
    int ne = snprintf(header, sizeof(header), "#BBE1,%u,%u,%u\r\n", (unsigned)run, (unsigned)id, addr);
    if (write_all(fd_e, header, ne)) goto rollback;
    files->frame_fd = fd_f; files->event_fd = fd_e;
    files->id = id; files->run_id = run; files->frame_bytes = nf; files->event_bytes = ne;
    files->prune_errors = 0;
    fd_f = fd_e = -1; result = RT_EOK;
    /* Only our validated file pairs are pruned; old bb_*.log/foreign files survive. */
    if (count >= BB_STORE_KEEP) {
        /* Scan all old valid pairs: a previous version may have left more than KEEP+1. */
        DIR *dir = opendir(root);
        if (!dir) files->prune_errors++;
        else {
            struct dirent *entry; int visited = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (++visited > 4096) { files->prune_errors++; break; }
                uint32_t old = file_id(entry->d_name, 'f'), old_run;
                if (!old || old > ids[BB_STORE_KEEP - 1] || valid_pair(root, old, &old_run)) continue;
                if (make_path(frame_path, sizeof(frame_path), root, 'f', old) || unlink(frame_path) < 0) files->prune_errors++;
                if (make_path(event_path, sizeof(event_path), root, 'e', old) || unlink(event_path) < 0) files->prune_errors++;
            }
            closedir(dir);
        }
    }
    goto done;
rollback:
    if (fd_f >= 0) { close(fd_f); fd_f = -1; unlink(frame_path); }
    if (fd_e >= 0) { close(fd_e); fd_e = -1; unlink(event_path); }
done:
    if (fd_f >= 0) close(fd_f);
    if (fd_e >= 0) close(fd_e);
    store_unlock(); return result;
}

int bb_store_event_line(char *line, int size, const bb_event_t *e)
{
    const bb_frame_t *f = &e->frame;
    return snprintf(line, size, "E,%u,%u,%u,%u,%u,%u,%d,%u,%u,%d,%u,%u,%d,%d,%d,%d,%d,%d\r\n",
        (unsigned)e->id, (unsigned)e->ts_ms, (unsigned)e->type, (unsigned)e->frame_offset,
        (unsigned)f->ts_ms, (unsigned)!!(f->flags & BB_F_SLAVE_ON), f->slave_temp,
        (unsigned)f->slave_humi, (unsigned)f->slave_up, f->chip_temp, (unsigned)f->rtt_ms,
        (unsigned)f->flags, f->ax, f->ay, f->az, f->gx, f->gy, f->gz);
}

static int decode_frame(const int64_t *v, bb_frame_t *f)
{
    if (v[0] < 0 || v[0] > UINT32_MAX || (v[1] != 0 && v[1] != 1) ||
        v[2] < INT16_MIN || v[2] > INT16_MAX || v[3] < 0 || v[3] > 10000 ||
        v[4] < 0 || v[4] > UINT32_MAX || v[5] < INT16_MIN || v[5] > INT16_MAX ||
        v[6] < 0 || v[6] > UINT16_MAX || v[7] < 0 || v[7] > 63 || !!(v[7] & 1) != v[1]) return -1;
    for (int i = 8; i < 14; i++) if (v[i] < INT16_MIN || v[i] > INT16_MAX) return -1;
    memset(f, 0, sizeof(*f)); f->ts_ms = v[0]; f->slave_temp = v[2]; f->slave_humi = v[3];
    f->slave_up = v[4]; f->chip_temp = v[5]; f->rtt_ms = v[6]; f->flags = v[7];
    f->ax = v[8]; f->ay = v[9]; f->az = v[10]; f->gx = v[11]; f->gy = v[12]; f->gz = v[13];
    return 0;
}

static int decode_event(const char *line, uint32_t session, uint32_t run, bb_event_t *e)
{
    int64_t v[18];
    if (numbers(line, "E,", v, 18) || v[0] < 1 || v[0] > UINT32_MAX || v[1] < 0 ||
        v[1] > UINT32_MAX || v[2] < 0 || v[2] > BB_EVT_VIB_OK || v[3] < 0 || v[3] > BB_STORE_FRAME_MAX + 128) return -1;
    memset(e, 0, sizeof(*e));
    if (decode_frame(v + 4, &e->frame)) return -1;
    e->id = v[0]; e->ts_ms = v[1]; e->type = v[2]; e->frame_offset = v[3];
    e->session_id = session; e->run_id = run; e->disk_source = 1; return 0;
}

int bb_store_events(const char *root, bb_event_t *out, int capacity)
{
    if (!out || capacity < 1 || capacity > BB_EVENT_RING_LEN || store_lock()) return -RT_EINVAL;
    uint32_t ids[BB_STORE_KEEP], maximum; int total = 0;
    int count = collect_ids(root, ids, BB_STORE_KEEP, &maximum);
    if (count < 0) { store_unlock(); return -RT_ERROR; }
    for (int i = count - 1; i >= 0; i--) {
        char path[80], line[256]; uint32_t run;
        if (make_path(path, sizeof(path), root, 'e', ids[i])) continue;
        int fd = open(path, O_RDONLY); if (fd < 0) continue;
        line_reader_t reader;
        if (file_header(fd, 'e', ids[i], &run, NULL) || reader_init(&reader, fd, 0)) { close(fd); continue; }
        int rc;
        while ((rc = reader_line(&reader, line, sizeof(line))) > 0) {
            bb_event_t event;
            if (rc != 1 || decode_event(line, ids[i], run, &event)) continue;
            if (total == capacity) { memmove(out, out + 1, (capacity - 1) * sizeof(*out)); total--; }
            out[total++] = event;
        }
        close(fd);
        if (rc < 0) { store_unlock(); return -RT_ERROR; }
    }
    store_unlock(); return total;
}

static int read_window_file(const char *root, uint32_t id, const bb_event_t *e,
                            int which, bb_frame_t *out, int count, int capacity)
{
    char path[80], line[192]; uint32_t run;
    if (make_path(path, sizeof(path), root, 'f', id)) return count;
    int fd = open(path, O_RDONLY); if (fd < 0) return which == 0 ? -RT_ERROR : count;
    if (file_header(fd, 'f', id, &run, NULL) || run != e->run_id) { close(fd); return which == 0 ? -RT_ERROR : count; }
    off_t end = lseek(fd, 0, SEEK_END), start = 0;
    if (which < 0) start = end > 65536 ? end - 65536 : 0;
    else if (!which) start = e->frame_offset > 65536 ? e->frame_offset - 65536 : 0;
    line_reader_t reader;
    if (reader_init(&reader, fd, start)) { close(fd); return -RT_ERROR; }
    if (start) reader_line(&reader, line, sizeof(line)); /* Skip a possible partial first line. */
    int rc;
    while ((rc = reader_line(&reader, line, sizeof(line))) > 0) {
        int64_t v[14]; bb_frame_t frame;
        if (rc != 1 || numbers(line, "", v, 14) || decode_frame(v, &frame)) continue;
        int32_t delta = (int32_t)(frame.ts_ms - e->ts_ms);
        if (delta < -(int32_t)BB_HISTORY_PRE_MS) continue;
        if (delta > (int32_t)BB_HISTORY_POST_MS) break;
        if (count && (int32_t)(frame.ts_ms - out[count - 1].ts_ms) <= 0) continue;
        if (count == capacity) { close(fd); return -RT_ENOMEM; }
        out[count++] = frame;
    }
    close(fd); return rc < 0 ? -RT_ERROR : count;
}

int bb_store_window(const char *root, const bb_event_t *e, bb_frame_t *out, int capacity)
{
    if (!e || !out || capacity < 1 || e->disk_source != 1 || !e->session_id || store_lock()) return -RT_EINVAL;
    uint32_t ids[BB_STORE_KEEP], maximum; int count = 0;
    int sessions = collect_ids(root, ids, BB_STORE_KEEP, &maximum), at = -1;
    for (int i = 0; i < sessions; i++) if (ids[i] == e->session_id) at = i;
    if (at < 0) { store_unlock(); return -RT_ERROR; }
    if (at + 1 < sessions) count = read_window_file(root, ids[at + 1], e, -1, out, count, capacity);
    if (count >= 0) count = read_window_file(root, ids[at], e, 0, out, count, capacity);
    if (count >= 0 && at > 0) count = read_window_file(root, ids[at - 1], e, 1, out, count, capacity);
    store_unlock(); return count;
}

int bb_history_events(bb_event_t *out, int n) { return bb_store_events(BB_HISTORY_ROOT, out, n); }
int bb_history_window(const bb_event_t *e, bb_frame_t *out, int n) { return bb_store_window(BB_HISTORY_ROOT, e, out, n); }
