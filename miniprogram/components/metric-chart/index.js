const { clock } = require('../../utils/format')
Component({
  properties: { 
    points: { type: Array, value: [] }, 
    node: { type: Number, value: 1 }, 
    field: { type: String, value: 'temperature' },
    unit: { type: String, value: '℃' }, 
    color: { type: String, value: '#59dfc6' } },
    data: { hasValues: false },
    observers: { 'points,node,field': function () { this.draw() } },
    lifetimes: { ready() { this.ready = true; this.draw() }, 
    detached() { this.ready = false } 
  },
  methods: {
    draw() {
      if (!this.ready) return
      this.createSelectorQuery().select('#plot').fields({ node: true, size: true }).exec(result => {
        if (!this.ready || !result[0] || !result[0].node) return
        const canvas = result[0].node, w = result[0].width, h = result[0].height
        const info = wx.getWindowInfo ? wx.getWindowInfo() : wx.getSystemInfoSync(), ratio = info.pixelRatio || 1
        canvas.width = Math.round(w * ratio); canvas.height = Math.round(h * ratio)
        const ctx = canvas.getContext('2d'); ctx.scale(ratio, ratio)
        const points = this.data.points || [], node = this.data.node - 1, field = this.data.field
        const values = points.map(p => { const n = (p.nodes || [])[node]; return { t: p.received_at, boot: p.boot, c: p.collection,
          v: n && n.online && n.fresh && p.running && Number.isFinite(n[field]) ? n[field] : null } })
        const good = values.filter(p => p.v !== null)
        this.setData({ hasValues: good.length > 0 })
        const left = 38, right = w - 10, top = 18, bottom = h - 27
        let lo = good.length ? Math.min(...good.map(p => p.v)) : 0, hi = good.length ? Math.max(...good.map(p => p.v)) : 1
        const padding = Math.max((hi - lo) * .18, this.data.field === 'load' ? .05 : 1)
        lo -= padding; hi += padding
        if (field !== 'temperature') lo = Math.max(0, lo)
        ctx.font = '10px sans-serif'; ctx.strokeStyle = '#303b4c'; ctx.fillStyle = '#8c9fb8'; ctx.lineWidth = 1
        for (let i = 0; i < 4; i++) { const y = top + (bottom - top) * i / 3; ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke(); ctx.fillText((hi - (hi - lo) * i / 3).toFixed(1), 0, y + 3) }
        if (!values.length) return
        const start = values[0].t, finish = values[values.length - 1].t, span = Math.max(finish - start, 1)
        const x = t => left + (right - left) * (t - start) / span, y = v => bottom - (bottom - top) * (v - lo) / (hi - lo)
        ctx.strokeStyle = this.data.color; ctx.lineWidth = 2; ctx.beginPath()
        let previous = null
        values.forEach(p => {
          if (p.v === null) { previous = null; return }
          if (!previous || p.t - previous.t > 5 || p.boot !== previous.boot || p.c !== previous.c) ctx.moveTo(x(p.t), y(p.v))
          else ctx.lineTo(x(p.t), y(p.v))
          previous = p
        }); ctx.stroke()
        good.forEach((p,i) => { if (good.length === 1 || i === good.length-1) { ctx.fillStyle = this.data.color; ctx.beginPath(); ctx.arc(x(p.t),y(p.v),3,0,Math.PI*2); ctx.fill() } })
        ctx.fillStyle = '#8c9fb8'; ctx.fillText(clock(start,false),left,bottom+20); ctx.fillText(clock(finish,false),Math.max(left,right-55),bottom+20)
      })
    }
  }
})
