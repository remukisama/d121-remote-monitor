const assert=require('node:assert/strict'),path=require('node:path')
const fmt=require('../miniprogram/utils/format')
assert.equal(fmt.numeric(0),'0.0')
assert.equal(fmt.numeric(null),'—')
assert.equal(fmt.clock(0),'1970/01/01 08:00:00')
assert.equal(fmt.event({title:'主站上传中断',source:'heartbeat',received:20,occurred:20,uptime:5}).timeLabel,'云端发现中断时间')
assert.equal(fmt.event({title:'通信中断',source:'device',received:20,occurred:10,uptime:5}).timeLabel,'推算发生时间')
const delayed=[]
const apiPath=require.resolve('../miniprogram/utils/api')
require.cache[apiPath]={exports:{ensureLogin:()=>true,request:url=>new Promise(resolve=>delayed.push({url,resolve}))}}
let definition
global.Page=value=>{definition=value}
require('../miniprogram/pages/events/index')
const page=Object.assign({},definition,{data:Object.assign({},definition.data),visible:true,setData(value){Object.assign(this.data,value)}})
async function run(){
 const first=page.load(false)
 page.filter({currentTarget:{dataset:{node:'1'}}})
 assert.equal(delayed.length,2)
 delayed[1].resolve({items:[{id:2,title:'通信中断',node:1,received:2,occurred:null}],next_before:null})
 await Promise.resolve();await Promise.resolve()
 delayed[0].resolve({items:[{id:1,title:'主站上传中断',node:0,received:1,occurred:null}],next_before:null})
 await first
 assert.equal(page.data.node,1);assert.equal(page.data.items[0].node,1)
 assert.equal(page.data.loading,false)
 console.log('PASS: valid zero values, UTC+8 labels and event filter race with reordered responses')
 const app={globalData:{}},tabs=[]
 global.getApp=()=>app;global.wx={switchTab:value=>tabs.push(value.url)}
 require.cache[apiPath].exports.request=async()=>({items:[]})
 require('../miniprogram/pages/home/index')
 const home=Object.assign({},definition)
 require('../miniprogram/pages/trends/index')
 const trends=Object.assign({},definition,{data:Object.assign({},definition.data),setData(value){Object.assign(this.data,value)}})
 for(const node of [2,1,2]) {
   home.trends({currentTarget:{dataset:{node:String(node)}}})
   assert.equal(tabs[tabs.length-1],'/pages/trends/index')
   trends.onShow();trends.onHide()
   assert.equal(trends.data.node,node)
   assert.equal(trends.data.metric,node===2?'cpu':'temperature')
   assert.equal(app.globalData.pendingTrendNode,undefined)
 }
 trends.selectNode({currentTarget:{dataset:{node:'1'}}})
 assert.ok(trends.data.metrics.some(metric=>metric.key==='vibration'))
 trends.selectMetric({currentTarget:{dataset:{metric:'vibration'}}})
 assert.equal(trends.data.unit,'g')
 console.log('PASS: both overview entries select the right node on a cached trend page; vibration is selectable')
}
run().catch(e=>{console.error(e);process.exit(1)})
