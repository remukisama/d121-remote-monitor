/* CRC journal on NOR; no filesystem operation runs in the collector or GUI. */
#include "cloud_outbox.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define MAGIC 0x434f4231U
typedef struct {uint32_t magic,kind,length,crc;} header_t;
static uint32_t crc_step(uint32_t c,const void *buffer,unsigned n)
{
    const unsigned char *p=buffer;
    while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320U:0);}
    return c;
}
static uint32_t checksum(uint32_t kind,uint32_t n,const void *p)
{
    uint32_t c=crc_step(0xffffffffU,&kind,4);c=crc_step(c,&n,4);
    return ~crc_step(c,p,n);
}
static int write_full(int fd,const void *p,unsigned n)
{
    const char *s=p;
    while(n){int got=write(fd,s,n);if(got<0&&(errno==EINTR||errno==-EINTR))continue;if(got<=0)return -1;s+=got;n-=got;}
    return 0;
}
static int read_full(int fd,void *p,unsigned n)
{
    char *s=p;unsigned used=0;
    while(used<n){int got=read(fd,s+used,n-used);if(got<0&&(errno==EINTR||errno==-EINTR))continue;if(got<0)return -1;if(!got)break;used+=(unsigned)got;}
    return (int)used;
}
static int entry(int fd,unsigned kind,const void *p,unsigned n)
{
    header_t h={MAGIC,kind,n,checksum(kind,n,p)};
    return write_full(fd,&h,sizeof(h))||write_full(fd,p,n)?-1:0;
}
static void key(const cloud_record_t *r,uint32_t k[4]){k[0]=r->boot;k[1]=r->collection;k[2]=r->node;k[3]=r->id;}
static int find(const cloud_outbox_t *q,const uint32_t k[4])
{
    for(unsigned i=0;i<q->count;i++){uint32_t a[4];key(&q->items[i],a);if(!memcmp(a,k,16))return (int)i;}
    return -1;
}
static int valid(const cloud_record_t *r)
{
    return r->boot && (r->node==1||r->node==2) && r->id &&
        memchr(r->before,0,sizeof(r->before)) && memchr(r->motion,0,sizeof(r->motion)) && memchr(r->diagnostic,0,sizeof(r->diagnostic));
}
int cloud_outbox_open(cloud_outbox_t *q,const char *path)
{
    if(!q||!path||strlen(path)+5>=sizeof(q->path))return -1;
    memset(q,0,sizeof(*q));strcpy(q->path,path);
    int fd=open(path,O_RDWR);
    /* DFS uses negative errno; host POSIX uses positive errno. */
    if(fd<0){if(errno==ENOENT||errno==-ENOENT){q->ready=1;return 0;}return -1;}
    struct stat st;int result=-1;
    if(fstat(fd,&st)||st.st_size<0||st.st_size>CLOUD_OUTBOX_LIMIT)goto done;
    unsigned offset=0;
    while(offset<(unsigned)st.st_size){
        header_t h;int n=read_full(fd,&h,sizeof(h));
        if(n<0)goto done;
        if((unsigned)n!=sizeof(h))goto torn;
        if(h.magic!=MAGIC || !((h.kind==1&&h.length==sizeof(cloud_record_t))||(h.kind==2&&h.length==16)))goto done;
        cloud_record_t r;memset(&r,0,sizeof(r));
        n=read_full(fd,&r,h.length);if(n<0)goto done;if((unsigned)n!=h.length)goto torn;
        if(checksum(h.kind,h.length,&r)!=h.crc)goto done; /* Corruption is never silently discarded. */
        uint32_t k[4];if(h.kind==1){if(!valid(&r))goto done;key(&r,k);}else memcpy(k,&r,16);
        int i=find(q,k);
        if(h.kind==1){
            if(i>=0){if(memcmp(&q->items[i],&r,sizeof(r)))goto done;}
            else {if(q->count==CLOUD_OUTBOX_CAP)goto done;q->items[q->count++]=r;}
        }else if(i>=0){memmove(&q->items[i],&q->items[i+1],(q->count-i-1)*sizeof(r));q->count--;}
        offset+=sizeof(h)+h.length;continue;
torn:
        /* Preserve every complete committed prefix; remove only an incomplete append tail. */
        if(ftruncate(fd,offset)||fsync(fd))goto done;
        q->repaired++;break;
    }
    q->bytes=offset;q->ready=1;result=0;
done:
    if(close(fd)<0){q->ready=0;result=-1;}return result;
}
static int compact(cloud_outbox_t *q)
{
    char temporary[132];snprintf(temporary,sizeof(temporary),"%s.tmp",q->path);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
    int ok=1;
    for(unsigned i=0;i<q->count;i++)if(entry(fd,1,&q->items[i],sizeof(cloud_record_t))){ok=0;break;}
    if(ok&&fsync(fd))ok=0;
    if(close(fd)<0)ok=0;
    if(!ok){unlink(temporary);return -1;}
    if(rename(temporary,q->path))return -1;
    q->bytes=q->count*(sizeof(header_t)+sizeof(cloud_record_t));return 0;
}
static int append(cloud_outbox_t *q,unsigned kind,const void *p,unsigned n)
{
    unsigned total=sizeof(header_t)+n;
    if(!q->ready)return -1;
    if(q->bytes+total>CLOUD_OUTBOX_LIMIT && compact(q))return -1;
    int fd=open(q->path,O_WRONLY|O_CREAT,0600);if(fd<0)return -1;
    off_t previous=lseek(fd,0,SEEK_END);
    if(previous<0 || (unsigned)previous!=q->bytes){close(fd);q->ready=0;return -1;}
    int ok=!entry(fd,kind,p,n) && fsync(fd)==0;
    if(!ok && (ftruncate(fd,previous)||fsync(fd)))q->ready=0;
    if(close(fd)<0){q->ready=0;ok=0;} /* Re-open before retrying an uncertain close. */
    if(!ok)return -1;
    q->bytes+=total;return 0;
}
int cloud_outbox_push(cloud_outbox_t *q,const cloud_record_t *item)
{
    if(!q||!item||!valid(item))return -1;
    uint32_t k[4];key(item,k);int i=find(q,k);
    if(i>=0)return memcmp(&q->items[i],item,8*sizeof(uint32_t))?-1:0; /* Keep the first durable context on retries. */
    if(q->count==CLOUD_OUTBOX_CAP)return -2;
    if(append(q,1,item,sizeof(*item)))return -1;
    q->items[q->count++]=*item;return 0;
}
const cloud_record_t *cloud_outbox_peek(const cloud_outbox_t *q){return q&&q->ready&&q->count?&q->items[0]:NULL;}
int cloud_outbox_pop(cloud_outbox_t *q,const cloud_record_t *expected)
{
    if(!q||!expected||!q->ready||!q->count||memcmp(&q->items[0],expected,sizeof(*expected)))return -1;
    uint32_t k[4];key(expected,k);if(append(q,2,k,sizeof(k)))return -1;
    memmove(q->items,q->items+1,(q->count-1)*sizeof(*expected));q->count--;return 0;
}
