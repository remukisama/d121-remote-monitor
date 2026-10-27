#include "protocol.h"
#include "metrics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned feed(struct ls_parser *p,const uint8_t *b,size_t n,uint64_t *now,struct ls_frame *f){
    unsigned count=0;
    for(size_t i=0;i<n;i++){int ready=ls_feed(p,b[i],(*now)++,f);while(ready){count++;ready=ls_next(p,f);}}
    return count;
}
int main(void){
    assert(ls_crc((const uint8_t *)"123456789",9)==0x4b37);
    struct ls_parser p={0};struct ls_frame f;uint8_t b[73],payload[64];uint64_t now=0;
    for(unsigned i=0;i<64;i++)payload[i]=(uint8_t)i;
    size_t n=ls_pack(b,2,3,payload,64);assert(n==73);
    assert(feed(&p,b,31,&now,&f)==0);assert(feed(&p,b+31,n-31,&now,&f)==1);
    assert(f.addr==2 && f.cmd==3 && f.len==64 && !memcmp(f.payload,payload,64));
    n=ls_pack(b,2,4,NULL,0);uint8_t aa=0xaa;
    assert(feed(&p,&aa,1,&now,&f)==0);assert(feed(&p,b,n,&now,&f)==1);
    b[5]^=1;assert(feed(&p,b,n,&now,&f)==0);assert(p.crc_errors==1);
    n=ls_pack(b,2,5,NULL,0);assert(feed(&p,b,n,&now,&f)==1);
    assert(feed(&p,b,n,&now,&f)==1);assert(feed(&p,b,n,&now,&f)==1);
    assert(feed(&p,b,4,&now,&f)==0);now+=101;ls_expire(&p,now);assert(p.partial_timeouts==1);
    assert(feed(&p,b,n,&now,&f)==1);
    uint8_t invalid[]={0xaa,0x55,2,1,65};assert(feed(&p,invalid,5,&now,&f)==0);
    assert(feed(&p,b,n,&now,&f)==1);assert(p.frame_errors>=1);
    n=ls_pack(b,2,1,NULL,0);b[n-1]=0;assert(feed(&p,b,n,&now,&f)==0);
    n=ls_pack(b,2,4,NULL,0);assert(feed(&p,b,n,&now,&f)==1);
    assert(ls_pack(b,2,4,payload,65)==0);assert(ls_pack(b,2,4,NULL,1)==0);
    uint32_t state=0x12345678;
    for(unsigned i=0;i<100000;i++){state=state*1664525U+1013904223U;ls_feed(&p,(uint8_t)(state>>24),now++,&f);assert(p.used<=73);}
    struct ls_cpu cpu={0};uint16_t percent=9999,load=0;
    assert(!ls_parse_cpu("cpu 100 0 0 100 0 0 0 0 999 999",&cpu,&percent));
    assert(ls_parse_cpu("cpu 120 0 0 180 0 0 0 0 9999 9999",&cpu,&percent));assert(percent==2000);
    assert(!ls_parse_cpu("cpu 1 0 0 1",&cpu,&percent));
    assert(ls_parse_cpu("cpu 2 0 0 1",&cpu,&percent));assert(percent==10000);
    assert(!ls_parse_cpu("cpu 2 0 0 1",&cpu,&percent));
    assert(!ls_parse_cpu("bad",&cpu,&percent));assert(!cpu.ready);
    uint32_t total=0,available=0;
    assert(ls_parse_memory("MemTotal: 387616 kB\nMemFree: 265408 kB\nMemAvailable: 314224 kB\n",&total,&available));
    assert(total==387616 && available==314224);
    assert(!ls_parse_memory("MemTotal: 100 kB\n",&total,&available));
    assert(!ls_parse_memory("MemTotal: 100 kB\nMemAvailable: 101 kB\n",&total,&available));
    assert(!ls_parse_memory("MemTotal: 0 kB\nMemAvailable: 0 kB\n",&total,&available));
    assert(ls_parse_load("0.123 0.01 0.01",&load));assert(load==123);
    assert(!ls_parse_load("nan",&load));assert(!ls_parse_load("-1",&load));assert(!ls_parse_load("1xyz",&load));
    assert(ls_parse_load("100.0",&load));assert(load==65535);
    puts("UNIT_PASS parser fragmentation/resync/CRC/tail/expiry/bounds + metrics validity");
    return 0;
}
