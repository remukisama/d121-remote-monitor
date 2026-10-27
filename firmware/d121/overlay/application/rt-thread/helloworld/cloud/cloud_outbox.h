#ifndef CLOUD_OUTBOX_H
#define CLOUD_OUTBOX_H
#include <stdint.h>
#define CLOUD_OUTBOX_CAP 256
#define CLOUD_CONTEXT_SIZE 112
#define CLOUD_OUTBOX_LIMIT (256U*1024U)
typedef struct {
    uint32_t boot,collection,node,run,session,id,type,ts;
    char before[CLOUD_CONTEXT_SIZE],motion[CLOUD_CONTEXT_SIZE],diagnostic[CLOUD_CONTEXT_SIZE];
} cloud_record_t;
typedef struct {
    cloud_record_t items[CLOUD_OUTBOX_CAP];
    char path[128];
    unsigned count,bytes,ready,repaired;
} cloud_outbox_t;
/* Single owner: cloud worker. Only successful fsync/atomic replacement changes RAM. */
int cloud_outbox_open(cloud_outbox_t *q,const char *path);
int cloud_outbox_push(cloud_outbox_t *q,const cloud_record_t *item);
const cloud_record_t *cloud_outbox_peek(const cloud_outbox_t *q);
int cloud_outbox_pop(cloud_outbox_t *q,const cloud_record_t *expected);
#endif
