const api = require('../../utils/api')
const starMetrics=[{key:'temperature',label:'温度',unit:'℃'},{key:'humidity',label:'湿度',unit:'%'},{key:'vibration',label:'振动',unit:'g'}]
const linuxMetrics=[{key:'cpu',label:'CPU',unit:'%'},{key:'memory_mb',label:'内存',unit:'MB'},{key:'load',label:'系统负载',unit:''}]
Page({
  data: { node: 1, metric: 'temperature', unit: '℃', minutes: 10, points: [], error: '', metrics: starMetrics },
  onShow() {
    if (!api.ensureLogin()) return
    const app=getApp(),target=app.globalData.pendingTrendNode
    if(target===1||target===2){this.applyNode(target);delete app.globalData.pendingTrendNode}
    this.visible=true;this.refresh();this.timer=setInterval(()=>this.refresh(),3000)
  },
  onHide(){this.visible=false; clearInterval(this.timer)}, onUnload(){this.onHide()},
  async refresh(){if(this.busy)return;this.busy=true;const minutes=this.data.minutes;try{const value=await api.request('/api/series?minutes='+minutes);if(this.visible&&minutes===this.data.minutes)this.setData({points:value.items,error:''})}catch(e){if(this.visible)this.setData({error:e.message})}finally{this.busy=false;if(this.visible&&minutes!==this.data.minutes)this.refresh()}},
  applyNode(node){if(node!==1&&node!==2)return;const metrics=node===1?starMetrics:linuxMetrics;this.setData({node,metrics,metric:metrics[0].key,unit:metrics[0].unit})},
  selectNode(e){this.applyNode(Number(e.currentTarget.dataset.node))},
  selectMetric(e){const metric=e.currentTarget.dataset.metric;const m=this.data.metrics.find(x=>x.key===metric);this.setData({metric,unit:m.unit})},
  selectTime(e){this.setData({minutes:Number(e.currentTarget.dataset.minutes)});this.refresh()}
})
