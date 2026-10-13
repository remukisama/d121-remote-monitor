const api=require('../../utils/api'),fmt=require('../../utils/format')
function reading(event,node) {
  const value=event.snapshot ? event.snapshot.nodes[node-1] : null
  return value ? {temperature:fmt.numeric(value.temperature),humidity:fmt.numeric(value.humidity),
    cpu:fmt.numeric(value.cpu),memory:fmt.numeric(value.memory_mb),load:fmt.numeric(value.load,2)} : null
}
function savedTime(event,node) {
  if(!event.snapshot)return ''
  const times=event.snapshot.node_received_at
  return fmt.clock(times ? times[node-1] : event.snapshot.received_at)
}
Page({
  data:{event:null,error:'',metric:'temperature',unit:'℃',chartNode:1},
  onLoad(query){this.id=Number(query.id);this.load()},
  async load(){
    try{
      const value=await api.request('/api/events/'+this.id),node=value.node===2?2:1
      this.setData({event:Object.assign(fmt.event(value),{
        snapshotTime:savedTime(value,node),last:reading(value,node)
      }),chartNode:node,metric:node===2?'cpu':'temperature',unit:node===2?'%':'℃',error:''})
    }catch(error){this.setData({error:error.message})}
  },
  selectNode(e){
    const node=Number(e.currentTarget.dataset.node)
    if(!this.data.event || this.data.event.node!==0 || ![1,2].includes(node))return
    this.setData({chartNode:node,metric:node===2?'cpu':'temperature',unit:node===2?'%':'℃',
      'event.last':reading(this.data.event,node),'event.snapshotTime':savedTime(this.data.event,node)})
  },
  selectMetric(e){this.setData({metric:e.currentTarget.dataset.metric,unit:e.currentTarget.dataset.unit})}
})
