/* UART1 ESP8266 normal-mode TCP transport + MQTT 3.1.1 QoS 0/1 publisher.
 * All network/filesystem work belongs to this low-priority worker. No GUI or
 * collector calls AT transactions. Secrets are never printed by this module.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "cloud_wire.h"
#include "cloud_outbox.h"
#include "bb.h"
#include "bb_system.h"

#define CONFIG_FILE "/data/cloud.cfg"
#define CONFIG_MAX 1600
#define PAYLOAD_MAX 640
#define OUTBOX_FILE "/data/cloud.outbox"
#define CONTEXT_CAP 60
enum { JOB_IDLE, JOB_PROBE, JOB_RUN, JOB_FSCHECK };
enum { AT_OK=1, F_FAIL=2, F_PROMPT=4, F_SENT=8, F_CONNACK=16, F_PUBACK=32, F_PING=64, F_SUBACK=128 };
enum { DROP_NONE, DROP_TCP, DROP_WIFI, DROP_RESET, DROP_PARSE, DROP_PAUSE };
typedef struct {unsigned boot,collection,ts;int valid[2];char state[112],motion[112],diagnostic[112];} context_t;
typedef struct {
    int job, online, configured, present;
    unsigned baud, attempts, reconnects, published, acknowledged, events_sent, queued, dropped, event_gaps;
    unsigned rx_bytes, tx_bytes, errors, timeouts, last_ack_ms, last_io_ms, boot;
    char phase[24];
    char failed_command[24], failed_phase[24];
    unsigned wifi_error, at_parse_errors, mqtt_parse_errors;
    unsigned session_compat;
    unsigned wifi_joins, wifi_reused, wifi_drops, tcp_closes, esp_resets;
    int wifi_rssi;
    unsigned wifi_rssi_valid;
    unsigned failure_at_ms, failure_wait_ms, failure_need, failure_flags, failure_disconnect;
    unsigned app_ack,app_retries,persist_errors,queue_full,repaired;
} cloud_status_t;
static volatile int wanted;
static volatile int worker_active;
static volatile unsigned probe_baud=115200;
static volatile unsigned pause_until;
static volatile int pause_pending;
static cloud_status_t status;
static cloud_config_t config;
static char config_text[CONFIG_MAX+1];
static unsigned config_used;
static int config_upload;
static rt_device_t device;
static int uart_owned;
static cloud_at_rx_t at_rx;
static cloud_mqtt_rx_t mqtt_rx;
static unsigned flags, expect_id;
static int disconnected, print_version, joining, closing;
static unsigned disconnect_reason;
static int legacy_at, associated_ap, ip_status;
static int tls_auth_verified;
static cloud_outbox_t outbox;
static context_t contexts[CONTEXT_CAP];
static unsigned context_head,context_count,cursor[2],sample_due,sequence;
static unsigned outbox_stage,app_last_send,spool_due;
static uint32_t app_expect[4];
static int app_active,app_received;
static unsigned collection;
static char latest[PAYLOAD_MAX];
static unsigned latest_sequence, sent_sequence;
static char latest_motion[128];
static unsigned motion_sequence,sent_motion_sequence;
static int diagnostic_payload(char *payload,size_t cap);

static void phase(const char *name)
{
    rt_enter_critical(); rt_strncpy(status.phase,name,sizeof(status.phase)-1); rt_exit_critical();
}
static void on_packet(void *ctx, uint8_t type, const uint8_t *data, unsigned len)
{
    (void)ctx;
    if(type==0x20) {
        /* Real bemfa.com:9501 returns SP=1 even for CleanSession=1. Our
         * client always re-subscribes on a clean connection; tolerate this
         * platform quirk only on its configured host, and count it visibly. */
        int bemfa=!strcmp(config.host,"bemfa.com")||!strcmp(config.host,"mqtt.bemfa.com");
        if(data[1] || (data[0]&&!bemfa))flags|=F_FAIL;
        else {if(data[0])status.session_compat++;flags|=F_CONNACK;}
    }
    if(type==0x40 && (((unsigned)data[0]<<8)|data[1])==expect_id) flags|=F_PUBACK;
    if(type==0xd0) flags|=F_PING;
    if(type==0x90 && (((unsigned)data[0]<<8)|data[1])==expect_id){
        if(data[2])flags|=F_FAIL;else flags|=F_SUBACK;
    }
    if(type==0x30 && app_active){ /* Retained ACKs do not authorize a local deletion. */
        unsigned n=((unsigned)data[0]<<8)|data[1];uint32_t received[4];
        if(n==strlen(config.event_topic)&&len>=n+2&&!memcmp(data+2,config.event_topic,n)&&
           !cloud_ack_parse(data+n+2,len-n-2,received)&&!memcmp(received,app_expect,sizeof(received)))app_received=1;
    }
}
static void on_byte(void *ctx, uint8_t b) { (void)ctx; cloud_mqtt_feed(&mqtt_rx,b); }
static void on_line(void *ctx, const char *line)
{
    (void)ctx;
    if(!strcmp(line,"OK"))flags|=AT_OK;
    if(!strcmp(line,"ERROR") || !strcmp(line,"FAIL") || !strcmp(line,"SEND FAIL") || !strncmp(line,"busy",4))flags|=F_FAIL;
    if(!strcmp(line,">"))flags|=F_PROMPT;
    if(!strcmp(line,"SEND OK"))flags|=F_SENT;
    if(!strncmp(line,"+CWJAP:",7) && line[7]>='0' && line[7]<='9' && !line[8])status.wifi_error=(unsigned)(line[7]-'0');
    if(!strncmp(line,"AT version:",11))legacy_at=line[11]=='1'&&line[12]=='.';
    if(!strncmp(line,"STATUS:",7)&&line[7]>='2'&&line[7]<='5'&&!line[8])ip_status=line[7]-'0';
    if(!strncmp(line,"+CWJAP:",7)&&line[7]=='"'){
        char quoted[70];int n=cloud_quote(quoted,sizeof(quoted),config.ssid);
        /* Only the configured SSID may be reused; never expose it in logs. */
        if(n>0&&!strncmp(line+7,quoted,(size_t)n)&&line[7+n]==','){
            associated_ap=1;
            const char *mac_end=strchr(line+7+n+2,'"');char *end;
            if(mac_end&&mac_end[1]==','){
                long channel=strtol(mac_end+2,&end,10);
                if(end!=mac_end+2&&*end==','){
                    const char *start=end+1;long rssi=strtol(start,&end,10);
                    if(end!=start&&(!*end||*end==',')&&channel>=1&&channel<=14&&rssi>=-127&&rssi<0){
                        status.wifi_rssi=(int)rssi;status.wifi_rssi_valid=1;
                    }
                }
            }
        }
    }
    if(!strncmp(line,"+CIPSSLCCONF:",13)){
        unsigned link,mode,pki,ca;char extra;
        if(sscanf(line+13,"%u,%u,%u,%u%c",&link,&mode,&pki,&ca,&extra)==4&&link==0&&mode==2&&pki==0&&ca==0)
            tls_auth_verified=1;
    }
    if(!strcmp(line,"CLOSED")){
        status.tcp_closes++;
        if(!closing){disconnected=1;disconnect_reason=DROP_TCP;}
    }
    if(!strcmp(line,"WIFI DISCONNECT")){
        status.wifi_rssi_valid=0;
        if(!joining){status.wifi_drops++;disconnected=1;disconnect_reason=DROP_WIFI;}
    }
    if(!strcmp(line,"ready")){status.esp_resets++;disconnected=1;disconnect_reason=DROP_RESET;}
    if(print_version && (!strncmp(line,"AT version:",11) || !strncmp(line,"SDK version:",12) ||
        !strncmp(line,"compile time:",13) || !strncmp(line,"Bin version:",12))) {
        rt_kprintf("[esp] %.90s\r\n",line);
    }
}
static void receiver_reset(void)
{
    memset(&at_rx,0,sizeof(at_rx)); memset(&mqtt_rx,0,sizeof(mqtt_rx));
    at_rx.on_line=on_line; at_rx.on_byte=on_byte; mqtt_rx.on_packet=on_packet;
    flags=0; disconnected=0;disconnect_reason=DROP_NONE;
}
static void number(char *dst, size_t cap, int valid, int64_t value)
{
    if(valid)snprintf(dst,cap,"%lld",(long long)value); else strcpy(dst,"null");
}
static void queue_event(unsigned node, unsigned run, unsigned session, unsigned id, unsigned type, unsigned ts)
{
    unsigned *seen=&cursor[node-1];
    if(id<=*seen)return;
    cloud_record_t r;memset(&r,0,sizeof(r));
    r.boot=status.boot;r.node=node;r.collection=collection;r.run=run;r.session=session;r.id=id;r.type=type;r.ts=ts;
    for(unsigned j=1;j<=context_count;j++){
        const context_t *c=&contexts[(context_head+CONTEXT_CAP-j)%CONTEXT_CAP];
        if(c->boot!=r.boot||c->collection!=r.collection||!c->valid[node-1]||(int32_t)(ts-c->ts)<0||ts-c->ts>60000)continue;
        strcpy(r.before,c->state);strcpy(r.motion,c->motion);strcpy(r.diagnostic,c->diagnostic);
        if(r.before[0])r.before[5]='5';
        if(r.motion[0])r.motion[5]='7';
        if(r.diagnostic[0])r.diagnostic[5]='8';
        break;
    }
    int result=cloud_outbox_push(&outbox,&r);
    if(result){if(result==-2)status.queue_full++;else status.persist_errors++;return;}
    if(id>*seen+1){status.event_gaps+=id-*seen-1;status.dropped+=id-*seen-1;}
    *seen=id;status.queued=outbox.count;
}
static void sample(void)
{
    unsigned now=bb_now_ms();
    if((int32_t)(now-sample_due)<0)return;
    sample_due=now+1000;
    bb_status_t s; bb_system_status_t l;
    bb_get_status(&s);bb_system_status(&l);
    if(s.collection_generation!=collection){cursor[0]=cursor[1]=0;collection=s.collection_generation;}
    char temp[24],humi[24],cpu[24],memory[24],load[24],uptime[24];
    unsigned age1=s.has_last?now-s.last.ts_ms:0,age2=l.has_last?now-l.last.ts_ms:0;
    int star=s.running&&s.has_last&&s.slave_online&&age1<=2500;
    int system_fresh=s.running&&l.has_last&&l.online&&!l.stale&&age2<=3500;
    int env=star&&(s.last.flags&BB_F_QUALITY_KNOWN)&&!(s.last.flags&BB_F_ENV_INVALID);
    number(temp,sizeof(temp),env,s.last.slave_temp);number(humi,sizeof(humi),env,s.last.slave_humi);
    unsigned valid=system_fresh?l.last.metrics.valid:0;
    number(cpu,sizeof(cpu),valid&RS485_SYS_CPU,l.last.metrics.cpu_centi);
    number(memory,sizeof(memory),valid&RS485_SYS_MEMORY,l.last.metrics.mem_total_kib-l.last.metrics.mem_available_kib);
    number(load,sizeof(load),valid&RS485_SYS_LOAD,l.last.metrics.load_milli);
    number(uptime,sizeof(uptime),valid&RS485_SYS_UPTIME,l.last.metrics.uptime_s);
    /* Both nodes in one 1 Hz update. Maximal valid 32-bit values encode to
     * <=110 JSON bytes, so the default topic's MQTT packet stays below 128. */
    int n=snprintf(latest,sizeof(latest),
        "{\"v\":2,\"d\":[%u,%u,%u,%d,[%d,%d,%u,%s,%s],[%d,%d,%u,%s,%s,%s,%s]]}",
        status.boot,collection,now/1000,s.running,s.running&&s.slave_online,star,(unsigned)s.last.flags,temp,humi,
        s.running&&l.online,system_fresh,valid,cpu,memory,load,uptime);
    if(n<0 || n>=sizeof(latest)){latest[0]=0;status.errors++;}
    else latest_sequence=++sequence;
    /* Preserve short movements from the existing 10Hz RAM ring; sending only
     * the current 1Hz IMU sample would often miss a brief knock. */
    bb_frame_t motion_frames[12];unsigned peak=0,samples=0;
    int frames=s.running&&s.has_last?bb_ring_recent(motion_frames,12):0;
    for(int i=0;i<frames;i++){
        const bb_frame_t *f=&motion_frames[i];
        if(now-f->ts_ms>=1000 || (int32_t)(f->ts_ms-s.last.ts_ms)>0 ||
           (f->flags&(BB_F_SLAVE_ON|BB_F_QUALITY_KNOWN))!=(BB_F_SLAVE_ON|BB_F_QUALITY_KNOWN) ||
           (f->flags&BB_F_IMU_INVALID))continue;
        unsigned value=cloud_vibration_mg(f->ax,f->ay,f->az);
        if(value>peak)peak=value;
        samples++;
    }
    char vibration[24];number(vibration,sizeof(vibration),samples>0,peak);
    n=snprintf(latest_motion,sizeof(latest_motion),"{\"v\":4,\"d\":[%u,%u,%u,%s,%u]}",
        status.boot,collection,now/1000,vibration,samples);
    if(n<0 || (unsigned)n>=sizeof(latest_motion)){latest_motion[0]=0;status.errors++;}
    else motion_sequence=latest_sequence;
    if(latest[0]&&strlen(latest)<CLOUD_CONTEXT_SIZE&&strlen(latest_motion)<CLOUD_CONTEXT_SIZE){
        context_t *c=&contexts[context_head];memset(c,0,sizeof(*c));
        c->boot=status.boot;c->collection=collection;c->ts=now;
        c->valid[0]=star&&(s.last.flags&BB_F_QUALITY_KNOWN)&&
            (!(s.last.flags&BB_F_ENV_INVALID)||!(s.last.flags&BB_F_IMU_INVALID));
        c->valid[1]=system_fresh&&valid;
        strcpy(c->state,latest);strcpy(c->motion,latest_motion);
        int dn=diagnostic_payload(c->diagnostic,sizeof(c->diagnostic));
        if(dn<0||(unsigned)dn>=sizeof(c->diagnostic))c->diagnostic[0]=0;
        context_head=(context_head+1)%CONTEXT_CAP;if(context_count<CONTEXT_CAP)context_count++;
    }
    if(!outbox.ready && (int32_t)(now-spool_due)>=0){
        if(cloud_outbox_open(&outbox,OUTBOX_FILE))status.persist_errors++;
        status.queued=outbox.count;status.repaired+=outbox.repaired;spool_due=now+5000;
    }
    if(!s.running)return;
    bb_event_t a[BB_EVENT_RING_LEN]; bb_system_event_t b[BB_SYSTEM_EVENTS];
    int count=bb_events_recent(a,BB_EVENT_RING_LEN);
    for(int i=0;i<count;i++)queue_event(1,a[i].run_id,a[i].session_id,a[i].id,a[i].type,a[i].ts_ms);
    count=bb_system_events(b,BB_SYSTEM_EVENTS);
    for(int i=0;i<count;i++)queue_event(2,b[i].run_id,b[i].session_id,b[i].id,b[i].type,b[i].frame.ts_ms);
}
static void pump(void)
{
    uint8_t buf[128];
    /* A bounded read pass prevents noisy RX from monopolizing even this worker. */
    for(int i=0;i<8;i++) {
        rt_size_t n=rt_device_read(device,0,buf,sizeof(buf));
        if(!n)break;
        status.rx_bytes+=n;
        for(rt_size_t j=0;j<n;j++)cloud_at_feed(&at_rx,buf[j]);
    }
    if(at_rx.errors || mqtt_rx.errors){status.at_parse_errors+=at_rx.errors;status.mqtt_parse_errors+=mqtt_rx.errors;at_rx.errors=mqtt_rx.errors=0;disconnected=1;disconnect_reason=DROP_PARSE;}
    if(!closing && pause_pending && (int32_t)(pause_until-bb_now_ms())>0){disconnected=1;disconnect_reason=DROP_PAUSE;}
    if(wanted==JOB_RUN)sample();
}
static int wait_flags(unsigned needed, unsigned timeout_ms, int job)
{
    unsigned began=bb_now_ms();
    while(bb_now_ms()-began<timeout_ms) {
        if(wanted!=job)return -1;
        pump();
        if((flags&F_FAIL)||disconnected){status.errors++;return -1;}
        if(flags&needed)return 0;
        rt_thread_mdelay(5);
    }
    status.timeouts++;return -1;
}
static int write_all(const void *data, unsigned n)
{
    const uint8_t *p=data;unsigned done=0,began=bb_now_ms();
    while(done<n) {
        if((wanted==JOB_IDLE&&!closing) || bb_now_ms()-began>2000)return -1;
        rt_size_t sent=rt_device_write(device,0,p+done,n-done);
        if(sent>n-done)return -1;
        done+=sent;status.tx_bytes+=sent;
        if(!sent)rt_thread_mdelay(5);
    }
    return 0;
}
static void record_failure(const char *command,unsigned needed,unsigned began)
{
    size_t n=strcspn(command,"=?\r\n");if(n>=sizeof(status.failed_command))n=sizeof(status.failed_command)-1;
    rt_enter_critical();
    memcpy(status.failed_command,command,n);status.failed_command[n]=0;
    strcpy(status.failed_phase,status.phase);
    status.failure_at_ms=bb_now_ms();status.failure_wait_ms=status.failure_at_ms-began;
    status.failure_need=needed;status.failure_flags=flags;status.failure_disconnect=disconnect_reason;
    rt_exit_critical();
}
static int at_command(const char *cmd, unsigned needed, unsigned timeout, int job)
{
    unsigned began=bb_now_ms();
    pump();flags=0;
    if(write_all(cmd,strlen(cmd))){if(wanted==job)record_failure(cmd,needed,began);return -1;}
    int result=wait_flags(needed,timeout,job);
    if(result && wanted==job)record_failure(cmd,needed,began);
    return result;
}
static void close_socket(void)
{
    unsigned before=status.errors;
    cloud_status_t previous=status;
    closing=1;
    int result=at_command("AT+CIPCLOSE\r\n",AT_OK,1500,wanted);
    closing=0;
    /* No existing socket is a documented/expected ERROR, not a data failure. */
    if(result && (flags&F_FAIL)){
        status.errors=before;
    }
    /* Cleanup must not replace the original send/join failure with CIPCLOSE. */
    strcpy(status.failed_command,previous.failed_command);strcpy(status.failed_phase,previous.failed_phase);
    status.failure_at_ms=previous.failure_at_ms;status.failure_wait_ms=previous.failure_wait_ms;
    status.failure_need=previous.failure_need;status.failure_flags=previous.failure_flags;
    status.failure_disconnect=previous.failure_disconnect;
    receiver_reset();
}
static int send_packet(const uint8_t *packet, int n)
{
    char cmd[40];
    if(n<=0)return -1;
    snprintf(cmd,sizeof(cmd),"AT+CIPSEND=%d\r\n",n);
    if(at_command(cmd,F_PROMPT,3000,JOB_RUN))return -1;
    /* Preserve protocol ACKs which may arrive before SEND OK. */
    flags&=~F_SENT;
    unsigned began=bb_now_ms();
    if(write_all(packet,(unsigned)n)){record_failure("UART_DATA",F_SENT,began);return -1;}
    int result=wait_flags(F_SENT,5000,JOB_RUN);
    if(result&&wanted==JOB_RUN)record_failure("SEND_OK",F_SENT,began);
    return result;
}
static int uart_open(unsigned baud)
{
    struct serial_configure c;
    device=rt_device_find("uart1");
    if(!device || device->ref_count)return -1;
    if(rt_device_control(device,RT_SERIAL_GET_CONFIG,&c)!=RT_EOK)return -1;
    c.baud_rate=baud; /* Preserve board function, flags, uart index/frequency. */
    if(rt_device_control(device,RT_DEVICE_CTRL_CONFIG,&c)!=RT_EOK ||
        rt_device_open(device,RT_DEVICE_FLAG_RDWR|RT_DEVICE_FLAG_INT_RX)!=RT_EOK)return -1;
    uart_owned=1;
    status.baud=baud;receiver_reset();return 0;
}
static void uart_close(void)
{
    if(device && uart_owned)rt_device_close(device);
    uart_owned=0;
    device=RT_NULL;
}
static int identify(int job)
{
    phase("identify");
    if(at_command("AT\r\n",AT_OK,1500,job))return -1;
    status.present=1;
    if(at_command("ATE0\r\n",AT_OK,1500,job))return -1;
    print_version=1;legacy_at=0;
    int result=at_command("AT+GMR\r\n",AT_OK,2000,job);
    print_version=0;return result;
}
static int connect_cloud(void)
{
    char ssid[70],pass[134],host[134],cmd[256];uint8_t packet[CLOUD_PACKET_MAX];
    tls_auth_verified=0;
    if(identify(JOB_RUN))return -1;
    close_socket();
    if(at_command(legacy_at?"AT+CWMODE_CUR=1\r\n":"AT+CWMODE=1\r\n",AT_OK,2000,JOB_RUN))return -1;
    phase("wifi");
    associated_ap=0;ip_status=0;status.wifi_rssi_valid=0;
    if(at_command("AT+CWJAP?\r\n",AT_OK,2000,JOB_RUN)||
       at_command("AT+CIPSTATUS\r\n",AT_OK,2000,JOB_RUN))return -1;
    if(associated_ap&&ip_status>=2&&ip_status<=4){status.wifi_reused++;status.wifi_error=0;}
    else {
        if(cloud_quote(ssid,sizeof(ssid),config.ssid)<0 || cloud_quote(pass,sizeof(pass),config.wifi_password)<0)return -1;
        snprintf(cmd,sizeof(cmd),"%s=%s,%s\r\n",legacy_at?"AT+CWJAP_CUR":"AT+CWJAP",ssid,pass);
        joining=1;status.wifi_joins++;status.wifi_error=0;
        int joined=at_command(cmd,AT_OK,35000,JOB_RUN);
        joining=0;
        if(joined)return -1;
    }
    if(at_command("AT+CIPMODE=0\r\n",AT_OK,2000,JOB_RUN) ||
        at_command("AT+CIPMUX=0\r\n",AT_OK,2000,JOB_RUN))return -1;
    /* Old firmware may lack CIPDINFO; its default +IPD,length: matches our parser. */
    unsigned before=status.errors;
    int info=at_command("AT+CIPDINFO=0\r\n",AT_OK,2000,JOB_RUN);
    if(info && (flags&F_FAIL))status.errors=before;
    if(wanted!=JOB_RUN)return -1;
    disconnected=0;phase("tcp");
    /* Clear a prior socket before a new clean MQTT session. ERROR means no socket. */
    receiver_reset();
    if(cloud_quote(host,sizeof(host),config.host)<0)return -1;
    if(config.tls){
        /* Require CA validation plus the configured host name. Never silently
         * fall back to unauthenticated SSL or plaintext on old AT firmware. */
        phase("tls");unsigned began=bb_now_ms();
        if(at_command("AT+CIPSSLCCONF=2,0,0\r\n",AT_OK,2000,JOB_RUN)||
           at_command("AT+CIPSSLCCONF?\r\n",AT_OK,2000,JOB_RUN))return -1;
        if(!tls_auth_verified){status.errors++;record_failure("TLS_AUTH",AT_OK,began);return -1;}
        snprintf(cmd,sizeof(cmd),"AT+CIPSSLCCN=%s\r\n",host);
        if(at_command(cmd,AT_OK,2000,JOB_RUN))return -1;
        snprintf(cmd,sizeof(cmd),"AT+CIPSSLCSNI=%s\r\n",host);
        if(at_command(cmd,AT_OK,2000,JOB_RUN))return -1;
    }
    phase("tcp");
    snprintf(cmd,sizeof(cmd),"AT+CIPSTART=\"%s\",%s,%u\r\n",config.tls?"SSL":"TCP",host,config.port);
    if(at_command(cmd,AT_OK,20000,JOB_RUN))return -1;
    phase("mqtt");
    int n=cloud_mqtt_connect(packet,sizeof(packet),&config);
    if(send_packet(packet,n) || wait_flags(F_CONNACK,5000,JOB_RUN))return -1;
    expect_id=1;flags&=~F_SUBACK;n=cloud_mqtt_subscribe(packet,sizeof(packet),config.event_topic,1);
    if(send_packet(packet,n)||wait_flags(F_SUBACK,5000,JOB_RUN))return -1;
    status.online=1;status.last_io_ms=bb_now_ms();phase("online");return 0;
}
static int publish(const char *topic, const char *payload, unsigned id)
{
    uint8_t packet[CLOUD_PACKET_MAX];
    expect_id=id; flags&=~F_PUBACK;
    int n=cloud_mqtt_publish(packet,sizeof(packet),topic,payload,(uint16_t)id,0,config.qos);
    if((!strcmp(config.host,"bemfa.com")||!strcmp(config.host,"mqtt.bemfa.com")) && n>127){status.errors++;return -1;}
    if(send_packet(packet,n))return -1;
    if(config.qos){
        if(wait_flags(F_PUBACK,5000,JOB_RUN))return -1;
        status.acknowledged++;status.last_ack_ms=bb_now_ms();
    }
    status.published++;status.last_io_ms=bb_now_ms();return 0;
}
static int diagnostic_payload(char *payload, size_t cap)
{
    bb_status_t s; rs485_bus_stats_t bus;
    bb_get_status(&s); rs485_bus_stats(&bus);
    unsigned backend = s.backend&&!strcmp(s.backend,"sdcard") ? 2 : s.backend&&!strcmp(s.backend,"nor") ? 1 : 0;
    /* Separate short packet on the existing event topic; v=3 cannot be mistaken
     * for an event. Counters remain observable without changing the state ABI. */
    return snprintf(payload,cap,"{\"v\":3,\"d\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%d]}",
        status.boot,s.collection_generation,bb_now_ms()/1000,backend,s.write_errors,s.dropped_frames,
        bus.ok,bus.timeouts,bus.crc_errors,(int)s.last.chip_temp);
}
static int reliable_step(unsigned *id)
{
    const cloud_record_t *r=cloud_outbox_peek(&outbox);
    if(!r){outbox_stage=0;app_active=app_received=0;return 0;}
    unsigned now=bb_now_ms();
    if(app_active){
        if(app_received){app_active=app_received=0;outbox_stage++;status.app_ack++;}
        else if(now-app_last_send<3000)return 0;
        else {status.app_retries++;app_active=0;}
    }
    const char *parts[]={r->before,r->motion,r->diagnostic};
    while(outbox_stage<3&&!parts[outbox_stage][0])outbox_stage++;
    if(outbox_stage==4){
        if(cloud_outbox_pop(&outbox,r)){status.persist_errors++;return 0;}
        status.events_sent++;status.queued=outbox.count;outbox_stage=0;return 0;
    }
    char payload[112];
    if(outbox_stage<3){
        strcpy(payload,parts[outbox_stage]);unsigned version,boot,col,uptime;
        if(sscanf(payload,"{\"v\":%u,\"d\":[%u,%u,%u",&version,&boot,&col,&uptime)!=4 ||
           boot!=r->boot||col!=r->collection){status.persist_errors++;return 0;}
        app_expect[0]=boot;app_expect[1]=col;app_expect[2]=outbox_stage==0?0:outbox_stage==1?3:4;app_expect[3]=uptime;
    }else{
        snprintf(payload,sizeof(payload),"{\"v\":1,\"d\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
            r->boot,r->collection,r->node,r->run,r->session,r->id,r->type,r->ts);
        app_expect[0]=r->boot;app_expect[1]=r->collection;app_expect[2]=r->node;app_expect[3]=r->id;
    }
    app_active=1;app_received=0;app_last_send=now;
    if(++*id>65535)*id=1;
    if(publish(config.event_topic,payload,*id)){app_active=0;return -1;}
    return 0;
}
static int load_config(void)
{
    int fd=open(CONFIG_FILE,O_RDONLY);if(fd<0)return -1;
    unsigned n=0;int got=0;char extra;
    while(n<CONFIG_MAX && (got=read(fd,config_text+n,CONFIG_MAX-n))>0)n+=(unsigned)got;
    int excess=read(fd,&extra,1);close(fd);
    if(got<0||excess!=0)return -1;
    config_text[n]=0;
    if(memchr(config_text,0,n) || cloud_config_parse(&config,config_text))return -1;
    status.configured=1;return 0;
}
static unsigned boot_counter(void)
{
    unsigned value=0;int fd=open("/data/cloud.boot",O_RDONLY);
    if(fd>=0){
        char extra;int valid=read(fd,&value,sizeof(value))==sizeof(value)&&read(fd,&extra,1)==0&&value&&value!=UINT32_MAX;
        if(close(fd)<0)valid=0;
        if(!valid)return 0; /* A damaged counter must never silently reuse an old boot identity. */
    }else if(errno!=ENOENT&&errno!=-ENOENT)return 0;
    value++;fd=open("/data/cloud.boot.tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0)return 0;
    int ok=write(fd,&value,sizeof(value))==sizeof(value)&&fsync(fd)==0;
    if(close(fd)<0)ok=0;
    if(!ok || rename("/data/cloud.boot.tmp","/data/cloud.boot"))return 0;
    return value;
}
static void storage_check(void)
{
    const char *path="/data/cloud.test";
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);
    if(fd<0){rt_kprintf("[cloud] fscheck cannot create temporary file errno=%d\r\n",errno);return;}
    const char *stage="write";struct stat st;char data[32];int ok=0;
    if(write(fd,"0123456789abcdef",16)!=16)goto done;
    stage="truncate";
    if(lseek(fd,12,SEEK_SET)!=12 || ftruncate(fd,8) || lseek(fd,0,SEEK_CUR)!=12)goto done;
    stage="gap";
    if(write(fd,"X",1)!=1 || fsync(fd) || fstat(fd,&st) || st.st_size!=13 ||
       lseek(fd,0,SEEK_SET)!=0 || read(fd,data,13)!=13 || memcmp(data,"01234567",8))goto done;
    for(int i=8;i<12;i++)if(data[i])goto done;
    if(data[12]!='X')goto done;
    ok=1;
done:
    if(close(fd)<0)ok=0;
    if(unlink(path)<0)ok=0;
    rt_kprintf("[cloud] fscheck %s stage=%s errno=%d\r\n",ok?"PASS":"FAIL",stage,ok?0:errno);
}
static void worker(void *arg)
{
    (void)arg;unsigned id=0,backoff=1000,diagnostic_due=0;
    phase("idle");status.boot=boot_counter();
    if(cloud_outbox_open(&outbox,OUTBOX_FILE))status.persist_errors++;
    status.queued=outbox.count;status.repaired=outbox.repaired;
    if(!load_config())wanted=JOB_RUN;
    for(;;) {
        if(wanted==JOB_IDLE){worker_active=0;status.job=0;if(status.configured)sample();rt_thread_mdelay(50);continue;}
        worker_active=1;status.job=wanted;
        if(wanted==JOB_FSCHECK){phase("fscheck");storage_check();wanted=JOB_IDLE;continue;}
        if(wanted==JOB_PROBE){
            unsigned baud=probe_baud;status.present=0;
            if(uart_open(baud)||identify(JOB_PROBE)){phase("probe_failed");status.errors++;}
            else {phase("probe_ok");rt_kprintf("[cloud] ESP AT ready baud=%u\r\n",baud);}
            uart_close();wanted=JOB_IDLE;continue;
        }
        if(!status.configured && load_config()){phase("need_config");wanted=JOB_IDLE;continue;}
        if(pause_pending && (int32_t)(pause_until-bb_now_ms())>0){phase("paused");sample();rt_thread_mdelay(50);continue;}
        pause_pending=0;
        status.attempts++;
        if(uart_open(config.baud) || connect_cloud())goto retry;
        backoff=1000;
        while(wanted==JOB_RUN && !disconnected){
            pump();if(disconnected){record_failure("RECEIVE",0,bb_now_ms());break;}
            /* Interleave up to one queued event with each fresh state. */
            if(latest[0] && latest_sequence!=sent_sequence){
                unsigned seq=latest_sequence;char payload[PAYLOAD_MAX];strcpy(payload,latest);
                if(++id>65535)id=1;
                if(publish(config.state_topic,payload,id))break;
                sent_sequence=seq;
            }
            if(latest_motion[0] && motion_sequence!=sent_motion_sequence){
                unsigned seq=motion_sequence;char payload[128];strcpy(payload,latest_motion);
                if(++id>65535)id=1;
                if(publish(config.event_topic,payload,id))break;
                sent_motion_sequence=seq;
            }
            if(reliable_step(&id))break;
            if((int32_t)(bb_now_ms()-diagnostic_due)>=0){
                char payload[256];int n=diagnostic_payload(payload,sizeof(payload));
                if(n<0 || (unsigned)n>=sizeof(payload)){status.errors++;break;}
                if(++id>65535)id=1;
                if(publish(config.event_topic,payload,id))break;
                diagnostic_due=bb_now_ms()+5000;
            }
            if(bb_now_ms()-status.last_io_ms>=10000){
                uint8_t ping[]={0xc0,0};flags&=~F_PING;
                if(send_packet(ping,2)||wait_flags(F_PING,5000,JOB_RUN))break;
                status.last_io_ms=bb_now_ms();
            }
            rt_thread_mdelay(20);
        }
retry:
        app_active=app_received=0;
        status.online=0;
        if(device && uart_owned){
            disconnected=0;close_socket();
        }
        uart_close();
        if(wanted==JOB_RUN){
            phase("backoff");status.reconnects++;
            unsigned began=bb_now_ms();
            while(wanted==JOB_RUN&&bb_now_ms()-began<backoff){sample();rt_thread_mdelay(50);}
            if(backoff<30000)backoff=backoff*2>30000?30000:backoff*2;
        }else phase("idle");
    }
}
static int hex_digit(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static const char *wait_name(unsigned needed)
{
    switch(needed){case AT_OK:return "ok";case F_PROMPT:return "send_prompt";case F_SENT:return "send_ok";
    case F_CONNACK:return "connack";case F_PUBACK:return "puback";case F_PING:return "pingresp";
    case F_SUBACK:return "suback";default:return "receive";}
}
static const char *drop_name(unsigned reason)
{
    switch(reason){case DROP_TCP:return "tcp_closed";case DROP_WIFI:return "wifi_lost";
    case DROP_RESET:return "esp_reset";case DROP_PARSE:return "rx_parse";case DROP_PAUSE:return "requested_pause";
    default:return "none";}
}
static int cmd_cloud(int argc,char **argv)
{
    if(argc==1 || (argc==2&&!strcmp(argv[1],"stat"))){
        cloud_status_t s;rt_enter_critical();s=status;rt_exit_critical();
        rt_kprintf("cloud phase=%s job=%d online=%d configured=%d present=%d baud=%u\r\n",s.phase,s.job,s.online,s.configured,s.present,s.baud);
        rt_kprintf("attempts=%u reconnects=%u submitted=%u puback=%u qos=%u\r\n",
            s.attempts,s.reconnects,s.published,s.acknowledged,status.configured?config.qos:0);
        rt_kprintf("events=%u queued=%u dropped=%u gaps=%u\r\n",s.events_sent,s.queued,s.dropped,s.event_gaps);
        rt_kprintf("durable=%u app_ack=%u retries=%u persist_errors=%u full=%u repaired=%u journal=%u\r\n",
            outbox.ready,s.app_ack,s.app_retries,s.persist_errors,s.queue_full,s.repaired,outbox.bytes);
        rt_kprintf("rx=%u tx=%u errors=%u timeouts=%u ack_ms=%u boot=%u\r\n",s.rx_bytes,s.tx_bytes,s.errors,s.timeouts,s.last_ack_ms,s.boot);
        rt_kprintf("failed=%s phase=%s wifi_error=%u at_parse=%u mqtt_parse=%u\r\n",s.failed_command,s.failed_phase,s.wifi_error,s.at_parse_errors,s.mqtt_parse_errors);
        rt_kprintf("session_compat=%u\r\n",s.session_compat);
        rt_kprintf("wifi_joins=%u wifi_reused=%u wifi_drop=%u tcp_close=%u esp_reset=%u rssi=%d rssi_valid=%u\r\n",
            s.wifi_joins,s.wifi_reused,s.wifi_drops,s.tcp_closes,s.esp_resets,s.wifi_rssi,s.wifi_rssi_valid);
        rt_kprintf("failure_at_ms=%u wait=%s wait_ms=%u flags=%u disconnect=%s\r\n",
            s.failure_at_ms,wait_name(s.failure_need),s.failure_wait_ms,s.failure_flags,drop_name(s.failure_disconnect));
        rt_kprintf("tls=%u auth_verified=%u\r\n",status.configured?config.tls:0,tls_auth_verified);
        return 0;
    }
    if(argc==2&&!strcmp(argv[1],"stop")){wanted=JOB_IDLE;return 0;}
    if(argc==3&&!strcmp(argv[1],"pause")){
        char *end;unsigned long seconds=strtoul(argv[2],&end,10);
        if(!*argv[2]||*end||seconds<1||seconds>60||wanted!=JOB_RUN)return -RT_EINVAL;
        pause_until=bb_now_ms()+(unsigned)seconds*1000;pause_pending=1;return 0;
    }
    if(argc==2&&!strcmp(argv[1],"fscheck")){
        if(config_upload || wanted!=JOB_IDLE || worker_active)return -RT_EBUSY;
        wanted=JOB_FSCHECK;return 0;
    }
    if(argc==2&&!strcmp(argv[1],"start")){
        if(config_upload || wanted!=JOB_IDLE || worker_active)return -RT_EBUSY;
        wanted=JOB_RUN;return 0;
    }
    if((argc==2||argc==3)&&!strcmp(argv[1],"probe")){
        if(config_upload || wanted!=JOB_IDLE || worker_active)return -RT_EBUSY;
        unsigned baud=115200;
        if(argc==3){char *end;unsigned long n=strtoul(argv[2],&end,10);if(!*argv[2]||*end)return -RT_EINVAL;baud=(unsigned)n;}
        if(!(baud==9600||baud==19200||baud==38400||baud==57600||baud==115200||baud==230400||baud==460800||baud==921600))return -RT_EINVAL;
        probe_baud=baud;wanted=JOB_PROBE;return 0;
    }
    if(argc>=3&&!strcmp(argv[1],"cfg")){
        if(wanted!=JOB_IDLE || worker_active)return -RT_EBUSY;
        if(argc==3&&!strcmp(argv[2],"begin")){config_upload=1;config_used=0;rt_kprintf("cfg begin ok\r\n");return 0;}
        if(argc==5&&!strcmp(argv[2],"part")&&config_upload){
            char *end; unsigned long offset=strtoul(argv[3],&end,10);
            if(!*argv[3]||*end||offset!=config_used)return -RT_EINVAL;
            size_t len=strlen(argv[4]);
            if(!len || len%2 || config_used+len/2>CONFIG_MAX)return -RT_EINVAL;
            for(size_t i=0;i<len;i++)if(hex_digit(argv[4][i])<0)return -RT_EINVAL;
            for(size_t i=0;i<len;i+=2)config_text[config_used++]=(char)((hex_digit(argv[4][i])<<4)|hex_digit(argv[4][i+1]));
            rt_kprintf("cfg part ok bytes=%u\r\n",config_used);return 0;
        }
        if(argc==3&&!strcmp(argv[2],"commit")&&config_upload){
            cloud_config_t next;config_text[config_used]=0;
            if(memchr(config_text,0,config_used)||cloud_config_parse(&next,config_text))return -RT_EINVAL;
            int fd=open(CONFIG_FILE ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -RT_ERROR;
            int ok=write(fd,config_text,config_used)==config_used && fsync(fd)==0;
            if(close(fd)<0)ok=0;
            if(!ok||rename(CONFIG_FILE ".tmp",CONFIG_FILE))return -RT_ERROR;
            config=next;status.configured=1;config_upload=0;rt_kprintf("cfg commit ok\r\n");return 0;
        }
    }
    rt_kprintf("cloud stat | probe [baud] | start | stop | cfg begin/part/commit\r\n");return -RT_EINVAL;
}
MSH_CMD_EXPORT_ALIAS(cmd_cloud,cloud,ESP8266 transport and MQTT status);
static int cloud_init(void)
{
    rt_thread_t t=rt_thread_create("cloud",worker,RT_NULL,16384,24,10);
    if(!t)return -RT_ENOMEM;
    if(rt_thread_startup(t)!=RT_EOK){rt_thread_delete(t);return -RT_ERROR;}
    return 0;
}
INIT_APP_EXPORT(cloud_init);
