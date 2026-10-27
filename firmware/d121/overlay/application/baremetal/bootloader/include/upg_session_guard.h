#ifndef UPG_SESSION_GUARD_H
#define UPG_SESSION_GUARD_H
#include <stdint.h>
#include <limits.h>
typedef struct {uint64_t end_ms;int armed;} upg_session_guard_t;
typedef struct {uint32_t length;unsigned arg_bytes,data_bytes;int bad;} upg_end_input_t;
static inline void upg_guard_activity(upg_session_guard_t *g){g->armed=0;}
static inline void upg_guard_end(upg_session_guard_t *g,uint64_t now,int valid){g->end_ms=now;g->armed=valid;}
static inline int upg_guard_elapsed(const upg_session_guard_t *g,uint64_t now,int idle)
{
    if(!g->armed || !idle || now<g->end_ms)return -1;
    uint64_t passed=now-g->end_ms;return passed>INT_MAX?INT_MAX:(int)passed;
}
/* End command has a little-endian length word followed by 32 reserved bytes. */
static inline unsigned upg_end_feed(upg_end_input_t *s,const uint8_t *buf,unsigned n)
{
    unsigned used=0;
    while(used<n && s->arg_bytes<4)s->length|=(uint32_t)buf[used++]<<(8*s->arg_bytes++);
    if(s->arg_bytes==4 && s->length!=32){s->bad=1;return n;}
    while(used<n && s->data_bytes<32){used++;s->data_bytes++;}
    return used;
}
static inline int upg_end_valid(const upg_end_input_t *s){return !s->bad && s->arg_bytes==4 && s->data_bytes==32;}
#endif
