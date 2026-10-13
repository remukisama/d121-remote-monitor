import json, queue, ssl, threading, time, uuid
import paho.mqtt.client as mqtt
from .protocol import decode

class Collector:
    def __init__(self, store, cfg, retention_days=30, acknowledge=False):
        self.store, self.cfg, self.retention_days = store, cfg, retention_days
        self.queue = queue.Queue(maxsize=4096)
        self.stop_event = threading.Event()
        self.client = None
        self.thread = None
        self.received = self.dropped = 0
        self.acknowledge=acknowledge
        self.acks_submitted=self.ack_errors=0

    def start(self):
        if not self.cfg.get('username') or not self.cfg.get('password'):
            self.store.connection(False, '云端接收账号尚未配置'); return
        if self.cfg['client_id'] == self.cfg.get('uid'):
            raise ValueError('Subscriber must not reuse the hardware client ID')
        client_id=self.cfg['client_id'][:48]+'-'+uuid.uuid4().hex[:8]
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,clean_session=True)
        self.client.username_pw_set(self.cfg['username'], self.cfg['password'])
        if self.cfg.get('tls',True): self.client.tls_set_context(ssl.create_default_context())
        self.client.on_connect=self.on_connect
        self.client.on_disconnect=lambda *_: self.store.connection(False,'云端接收连接中断，暂时无法判断设备是否掉线')
        self.client.on_message=self.on_message
        self.client.on_subscribe=self.on_subscribe
        self.client.reconnect_delay_set(min_delay=1,max_delay=30)
        self.client.connect_async(self.cfg['host'],self.cfg['port'],keepalive=30)
        self.client.loop_start()
        self.thread=threading.Thread(target=self.run,name='d121-archive',daemon=True); self.thread.start()

    def on_connect(self, client, userdata, flags, reason, props):
        if reason.is_failure:
            self.store.connection(False,'云端接收账户验证失败'); return
        result, _ = client.subscribe([(self.cfg['state_topic'],0),(self.cfg['event_topic'],0)])
        if result != mqtt.MQTT_ERR_SUCCESS: self.store.connection(False,'主题订阅请求失败')

    def on_subscribe(self,client,userdata,mid,reasons,props):
        ok=len(reasons)==2 and not any(reason.is_failure for reason in reasons)
        self.store.connection(ok,None if ok else '服务器没有批准两个主题的订阅')

    def on_message(self, client, userdata, msg):
        try:
            self.queue.put_nowait((msg.topic,bytes(msg.payload),time.time(),msg.retain)); self.received+=1
        except queue.Full: self.dropped+=1

    def run(self):
        prune_at=time.monotonic()+3600
        while not self.stop_event.is_set():
            try:
                topic,payload,stamp,retained=self.queue.get(timeout=0.5)
                kind,value=decode(topic,payload,self.cfg)
                self.store.ingest(kind,value,stamp,retained)
                self.store.archive_error=None
                # ingest returns only after the transaction commits. A lost ACK causes
                # a harmless device retry; failed writes never receive an ACK.
                if self.acknowledge and self.client and not retained and kind in ('event','context'):
                    key=([value['boot'],value['collection'],value['node'],value['event_id']] if kind=='event' else
                         [value['boot'],value['collection'],value['context_kind'],value['uptime_s']])
                    try:
                        result=self.client.publish(self.cfg['event_topic']+'/set',json.dumps(dict(v=6,d=key),separators=(',',':')),qos=0,retain=False)
                        if result.rc!=mqtt.MQTT_ERR_SUCCESS:raise OSError('Acknowledgement not queued')
                        self.acks_submitted+=1
                    except Exception:self.ack_errors+=1
            except queue.Empty: pass
            except (ValueError,UnicodeError,KeyError,TypeError): self.store.rejected+=1
            except Exception:
                self.store.archive_error='历史记录服务发生写入异常，请检查服务器存储'
            try: self.store.check_offline()
            except Exception:
                # A lost database connection must not kill the archive worker.
                self.store.archive_error='历史记录服务发生写入异常，请检查服务器存储'
            if time.monotonic()>=prune_at:
                try: self.store.prune(self.retention_days)
                except Exception: self.store.error='历史保留清理失败'
                prune_at=time.monotonic()+3600

    def stop(self):
        self.stop_event.set()
        if self.client: self.client.disconnect(); self.client.loop_stop()
        if self.thread: self.thread.join(timeout=5)
