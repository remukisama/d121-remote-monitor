#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "metrics.h"
#include "modbus_rtu.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number){(void)signal_number;stopped=1;}
struct sampler {
    pthread_mutex_t mutex; pthread_cond_t condition; pthread_t thread;
    struct ls_metrics latest; unsigned interval;int quit;
};
struct link {
    int uart,chip,line,lock,saved,exclusive,test,legacy;
    struct termios original;
    uint32_t requests,responses,rejected,tx_errors,rx_bytes,tx_bytes;
};
static void *collect_thread(void *arg){
    struct sampler *s=arg;struct ls_cpu cpu={0};uint32_t sequence=0;
    for(;;){
        struct ls_metrics m;ls_collect(&m,&cpu,++sequence);
        pthread_mutex_lock(&s->mutex);s->latest=m;
        struct timespec until;clock_gettime(CLOCK_MONOTONIC,&until);
        until.tv_sec+=s->interval/1000;until.tv_nsec+=(long)(s->interval%1000)*1000000;
        if(until.tv_nsec>=1000000000L){until.tv_sec++;until.tv_nsec-=1000000000L;}
        int r=0;
        while(!s->quit && r!=ETIMEDOUT) {
            r=pthread_cond_timedwait(&s->condition,&s->mutex,&until);
            if(r && r!=ETIMEDOUT){s->quit=1;break;}
        }
        int quit=s->quit;pthread_mutex_unlock(&s->mutex);if(quit)break;
    }
    return NULL;
}
static int sampler_start(struct sampler *s,unsigned interval){
    memset(s,0,sizeof(*s));s->interval=interval;
    int r=pthread_mutex_init(&s->mutex,NULL);if(r)return r;
    pthread_condattr_t a;r=pthread_condattr_init(&a);
    if(r){pthread_mutex_destroy(&s->mutex);return r;}
    r=pthread_condattr_setclock(&a,CLOCK_MONOTONIC);
    if(!r)r=pthread_cond_init(&s->condition,&a);
    pthread_condattr_destroy(&a);
    if(r){pthread_mutex_destroy(&s->mutex);return r;}
    r=pthread_create(&s->thread,NULL,collect_thread,s);
    if(r){pthread_cond_destroy(&s->condition);pthread_mutex_destroy(&s->mutex);}
    return r;
}
static void sampler_stop(struct sampler *s){
    pthread_mutex_lock(&s->mutex);s->quit=1;pthread_cond_broadcast(&s->condition);pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread,NULL);pthread_cond_destroy(&s->condition);pthread_mutex_destroy(&s->mutex);
}
static struct ls_metrics sample_copy(struct sampler *s){
    struct ls_metrics m;pthread_mutex_lock(&s->mutex);m=s->latest;pthread_mutex_unlock(&s->mutex);return m;
}
static int en(struct link *l,int level){
    if(l->test)return 0;
    struct gpiohandle_data d;memset(&d,0,sizeof(d));d.values[0]=(uint8_t)level;
    return ioctl(l->line,GPIOHANDLE_SET_LINE_VALUES_IOCTL,&d);
}
static int gpio_open(struct link *l){
    l->chip=open("/dev/gpiochip5",O_RDONLY|O_CLOEXEC);if(l->chip<0)return -1;
    struct gpiochip_info info;memset(&info,0,sizeof(info));
    if(ioctl(l->chip,GPIO_GET_CHIPINFO_IOCTL,&info))return -1;
    if(strcmp(info.label,"GPA80") || info.lines!=16){errno=ENODEV;return -1;}
    struct gpiohandle_request req;memset(&req,0,sizeof(req));
    req.lines=1;req.lineoffsets[0]=8;req.flags=GPIOHANDLE_REQUEST_OUTPUT;
    snprintf(req.consumer_label,sizeof(req.consumer_label),"loongson-slave2-en");
    if(ioctl(l->chip,GPIO_GET_LINEHANDLE_IOCTL,&req))return -1;
    l->line=req.fd;
    struct gpiohandle_data d;memset(&d,0,sizeof(d));
    if(ioctl(l->line,GPIOHANDLE_GET_LINE_VALUES_IOCTL,&d))return -1;
    if(d.values[0]){errno=EIO;return -1;}return 0;
}
static int link_close(struct link *l){
    int failed=0;
    if(l->line>=0 && en(l,0)){perror("restore EN low");failed=1;}
    if(l->uart>=0){
        if(l->saved && tcsetattr(l->uart,TCSANOW,&l->original)){perror("restore UART");failed=1;}
        if(l->exclusive && ioctl(l->uart,TIOCNXCL)){perror("release UART");failed=1;}
        close(l->uart);
    }
    if(l->line>=0)close(l->line);
    if(l->chip>=0)close(l->chip);
    if(l->lock>=0)close(l->lock);
    return failed;
}
static int link_open(struct link *l,const char *path){
    if(!l->test){
        l->lock=open("/run/loongson-slave2.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(l->lock<0 || flock(l->lock,LOCK_EX|LOCK_NB))return -1;
        if(gpio_open(l))return -1;
    }
    l->uart=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);if(l->uart<0)return -1;
    if(ioctl(l->uart,TIOCEXCL))return -1;
    l->exclusive=1;
    if(tcgetattr(l->uart,&l->original))return -1;
    l->saved=1;
    struct termios t=l->original;cfmakeraw(&t);
    t.c_cflag&=~(CSIZE|PARENB|CSTOPB|CRTSCTS);t.c_cflag|=CS8|CLOCAL|CREAD;
    if(!l->legacy)t.c_cflag|=CSTOPB;
    t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
    if(cfsetispeed(&t,B115200) || cfsetospeed(&t,B115200) || tcsetattr(l->uart,TCSANOW,&t) || tcflush(l->uart,TCIOFLUSH))return -1;
    /* Reject unsupported drain ioctl before ever driving EN high. */
    int queued=0,status=0;
    if(ioctl(l->uart,TIOCOUTQ,&queued))return -1;
    if(!l->test && ioctl(l->uart,TIOCSERGETLSR,&status))return -1;
    return 0;
}
static int send_data(struct link *l,const uint8_t *b,size_t n){
    size_t written=0;
    if(!n || n>DMB_ADU_MAX){errno=EINVAL;return -1;}
    if(en(l,1))return -1;
    int ret=-1;uint64_t deadline=ls_monotonic_ms()+100;
    struct timespec settle={0,100000};nanosleep(&settle,NULL);
    while(written<n && !stopped && ls_monotonic_ms()<deadline){
        ssize_t w=write(l->uart,b+written,n-written);
        if(w>0)written+=(size_t)w;
        else if(w<0 && errno!=EAGAIN && errno!=EINTR)goto out;
        else {struct pollfd p={l->uart,POLLOUT,0};if(poll(&p,1,5)<0 && errno!=EINTR)goto out;}
    }
    if(written!=n){errno=stopped?ECANCELED:ETIMEDOUT;goto out;}
    while(!stopped && ls_monotonic_ms()<deadline){
        int queued=0,status=0;
        if(ioctl(l->uart,TIOCOUTQ,&queued))goto out;
        if(!l->test && ioctl(l->uart,TIOCSERGETLSR,&status))goto out;
        if(!queued && (l->test || (status&TIOCSER_TEMT))){ret=0;break;}
        struct timespec wait={0,100000};nanosleep(&wait,NULL);
    }
    if(ret)errno=stopped?ECANCELED:ETIMEDOUT;
out:;
    int saved_errno=errno;if(en(l,0)){ret=-1;saved_errno=errno;}
    errno=saved_errno;if(!ret){l->responses++;l->tx_bytes+=(uint32_t)n;}else l->tx_errors++;
    return ret;
}
static int send_frame(struct link *l,uint8_t address,uint8_t command,const uint8_t *payload,size_t length){
    uint8_t b[73];size_t n=ls_pack(b,address,command,payload,length);
    return send_data(l,b,n);
}
static uint32_t age_ms(const struct ls_metrics *m,uint64_t now){
    if(!m->sequence)return UINT32_MAX;
    uint64_t age=now-m->sampled_ms;return age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
}
static void system_payload(uint8_t b[64],uint8_t address,const struct ls_metrics *m,const struct link *l,
                           const struct ls_parser *p,uint64_t now,uint64_t started,unsigned interval){
    memset(b,0,64);uint32_t age=age_ms(m,now);
    b[0]=1;b[1]=2;b[2]=age<=interval*3?m->valid:0;b[3]=address;
    ls_put32(b+4,m->sequence);ls_put32(b+8,m->uptime_s);ls_put32(b+12,age);
    ls_put16(b+16,m->cpu_centi);ls_put16(b+18,m->load_milli);
    ls_put32(b+20,m->total_kib);ls_put32(b+24,m->available_kib);
    ls_put32(b+28,(uint32_t)((now-started)/1000));
    ls_put32(b+32,l->requests);ls_put32(b+36,l->responses);
    ls_put32(b+40,p->crc_errors);ls_put32(b+44,p->frame_errors);
    ls_put32(b+48,l->tx_errors);ls_put32(b+52,l->rx_bytes);ls_put32(b+56,l->tx_bytes);ls_put32(b+60,l->rejected);
}
static int handle(struct link *l,struct ls_parser *parser,const struct ls_frame *f,uint8_t address,
                  struct sampler *s,uint64_t started){
    if(f->addr!=address || (f->cmd&0x80))return 0;
    l->requests++;uint8_t b[64];size_t size=0;
    if(f->cmd==LS_CMD_ECHO){memcpy(b,f->payload,f->len);size=f->len;}
    else if(f->len){l->rejected++;return 0;}
    else if(f->cmd==LS_CMD_QUERY){
        struct ls_metrics m=sample_copy(s);memset(b,0,24);
        ls_put32(b,m.uptime_s);b[22]=0;b[23]=0xa1;size=24;
    }else if(f->cmd==LS_CMD_SYSTEM){
        struct ls_metrics m=sample_copy(s);
        system_payload(b,address,&m,l,parser,ls_monotonic_ms(),started,s->interval);size=64;
    }else if(f->cmd==LS_CMD_IDENTITY){
        memset(b,0,32);b[0]=1;b[1]=2;b[2]=address;b[3]=7;b[4]=1;
        memcpy(b+8,"LS2K0300-LoongOS",sizeof("LS2K0300-LoongOS")-1);size=32;
    }else{l->rejected++;return 0;}
    return send_frame(l,address,(uint8_t)(f->cmd|0x80),b,size);
}
static int read_modbus(void *context,uint8_t function,uint16_t start,uint16_t count,uint16_t *out){
    if(function!=4)return 1;
    if((unsigned)start+count>DMB_SYSTEM_REGS)return 2;
    memcpy(out,(uint16_t *)context+start,count*sizeof(*out));return 0;
}
static int handle_modbus(struct link *l,struct ls_parser *parser,const uint8_t *request,size_t length,
                         uint8_t address,struct sampler *s,uint64_t started){
    if(!dmb_valid(request,length)){
        if(length>=4)parser->crc_errors++;else parser->frame_errors++;
        return 0;
    }
    if(request[0]!=address && request[0]!=0)return 0;
    if(request[1]&0x80)return 0;
    l->requests++;
    uint16_t regs[DMB_SYSTEM_REGS]={DMB_MAP_VERSION,2,address};uint8_t reply[DMB_ADU_MAX];
    struct ls_metrics m=sample_copy(s);uint64_t now=ls_monotonic_ms();uint32_t age=age_ms(&m,now);
    regs[3]=age<=s->interval*3?m.valid:0;
    dmb_regs32(regs+4,m.sequence);dmb_regs32(regs+6,m.uptime_s);dmb_regs32(regs+8,age);
    regs[10]=m.cpu_centi;regs[11]=m.load_milli;
    dmb_regs32(regs+12,m.total_kib);dmb_regs32(regs+14,m.available_kib);
    dmb_regs32(regs+16,(uint32_t)((now-started)/1000));dmb_regs32(regs+18,l->requests);
    dmb_regs32(regs+20,l->responses);dmb_regs32(regs+22,parser->crc_errors);
    dmb_regs32(regs+24,parser->frame_errors);dmb_regs32(regs+26,l->tx_errors);
    dmb_regs32(regs+28,l->rx_bytes);dmb_regs32(regs+30,l->tx_bytes);dmb_regs32(regs+32,l->rejected);
    size_t n=dmb_server(request,length,address,regs,read_modbus,NULL,reply,sizeof(reply));
    if(n && (reply[1]&0x80))l->rejected++;
    return n?send_data(l,reply,n):0;
}
static void notify_systemd(const char *message){
    const char *path=getenv("NOTIFY_SOCKET");if(!path || !*path)return;
    struct sockaddr_un a;memset(&a,0,sizeof(a));size_t n=strlen(path);
    if(n>=sizeof(a.sun_path))return;
    a.sun_family=AF_UNIX;memcpy(a.sun_path,path,n+1);
    if(path[0]=='@')a.sun_path[0]=0;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return;
    sendto(fd,message,strlen(message),MSG_DONTWAIT,(struct sockaddr *)&a,
           (socklen_t)(offsetof(struct sockaddr_un,sun_path)+n+(path[0]=='@'?0:1)));close(fd);
}
static int json_status(FILE *f,const struct ls_metrics *m,const struct link *l,const struct ls_parser *p,
                       uint8_t address,uint64_t started,unsigned interval){
    uint64_t now=ls_monotonic_ms();uint32_t age=age_ms(m,now);
    return fprintf(f,"{\"version\":1,\"protocol\":\"%s\",\"node\":%u,\"sequence\":%u,\"valid\":%u,\"sample_age_ms\":%u,"
                   "\"uptime_s\":%u,\"process_uptime_s\":%u,\"cpu_centi_pct\":%u,\"load1_milli\":%u,"
                   "\"mem_total_kib\":%u,\"mem_available_kib\":%u,\"requests\":%u,\"responses\":%u,"
                   "\"crc_errors\":%u,\"frame_errors\":%u,\"partial_timeouts\":%u,\"junk_bytes\":%u,"
                   "\"rejected\":%u,\"tx_errors\":%u,\"rx_bytes\":%u,\"tx_bytes\":%u}\n",
                   l->legacy?"aa55":"modbus-rtu",address,m->sequence,age<=interval*3?m->valid:0,age,m->uptime_s,(uint32_t)((now-started)/1000),
                   m->cpu_centi,m->load_milli,m->total_kib,m->available_kib,l->requests,l->responses,
                   p->crc_errors,p->frame_errors,p->partial_timeouts,p->junk_bytes,l->rejected,l->tx_errors,l->rx_bytes,l->tx_bytes)<0?-1:0;
}
static int write_status(const char *path,const struct ls_metrics *m,const struct link *l,const struct ls_parser *p,
                         uint8_t address,uint64_t started,unsigned interval){
    char temp[512];if(snprintf(temp,sizeof(temp),"%s.tmp.XXXXXX",path)>=(int)sizeof(temp)){errno=ENAMETOOLONG;return -1;}
    int fd=mkstemp(temp);if(fd<0)return -1;
    if(fcntl(fd,F_SETFD,FD_CLOEXEC) || fchmod(fd,0644)){close(fd);unlink(temp);return -1;}
    FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(temp);return -1;}
    int r=json_status(f,m,l,p,address,started,interval);
    if(fclose(f))r=-1;
    if(!r && rename(temp,path))r=-1;
    if(r)unlink(temp);
    return r;
}
static int number(const char *s,unsigned min,unsigned max,unsigned *value){
    if(!*s || strspn(s,"0123456789")!=strlen(s))return -1;
    errno=0;char *end;unsigned long n=strtoul(s,&end,10);
    if(errno || *end || n<min || n>max)return -1;
    *value=(unsigned)n;return 0;
}
int main(int argc,char **argv){
    unsigned address=2,interval=1000,duration=0;int snapshot=0,release=0;
    const char *uart="/dev/ttyS1",*stats=NULL;
    struct link l={.uart=-1,.chip=-1,.line=-1,.lock=-1};
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--version")){puts("loongson-slave2 1.1.0-modbus");return 0;}
        if(!strcmp(argv[i],"--snapshot"))snapshot=1;
        else if(!strcmp(argv[i],"--release-en"))release=1;
        else if(!strcmp(argv[i],"--legacy"))l.legacy=1;
        else if(!strcmp(argv[i],"--address") && i+1<argc){if(number(argv[++i],2,247,&address))return 2;}
        else if(!strcmp(argv[i],"--sample-ms") && i+1<argc){if(number(argv[++i],200,60000,&interval))return 2;}
        else if(!strcmp(argv[i],"--duration") && i+1<argc){if(number(argv[++i],1,3600,&duration))return 2;}
        else if(!strcmp(argv[i],"--stats") && i+1<argc)stats=argv[++i];
#ifdef LS_HOST_TEST
        else if(!strcmp(argv[i],"--test-uart") && i+1<argc){uart=argv[++i];l.test=1;}
#endif
        else {fprintf(stderr,"usage: %s [--address 2..254] [--sample-ms 200..60000] [--duration 1..3600] [--stats file] [--snapshot|--release-en]\n",argv[0]);return 2;}
    }
    if(snapshot && release)return 2;
    struct sigaction action;memset(&action,0,sizeof(action));action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL)){perror("signal setup");return 1;}
    setvbuf(stdout,NULL,_IOLBF,0);uint64_t started=ls_monotonic_ms();
    struct ls_parser parser={0};
    if(snapshot){
        struct ls_cpu cpu={0};struct ls_metrics m;ls_collect(&m,&cpu,1);
        struct timespec wait={interval/1000,(long)(interval%1000)*1000000};nanosleep(&wait,NULL);
        if(stopped)return 0;
        ls_collect(&m,&cpu,2);
        return json_status(stdout,&m,&l,&parser,(uint8_t)address,started,interval)?1:0;
    }
    if(release){int r=gpio_open(&l);if(r)perror("release EN request");int close_error=link_close(&l);return r || close_error?1:0;}
    if(link_open(&l,uart)){perror("link setup");link_close(&l);return 1;}
    struct sampler s;int r=sampler_start(&s,interval);
    if(r){fprintf(stderr,"sampler setup: %s\n",strerror(r));link_close(&l);return 1;}
    printf("START address=%u uart=%s 115200/8N%d protocol=%s EN=GPIO88 sample_ms=%u\n",address,uart,l.legacy?1:2,l.legacy?"aa55":"modbus-rtu",interval);
    notify_systemd("READY=1\nSTATUS=RS485 slave ready; independent system sampling active");
    int result=0;uint64_t last_status=0,last_log=started,last_rtu=0;
    uint8_t rtu[DMB_ADU_MAX];size_t rtu_used=0;int rtu_overflow=0;
    while(!stopped && (!duration || ls_monotonic_ms()-started<(uint64_t)duration*1000)){
        struct pollfd p={l.uart,POLLIN,0};int ready=poll(&p,1,(rtu_used || rtu_overflow)?2:50);
        if(ready<0){if(errno==EINTR)continue;perror("poll");result=1;break;}
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL)){fprintf(stderr,"UART disconnected\n");result=1;break;}
        if(!l.legacy && (rtu_used || rtu_overflow) && ls_monotonic_ms()-last_rtu>=2){
            if(rtu_overflow)parser.frame_errors++;
            else if(handle_modbus(&l,&parser,rtu,rtu_used,(uint8_t)address,&s,started)){
                if(!stopped){perror("Modbus send");result=1;}goto done;
            }
            rtu_used=0;rtu_overflow=0;
        }
        if(p.revents&POLLIN){
            uint8_t b[512];ssize_t n=read(l.uart,b,sizeof(b));
            if(n<0 && errno!=EINTR && errno!=EAGAIN){perror("read");result=1;break;}
            if(n>0){
                l.rx_bytes+=(uint32_t)n;
                if(!l.legacy){
                    if((size_t)n>sizeof(rtu)-rtu_used)rtu_overflow=1;
                    else if(!rtu_overflow){memcpy(rtu+rtu_used,b,(size_t)n);rtu_used+=(size_t)n;}
                    last_rtu=ls_monotonic_ms();
                }else for(ssize_t i=0;i<n;i++){
                    struct ls_frame f;int complete=ls_feed(&parser,b[i],ls_monotonic_ms(),&f);
                    while(complete){
                        if(handle(&l,&parser,&f,(uint8_t)address,&s,started)){
                            if(!stopped){perror("send");result=1;}goto done;
                        }
                        complete=ls_next(&parser,&f);
                    }
                }
            }
        }
        uint64_t now=ls_monotonic_ms();ls_expire(&parser,now);
        if(now-last_status>=1000){
            struct ls_metrics m=sample_copy(&s);
            if(m.sequence && age_ms(&m,now)<=interval*3)notify_systemd("WATCHDOG=1");
            if(stats && write_status(stats,&m,&l,&parser,(uint8_t)address,started,interval)){perror("status write");result=1;break;}
            last_status=now;
        }
        if(now-last_log>=30000){printf("STATS requests=%u responses=%u crc=%u frame=%u tx_errors=%u\n",l.requests,l.responses,parser.crc_errors,parser.frame_errors,l.tx_errors);last_log=now;}
    }
done:
    sampler_stop(&s);
    if(link_close(&l))result=1;
    notify_systemd("STOPPING=1");
    printf("STOP exit=%d requests=%u responses=%u rejected=%u crc=%u frame=%u tx_errors=%u\n",
           result,l.requests,l.responses,l.rejected,parser.crc_errors,parser.frame_errors,l.tx_errors);
    return result;
}
