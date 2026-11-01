/* Production journal exercised on POSIX files, with commit faults.
 * Migrated from the 2026-10-13 host regression; no board or NOR/DFS test.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include "cloud_outbox.h"
static int fail_sync;
static int negative_errno;
int __real_open(const char *path,int flags,...);
int __wrap_open(const char *path,int flags,...)
{
 mode_t mode=0;
 if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
 int fd=__real_open(path,flags,mode);
 if(fd<0&&negative_errno)errno=-errno;
 return fd;
}
int __real_fsync(int fd);
int __wrap_fsync(int fd){if(fail_sync){fail_sync--;errno=EIO;return -1;}return __real_fsync(fd);}
static cloud_record_t record(unsigned id)
{
 cloud_record_t r;memset(&r,0,sizeof(r));r.boot=7;r.collection=9;r.node=1;r.run=4;r.session=8;r.id=id;r.type=1;r.ts=id*1000;
 strcpy(r.before,"{\"v\":5,\"d\":[7,9,10,1,[1,1,33,0,0],[1,1,15,0,0,0,0]]}");return r;
}
int main(void)
{
 char dir[]="/tmp/d121-outbox-XXXXXX";assert(mkdtemp(dir));char path[150];snprintf(path,sizeof(path),"%s/journal",dir);
 cloud_outbox_t *q=calloc(1,sizeof(*q)),*reopen=calloc(1,sizeof(*q));assert(q&&reopen);
 assert(!cloud_outbox_open(q,path));
 negative_errno=1;assert(!cloud_outbox_open(q,path)&&q->ready);negative_errno=0;
 cloud_record_t r=record(1),second=record(2);
 assert(!cloud_outbox_push(q,&r));assert(!cloud_outbox_push(q,&second));
 assert(!cloud_outbox_open(reopen,path));assert(reopen->count==2&&reopen->items[0].boot==7);
 assert(cloud_outbox_pop(reopen,&second));assert(reopen->count==2);
 fail_sync=1;assert(cloud_outbox_pop(q,&r));assert(q->count==2);
 assert(!cloud_outbox_open(reopen,path)&&reopen->count==2); /* No durable ACK, no deletion. */
 assert(!cloud_outbox_pop(q,&r));assert(!cloud_outbox_open(reopen,path)&&reopen->count==1&&reopen->items[0].id==2);
 r=record(3);fail_sync=1;assert(cloud_outbox_push(q,&r));assert(q->count==1);
 assert(!cloud_outbox_open(reopen,path)&&reopen->count==1); /* Failed append rolled back. */
 assert(!cloud_outbox_push(q,&r));unsigned good_bytes=q->bytes;
 int fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0&&write(fd,"bad",3)==3);close(fd);
 assert(!cloud_outbox_open(reopen,path)&&reopen->count==2&&reopen->bytes==good_bytes&&reopen->repaired==1);
 for(unsigned id=4;id<258;id++){r=record(id);assert(!cloud_outbox_push(reopen,&r));}
 r=record(258);assert(cloud_outbox_push(reopen,&r)==-2&&reopen->count==256);
 assert(!cloud_outbox_open(q,path)&&q->count==256); /* Full refuses new records, preserves old ones. */
 while(q->count){r=*cloud_outbox_peek(q);assert(!cloud_outbox_pop(q,&r));}
 for(unsigned i=300;i<1300;i++){r=record(i);assert(!cloud_outbox_push(q,&r));assert(!cloud_outbox_pop(q,&r));}
 assert(q->bytes<CLOUD_OUTBOX_LIMIT);assert(!cloud_outbox_open(reopen,path)&&reopen->count==0);
 r=record(1400);assert(!cloud_outbox_push(q,&r));
 fd=open(path,O_RDWR);assert(fd>=0);unsigned char b;assert(lseek(fd,20,SEEK_SET)==20&&read(fd,&b,1)==1);b^=1;
 assert(lseek(fd,20,SEEK_SET)==20&&write(fd,&b,1)==1);close(fd);
 assert(cloud_outbox_open(reopen,path)&&!reopen->ready); /* Complete corrupt records fail closed. */
 assert(cloud_outbox_push(reopen,&second));assert(unlink(path)==0);assert(rmdir(dir)==0);free(q);free(reopen);
 puts("PASS durable outbox: reboot replay, original identity, failed commit/ACK, torn append, full queue, compaction, corruption refusal");
}
