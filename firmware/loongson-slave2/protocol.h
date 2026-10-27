#ifndef LS_PROTOCOL_H
#define LS_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define LS_MAX_PAYLOAD 64
#define LS_MAX_FRAME 73
#define LS_SYSTEM_LEN 64
#define LS_CMD_QUERY 1
#define LS_CMD_ECHO 3
#define LS_CMD_SYSTEM 4
#define LS_CMD_IDENTITY 5
struct ls_frame { uint8_t addr,cmd,len,payload[64]; };
struct ls_parser {
    uint8_t bytes[73]; size_t used; uint64_t last_ms;
    uint32_t crc_errors,frame_errors,junk_bytes,partial_timeouts;
};
uint16_t ls_crc(const uint8_t *,size_t);
size_t ls_pack(uint8_t *,uint8_t,uint8_t,const uint8_t *,size_t);
/* Feed one byte; incomplete input expires after 100ms. */
int ls_feed(struct ls_parser *,uint8_t,uint64_t,struct ls_frame *);
int ls_next(struct ls_parser *,struct ls_frame *);
void ls_expire(struct ls_parser *,uint64_t);
void ls_put16(uint8_t *,uint16_t);
void ls_put32(uint8_t *,uint32_t);
#endif
