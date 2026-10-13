const api=require('../../utils/api'),fmt=require('../../utils/format')
Page({
 data:{items:[],node:-1,next:null,loading:false,error:''},
 onShow(){this.visible=true;if(api.ensureLogin())this.load(false)},onHide(){this.visible=false},onPullDownRefresh(){this.load(false).finally(()=>wx.stopPullDownRefresh())},
 async load(more){if(this.data.loading)return;const generation=this.generation=(this.generation||0)+1;this.setData({loading:true});try{const path='/api/events?limit=20'+(this.data.node>=0?'&node='+this.data.node:'')+(more&&this.data.next?'&before='+this.data.next:'');const value=await api.request(path);if(generation===this.generation&&this.visible)this.setData({items:more?this.data.items.concat(value.items.map(fmt.event)):value.items.map(fmt.event),next:value.next_before,error:''})}catch(e){if(generation===this.generation&&this.visible)this.setData({error:e.message})}finally{if(generation===this.generation)this.setData({loading:false})}},
 filter(e){this.setData({node:Number(e.currentTarget.dataset.node),items:[],next:null,loading:false});this.load(false)},
 more(){if(this.data.next)this.load(true)},onReachBottom(){this.more()},
 open(e){wx.navigateTo({url:'/pages/detail/index?id='+e.currentTarget.dataset.id})}
})
