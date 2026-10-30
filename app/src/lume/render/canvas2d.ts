/**
 * LUME Canvas2D 后端：把一帧 Draw 指令流画到目标画布。
 *
 * 职责边界：只执行指令，不做任何业务判断、不做布局、不看时间。
 * 文字路径走字形图集（光栅化一次 + drawImage 采样 + 逐字变换/裁剪），
 * 命中图集后每帧成本 = N 次 drawImage，与文字复杂度无关。
 *
 * 预览与导出共用本函数（指南 §4.5：禁止两套逻辑）。
 */

import type { TextStyle, TextDraw, Draw } from '../core/types'
import { GlyphAtlas } from './atlas'
import { measureContext } from '../text/layout'

// ─────────────────────────── 图集单例 ───────────────────────────

let atlas: GlyphAtlas | null = null

function theAtlas(): GlyphAtlas {
  if (!atlas) atlas = new GlyphAtlas(2048)
  return atlas
}

export function atlasStats(): { hits: number; misses: number; evictions: number; entries: number; hitRate: number } {
  const a = theAtlas()
  return { hits: a.hits, misses: a.misses, evictions: a.evictions, entries: a.count, hitRate: a.hitRate }
}

export function resetAtlas(): void {
  theAtlas().clear()
}

// ─────────────────────────── tight 度量（图集 cell 尺寸） ───────────────────────────

interface TightMetrics {
  inkWidth: number
  ascent: number
  descent: number
  /** 墨迹左缘相对绘制原点的偏移（actualBoundingBoxLeft） */
  abbLeft: number
  pad: number
}

function padOf(style: TextStyle): number {
  const stroke = style.stroke?.width ?? 0
  const shadow = style.shadow
  const shadowPad = shadow ? (shadow.blur ?? 0) + Math.max(Math.abs(shadow.offsetX ?? 0), Math.abs(shadow.offsetY ?? 0)) : 0
  return Math.ceil(stroke / 2 + shadowPad) + 2
}

function tightMeasure(font: string, text: string, style: TextStyle): TightMetrics {
  const ctx = measureContext()
  ctx.font = font
  const m = ctx.measureText(text)
  const ascent = m.actualBoundingBoxAscent || 0
  const descent = m.actualBoundingBoxDescent || 0
  const abbLeft = m.actualBoundingBoxLeft ?? 0
  return {
    inkWidth: abbLeft + m.actualBoundingBoxRight,
    ascent,
    descent,
    abbLeft,
    pad: padOf(style),
  }
}

// ─────────────────────────── 样式签名（图集 key 的一部分） ───────────────────────────

function styleKey(s: TextStyle): string {
  let k = `a${s.alpha ?? 1}`
  k += s.fill ? `|f${s.fill}` : '|f-'
  if (s.stroke) k += `|s${s.stroke.color}:${s.stroke.width}`
  if (s.shadow) k += `|h${s.shadow.color}:${s.shadow.blur}:${s.shadow.offsetX ?? 0}:${s.shadow.offsetY ?? 0}`
  return k
}

// ─────────────────────────── 烘焙 ───────────────────────────

function paintRun(
  g: CanvasRenderingContext2D,
  ox: number,
  oy: number,
  font: string,
  text: string,
  style: TextStyle,
  m: TightMetrics,
  bl: number,
): void {
  g.save()
  g.font = font
  // 注意：段间字距由 layoutRun 统一处理，这里刻意不设 ctx.letterSpacing，
  // 保证烘焙宽度与 tight 度量一致（不溢出 cell）。
  g.textBaseline = 'alphabetic'
  g.textAlign = 'left'
  const x = ox + m.pad + m.abbLeft
  const y = oy + bl
  if (style.stroke && style.stroke.color) {
    g.lineWidth = style.stroke.width
    g.strokeStyle = style.stroke.color
    g.lineJoin = 'round'
    g.strokeText(text, x, y)
  }
  if (style.fill) {
    if (style.shadow && style.shadow.color) {
      g.shadowColor = style.shadow.color
      g.shadowBlur = style.shadow.blur ?? 0
      g.shadowOffsetX = style.shadow.offsetX ?? 0
      g.shadowOffsetY = style.shadow.offsetY ?? 0
    }
    g.fillStyle = style.fill
    g.fillText(text, x, y)
  }
  g.restore()
}

// ─────────────────────────── 文字绘制 ───────────────────────────

function drawText(ctx: CanvasRenderingContext2D, d: TextDraw): void {
  const A = theAtlas()
  const boxes = d.layout.boxes
  for (let i = 0; i < boxes.length; i++) {
    const box = boxes[i]
    const st = d.charStates?.[i]
    const reveal = st?.reveal ?? 1
    if (reveal <= 0) continue

    const style: TextStyle = st?.color ? { ...d.style, fill: st.color } : d.style
    const key = `${d.layout.font}|${d.layout.letterSpacing}|${box.text}|${styleKey(style)}`
    const m = tightMeasure(d.layout.font, box.text, style)
    const w = m.inkWidth + m.pad * 2
    const h = m.ascent + m.descent + m.pad * 2
    const bl = m.ascent + m.pad
    const cell = A.lookup(key) ?? A.alloc(key, w, h, bl, (g, ox, oy) => paintRun(g, ox, oy, d.layout.font, box.text, style, m, bl))

    ctx.save()
    // 以字形盒中心为原点做变换 —— 旋转/缩放围绕字形视觉中心
    const cx = d.x + box.x + box.width / 2 + (st?.dx ?? 0)
    const cy = d.y - cell.bl + cell.h / 2 + (st?.dy ?? 0)
    ctx.translate(cx, cy)
    if (st?.rotation) ctx.rotate(st.rotation)
    const sc = st?.scale ?? 1
    if (sc !== 1) ctx.scale(sc, sc)
    // 卡拉OK揭示：从字形盒左缘起裁剪
    if (reveal < 1) {
      ctx.beginPath()
      ctx.rect(-box.width / 2 - 1, -cell.h / 2 - 1, box.width * reveal + 1, cell.h + 2)
      ctx.clip()
    }
    const alpha = (d.style.alpha ?? 1) * (st?.alpha ?? 1)
    if (alpha !== 1) ctx.globalAlpha *= alpha
    ctx.drawImage(A.canvas, cell.x, cell.y, cell.w, cell.h, -cell.w / 2, -cell.h / 2, cell.w, cell.h)
    ctx.restore()
  }
}

// ─────────────────────────── 主入口 ───────────────────────────

/** 执行一帧指令流。纯函数：同 draws 同 ctx 状态必出同画面。 */
export function renderDraws(ctx: CanvasRenderingContext2D, draws: readonly Draw[]): void {
  for (const d of draws) {
    switch (d.kind) {
      case 'rect': {
        ctx.save()
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        ctx.fillStyle = d.fill
        ctx.fillRect(d.x, d.y, d.w, d.h)
        ctx.restore()
        break
      }
      case 'gradient': {
        const cx = d.x + d.w / 2
        const cy = d.y + d.h / 2
        const a = d.angleDeg * (Math.PI / 180)
        const dx = Math.cos(a) * d.w
        const dy = Math.sin(a) * d.h
        const g = ctx.createLinearGradient(cx - dx / 2, cy - dy / 2, cx + dx / 2, cy + dy / 2)
        g.addColorStop(0, d.color)
        g.addColorStop(1, d.color2)
        ctx.fillStyle = g
        ctx.fillRect(d.x, d.y, d.w, d.h)
        break
      }
      case 'vignette': {
        const cx = d.x + d.w / 2
        const cy = d.y + d.h / 2
        const r = Math.hypot(d.w, d.h) / 2
        const g = ctx.createRadialGradient(cx, cy, r * 0.35, cx, cy, r)
        g.addColorStop(0, d.inner)
        g.addColorStop(1, d.edge)
        ctx.fillStyle = g
        ctx.fillRect(d.x, d.y, d.w, d.h)
        break
      }
      case 'circle': {
        ctx.save()
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        ctx.fillStyle = d.fill
        ctx.beginPath()
        ctx.arc(d.cx, d.cy, Math.max(0, d.r), 0, Math.PI * 2)
        ctx.fill()
        ctx.restore()
        break
      }
      case 'ring': {
        ctx.save()
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        ctx.strokeStyle = d.color
        ctx.lineWidth = Math.max(0.1, d.width)
        ctx.beginPath()
        ctx.arc(d.cx, d.cy, Math.max(0, d.r), 0, Math.PI * 2)
        ctx.stroke()
        ctx.restore()
        break
      }
      case 'radial': {
        ctx.save()
        const x = d.x ?? 0
        const y = d.y ?? 0
        const w = d.w ?? ctx.canvas.width
        const h = d.h ?? ctx.canvas.height
        const r0 = Math.max(0, d.rInner)
        const r1 = Math.max(r0 + 0.01, d.rOuter)
        const g = ctx.createRadialGradient(d.cx, d.cy, r0, d.cx, d.cy, r1)
        g.addColorStop(0, d.inner)
        g.addColorStop(1, d.outer)
        ctx.fillStyle = g
        ctx.fillRect(x, y, w, h)
        ctx.restore()
        break
      }
      case 'line': {
        ctx.save()
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        ctx.strokeStyle = d.color
        ctx.lineWidth = Math.max(0.1, d.width)
        if (d.dash) ctx.setLineDash(d.dash)
        ctx.beginPath()
        ctx.moveTo(d.x1, d.y1)
        ctx.lineTo(d.x2, d.y2)
        ctx.stroke()
        ctx.restore()
        break
      }
      case 'transform': {
        ctx.save()
        ctx.transform(d.a, d.b, d.c, d.d, d.e, d.f)
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        // 递归执行 —— 嵌套组合合法（内核执行器可重入）
        for (const sub of d.draws) execDraw(ctx, sub)
        ctx.restore()
        break
      }
      case 'bitmap': {
        // 兜底：0×0 源（首帧布局未就绪时缓存 canvas 可能是 0 尺寸）或 0 目标尺寸
        // 会令 drawImage 抛 InvalidStateError。跳过即可，下一帧尺寸就绪自然恢复。
        const src = d.src as { width?: number; height?: number } | undefined
        if (!d.src || !d.w || !d.h) break
        if (src && 'width' in src && ((src.width ?? 0) === 0 || (src.height ?? 0) === 0)) break
        if (d.sx !== undefined && d.sw !== undefined && (d.sw === 0 || (d.sh ?? 0) === 0)) break
        ctx.save()
        if (d.alpha !== undefined && d.alpha !== 1) ctx.globalAlpha *= d.alpha
        if (d.sx !== undefined && d.sw !== undefined) {
          ctx.drawImage(d.src, d.sx, d.sy ?? 0, d.sw, d.sh ?? 0, d.x, d.y, d.w, d.h)
        } else {
          ctx.drawImage(d.src, d.x, d.y, d.w, d.h)
        }
        ctx.restore()
        break
      }
      case 'text':
        drawText(ctx, d)
        break
    }
  }
}

/** 单条指令执行（renderDraws 与 transform 递归共用）。 */
function execDraw(ctx: CanvasRenderingContext2D, d: Draw): void {
  renderDraws(ctx, [d])
}
