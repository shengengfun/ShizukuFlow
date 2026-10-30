/**
 * LUME 文字布局层：整形 + 测量 + 布局缓存。
 *
 * 「动画与布局分离」（GuideOfUI.md §3.2，全引擎性能生命线）：
 * 布局只取决于 文本 + 字体 + 字距，与时间 t 无关 —— 缓存后逐帧动画零布局开销。
 * 将来下沉 Rust，这里平移为 HarfBuzz 整形 + 布局缓存，缓存键语义保持一致。
 */

import { LruCache, type CacheStats } from '../core/cache'
import type { TextLayout } from '../core/types'

/** 共享离屏测量画布（模块级懒初始化；浏览器环境必需）。 */
let measureCtx: CanvasRenderingContext2D | null = null

export function measureContext(): CanvasRenderingContext2D {
  if (!measureCtx) {
    const cv = document.createElement('canvas')
    cv.width = 8
    cv.height = 8
    measureCtx = cv.getContext('2d')
    if (!measureCtx) throw new Error('LUME: 2D context unavailable')
  }
  return measureCtx
}

const layoutCache = new LruCache<string, TextLayout>(1024)

export function layoutStats(): CacheStats {
  return { ...layoutCache.stats, size: layoutCache.size } as CacheStats & { size: number }
}

export function resetLayoutCache(): void {
  layoutCache.clear()
}

/**
 * 布局一段文字。`segments` 是逐段（音节/字符）文本，段间距 = letterSpacing。
 * 同一 (segments, font, letterSpacing) 组合全宇宙只测一次。
 */
export function layoutRun(segments: readonly string[], font: string, letterSpacing: number): TextLayout {
  const key = `${font}|${letterSpacing}|${segments.join('\u0000')}`
  const hit = layoutCache.get(key)
  if (hit) return hit

  const ctx = measureContext()
  ctx.font = font
  let x = 0
  const boxes = segments.map((text) => {
    const width = ctx.measureText(text).width
    const box = { text, x, width }
    x += width + letterSpacing
    return box
  })
  const totalWidth = segments.length > 0 ? x - letterSpacing : 0
  const layout: TextLayout = { font, letterSpacing, boxes, totalWidth }
  layoutCache.set(key, layout)
  return layout
}

export interface FontBoxMetrics {
  /** em-box 上半高（基线到 em 顶） */
  ascent: number
  /** em-box 下半高（基线到 em 底） */
  descent: number
}

const middleCache = new LruCache<string, FontBoxMetrics>(256)

function fontBox(font: string): FontBoxMetrics {
  const hit = middleCache.get(font)
  if (hit) return hit
  const ctx = measureContext()
  ctx.font = font
  const m = ctx.measureText('国Mjg')
  const size = parseFloat(font) || 96
  const metrics: FontBoxMetrics = {
    ascent: m.fontBoundingBoxAscent ?? size * 0.88,
    descent: m.fontBoundingBoxDescent ?? size * 0.12,
  }
  middleCache.set(font, metrics)
  return metrics
}

/**
 * 把「middle 基线锚点」换算成「alphabetic 基线 y」。
 * 旧渲染管线用 textBaseline='middle' 定位，本函数保证迁移后视觉位置不变。
 */
export function baselineForMiddle(font: string, middleY: number): number {
  const { ascent, descent } = fontBox(font)
  return middleY + (ascent - descent) / 2
}
