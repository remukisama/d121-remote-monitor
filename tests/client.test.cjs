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
}
run().catch(e=>{console.error(e);process.exit(1)})
