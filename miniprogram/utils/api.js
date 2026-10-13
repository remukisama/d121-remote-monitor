function request(path, method = 'GET', data) {
  const app = getApp(), config = app.globalData.config
  const headers = { 'Content-Type': 'application/json', Authorization: 'Bearer ' + app.globalData.token }
  return new Promise((resolve, reject) => {
    const success = r => {
      if (r.statusCode >= 200 && r.statusCode < 300) resolve(r.data)
      else {
        if (r.statusCode === 401) { app.globalData.token = ''; wx.removeStorageSync('d121_session'); ensureLogin() }
        const error = new Error((r.data && r.data.detail) || '服务暂时不可用')
        error.status = r.statusCode; reject(error)
      }
    }
    const fail = () => reject(new Error('暂时连接不到查看服务，请检查手机网络。最后保存的数据仍可在服务恢复后查看。'))
    if (config.mode === 'cloud') {
      if (!config.envId || !config.service) return reject(new Error('微信云托管环境尚未配置'))
      wx.cloud.callContainer({ config: { env: config.envId }, path, method, data,
        header: Object.assign({ 'X-WX-SERVICE': config.service }, headers), success, fail })
    } else wx.request({ url: config.baseUrl + path, method, data, header: headers, timeout: 10000, success, fail })
  })
}
function saveSession(value) { getApp().globalData.token = value.token; wx.setStorageSync('d121_session', value.token) }
function ensureLogin() {
  if (getApp().globalData.token) return true
  const pages=getCurrentPages()
  if ((pages.length && pages[pages.length-1].route==='pages/login/index') || getApp().globalData.openingLogin) return false
  getApp().globalData.openingLogin=true
  wx.navigateTo({ url: '/pages/login/index',complete:()=>{getApp().globalData.openingLogin=false} }); return false
}
module.exports = { request, saveSession, ensureLogin }
