// Controlled, reversible board incidents checked through the deployed MySQL API.
const fs=require('node:fs'),path=require('node:path'),{spawnSync}=require('node:child_process')
const automator=require('miniprogram-automator')
const root=path.resolve(__dirname,'..'),pause=ms=>new Promise(resolve=>setTimeout(resolve,ms))
async function main() {
  const mini=await automator.connect({wsEndpoint:'ws://127.0.0.1:9420'})
  const report={checked_at:new Date().toISOString(),checks:[]}
  let slaveStopped=false
  function command(port,text) {
    const result=spawnSync('pwsh.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',
      'D:\\xuantie\\work\\scripts\\serial\\board_cmd.ps1','-Port',port,'-Commands',text,'-TimeoutMs','6000'],
      {encoding:'utf8',timeout:10000,windowsHide:true})
    fs.appendFileSync(path.join(root,'output','cloud-hardware-serial.txt'),port+' '+text+'\n'+(result.stdout||'')+'\n')
    if(result.status!==0)throw new Error('Board command failed: '+port+' '+text)
  }
  async function request(url) {
    await mini.evaluate(url=>{
      const app=getApp(),cfg=app.globalData.config;app.globalData.hardwareProbe=null
      wx.cloud.callContainer({config:{env:cfg.envId},path:url,method:'GET',
        header:{'X-WX-SERVICE':cfg.service,Authorization:'Bearer '+app.globalData.token},
        success:r=>{app.globalData.hardwareProbe={status:r.statusCode,data:r.data}},
        fail:()=>{app.globalData.hardwareProbe={error:'Cloud transport failed'}}})
      return true
    },url)
    for(let i=0;i<30;i++) {
      await pause(500)
      const value=JSON.parse(await mini.evaluate(()=>JSON.stringify(getApp().globalData.hardwareProbe)))
      if(value) {
        if(value.error||value.status!==200)throw new Error(value.error||'Cloud status '+value.status)
        return value.data
      }
    }
    throw new Error('Cloud response timeout')
  }
  async function until(predicate,seconds) {
    const deadline=Date.now()+seconds*1000
    do {const value=await request('/api/status');if(predicate(value))return value;await pause(500)}while(Date.now()<deadline)
    throw new Error('Expected cloud device state not observed')
  }
  function pass(name,value={}) {report.checks.push({name,...value});console.log('PASS '+name+': '+JSON.stringify(value))}
  try {
    const baseline=await request('/api/status')
    if(baseline.state!=='online'||baseline.latest.nodes.some(node=>!node.online||!node.fresh)||!baseline.diagnostic)throw new Error('Cloud hardware baseline is not healthy')
    const recent=await request('/api/events?limit=1'),lastId=recent.items[0]?.id||0
    slaveStopped=true;command('COM5','rs485_slave stop')
    await until(value=>value.state==='online'&&!value.latest.nodes[0].online&&value.latest.nodes[1].online,12)
    const starEvents=await request('/api/events?node=1&limit=10')
    const star=starEvents.items.find(event=>event.id>lastId&&event.title==='通信中断')
    if(!star?.snapshot || star.snapshot.nodes[0].temperature===null)throw new Error('Star pre-outage data missing')
    const starDetail=await request('/api/events/'+star.id)
    if(starDetail.available_points<2 || starDetail.diagnosis.confidence!=='原因尚未确认')throw new Error('Star curve or diagnosis failed')
    report.star_incident=starDetail
    pass('Star offline while Loongson stays online; prior readings and curve saved',{event_id:star.id,points:starDetail.available_points})
    command('COM5','rs485_slave start 1');slaveStopped=false
    await until(value=>value.state==='online'&&value.latest.nodes.every(node=>node.online&&node.fresh),15)
    pass('Star communication restored')
    const masterBefore=(await request('/api/events?node=0&limit=1')).items[0]?.id||0
    command('COM6','cloud pause 45') // Board resumes automatically, including on test failure.
    const offline=await until(value=>value.state==='offline',30)
    if(!offline.collector.connected)throw new Error('Cloud receiver also disconnected')
    const masterEvents=await request('/api/events?node=0&limit=10')
    const incidents=masterEvents.items.filter(event=>event.id>masterBefore&&event.title==='主站上传中断')
    const master=incidents[0]
    report.master_outage_count=incidents.length
    if(!master?.snapshot || master.snapshot.nodes[0].temperature===null || master.snapshot.nodes[1].cpu===null)throw new Error('Master pre-outage node snapshots missing')
    const masterDetail=await request('/api/events/'+master.id)
    if(masterDetail.available_points<2)throw new Error('Master pre-outage curve missing')
    report.offline_status=offline;report.master_incident=masterDetail
    pass('Master upload interrupted; cloud stays connected and both node histories remain readable',{event_id:master.id,points:masterDetail.available_points})
    const recovered=await until(value=>value.state==='online'&&value.latest.nodes.every(node=>node.online&&node.fresh),65)
    const resolved=await request('/api/events/'+master.id)
    if(!resolved.resolved || !resolved.diagnosis.observations.some(text=>text.includes('未观察到主站重新启动')))throw new Error('Same-boot recovery was not persisted')
    if(recovered.latest.boot!==baseline.latest.boot || recovered.diagnostic.storage!==baseline.diagnostic.storage || recovered.diagnostic.write_errors!==baseline.diagnostic.write_errors || recovered.diagnostic.dropped_frames!==baseline.diagnostic.dropped_frames)throw new Error('Boot or local storage changed during cloud test')
    report.recovered_status=recovered;report.master_resolved=resolved
    pass('Both nodes recovered; MySQL incident resolved and TF recording retained',{storage:recovered.diagnostic.storage,write_errors:recovered.diagnostic.write_errors,dropped_frames:recovered.diagnostic.dropped_frames})
    command('COM6','bbstat');command('COM6','cloud stat');command('COM6','apphealth')
    if(incidents.length!==1)throw new Error('One upload interruption produced '+incidents.length+' incidents; check concurrent old versions or replicas')
    pass('One controlled upload interruption creates exactly one archived incident',{count:incidents.length})
    report.success=true
  } catch(error) {report.success=false;report.error=String(error.message);throw error}
  finally {
    if(slaveStopped){try{command('COM5','rs485_slave start 1');report.star_restored=true}catch(error){report.restore_error=String(error.message)}}
    fs.writeFileSync(path.join(root,'output','cloud-hardware-verification.json'),JSON.stringify(report,null,2))
    await mini.evaluate(()=>{delete getApp().globalData.hardwareProbe;return true}).catch(()=>{})
    await mini.disconnect()
  }
}
main().catch(error=>{console.error(String(error.message));process.exitCode=1})
