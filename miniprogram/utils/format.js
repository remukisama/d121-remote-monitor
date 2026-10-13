function numeric(value, decimals = 1) { return value === null || value === undefined || !Number.isFinite(Number(value)) ? '—' : Number(value).toFixed(decimals) }
const pad = n => String(n).padStart(2, '0')
function clock(seconds, full = true) {
  if (seconds === null || seconds === undefined) return '时间尚未确定'
  const d = new Date((seconds + 8 * 3600) * 1000)
  const time = `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`
  return full ? `${d.getUTCFullYear()}/${pad(d.getUTCMonth()+1)}/${pad(d.getUTCDate())} ${time}` : time
}
function duration(seconds) {
  if (seconds === null || seconds === undefined) return '—'
  const n = Math.max(0, Math.floor(seconds))
  if (n >= 86400) return `${Math.floor(n / 86400)}天 ${Math.floor(n % 86400 / 3600)}小时`
  if (n >= 3600) return `${Math.floor(n / 3600)}小时 ${Math.floor(n % 3600 / 60)}分`
  return `${Math.floor(n / 60)}分 ${n % 60}秒`
}
function event(value) { return Object.assign({}, value, { timeText: clock(value.occurred === null ? value.received : value.occurred),
  receiveText: clock(value.received), uptimeText: duration(value.uptime),
  bad: /中断|过高|越限|停止/.test(value.title), estimated: value.occurred !== null && value.source !== 'heartbeat',
  timeLabel: value.source === 'heartbeat' ? '云端发现中断时间' : value.occurred !== null ? '推算发生时间' : '云端接收时间' }) }
function status(value) {
  const labels = { online: '主站正在上传', offline: '主站上传中断', unknown: '云端接收连接异常', waiting: '等待第一条新数据' }
  const current = value.state === 'online'
  const nodes = (value.latest ? value.latest.nodes : []).map(n => Object.assign({}, n, {
    label: !current ? '最后已知状态' : !value.latest.running ? '采集已停止' : !n.online ? '通信中断' : !n.fresh ? '数据未更新' : '在线',
    tone: !current || !n.online ? 'bad' : !n.fresh || !value.latest.running ? 'warn' : '',
    temperatureText: numeric(n.temperature), humidityText: numeric(n.humidity), vibrationText:numeric(n.vibration,3), cpuText: numeric(n.cpu),
    memoryText: numeric(n.memory_mb), loadText: numeric(n.load, 2), uptimeText: duration(n.uptime_s)
  }))
  return Object.assign({}, value, { label: labels[value.state], current, nodes,
    lastText: value.latest ? clock(value.latest.received_at) : '还没有数据',
    storageText: value.diagnostic ? value.diagnostic.storage : '等待设备诊断',
    collectingText: value.latest ? value.latest.running ? '正在采集' : '采集已停止' : '尚未获知' })
}
module.exports = { numeric, clock, duration, event, status }
