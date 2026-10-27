/* SPDX-License-Identifier: Apache-2.0
 * Portable RTU ADU helpers. Canonical copy: D:/xuantie/protocol/modbus_rtu.h.
 * Serial framing and coherent register snapshots belong to the platform port.
 */
#ifndef D121_MODBUS_RTU_H
#define D121_MODBUS_RTU_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define DMB_ADU_MAX 256
#define DMB_STAR_REGS 20
#define DMB_SYSTEM_REGS 34
#define DMB_MAP_VERSION 0x0100
typedef int (*dmb_read_fn)(void *,uint8_t,uint16_t,uint16_t,uint16_t *);
typedef int (*dmb_write_fn)(void *,uint16_t,uint16_t);
static inline uint16_t dmb_get16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]<<8|p[1]);}
static inline uint32_t dmb_get32(const uint8_t *p){return (uint32_t)dmb_get16(p)<<16|dmb_get16(p+2);}
static inline void dmb_put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static inline void dmb_regs32(uint16_t *p,uint32_t v){p[0]=(uint16_t)(v>>16);p[1]=(uint16_t)v;}
static inline uint16_t dmb_crc(const uint8_t *p,size_t n){
    uint16_t crc=0xffff;
    for(size_t i=0;i<n;i++){crc^=p[i];for(unsigned j=0;j<8;j++)crc=crc&1?(crc>>1)^0xa001:crc>>1;}
    return crc;
}
static inline size_t dmb_finish(uint8_t *p,size_t n){
    uint16_t crc=dmb_crc(p,n);p[n]=(uint8_t)crc;p[n+1]=(uint8_t)(crc>>8);return n+2;
}
static inline int dmb_valid(const uint8_t *p,size_t n){
    return p && n>=4 && n<=DMB_ADU_MAX &&
        dmb_crc(p,n-2)==(uint16_t)(p[n-2]|(uint16_t)p[n-1]<<8);
}
static inline size_t dmb_request(uint8_t *p,uint8_t unit,uint8_t function,uint16_t start,uint16_t value){
    if(!p || unit>247 || (function!=3 && function!=4 && function!=6))return 0;
    p[0]=unit;p[1]=function;dmb_put16(p+2,start);dmb_put16(p+4,value);return dmb_finish(p,6);
}
static inline size_t dmb_server(const uint8_t *request,size_t n,uint8_t unit,void *ctx,
                               dmb_read_fn read_regs,dmb_write_fn write_reg,uint8_t *reply,size_t capacity){
    if(!unit || unit>247 || !dmb_valid(request,n) || request[0]>247 ||
       (request[0]!=unit && request[0]!=0) || request[1]&0x80)return 0;
    int broadcast=request[0]==0,exception=0;
    uint8_t function=request[1];size_t length=0;
    if(broadcast && function!=6)return 0;
    if(!broadcast && (!reply || capacity<5))return 0;
    if(function==3 || function==4){
        if(n!=8)exception=3;
        else {
            uint16_t start=dmb_get16(request+2),count=dmb_get16(request+4),values[125];
            if(!count || count>125)exception=3;
            else if((uint32_t)start+count>65536U)exception=2;
            else if(capacity<(size_t)count*2+5)return 0;
            else if(!read_regs)exception=1;
            else if(!(exception=read_regs(ctx,function,start,count,values))){
                reply[2]=(uint8_t)(count*2);
                for(unsigned i=0;i<count;i++)dmb_put16(reply+3+i*2,values[i]);
                length=3+(size_t)count*2;
            }
        }
    }else if(function==6){
        if(n!=8)exception=3;
        else if(!broadcast && capacity<8)return 0;
        else if(!write_reg)exception=1;
        else if(!(exception=write_reg(ctx,dmb_get16(request+2),dmb_get16(request+4)))){
            if(!broadcast){memcpy(reply,request,6);length=6;}
        }
    }else exception=1;
    if(broadcast)return 0;
    reply[0]=unit;reply[1]=function;
    if(exception){reply[1]|=0x80;reply[2]=(uint8_t)exception;length=3;}
    return dmb_finish(reply,length);
}
#endif
