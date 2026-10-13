// Real WeChat simulator QA. Never log credentials or authorization tokens.
const fs = require('node:fs'), path = require('node:path')
const automator = require('miniprogram-automator')
const { spawnSync } = require('node:child_process')
const root = path.resolve(__dirname, '..'), output = path.join(root, 'output', 'native')
fs.mkdirSync(output, { recursive: true })
const pause = ms => new Promise(resolve => setTimeout(resolve, ms))
const guard = setTimeout(() => { console.error('Native QA timeout'); process.exit(1) }, 90000); guard.unref()
function capture(name) {
  const r=spawnSync('pwsh.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',path.join(root,'tools','capture_ide.ps1'),'-Output',name+'.png'],{encoding:'utf8',timeout:10000,windowsHide:true})
  if(r.status!==0)throw new Error('Native window capture failed')
}
async function main() {
  const cfg = JSON.parse(fs.readFileSync(path.join(root, 'config.private.json'), 'utf8').replace(/^\uFEFF/, ''))
  const routeConfig=require('../miniprogram/config')
  let token
  if(routeConfig.mode!=='cloud') {
    const response = await fetch('http://127.0.0.1:8765/api/auth/local', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({code:cfg.server.development_token}) })
    if (!response.ok) throw new Error('Local login failed')
    token=(await response.json()).token
  }
  const mini = await automator.connect({ wsEndpoint: 'ws://127.0.0.1:9420' })
  console.log('Native automation connected')
  const errors = []
  mini.on('exception', e => errors.push(String(e.message || 'Simulator exception')))
  // The IDE bridge needs a serializable return value to acknowledge evaluate.
  if(routeConfig.mode==='cloud') {
    if(!await mini.evaluate(()=>!!getApp().globalData.token))throw new Error('Run cloud_smoke.cjs to complete real WeChat login first')
    console.log('Using the verified WeChat cloud session (credential hidden)')
  } else {
    await mini.evaluate(t => { wx.setStorageSync('d121_session', t); getApp().globalData.token=t;return true }, token)
    console.log('Local test session applied (credential hidden)')
  }
  // Explicit JSON results avoid this IDE version's Page.getData/callWxMethod bridge timeout.
  // Fresh routes keep this IDE's cached tab stacks from returning another page's data.
  const waitPage=async path=>{
    for(let i=0;i<40;i++){
      const current=await mini.evaluate(()=>{const p=getCurrentPages();return p[p.length-1].route})
      if(current===path.replace(/^\//,''))return
      await pause(250)
    }
    throw new Error('Simulator route did not settle: '+path)
  }
  const route=async(method,url)=>{await mini.evaluate(u=>{wx.reLaunch({url:u});return true},url);await waitPage(url.split('?')[0]);await pause(1500)}
  const pageData=async()=>JSON.parse(await mini.evaluate(()=>{const pages=getCurrentPages();return JSON.stringify(pages[pages.length-1].data)}))
  const callPage=(method,arg)=>mini.evaluate(v=>{const pages=getCurrentPages();pages[pages.length-1][v.method](v.arg);return true},{method,arg:arg||null})
  await route('switchTab','/pages/home/index'); await pause(1000)
  console.log('Native overview route opened')
  let data = await pageData()
  for(let i=0;i<30 && (!data.status || data.status.state!=='online');i++){await pause(500);data=await pageData()}
  if (data.error || !data.status || data.status.state !== 'online' || data.status.nodes.length !== 2) throw new Error('Home did not show two live cloud nodes: '+(data.error || 'state missing'))
  capture('home')
  console.log('PASS native overview: two actual live cloud nodes')
  // Exercise actual page callbacks and switchTab/onShow. Physical tapping is checked on the phone;
  // this IDE version's Element bridge hangs, so do not claim simulator pointer coverage.
  for(const node of [2,1,2,1]) {
    await callPage('trends',{currentTarget:{dataset:{node}}});await waitPage('/pages/trends/index');await pause(500)
    data=await pageData()
    if(data.node!==node || data.metric!==(node===2?'cpu':'temperature'))throw new Error('Overview selected the wrong trend node: '+node)
    if(node===1 && !data.metrics.some(x=>x.key==='vibration'))throw new Error('Star vibration selector missing')
    await mini.evaluate(()=>{wx.switchTab({url:'/pages/home/index'});return true});await waitPage('/pages/home/index');await pause(500)
  }
  console.log('PASS native overview navigation: correct Star/Loongson node on fresh and cached trend tabs')
  await callPage('trends',{currentTarget:{dataset:{node:1}}});await waitPage('/pages/trends/index');await pause(2500)
  data=await pageData()
  if (data.error || !data.points.length) throw new Error('Trend data missing')
  capture('trends')
  console.log('PASS native trends: '+data.points.length+' archived real samples')
  await callPage('selectMetric',{currentTarget:{dataset:{metric:'vibration'}}});await pause(600)
  data=await pageData()
  if(data.metric!=='vibration'||data.unit!=='g'||!data.points.some(p=>Number.isFinite(p.nodes?.[0]?.vibration)))throw new Error('Archived real vibration data missing from Star trend')
  capture('trends-vibration')
  console.log('PASS native Star vibration trend: real archived values, unit g')
  await route('switchTab','/pages/events/index'); await pause(2000)
  data=await pageData()
  capture('events')
  const originalIds=data.items.map(e=>e.id)
  if(data.next){
    await callPage('more');await pause(1000)
    const older=await pageData()
    if(older.items.length<=originalIds.length || new Set(older.items.map(e=>e.id)).size!==older.items.length)throw new Error('Event pagination failed')
    console.log('PASS native event pagination: older records added without duplicates')
  }
  await callPage('filter',{currentTarget:{dataset:{node:'2'}}});await pause(1000)
  const filtered=await pageData()
  if(filtered.error || filtered.node!==2 || filtered.items.some(e=>e.node!==2))throw new Error('Loongson event filter failed')
  capture('events-loongson')
  console.log('PASS native Loongson event filter: '+filtered.items.length+' archived events')
  await callPage('filter',{currentTarget:{dataset:{node:'0'}}});await pause(1000)
  data=await pageData()
  if (data.items.length) {
    await route('navigateTo','/pages/detail/index?id='+data.items[0].id); await pause(2000)
    const detail=await pageData()
    if (!detail.event || detail.error) throw new Error('Event details failed')
    capture('detail')
    console.log('PASS native event detail: '+detail.event.title)
    if(detail.event.node===0){
      await callPage('selectNode',{currentTarget:{dataset:{node:'2'}}});await pause(500)
      const linux=await pageData()
      if(linux.chartNode!==2 || linux.metric!=='cpu' || !linux.event.last)throw new Error('Master outage Loongson snapshot switch failed')
      capture('detail-loongson')
      console.log('PASS native master outage detail: both node snapshots and curves selectable')
    }
  } else console.log('Native event list is empty; controlled hardware incident test pending')
  await route('switchTab','/pages/device/index');await pause(1000)
  const device=await pageData()
  if(device.error || !device.status || !device.status.diagnostic)throw new Error('Device diagnostics unavailable')
  capture('device')
  console.log('PASS native device diagnostics')
  if (errors.length) throw new Error('Simulator exceptions: '+errors.join('; '))
  await route('switchTab','/pages/home/index')
  await mini.disconnect()
}
main().catch(e=>{console.error(String(e.message));process.exit(1)})
