"""Only accept the production D121 wire formats, never PC probe payloads."""
import json

EVENT_NAMES = {1: ['通信恢复', '通信中断', '主站温度过高', '主站温度恢复', '振动越限', '振动恢复'],
               2: ['', '通信恢复', '通信中断', '采集程序重新启动', '数据停止更新', '数据恢复更新']}
NODE_NAMES = {0: '玄铁主站', 1: '星火传感从站', 2: '龙芯系统从站'}

def integer(value, low=0, high=4294967295):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('Invalid integer field')
    return value

def metric(value, scale=1, low=0, high=4294967295):
    return None if value is None else integer(value, low, high) / scale

def decode(topic, raw, cfg):
    if len(raw) > 2048:
        raise ValueError('Oversized message')
    value = json.loads(raw)
    if not isinstance(value, dict) or 'source' in value:
        raise ValueError('Not a device message')
    d = value.get('d')
    if not isinstance(d, list):
        raise ValueError('Missing device data')
    if topic==cfg['event_topic'] and value.get('v')==6:
        if len(d)!=4:raise ValueError('Malformed archive acknowledgement')
        for x in d:integer(x)
        integer(d[2],0,4)
        return 'ack',dict(key=d)
    if topic==cfg['event_topic'] and value.get('v') in (5,7,8):
        version=value['v'];original={5:2,7:4,8:3}[version]
        kind,frame=decode(cfg['state_topic'] if version==5 else topic,
                         json.dumps(dict(v=original,d=d),separators=(',',':')).encode(),cfg)
        if not frame['boot']:raise ValueError('Replay requires a durable boot identity')
        return 'context',dict(boot=frame['boot'],collection=frame['collection'],uptime_s=frame['uptime_s'],
                              context_kind={5:0,7:3,8:4}[version],frame=frame)
    if topic == cfg['state_topic'] and value.get('v') == 2:
        if len(d) != 6 or not isinstance(d[4], list) or not isinstance(d[5], list) or len(d[4]) != 5 or len(d[5]) != 7:
            raise ValueError('Malformed state')
        boot, collection, uptime, running = [integer(x) for x in d[:4]]
        integer(running, 0, 1)
        a, b = d[4], d[5]
        for x in a[:2] + b[:2]: integer(x, 0, 1)
        integer(a[2], 0, 63); integer(b[2], 0, 15)
        star = dict(node=1, name=NODE_NAMES[1], online=bool(a[0] and running), fresh=bool(a[1] and running), flags=a[2],
                    temperature=metric(a[3], 100, -32768, 32767), humidity=metric(a[4], 100, 0, 10000))
        linux = dict(node=2, name=NODE_NAMES[2], online=bool(b[0] and running), fresh=bool(b[1] and running), valid=b[2],
                     cpu=metric(b[3], 100, 0, 10000), memory_mb=metric(b[4], 1024), load=metric(b[5], 1000), uptime_s=metric(b[6]))
        if not star['fresh']: star.update(temperature=None, humidity=None)
        if not (star['flags'] & 32) or star['flags'] & 8: star.update(temperature=None,humidity=None)
        if not linux['fresh']: linux.update(cpu=None, memory_mb=None, load=None, uptime_s=None)
        for field,bit in [('cpu',1),('memory_mb',2),('load',4),('uptime_s',8)]:
            if not linux['valid'] & bit: linux[field]=None
        return 'state', dict(boot=boot, collection=collection, uptime_s=uptime, running=bool(running), nodes=[star, linux])
    if topic == cfg['event_topic'] and value.get('v') == 1:
        if len(d) != 8: raise ValueError('Malformed event')
        d = [integer(x) for x in d]
        boot, collection, node, run, session, identity, kind, uptime_ms = d
        if node not in EVENT_NAMES or not identity or kind >= len(EVENT_NAMES[node]) or not EVENT_NAMES[node][kind]:
            raise ValueError('Unknown event')
        return 'event', dict(boot=boot, collection=collection, node=node, run=run, session=session, event_id=identity,
                             type=kind, title=EVENT_NAMES[node][kind], uptime_s=uptime_ms / 1000)
    # Motion is a separate short packet so existing v2 telemetry stays compatible.
    if topic == cfg['event_topic'] and value.get('v') == 4:
        if len(d) != 5: raise ValueError('Malformed motion summary')
        boot,collection,uptime,peak,count=d
        for x in (boot,collection,uptime): integer(x)
        integer(count,0,12)
        peak=metric(peak,1,0,2500)
        if (count==0)!=(peak is None): raise ValueError('Invalid motion sample count')
        return 'motion',dict(boot=boot,collection=collection,uptime_s=uptime,peak_mg=peak,sample_count=count)
    # Diagnostics share the event topic and use their own version discriminator.
    if topic == cfg['event_topic'] and value.get('v') == 3:
        if len(d) != 10: raise ValueError('Malformed diagnostic')
        boot, collection, uptime, backend, errors, dropped, bus_ok, bus_timeout, bus_crc, chip = d
        for x in d[:9]: integer(x)
        integer(backend, 0, 2)
        return 'diagnostic', dict(boot=boot, collection=collection, uptime_s=uptime,
            storage=['内存缓存', '板载 Flash', 'TF 卡'][backend], write_errors=errors, dropped_frames=dropped,
            bus_ok=bus_ok, bus_timeouts=bus_timeout, bus_crc_errors=bus_crc, chip_temperature=metric(chip,10,-32768,32767))
    raise ValueError('Unknown device message')

def diagnosis(event, snapshot, diagnostic=None):
    node, title = event['node'], event['title']
    findings, checks, confidence = [], [], '原因尚未确认'
    if node == 0:
        findings = ['云端已超过约定时间没有收到主站新数据。', '掉线前曲线来自云端已经保存的数据，设备离线后仍可查看。']
        checks = ['检查玄铁主站和 ESP8266 是否供电。', '检查 WiFi、路由器和互联网连接。', '恢复后查看主站是否重新开机及通信诊断。']
    elif title == '通信中断':
        findings = [f'主站记录到{NODE_NAMES[node]}停止应答。']
        if snapshot:
            other = snapshot['nodes'][2-node]
            if other['online']:
                findings.append('当时另一从站仍在应答，故障范围更可能在这台从站或它的通信支路。')
            else:
                findings.append('当时两台从站都未正常应答，应优先检查公共供电和 RS485 总线。')
        checks = ['检查这台从站的电源、A/B 接线和发送使能。', '检查从站采集程序是否正常运行。']
    elif title == '数据停止更新':
        findings = ['龙芯仍能通信，但采样数据停止更新或已过期。']; confidence = '已确认数据未更新'
        checks = ['检查龙芯采集进程和系统负载。']
    elif title == '采集程序重新启动':
        findings = ['主站检测到龙芯运行计数回退，记录了程序重新启动事件。']; confidence = '已记录重新启动'
        checks = ['检查龙芯服务日志，区分进程重启和整机重启。']
    elif title == '主站温度过高':
        findings = ['主站温度触发了设备上的越限规则。']; confidence = '已确认温度越限'
        checks = ['检查主站散热和周围温度。']
    elif title == '振动越限':
        findings = ['设备上的振动规则触发越限，尚未细分为冲击、跌落或倾斜。']; confidence = '已确认振动越限'
        checks = ['检查设备安装和现场振动。']
    else:
        findings = ['设备记录了状态恢复。']; confidence = '已记录状态恢复'
    if diagnostic:
        findings.append(f"当时记录介质为{diagnostic['storage']}，累计写入错误 {diagnostic['write_errors']} 次，通信超时 {diagnostic['bus_timeouts']} 次。")
    return dict(confidence=confidence, observations=findings, checks=checks,
                limitation='仅凭掉线现象无法确定断电、接触不良或网络故障；需要恢复后的日志或现场检查进一步确认。')
