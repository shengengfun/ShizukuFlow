import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App'
import { ErrorBoundary } from './components/ErrorBoundary'
import './globals.css'

/**
 * 全局兜底横幅：捕获「渲染错误边界」管不到的未处理异常 / Promise 拒绝，
 * 在页面顶部贴一条红色横幅，而不是让窗口静默白屏。
 * 这正面回应了「程序会不会整个找不到页面」的顾虑 —— 即便 webview 内 JS 出错，
 * 用户也一定看得到原因。
 */
function showGlobalError(message: string) {
  if (typeof document === 'undefined') return
  let el = document.getElementById('global-error-banner')
  if (!el) {
    el = document.createElement('div')
    el.id = 'global-error-banner'
    el.style.cssText =
      'position:fixed;top:0;left:0;right:0;z-index:99999;background:#7f1d1d;color:#fff;' +
      'font:12px/1.5 ui-monospace,monospace;padding:8px 12px;white-space:pre-wrap;' +
      'box-shadow:0 2px 8px rgba(0,0,0,.4);max-height:40vh;overflow:auto;'
    document.body.appendChild(el)
  }
  el.textContent = `ShizukuFlow 运行异常（程序未退出）：${message}`
}

window.addEventListener('error', (e) => {
  showGlobalError(e.message || String(e.error))
})
window.addEventListener('unhandledrejection', (e) => {
  const r = e.reason
  showGlobalError(r && r.message ? r.message : String(r))
})

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </React.StrictMode>,
)
