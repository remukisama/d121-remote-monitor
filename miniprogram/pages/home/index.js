const api = require('../../utils/api'), fmt = require('../../utils/format')
Page({
  data: { status: null, events: [], loading: true, error: '' },
  onShow() { if (!api.ensureLogin()) return; this.visible = true; this.refresh(); this.timer = setInterval(() => this.refresh(), 2000) },
  onHide() { this.visible = false; clearInterval(this.timer) },
  onUnload() { this.onHide() },
  onPullDownRefresh() { this.refresh().finally(() => wx.stopPullDownRefresh()) },
  async refresh() {
    if (this.busy) return; this.busy = true
    try { const [status, events] = await Promise.all([api.request('/api/status'), api.request('/api/events?limit=3')])
      if (this.visible) this.setData({ status: fmt.status(status), events: events.items.map(fmt.event), loading: false, error: '' })
    } catch (e) { if (this.visible) this.setData({ error: e.message, loading: false }); if (e.status === 401) { this.onHide(); api.ensureLogin() } }
    finally { this.busy = false }
  },
  openEvent(e) { wx.navigateTo({ url: '/pages/detail/index?id=' + e.currentTarget.dataset.id }) },
  allEvents() { wx.switchTab({ url: '/pages/events/index' }) },
  trends() { wx.switchTab({ url: '/pages/trends/index' }) }
})
