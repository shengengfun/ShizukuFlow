import { Component, type ReactNode } from 'react'

interface Props {
  children: ReactNode
}

interface State {
  error: Error | null
}

/**
 * 全局错误边界。
 *
 * 用途：任何 React 渲染期异常都不会变成「白屏 / 找不到页面」——
 * 而是显示一段可读的错误信息，程序其余部分仍可运行。
 *
 * 配合 `main.tsx` 里的全局 `window.onerror` / `unhandledrejection` 横幅，
 * 构成「前端永不静默崩溃」的双保险。这也正面回应了一个常见顾虑：
 * 即便 webview 里某段 JS 出错，用户看到的也是明确提示，不是空白窗口。
 */
export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: unknown) {
    // 打到控制台，方便排查；不在这里弹原生对话框（那样会打断操作）
    console.error('[ShizukuFlow] 界面渲染错误：', error, info)
  }

  render() {
    if (this.state.error) {
      return (
        <div className="flex h-full w-full flex-col items-center justify-center gap-3 bg-[#08080b] p-8 text-center text-foreground">
          <div className="text-lg font-semibold text-destructive">界面渲染出错</div>
          <pre className="text-muted-foreground max-w-lg overflow-auto rounded bg-card p-3 text-left text-xs">
            {String(this.state.error.stack || this.state.error.message)}
          </pre>
        </div>
      )
    }
    return this.props.children
  }
}
