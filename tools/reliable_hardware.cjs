// Actual NOR -> Bemfa -> deployed MySQL validation. No credentials in reports.
// `cycle` checks reconnection, `prepare` safely queues records for power cycling,
// and `verify` checks the same original records after the operator restarts D121.
const fs=require('node:fs'),path=require('node:path'),{spawnSync}=require('node:child_process')
const automator=require('miniprogram-automator')
const root=path.resolve(__dirname,'..'),mode=process.argv[2]||'cycle'
const pause=ms=>new Promise(r=>setTimeout(r,ms))
const reportPath=path.join(root,'output','reliable-'+mode+'.json')
const guard=setTimeout(()=>{console.error('Reliable hardware QA timeout');process.exit(1)},180000);guard.unref()
async function main(){
 if(!['cycle','prepare','verify'].includes(mode))throw new Error('Expected cycle, prepare, or verify')
 const mini=await automator.connect({wsEndpoint:'ws://127.0.0.1:9420'})
 const report={mode,checked_at:new Date().toISOString(),checks:[]}
 let stopped=false,cloudStopped=false,captureStopped=false,prepared=false
 function command(port,text){
  const r=spawnSync('pwsh.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',
   'D:\\xuantie\\work\\scripts\\serial\\board_cmd.ps1','-Port',port,'-Commands',text,'-TimeoutMs','6000'],
   {encoding:'utf8',timeout:10000,windowsHide:true})
  fs.appendFileSync(path.join(root,'output','reliable-serial.txt'),new Date().toISOString()+' '+port+' '+text+'\n'+(r.stdout||'')+'\n')
  if(r.status!==0)throw new Error('Board command failed: '+port+' '+text)
  return r.stdout||''
 }
 function board(){
  const s=command('COM6','cloud stat'),n=key=>{const v=s.match(new RegExp('\\b'+key+'=(\\d+)'));if(!v)throw new Error('Missing board statistic: '+key);return Number(v[1])}
  return {boot:n('boot'),queued:n('queued'),durable:n('durable'),app_ack:n('app_ack'),events:n('events'),
   persist_errors:n('persist_errors'),full:n('full'),gaps:n('gaps'),retries:n('retries'),journal:n('journal')}
 }
 async function request(url){
  await mini.evaluate(url=>{
   const app=getApp(),cfg=app.globalData.config;app.globalData.reliableProbe=null
   wx.cloud.callContainer({config:{env:cfg.envId},path:url,method:'GET',
    header:{'X-WX-SERVICE':cfg.service,Authorization:'Bearer '+app.globalData.token},
    success:r=>{app.globalData.reliableProbe={status:r.statusCode,data:r.data}},
    fail:()=>{app.globalData.reliableProbe={error:'Cloud transport failed'}}});return true
  },url)
  for(let i=0;i<40;i++){await pause(250);const v=JSON.parse(await mini.evaluate(()=>JSON.stringify(getApp().globalData.reliableProbe)))
   if(v){if(v.error||v.status!==200)throw new Error(v.error||'Cloud status '+v.status);return v.data}}
  throw new Error('Cloud response timeout')
 }
 async function until(fn,seconds){const end=Date.now()+seconds*1000;do{const v=await fn();if(v)return v;await pause(700)}while(Date.now()<end);throw new Error('Expected hardware/cloud state not observed')}
 function pass(name,value={}){report.checks.push({name,...value});console.log('PASS '+name+': '+JSON.stringify(value))}
 async function drained(){return until(async()=>{const b=board();if(!b.durable||b.persist_errors)throw new Error('NOR queue unavailable or persistence failed');return b.queued===0?b:null},60)}
 async function online(){return until(async()=>{const s=await request('/api/status');return s.state==='online'&&s.latest?.nodes.every(n=>n.online&&n.fresh)?s:null},25)}
 async function archive(pre){
  const result=await until(async()=>{
   const all=(await request('/api/events?node=1&limit=50')).items
   const events=all.filter(e=>e.id>pre.last_id&&e.boot===pre.boot&&e.collection===pre.collection&&e.source==='device')
   return events.some(e=>e.title==='通信中断')&&events.some(e=>e.title==='通信恢复')?events:null
  },30)
  const down=result.filter(e=>e.title==='通信中断'),up=result.filter(e=>e.title==='通信恢复')
  if(down.length!==1||up.length!==1)throw new Error('Replayed incident was duplicated')
  const d=await request('/api/events/'+down[0].id)
  report.incident=d;report.recovery=up[0]
  if(!d.snapshot?.replayed||!d.snapshot.nodes[0].online||!Number.isFinite(d.snapshot.nodes[0].temperature))throw new Error('Original pre-outage context was not archived')
  if(d.snapshot.boot!==pre.boot||d.snapshot.collection!==pre.collection||d.snapshot.uptime_s>d.uptime)throw new Error('Snapshot identity or original ordering changed')
  report.incident=d;report.recovery=up[0]
  pass('MySQL saved one original outage/recovery pair and replayed pre-outage readings',{incident_id:d.id,recovery_id:up[0].id,original_boot:d.boot,collection:d.collection,vibration:d.snapshot.nodes[0].vibration})
  await pause(3500)
  const again=(await request('/api/events?node=1&limit=50')).items.filter(e=>e.id>pre.last_id&&e.boot===pre.boot&&e.collection===pre.collection&&e.source==='device')
  if(again.filter(e=>e.title==='通信中断').length!==1||again.filter(e=>e.title==='通信恢复').length!==1)throw new Error('Archive gained duplicate records')
  if(!(await request('/api/events/42')).snapshot)throw new Error('Existing historical incident disappeared')
  pass('Retry did not duplicate events; older history remains readable')
 }
 try{
  if(mode==='verify'){
   const pre=JSON.parse(fs.readFileSync(path.join(root,'output','reliable-prepare.json'),'utf8'))
   if(!pre.prepared)throw new Error('Power-cycle test was not prepared')
   const after=await drained();report.after=after
   if(after.boot<=pre.baseline.boot)throw new Error('D121 has not restarted since preparation')
   if(after.app_ack<2)throw new Error('No application acknowledgments after restart')
   pass('Actual NOR records survived D121 restart and drained only after cloud ACK',{old_boot:pre.baseline.boot,new_boot:after.boot,acks:after.app_ack})
   await archive(pre.baseline)
   command('COM6','bb start 1');await online()
   const s=await request('/api/status');report.final_status=s
   if(s.latest.boot!==after.boot||s.diagnostic?.write_errors||s.diagnostic?.dropped_frames)throw new Error('Capture did not resume cleanly with new boot identity')
   pass('TF recording and both live nodes resumed after restart',{boot:s.latest.boot,storage:s.diagnostic.storage})
  }else{
   const status=await online(),b=await drained(),last=(await request('/api/events?limit=1')).items[0]?.id||0
   report.baseline={...b,last_id:last,collection:status.latest.collection,collector:status.collector,diagnostic:status.diagnostic}
   if(!(status.collector.acks_submitted>=0))throw new Error('Acknowledgment backend is not deployed')
   command('COM6','cloud stop');cloudStopped=true;await pause(1000)
   // Keep collecting offline long enough that the saved context is newer than the last cloud sample.
   await pause(6000);stopped=true;command('COM5','rs485_slave stop');await pause(5000)
   const offline=board();if(offline.queued<1||!offline.durable||offline.persist_errors)throw new Error('Offline event was not durably queued')
   command('COM5','rs485_slave start 1');stopped=false;await pause(4000)
   report.queued=board();if(report.queued.queued<2||report.queued.persist_errors)throw new Error('Recovery event was not durably queued')
   pass('Outage and recovery collected while cloud was stopped, with committed NOR journal',{pending:report.queued.queued,journal:report.queued.journal,boot:report.queued.boot})
   if(mode==='prepare'){
    command('COM6','bb stop');captureStopped=true
    command('COM6','bbstat');prepared=true;report.prepared=true
    pass('TF files closed; queued events ready for a D121-only power cycle')
   }else{
    command('COM6','cloud start');cloudStopped=false
    const after=await drained();report.after=after
    if(after.app_ack<=b.app_ack||after.events<b.events+2)throw new Error('Queue drained without expected cloud ACK/event counts')
    pass('Reconnect replay received application acknowledgments and cleared queue',{pending:after.queued,acks:after.app_ack-b.app_ack,delivered:after.events-b.events})
    await archive(report.baseline)
    const final=await online();report.final_status=final
    if(final.diagnostic.write_errors!==status.diagnostic.write_errors||final.diagnostic.dropped_frames!==status.diagnostic.dropped_frames)throw new Error('Local TF capture errors during replay')
    pass('Both nodes live; TF capture unaffected',{storage:final.diagnostic.storage,write_errors:final.diagnostic.write_errors,dropped_frames:final.diagnostic.dropped_frames})
   }
  }
  command('COM6','apphealth');report.success=true
 }catch(e){report.success=false;report.error=String(e.message);throw e}
 finally{
  if(stopped)try{command('COM5','rs485_slave start 1')}catch(e){report.star_restore_error=String(e.message)}
  if(cloudStopped&&!prepared)try{command('COM6','cloud start')}catch(e){report.cloud_restore_error=String(e.message)}
  if(captureStopped&&!prepared)try{command('COM6','bb start 1')}catch(e){report.capture_restore_error=String(e.message)}
  fs.writeFileSync(reportPath,JSON.stringify(report,null,2))
  await mini.evaluate(()=>{delete getApp().globalData.reliableProbe;return true}).catch(()=>{})
  await mini.disconnect()
 }
}
main().catch(e=>{console.error(String(e.message));process.exitCode=1})
