#define _POSIX_C_SOURCE 200809L
#include "metrics.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint64_t ls_monotonic_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)){perror("CLOCK_MONOTONIC");abort();}
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
int ls_parse_cpu(const char *s,struct ls_cpu *prev,uint16_t *out){
    uint64_t v[8]={0},total=0;
    int n=sscanf(s,"cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64,
                 &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);
    if(n<4){prev->ready=0;return 0;}
    for(int i=0;i<n;i++){if(UINT64_MAX-total<v[i]){prev->ready=0;return 0;}total+=v[i];}
    uint64_t idle=v[3]+v[4];
    int valid=prev->ready && total>prev->total && idle>=prev->idle;
    if(valid){uint64_t dt=total-prev->total,di=idle-prev->idle;
        if(di>dt)valid=0;
        else *out=(uint16_t)(((double)(dt-di)*10000.0/(double)dt)+0.5);
    }
    prev->total=total;prev->idle=idle;prev->ready=1;return valid;
}
int ls_parse_memory(const char *s,uint32_t *total,uint32_t *available){
    uint64_t t=0,a=0;int have_t=0,have_a=0;
    for(const char *p=s;*p;){
        char key[64],unit[8];uint64_t value;
        if(sscanf(p,"%63s %"SCNu64" %7s",key,&value,unit)==3 && !strcmp(unit,"kB")){
            if(!strcmp(key,"MemTotal:")){t=value;have_t=1;}
            if(!strcmp(key,"MemAvailable:")){a=value;have_a=1;}
        }
        const char *next=strchr(p,'\n');if(!next)break;p=next+1;
    }
    if(!have_t || !have_a || !t || a>t || t>UINT32_MAX)return 0;
    *total=(uint32_t)t;*available=(uint32_t)a;return 1;
}
int ls_parse_load(const char *s,uint16_t *out){
    char *end;errno=0;double v=strtod(s,&end);
    if(end==s || errno || !isfinite(v) || v<0 || (*end && *end!=' ' && *end!='\n' && *end!='\t'))return 0;
    if(v>65.535)v=65.535;
    *out=(uint16_t)(v*1000.0+0.5);return 1;
}
static int read_text(const char *path,char *b,size_t size){
    b[0]=0;FILE *f=fopen(path,"r");if(!f)return 0;
    size_t n=fread(b,1,size-1,f);int ok=!ferror(f) && n<size-1;
    fclose(f);b[n]=0;return ok;
}
static int read_line(const char *path,char *b,size_t size){
    b[0]=0;FILE *f=fopen(path,"r");if(!f)return 0;
    int ok=fgets(b,(int)size,f)!=NULL && !ferror(f);fclose(f);return ok;
}
void ls_collect(struct ls_metrics *m,struct ls_cpu *cpu,uint32_t sequence){
    char text[8192];memset(m,0,sizeof(*m));m->sequence=sequence;
    if(read_line("/proc/stat",text,sizeof(text))){
        if(ls_parse_cpu(text,cpu,&m->cpu_centi))m->valid|=LS_VALID_CPU;
    }else cpu->ready=0;
    if(read_text("/proc/meminfo",text,sizeof(text)) && ls_parse_memory(text,&m->total_kib,&m->available_kib))m->valid|=LS_VALID_MEMORY;
    if(read_text("/proc/loadavg",text,sizeof(text)) && ls_parse_load(text,&m->load_milli))m->valid|=LS_VALID_LOAD;
    struct timespec t;
    if(!clock_gettime(CLOCK_BOOTTIME,&t)){m->uptime_s=(uint32_t)t.tv_sec;m->valid|=LS_VALID_UPTIME;}
    m->sampled_ms=ls_monotonic_ms();
}
