/**
 * 渲染后端能力探测。
 *
 * WebView2（Windows）/ WKWebView（macOS）默认就用 GPU 加速 Canvas2D / WebGL2，
 * 我们不去关它。这里只是把「到底有没有 WebGL2、跑在哪家显卡上」探出来，
 * 显示给用户在状态栏 —— 既是透明告知，也方便日后把重特效（粒子装饰、模糊）
 * 切到 WebGL2 后端时做能力门控。
 */

export interface RenderCaps {
  webgl2: boolean
  webgl: boolean
  /** 显卡型号（UNMASKED_RENDERER），探测不到则为 null */
  gpuRenderer: string | null
}

export function detectRenderer(): RenderCaps {
  if (typeof document === 'undefined') {
    return { webgl2: false, webgl: false, gpuRenderer: null }
  }
  try {
    const c = document.createElement('canvas')
    const gl2 = c.getContext('webgl2') as WebGL2RenderingContext | null
    const webgl2 = !!gl2
    let gpuRenderer: string | null = null
    if (gl2) {
      const dbg = gl2.getExtension('WEBGL_debug_renderer_info')
      if (dbg) {
        gpuRenderer = gl2.getParameter(dbg.UNMASKED_RENDERER_WEBGL) as string
      }
    }
    const gl1 = c.getContext('webgl') as WebGLRenderingContext | null
    return { webgl2, webgl: !!gl1, gpuRenderer }
  } catch {
    return { webgl2: false, webgl: false, gpuRenderer: null }
  }
}

/** 给状态栏用的简短后端标签。 */
export function rendererLabel(caps: RenderCaps): string {
  if (caps.webgl2) return 'WebGL2（GPU 加速）'
  if (caps.webgl) return 'WebGL（GPU 加速）'
  return 'Canvas2D'
}
