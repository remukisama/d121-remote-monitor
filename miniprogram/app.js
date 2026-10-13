const config = require('./config')
App({
  globalData: { config, token: '' },
  onLaunch() {
    this.globalData.token = wx.getStorageSync('d121_session') || ''
    if (config.mode === 'cloud' && wx.cloud) wx.cloud.init({ env: config.envId, traceUser: false })
  }
})
