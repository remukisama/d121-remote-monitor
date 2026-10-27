/* Exercise production setup/send/cleanup. Only GPIO hardware ioctls are simulated. */
#define main slave_program_main
#include "../main.c"
#undef main
#include <assert.h>
#include <pty.h>
#include <stdarg.h>

static char test_lock[256];
static int directions[32], direction_count, exclusive_calls, release_calls, forced_busy;
int __real_open(const char *path,int flags,...);
int __real_ioctl(int fd,unsigned long request,...);
int __wrap_open(const char *path,int flags,...){
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    if(!strcmp(path,"/run/loongson-slave2.lock"))path=test_lock;
    if(!strcmp(path,"/dev/gpiochip5"))path="/dev/null";
    return __real_open(path,flags,mode);
}
int __wrap_ioctl(int fd,unsigned long request,...){
    if(request==TIOCEXCL){exclusive_calls++;return __real_ioctl(fd,request);}
    if(request==TIOCNXCL){release_calls++;return __real_ioctl(fd,request);}
    va_list args;va_start(args,request);void *arg=va_arg(args,void *);va_end(args);
    if(request==GPIO_GET_CHIPINFO_IOCTL){
        struct gpiochip_info *info=arg;memset(info,0,sizeof(*info));strcpy(info->label,"GPA80");info->lines=16;return 0;
    }
    if(request==GPIO_GET_LINEHANDLE_IOCTL){
        struct gpiohandle_request *req=arg;
        assert(req->lines==1 && req->lineoffsets[0]==8 && req->flags==GPIOHANDLE_REQUEST_OUTPUT);
        req->fd=__real_open("/dev/null",O_RDONLY);return req->fd<0?-1:0;
    }
    if(request==GPIOHANDLE_GET_LINE_VALUES_IOCTL){memset(arg,0,sizeof(struct gpiohandle_data));return 0;}
    if(request==GPIOHANDLE_SET_LINE_VALUES_IOCTL){
        assert(direction_count<(int)(sizeof(directions)/sizeof(directions[0])));
        directions[direction_count++]=((struct gpiohandle_data *)arg)->values[0];return 0;
    }
    if(request==TIOCSERGETLSR){*(int *)arg=forced_busy?0:TIOCSER_TEMT;return 0;}
    if(request==TIOCOUTQ && forced_busy){*(int *)arg=1;return 0;}
    return __real_ioctl(fd,request,arg);
}
static struct link empty_link(void){
    struct link l={.uart=-1,.chip=-1,.line=-1,.lock=-1};return l;
}
int main(void){
    char directory[]="/tmp/ls-runtime-XXXXXX";assert(mkdtemp(directory));
    assert(snprintf(test_lock,sizeof(test_lock),"%s/process.lock",directory)>0);
    int master_fd,slave_fd;char uart[128];assert(!openpty(&master_fd,&slave_fd,uart,NULL,NULL));
    struct termios original,restored;assert(!tcgetattr(slave_fd,&original));
    original.c_lflag|=ECHO|ICANON;assert(!tcsetattr(slave_fd,TCSANOW,&original));
    struct link l=empty_link();assert(!link_open(&l,uart));assert(exclusive_calls==1);
    struct link second=empty_link();assert(link_open(&second,uart)<0);
    assert(second.chip<0 && second.uart<0);assert(!link_close(&second));
    uint8_t payload[]={0,1,2,3};direction_count=0;
    assert(!send_frame(&l,2,LS_CMD_ECHO|0x80,payload,sizeof(payload)));
    assert(direction_count==2 && directions[0]==1 && directions[1]==0);
    uint8_t actual[73];struct pollfd pending={master_fd,POLLIN,0};assert(poll(&pending,1,100)==1);
    ssize_t got=read(master_fd,actual,sizeof(actual));assert(got==13);
    uint8_t expected[73];assert(ls_pack(expected,2,LS_CMD_ECHO|0x80,payload,sizeof(payload))==(size_t)got);
    assert(!memcmp(actual,expected,(size_t)got));
    forced_busy=1;direction_count=0;uint64_t before=ls_monotonic_ms();
    assert(send_frame(&l,2,0x84,payload,sizeof(payload))<0);
    assert(ls_monotonic_ms()-before<500 && direction_count==2 && directions[1]==0);
    assert(l.tx_errors==1 && l.responses==1);forced_busy=0;
    int uart_fd=l.uart;l.uart=-1;direction_count=0;
    assert(send_frame(&l,2,0x84,payload,sizeof(payload))<0);
    assert(direction_count==2 && directions[0]==1 && directions[1]==0);l.uart=uart_fd;
    direction_count=0;assert(!link_close(&l));assert(direction_count==1 && directions[0]==0 && release_calls==1);
    assert(!tcgetattr(slave_fd,&restored));
    assert(restored.c_lflag==original.c_lflag && restored.c_iflag==original.c_iflag &&
           restored.c_oflag==original.c_oflag && restored.c_cflag==original.c_cflag);
    puts("RUNTIME_PASS production EN high/low on success, busy timeout and write failure; real process lock; UART exclusive request/release and termios restore (GPIO simulated)");

    struct ls_metrics m={.sequence=1,.sampled_ms=1000,.valid=15};struct ls_parser parser={0};
    uint8_t b[64];system_payload(b,2,&m,&l,&parser,1600,0,200);assert(b[2]==15);
    system_payload(b,2,&m,&l,&parser,1601,0,200);assert(b[2]==0);
    m.sequence=0;system_payload(b,2,&m,&l,&parser,1000,0,200);assert(b[2]==0);
    m.sequence=1;m.sampled_ms=ls_monotonic_ms();
    char status[256];assert(snprintf(status,sizeof(status),"%s/status.json",directory)>0);
    assert(!write_status(status,&m,&l,&parser,2,0,200));
    int prior=open(status,O_RDONLY);assert(prior>=0);struct stat prior_stat,current_stat;
    assert(!fstat(prior,&prior_stat));m.sequence=2;assert(!write_status(status,&m,&l,&parser,2,0,200));
    assert(!stat(status,&current_stat) && current_stat.st_ino!=prior_stat.st_ino);
    char old_text[2048]={0};assert(read(prior,old_text,sizeof(old_text)-1)>0);
    assert(strstr(old_text,"\"sequence\":1"));
    close(prior);close(master_fd);close(slave_fd);unlink(status);unlink(test_lock);assert(!rmdir(directory));
    puts("RUNTIME_PASS sample expiry boundary and no-sample validity; status replacement preserves prior open snapshot");
    return 0;
}
