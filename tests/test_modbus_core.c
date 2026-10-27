#include <assert.h>
#include <stdio.h>
#include "../firmware/common/modbus_rtu.h"
static unsigned reads,writes;
static int read_registers(void *ctx,uint8_t fn,uint16_t start,uint16_t count,uint16_t *out){
    (void)ctx;(void)fn;reads++;
    for(unsigned i=0;i<count;i++)out[i]=(uint16_t)(start+i);
    return 0;
}
static int write_register(void *ctx,uint16_t reg,uint16_t value){
    (void)ctx;if(reg)return 2;if(value>1)return 3;writes++;return 0;
}
int main(void){
    /* Published standard example, not packed by the implementation under test. */
    const uint8_t golden[]={1,4,2,255,255,0xb8,0x80};
    assert(dmb_valid(golden,sizeof(golden)));
    uint8_t request[256],reply[256];
    assert(dmb_request(request,1,4,0,125)==8);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==255);
    assert(reply[2]==250 && dmb_get16(reply+3+124*2)==124 && dmb_valid(reply,255));
    dmb_request(request,1,4,0,126);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[2]==3 && reads==1);
    dmb_request(request,1,4,65535,2);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[2]==2);
    dmb_request(request,1,4,0,0);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[2]==3);
    dmb_request(request,0,4,0,1);
    assert(!dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply)));
    dmb_request(request,2,4,0,1);
    assert(!dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply)));
    dmb_request(request,0,6,0,1);
    assert(!dmb_server(request,8,1,NULL,read_registers,write_register,NULL,0) && writes==1);
    dmb_request(request,1,6,0,2);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[1]==0x86 && reply[2]==3 && writes==1);
    request[7]^=1;
    assert(!dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply)));
    dmb_request(request,1,6,0,1);
    assert(!dmb_server(request,8,1,NULL,read_registers,write_register,reply,5) && writes==1);
    request[1]=0x10;dmb_finish(request,6);
    assert(dmb_server(request,8,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[1]==0x90 && reply[2]==1);
    request[1]=4;request[6]=0;dmb_finish(request,7);
    assert(dmb_server(request,9,1,NULL,read_registers,write_register,reply,sizeof(reply))==5 && reply[2]==3);
    unsigned state=17;
    for(unsigned j=0;j<100000;j++){
        unsigned n=j%257;for(unsigned i=0;i<n;i++){state=state*1664525U+1013904223U;request[i]=(uint8_t)(state>>24);}
        size_t length=dmb_server(request,n,1,NULL,read_registers,write_register,reply,sizeof(reply));
        assert(length<=256);
    }
    puts("PASS RTU known CRC vector, 125-register maximum, quantity/range/length errors, no broadcast/foreign/CRC replies, broadcast write, capacity guard, malformed-input fuzz");
    return 0;
}
