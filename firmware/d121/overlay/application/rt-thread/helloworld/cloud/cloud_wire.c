#include "cloud_wire.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

uint32_t cloud_vibration_mg(int16_t ax, int16_t ay, int16_t az)
{
    uint64_t n=(int64_t)ax*ax+(int64_t)ay*ay+(int64_t)az*az;
    uint64_t root=0,bit=(uint64_t)1<<62;
    while(bit>n)bit>>=2;
    while(bit){
        if(n>=root+bit){n-=root+bit;root=(root>>1)+bit;}
        else root>>=1;
        bit>>=2;
    }
    int deviation=(int)root-16384;
    if(deviation<0)deviation=-deviation;
    return (uint32_t)deviation*1000/16384;
}

int cloud_config_parse(cloud_config_t *out, const char *text)
{
    cloud_config_t c;
    unsigned seen = 0;
    const char *names[] = {"ssid", "wifi_password", "host", "client_id", "username",
                          "password", "state_topic", "event_topic", "port", "tls", "baud", "qos"};
    if (!out || !text) return -1;
    memset(&c, 0, sizeof(c));
    while (*text) {
        const char *end = strchr(text, '\n'), *eq;
        char key[24], value[128];
        size_t len = end ? (size_t)(end - text) : strlen(text);
        if (!len || !(eq = memchr(text, '=', len)) || (size_t)(eq-text) >= sizeof(key)) return -1;
        size_t k = (size_t)(eq - text), v = len-k-1;
        if (!k || v >= sizeof(value)) return -1;
        memcpy(key, text, k); key[k] = 0;
        memcpy(value, eq+1, v); value[v] = 0;
        for (size_t j = 0; j < v; j++) if ((unsigned char)value[j] < 32 || value[j] == 127) return -1;
        int i;
        for (i = 0; i < 12 && strcmp(key, names[i]); i++) {}
        if (i == 12 || (seen & (1U<<i))) return -1;
        seen |= 1U<<i;
        if (i < 8) {
            char *dst[] = {c.ssid,c.wifi_password,c.host,c.client_id,c.username,c.password,c.state_topic,c.event_topic};
            size_t caps[] = {sizeof(c.ssid),sizeof(c.wifi_password),sizeof(c.host),sizeof(c.client_id),
                             sizeof(c.username),sizeof(c.password),sizeof(c.state_topic),sizeof(c.event_topic)};
            if (v >= caps[i]) return -1;
            memcpy(dst[i], value, v+1);
        } else {
            char *tail;
            for (size_t j=0; j<v; j++) if (value[j]<'0' || value[j]>'9') return -1;
            errno=0; unsigned long n=strtoul(value,&tail,10);
            if (!v || errno || *tail || n>3000000UL) return -1;
            if (i==8) c.port=(unsigned)n;
            if (i==9) c.tls=(unsigned)n;
            if (i==10) c.baud=(unsigned)n;
            if (i==11) c.qos=(unsigned)n;
        }
        text = end ? end+1 : text+len;
    }
    if ((seen != 2047 && seen != 4095) || !*c.ssid || !*c.host || !*c.client_id || !*c.state_topic || !*c.event_topic ||
        !c.port || c.port>65535 || c.tls>1 || c.qos>1 || (*c.password && !*c.username)) return -1;
    const unsigned rates[]={9600,19200,38400,57600,115200,230400,460800,921600};
    unsigned i;
    for (i=0;i<sizeof(rates)/sizeof(rates[0]) && c.baud!=rates[i];i++) {}
    if (i==sizeof(rates)/sizeof(rates[0])) return -1;
    if (strpbrk(c.state_topic,"+#") || strpbrk(c.event_topic,"+#") || !strcmp(c.state_topic,c.event_topic)) return -1;
    *out=c; return 0;
}

int cloud_quote(char *out, size_t cap, const char *text)
{
    size_t used=0;
    if (!out || !text || cap<3) return -1;
    out[used++]='"';
    for (;*text;text++) {
        unsigned char b=(unsigned char)*text;
        if (b<32 || b==127) return -1;
        if (b=='"' || b=='\\' || b==',') {
            if (used+2>=cap) return -1;
            out[used++]='\\';
        }
        if (used+1>=cap) return -1;
        out[used++]=(char)b;
    }
    if (used+2>cap) return -1;
    out[used++]='"'; out[used]=0; return (int)used;
}

static int mqtt_finish(uint8_t *out, size_t cap, uint8_t type, const uint8_t *body, size_t n)
{
    uint8_t size[4]; size_t k=0, v=n;
    if (!out || !body || n>268435455U) return -1;
    do { size[k]=(uint8_t)(v%128); v/=128; if(v)size[k]|=128; k++; } while(v);
    if (1+k+n>cap) return -1;
    out[0]=type; memcpy(out+1,size,k); memcpy(out+1+k,body,n); return (int)(1+k+n);
}
static int mqtt_string(uint8_t *buf, size_t cap, size_t *n, const char *s)
{
    if (!s) return -1;
    size_t len=strlen(s);
    if (len>65535 || *n+2+len>cap) return -1;
    buf[(*n)++]=(uint8_t)(len>>8); buf[(*n)++]=(uint8_t)len;
    memcpy(buf+*n,s,len); *n+=len; return 0;
}
int cloud_mqtt_connect(uint8_t *out, size_t cap, const cloud_config_t *c)
{
    uint8_t buf[512]={0,4,'M','Q','T','T',4,2,0,30}; size_t n=10;
    if (!c || !*c->client_id) return -1;
    if (*c->username) buf[7]|=128;
    if (*c->password) { if (!*c->username) return -1; buf[7]|=64; }
    if (mqtt_string(buf,sizeof(buf),&n,c->client_id) ||
        (*c->username && mqtt_string(buf,sizeof(buf),&n,c->username)) ||
        (*c->password && mqtt_string(buf,sizeof(buf),&n,c->password))) return -1;
    return mqtt_finish(out,cap,0x10,buf,n);
}
int cloud_mqtt_publish(uint8_t *out, size_t cap, const char *topic, const char *payload, uint16_t id, int duplicate, unsigned qos)
{
    uint8_t buf[CLOUD_PACKET_MAX]; size_t n=0;
    if (!topic || !*topic || strpbrk(topic,"+#") || !payload || qos>1 || (qos==1&&!id) || (!qos&&duplicate) ||
        mqtt_string(buf,sizeof(buf),&n,topic)) return -1;
    size_t len=strlen(payload);
    if (n+(qos?2:0)+len>sizeof(buf)) return -1;
    if(qos){buf[n++]=(uint8_t)(id>>8); buf[n++]=(uint8_t)id;}
    memcpy(buf+n,payload,len); n+=len;
    return mqtt_finish(out,cap,(uint8_t)(0x30|(qos?2:0)|(duplicate?8:0)),buf,n);
}
int cloud_mqtt_subscribe(uint8_t *out,size_t cap,const char *topic,uint16_t id)
{
    uint8_t buf[128];size_t n=2;buf[0]=(uint8_t)(id>>8);buf[1]=(uint8_t)id;
    if(!id||!topic||!*topic||strpbrk(topic,"+#")||mqtt_string(buf,sizeof(buf),&n,topic)||n==sizeof(buf))return -1;
    buf[n++]=0;return mqtt_finish(out,cap,0x82,buf,n);
}
int cloud_ack_parse(const uint8_t *p,unsigned n,uint32_t key[4])
{
    const char prefix[]="{\"v\":6,\"d\":[";unsigned i=sizeof(prefix)-1;
    if(!p||!key||n<i||memcmp(p,prefix,i))return -1;
    uint32_t values[4];
    for(unsigned j=0;j<4;j++){
        uint32_t v=0;unsigned start=i;
        while(i<n&&p[i]>='0'&&p[i]<='9'){
            unsigned digit=p[i++]-'0';if(v>429496729U||(v==429496729U&&digit>5))return -1;v=v*10+digit;
        }
        if(i==start||i>=n||(i-start>1&&p[start]=='0')||p[i++]!=(j<3?',':']'))return -1;
        values[j]=v;
    }
    if(i+1!=n||p[i]!='}')return -1;
    memcpy(key,values,sizeof(values));return 0;
}

void cloud_at_feed(cloud_at_rx_t *r, uint8_t b)
{
    if (!r) return;
    if (r->remaining) { r->remaining--; if(r->on_byte)r->on_byte(r->ctx,b); return; }
    if (b=='\r') return;
    if (b=='\n') {
        if (!r->overflow && r->used) { r->line[r->used]=0; if(r->on_line)r->on_line(r->ctx,r->line); }
        r->used=r->overflow=0; return;
    }
    if (r->overflow) return;
    if (b=='>' && !r->used) { if(r->on_line)r->on_line(r->ctx,">"); return; }
    /* The space after a CIPSEND prompt is cosmetic, not part of the next token. */
    if (b==' ' && !r->used) return;
    if (r->used+1>=sizeof(r->line)) { r->overflow=1; r->errors++; return; }
    r->line[r->used++]=(char)b; r->line[r->used]=0;
    if (b==':' && r->used>=6 && !memcmp(r->line,"+IPD,",5)) {
        unsigned n=0;
        for(unsigned i=5;i<r->used-1;i++) {
            if(r->line[i]<'0'||r->line[i]>'9'||n>409) { r->errors++;r->overflow=1;return; }
            n=n*10+(unsigned)(r->line[i]-'0');
        }
        if(r->used==6 || n>4096) {r->errors++;r->overflow=1;return;}
        r->remaining=n; r->used=0;
    }
}
void cloud_mqtt_feed(cloud_mqtt_rx_t *r, uint8_t b)
{
    if (!r || r->phase==3) return;
    if (!r->phase) { r->type=b; r->size=r->used=r->shift=0; r->phase=1; return; }
    if (r->phase==1) {
        if (r->shift>=28) goto failed;
        r->size|=(unsigned)(b&127)<<r->shift; r->shift+=7;
        if (b&128) return;
        if (r->size>sizeof(r->data)) goto failed;
        if(((r->type==0x20||r->type==0x40)&&r->size!=2)||(r->type==0xd0&&r->size!=0)||
           (r->type==0x90&&r->size!=3))goto failed;
        if(r->size) { r->phase=2;return; }
    } else {
        r->data[r->used++]=b;
        if(r->used<r->size)return;
    }
    if (!((r->type==0x20 && r->size==2 && r->data[0]<=1 && r->data[1]<=5 && (!r->data[1]||!r->data[0])) ||
          (r->type==0x40 && r->size==2 && (r->data[0]||r->data[1])) ||
          (r->type==0xd0 && r->size==0) ||
          (r->type==0x90 && r->size==3 && (r->data[0]||r->data[1]) && (r->data[2]<=1||r->data[2]==128)) ||
          ((r->type==0x30||r->type==0x31) && r->size>=3 &&
           (((unsigned)r->data[0]<<8)|r->data[1])>0 && (((unsigned)r->data[0]<<8)|r->data[1])+2<=r->size))) goto failed;
    if(r->on_packet)r->on_packet(r->ctx,r->type,r->data,r->size);
    r->phase=0;return;
failed: r->errors++;r->phase=3;
}
