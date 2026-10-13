// Check the actual replayed incident page through the native WeChat simulator.
const fs=require('node:fs'),path=require('node:path'),automator=require('miniprogram-automator')
const root=path.resolve(__dirname,'..'),pause=ms=>new Promise(r=>setTimeout(r,ms))
async function main(){
 const verified=JSON.parse(fs.readFileSync(path.join(root,'output','reliable-verify.json'),'utf8'))
 if(!verified.success)throw new Error('Actual restart verification must pass first')
 const id=verified.incident.id,mini=await automator.connect({wsEndpoint:'ws://127.0.0.1:9420'})
 const report={checked_at:new Date().toISOString(),event_id:id,success:false}
 try{
  await mini.evaluate(id=>{wx.reLaunch({url:'/pages/detail/index?id='+id});return true},id)
  let data
  for(let i=0;i<60;i++){
   await pause(250)
   data=JSON.parse(await mini.evaluate(()=>{const p=getCurrentPages(),v=p[p.length-1];return JSON.stringify({route:v.route,...v.data})}))
   if(data.route==='pages/detail/index'&&data.event?.id===id)break
   if(data.route==='pages/detail/index'&&data.error)throw new Error('Replayed incident page failed')
  }
  const e=data.event
  if(!e||e.id!==id||!e.snapshot?.replayed||e.boot!==verified.incident.boot)throw new Error('Native page did not load the original replayed event')
  const label=e.snapshot.time_estimated?'读数时间（推算）':'补传接收于'
  if(e.snapshotTimeLabel!==label||!e.snapshotTime||e.last?.temperature==='—'||e.last?.vibration==='—')throw new Error('Replayed snapshot readings or time-source label missing')
  report.boot=e.boot;report.snapshot_label=e.snapshotTimeLabel;report.temperature=e.last.temperature;report.vibration=e.last.vibration
  report.success=true
  console.log('PASS native replayed event: '+JSON.stringify(report))
 }catch(e){report.error=String(e.message);throw e}
 finally{
  fs.writeFileSync(path.join(root,'output','reliable-native.json'),JSON.stringify(report,null,2))
  await mini.evaluate(()=>{wx.switchTab({url:'/pages/home/index'});return true}).catch(()=>{})
  await mini.disconnect()
 }
}
main().catch(e=>{console.error(String(e.message));process.exitCode=1})
