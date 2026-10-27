#include "protocol.h"
#include <string.h>
void ls_put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
void ls_put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
uint16_t ls_crc(const uint8_t *p,size_t n) {
    uint16_t c=0xffff;
    for(size_t i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c&1)?(c>>1)^0xa001:c>>1;}
    return c;
}
size_t ls_pack(uint8_t *b,uint8_t addr,uint8_t cmd,const uint8_t *p,size_t n) {
    if(n>64 || (n && !p))return 0;
    b[0]=0xaa;b[1]=0x55;b[2]=addr;b[3]=cmd;b[4]=(uint8_t)n;
    if(n)memcpy(b+5,p,n);
    ls_put16(b+5+n,ls_crc(b+2,n+3));b[n+7]=13;b[n+8]=10;
    return n+9;
}
static void drop(struct ls_parser *p,size_t n){p->used-=n;memmove(p->bytes,p->bytes+n,p->used);}
void ls_expire(struct ls_parser *p,uint64_t now) {
    if(p->used && now-p->last_ms>100){p->used=0;p->partial_timeouts++;}
}
int ls_feed(struct ls_parser *p,uint8_t b,uint64_t now,struct ls_frame *f) {
    ls_expire(p,now);p->last_ms=now;
    if(p->used==sizeof(p->bytes)){drop(p,1);p->frame_errors++;}
    p->bytes[p->used++]=b;
    return ls_next(p,f);
}
int ls_next(struct ls_parser *p,struct ls_frame *f) {
    while(p->used){
        if(p->bytes[0]!=0xaa){drop(p,1);p->junk_bytes++;continue;}
        if(p->used<2)break;
        if(p->bytes[1]!=0x55){drop(p,1);p->junk_bytes++;continue;}
        if(p->used<5)break;
        size_t n=p->bytes[4];
        if(n>64){drop(p,1);p->frame_errors++;continue;}
        if(p->used<n+9)break;
        if(p->bytes[n+7]!=13 || p->bytes[n+8]!=10){drop(p,1);p->frame_errors++;continue;}
        uint16_t c=ls_crc(p->bytes+2,n+3);
        if(p->bytes[n+5]!=(uint8_t)c || p->bytes[n+6]!=(uint8_t)(c>>8)){drop(p,1);p->crc_errors++;continue;}
        f->addr=p->bytes[2];f->cmd=p->bytes[3];f->len=(uint8_t)n;
        if(n)memcpy(f->payload,p->bytes+5,n);
        drop(p,n+9);return 1;
    }
    return 0;
}
