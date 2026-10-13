// Verify the deployed backend through the real WeChat simulator transport.
// No mock login, local development token, or account credential is logged.
const fs = require('node:fs'), path = require('node:path')
const automator = require('miniprogram-automator')
const root = path.resolve(__dirname, '..')
const pause = ms => new Promise(resolve => setTimeout(resolve, ms))
const guard = setTimeout(() => { console.error('Cloud QA timeout'); process.exit(1) }, 120000)
guard.unref()
async function main() {
  const cfg = JSON.parse(fs.readFileSync(path.join(root, 'config.private.json'), 'utf8').replace(/^\uFEFF/, ''))
  const mini = await automator.connect({wsEndpoint:'ws://127.0.0.1:9420'})
  const report = {checked_at:new Date().toISOString(),checks:[]}
  async function request(url, method='GET', data) {
    await mini.evaluate(v => {
      const app=getApp(), config=app.globalData.config
      app.globalData.cloudProbe=null
      wx.cloud.callContainer({config:{env:config.envId},path:v.url,method:v.method,data:v.data,
        header:{'X-WX-SERVICE':config.service,'Content-Type':'application/json',Authorization:'Bearer '+app.globalData.token},
        success:r=>{app.globalData.cloudProbe={status:r.statusCode,data:r.data}},
        fail:e=>{app.globalData.cloudProbe={error:e.errMsg||'Cloud transport failed'}}})
      return true
    }, {url,method,data:data||null})
    for(let i=0;i<60;i++) {
      await pause(500)
      const raw=await mini.evaluate(()=>JSON.stringify(getApp().globalData.cloudProbe))
      if(raw && raw!=='null') {
        const value=JSON.parse(raw)
        if(value.error)throw new Error(value.error)
        return value
      }
    }
    throw new Error('Cloud response timeout: '+url)
  }
  function pass(name,value) { report.checks.push({name,...value}); console.log('PASS '+name+': '+JSON.stringify(value)) }
  async function login(pairing) {
    await mini.evaluate(()=>{wx.reLaunch({url:'/pages/login/index'});return true})
    await pause(1500)
    await mini.evaluate(pairing=>{const pages=getCurrentPages(),page=pages[pages.length-1];page.setData({pairing});page.login();return true},pairing)
    for(let i=0;i<50;i++) {
      await pause(500)
      const value=JSON.parse(await mini.evaluate(()=>{const pages=getCurrentPages(),page=pages[pages.length-1];return JSON.stringify({authenticated:!!getApp().globalData.token,error:page.data.error,busy:page.data.busy})}))
      if(value.authenticated)return
      if(value.error && !value.busy)throw new Error('Real WeChat login failed: '+value.error)
    }
    throw new Error('Real WeChat login timeout')
  }
  try {
    const route=JSON.parse(await mini.evaluate(()=>JSON.stringify(getApp().globalData.config)))
    if(route.mode!=='cloud' || route.envId!==cfg.deployment.cloud_env_id || route.service!==cfg.deployment.cloud_service)throw new Error('Simulator has not recompiled the cloud routing settings')
    await mini.evaluate(()=>{wx.removeStorageSync('d121_session');getApp().globalData.token='';return true})
    const health=await request('/health')
    if(health.status!==200 || !health.data.ok)throw new Error('Cloud health check failed: '+JSON.stringify(health))
    pass('cloud health', {status:health.status,service:health.data.service})
    const unauth=await request('/api/status')
    if(unauth.status!==401)throw new Error('Anonymous device status was not rejected')
    pass('anonymous access rejected',{status:unauth.status})
    const local=await request('/api/auth/local','POST',{code:'cloud-local-login-must-be-disabled'})
    if(local.status!==403)throw new Error('Local development login was not disabled')
    pass('local development login disabled',{status:local.status})
    // A deliberately invalid code lets us distinguish upstream connectivity from
    // account authorization without consuming a real wx.login credential.
    const upstream=await request('/api/auth/wechat','POST',{code:'cloud-connectivity-probe-invalid-code'})
    if(upstream.status===503)throw new Error('Cloud WeChat upstream unavailable: '+(upstream.data.diagnostic_code||'diagnostic update not deployed'))
    if(upstream.status!==401)throw new Error('Unexpected invalid-code response: '+upstream.status)
    pass('WeChat upstream reachable',{status:upstream.status})
    await login(cfg.server.pairing_code)
    pass('real WeChat login and pairing',{authenticated:true})
    const status=await request('/api/status')
    if(status.status!==200)throw new Error('Cloud device status failed')
    report.status=status.data
    const nodes=status.data.latest?.nodes||[]
    pass('cloud device status',{state:status.data.state,collector:status.data.collector,node_count:nodes.length})
    if(nodes.length!==2 || nodes.some(node=>!node.online || !node.fresh))throw new Error('Both cloud nodes are not reporting fresh live data')
    // State and motion arrive as separate packets; allow the matching motion to land.
    let star=nodes.find(node=>node.node===1)
    for(let i=0;i<12 && (!Number.isFinite(star?.vibration)||!star?.vibration_samples);i++){
      await pause(400);star=(await request('/api/status')).data.latest?.nodes?.find(node=>node.node===1)
    }
    if(!star || !Number.isFinite(star.vibration) || !(star.vibration_samples>0))throw new Error('Real Star vibration packets not joined to cloud samples')
    pass('cloud live vibration',{peak_g:star.vibration,valid_samples:star.vibration_samples})
    const series=await request('/api/series?minutes=10')
    if(series.status!==200)throw new Error('Cloud archived samples failed')
    pass('cloud archived samples',{count:series.data.items.length})
    const motionPoints=series.data.items.filter(p=>Number.isFinite(p.nodes?.[0]?.vibration))
    if(!motionPoints.length)throw new Error('MySQL archived vibration series is empty')
    pass('cloud archived vibration',{count:motionPoints.length})
    const events=await request('/api/events?limit=20')
    if(events.status!==200)throw new Error('Cloud archived events failed')
    pass('cloud archived events',{count:events.data.items.length})
    const priorReport=path.join(root,'output','cloud-hardware-verification.json')
    if(fs.existsSync(priorReport)){
      const id=JSON.parse(fs.readFileSync(priorReport,'utf8')).master_incident?.id
      if(id){const prior=await request('/api/events/'+id);if(prior.status!==200||!prior.data.snapshot)throw new Error('Pre-deployment incident history was not preserved');pass('pre-deployment history preserved',{event_id:id})}
    }
    if(events.data.items.length) {
      const detail=await request('/api/events/'+events.data.items[0].id)
      if(detail.status!==200)throw new Error('Cloud event detail failed')
      pass('cloud event detail',{id:events.data.items[0].id})
    }
    const initialReceived=status.data.collector.received
    await pause(2500)
    const next=await request('/api/status')
    if(next.status!==200 || next.data.state!=='online' || !next.data.collector.connected || next.data.collector.received<=initialReceived || !series.data.items.length)throw new Error('Cloud receiver is not continuously archiving live device packets: '+JSON.stringify(next.data.collector))
    pass('continuous cloud reception',{received_before:initialReceived,received_after:next.data.collector.received})
    // Hide polling tabs while testing token revocation and persisted membership.
    await mini.evaluate(()=>{wx.reLaunch({url:'/pages/login/index'});return true})
    await pause(500)
    const logout=await request('/api/auth/logout','POST')
    if(logout.status!==200 || (await request('/api/status')).status!==401)throw new Error('Cloud logout did not revoke the old session')
    await mini.evaluate(()=>{getApp().globalData.token='';wx.removeStorageSync('d121_session');return true})
    pass('cloud logout revokes old session',{revoked:true})
    await login('')
    if((await request('/api/status')).status!==200)throw new Error('Previously paired WeChat could not access the device after login')
    pass('paired owner can log in again without pairing code',{authenticated:true})
    report.success=true
  } catch(error) {
    report.success=false;report.error=String(error.message);throw error
  } finally {
    fs.mkdirSync(path.join(root,'output'),{recursive:true})
    fs.writeFileSync(path.join(root,'output','cloud-verification.json'),JSON.stringify(report,null,2))
    await mini.evaluate(()=>{delete getApp().globalData.cloudProbe;return true}).catch(()=>{})
    await mini.disconnect()
  }
}
main().catch(error=>{console.error(String(error.message));process.exitCode=1})
