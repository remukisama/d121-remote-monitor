#ifndef BB_STORE_H
#define BB_STORE_H
#include "bb.h"
#define BB_STORE_KEEP 16
#define BB_STORE_FRAME_MAX (1024U * 1024U)
#define BB_STORE_EVENT_MAX (128U * 1024U)
typedef struct {
    int frame_fd, event_fd;
    rt_uint32_t id, run_id, frame_bytes, event_bytes, prune_errors;
} bb_disk_files_t;
int bb_store_new(const char *root, rt_uint32_t run_id, rt_uint8_t addr, bb_disk_files_t *files);
int bb_store_event_line(char *line, int size, const bb_event_t *event);
int bb_store_events(const char *root, bb_event_t *out, int n);
int bb_store_window(const char *root, const bb_event_t *event, bb_frame_t *out, int capacity);
#endif
