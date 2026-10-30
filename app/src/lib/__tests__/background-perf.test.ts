/**
 * 背景 / 颗粒离屏缓存性能验证（LUME 性能命门 #4，指南 §3.3）。
 *
 * 证明两件事：
 *  1. 主画布每帧不再重绘背景的几百条线/圆/渐变 —— 而是一次 drawImage blit；
 *  2. 背景/颗粒的光栅化被缓存摊销（renders 远小于 帧数 × 图层数）。
 *
 * 用计数桩 ctx 替代真实 Canvas2D，无需浏览器/DOM。
 */
import { describe, it, expect, beforeEach } from 'vitest'
import { renderFrame, bgCacheStats, resetBgCache } from '../renderFrame'
import type { Project } from '../api'

/** 计数桩：实现 Canvas2D 用到的全部方法，并记录关键操作次数。 */
function makeCountingCtx(c: Record<string, number>) {
  const grad = { addColorStop: () => {} }
  const ctx: Record<string, unknown> = {
    // 计数器关心的「重」操作
    drawImage: () => { c.drawImage++ },
    arc: () => { c.arc++ },
    stroke: () => { c.stroke++ },
    fill: () => { c.fill++ },
    fillRect: () => { c.fillRect++ },
    clearRect: () => { c.clearRect++ },
    fillText: () => { c.fillText++ },
    strokeText: () => { c.strokeText++ },
    createLinearGradient: () => { c.createLinearGradient++; return grad },
    createRadialGradient: () => { c.createRadialGradient++; return grad },
    // 其余方法：no-op
    save: () => {}, restore: () => {}, beginPath: () => {}, closePath: () => {},
    moveTo: () => {}, lineTo: () => {}, rect: () => {}, clip: () => {},
    translate: () => {}, rotate: () => {}, scale: () => {}, transform: () => {},
    setLineDash: () => {}, setTransform: () => {},
    measureText: () => ({
      width: 0, actualBoundingBoxAscent: 0, actualBoundingBoxDescent: 0,
      actualBoundingBoxLeft: 0, actualBoundingBoxRight: 0,
    }),
  }
  // 允许任意属性读写（fillStyle / font / globalAlpha ...）
  return new Proxy(ctx, {
    get(t, k) { return k in t ? t[k as string] : 0 },
    set(t, k, v) { t[k as string] = v; return true },
  }) as unknown as CanvasRenderingContext2D
}

/** 极简工程：一个镜头，halftone 背景（原实现 286 圆/帧）+ grain 覆盖（420 圆/帧）。 */
function heavyProject(): Project {
  return {
    format: 'shizukuflow', version: 1,
    meta: { title: 'bench', artist: '', created: '', modified: '', generator: '' },
    canvas: { width: 1280, height: 720, fps: 30, background: '#000000', safe_area: { enabled: false, margin: 0.1 } },
    media: { audio: null, reference: [] },
    timing: { bpm: 120, offset: 0, beats: [], downbeats: [], source: '' },
    styles: [{ id: 's1', font: 'sans-serif', weight: 400, size: 80, letter_spacing: 0, line_height: 1.2, fill: '#ffffff', stroke: null, shadow: null }],
    lyrics: [],
    shots: [{
      id: 'sh1', start: 0, end: 999, track: 0, lyric_refs: [],
      layers: {
        background: [{ part: 'background', params: { kind: 'halftone' } }],
        text: [], decor: [],
        overlay: [{ part: 'overlay', params: { kind: 'grain', amount: 420, alpha: 0.05 } }],
      },
      motion: { entrance: null, hold: null, exit: null, camera: null },
      transition: null, seed: 1, locked: false,
    }],
    assets: [],
    export: { preset: 'mp4', video: { codec: 'h264', crf: 18, preset: 'medium', encoder: 'auto' }, gif: { fps: 30, width: 720, dither: 'sierra' }, apng: { fps: 30, colors: 256 } },
  }
}

describe('背景离屏缓存 · 性能', () => {
  beforeEach(() => { resetBgCache() })

  it('热帧主画布不再重绘背景重图元（arc/gradient/stroke 增量=0），只 blit', () => {
    const main = { drawImage: 0, arc: 0, stroke: 0, fill: 0, createLinearGradient: 0, createRadialGradient: 0 } as Record<string, number>
    const mainCtx = makeCountingCtx(main)
    const offCtx = makeCountingCtx({} as Record<string, number>)
    // 所有离屏画布（背景缓存 / 图集 / 度量）共用 offCtx
    ;(globalThis as unknown as { document: unknown }).document = {
      createElement: (_tag: string) => ({ width: 0, height: 0, getContext: () => offCtx }),
    }

    const p = heavyProject()
    // 预热：t=0.5 渲染一次（触发缓存 miss + 一次光栅化）
    renderFrame(mainCtx, 1280, 720, p, 0.5, '#3df')
    // 再渲染同一时刻一帧（缓存命中，应只 blit）
    const before = { ...main }
    renderFrame(mainCtx, 1280, 720, p, 0.5, '#3df')
    const heavyDelta = (main.arc - before.arc) + (main.stroke - before.stroke) +
      (main.createLinearGradient - before.createLinearGradient) + (main.createRadialGradient - before.createRadialGradient)
    expect(heavyDelta).toBe(0) // 背景已 blit，主画布零重图元
    expect(main.drawImage - before.drawImage).toBe(2) // 背景 + 颗粒 各一次 blit
  })

  it('光栅化被缓存摊销：1 秒内渲染 30 帧，实际重渲次数远小于 30×图层数', () => {
    const mainCtx = makeCountingCtx({} as Record<string, number>)
    const offCtx = makeCountingCtx({} as Record<string, number>)
    ;(globalThis as unknown as { document: unknown }).document = {
      createElement: (_tag: string) => ({ width: 0, height: 0, getContext: () => offCtx }),
    }
    const p = heavyProject()
    for (let i = 0; i < 30; i++) renderFrame(mainCtx, 1280, 720, p, i / 30, '#3df')
    // 旧实现：每帧 2 个图层各全量光栅（halftone 286 圆 + grain 420 圆）= 30×2 = 60 次全量重绘。
    // 新实现：背景命中缓存 + grain 每 0.1s 刷新，实际 renders 应 << 60。
    const s = bgCacheStats()
    expect(s.renders).toBeLessThan(60)
    // 命中率应很高（绝大多数帧命中缓存，只 blit）
    expect(s.hitRate).toBeGreaterThan(0.8)
  })
})
