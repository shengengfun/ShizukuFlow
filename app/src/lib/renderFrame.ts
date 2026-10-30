/**
 * 适配层：ShizukuFlow 工程 → LUME 指令流。
 *
 * 渲染内核已整体迁入 `src/lume/`（Letter Universal Motion Engine · 琉明）。
 * 本文件只做一件事：把「工程 + 时间 t」编译成 LUME 的 Draw 指令流，
 * 业务语义（歌词、卡拉OK、镜头、letterbox）全部留在这一层 ——
 * 内核永远不懂「歌词」，只懂指令（GuideOfUI.md §6.2 红线）。
 *
 * 保持不变的设计原则：
 * - `renderFrame(project, t)` 纯函数：同工程同 t 出同画面（可复现）；
 * - 多轨合成：同一时刻所有 active 镜头按 `(track, start)` 升序叠放；
 * - 预览与导出共用本入口。
 */

import type { Project, Lyric, Style, Shot, PartRef } from './api'
import {
  layoutRun,
  baselineForMiddle,
  renderDraws,
  lumeStats,
  lumeReset,
  applyOperators,
  ease,
  hash01,
  LruCache,
  hitRate,
  type Draw,
  type TextStyle,
  type CharState,
  type OperatorSpec,
  LUME_VERSION,
} from '../lume'

const clamp01 = (v: number) => Math.max(0, Math.min(1, v))

/** 把画布按比例适配进容器（letterbox 保持 16:9 不变形）。 */
function fit(
  cw: number,
  ch: number,
  aspect: number,
): { x: number; y: number; w: number; h: number } {
  const ca = cw / ch
  if (ca > aspect) {
    const h = ch
    const w = h * aspect
    return { x: (cw - w) / 2, y: 0, w, h }
  } else {
    const w = cw
    const h = w / aspect
    return { x: 0, y: (ch - h) / 2, w, h }
  }
}

function styleOf(p: Project, id: string): Style {
  return p.styles.find((s) => s.id === id) ?? p.styles[0]
}

/** 取某时刻所有 active 镜头，按 (track, start) 升序 —— 决定合成叠放次序。 */
export function activeShots(p: Project, t: number): Shot[] {
  return p.shots
    .filter((s) => t >= s.start && t < s.end)
    .sort((a, b) => a.track - b.track || a.start - b.start)
}

/** 找一行歌词在当前时刻是否 active。 */
function lyricActiveAt(l: Lyric, t: number): boolean {
  return t >= l.start && t < l.end
}

/**
 * 画一帧。等价于旧版视觉输出，但渲染全程走 LUME（布局缓存 + 字形图集）。
 * @param t 时间（秒）
 * @param accent 强调色（水蓝），用于「正在唱」那一字的下划提示
 */
export function renderFrame(
  ctx: CanvasRenderingContext2D,
  cssW: number,
  cssH: number,
  p: Project,
  t: number,
  accent: string,
): void {
  ctx.clearRect(0, 0, cssW, cssH)
  // 容器底色
  ctx.fillStyle = '#08080b'
  ctx.fillRect(0, 0, cssW, cssH)

  const aspect = p.canvas.width / p.canvas.height
  const box = fit(cssW, cssH, aspect)
  // 裁剪到工程画幅，避免文字画到 letterbox 黑边上
  ctx.save()
  ctx.beginPath()
  ctx.rect(box.x, box.y, box.w, box.h)
  ctx.clip()

  // 工程级背景打底（镜头没提供背景部件时仍是纯色）
  ctx.fillStyle = p.canvas.background || '#000000'
  ctx.fillRect(box.x, box.y, box.w, box.h)

  const draws: Draw[] = []
  const shots = activeShots(p, t)
  if (shots.length === 0) {
    compileIdle(draws, box)
  } else {
    for (const s of shots) {
      compileShot(draws, box, p, s, t, accent)
    }
  }
  renderDraws(ctx, draws)

  ctx.restore()
}

/** 没有任何镜头 active 时的极淡提示（间奏 / 空镜）。 */
function compileIdle(draws: Draw[], box: { x: number; y: number; w: number; h: number }): void {
  const size = Math.round(box.h * 0.02)
  const font = `${size}px ui-monospace, monospace`
  const layout = layoutRun(['— 间奏 / 空镜 —'], font, 0)
  draws.push({
    kind: 'text',
    layout,
    x: box.x + box.w / 2 - layout.totalWidth / 2,
    y: baselineForMiddle(font, box.y + box.h / 2),
    style: { fill: '#ffffff', alpha: 0.10 },
  })
}

/** 把单个镜头编译为指令：bg → text(歌词/静态) → decor → overlay，再整体套运镜/转场。 */
function compileShot(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  p: Project,
  shot: Shot,
  t: number,
  accent: string,
): void {
  const inner: Draw[] = []
  const bpm = p.timing?.bpm || 120
  for (const part of shot.layers.background) compileBackgroundPart(inner, box, part, t, bpm)

  // 安全区折减：文字是先按画幅排好、再被运镜整体缩放的。
  // 若镜头有 zoom>1 / punch，开头几帧文字必然出画 —— fitting 必须预留运镜余量。
  // （片尾署名 pan 是刻意出画的构图手段，不在此列。）
  const camPr = ((shot.motion?.camera?.params ?? {}) as Record<string, unknown>)
  const zoomArr = camPr.zoom as [number, number] | undefined
  const punch = (camPr.punch as number) || 0
  const zMax = Math.max(1, ...(zoomArr ?? [1]).map(Math.abs)) * (1 + punch)
  const safe = 1 / zMax

  if (shot.lyric_refs.length > 0) {
    for (const ref of shot.lyric_refs) {
      const lyric = p.lyrics.find((l) => l.id === ref)
      if (lyric && lyricActiveAt(lyric, t)) compileLyric(inner, box, p, lyric, t, accent, shot, safe)
    }
  }

  for (const part of shot.layers.text) compileTextPart(inner, box, part, t, safe)
  for (const part of shot.layers.decor) compileDecorPart(inner, box, part, t, p)
  for (const part of shot.layers.overlay) compileOverlayPart(inner, box, part, t, bpm)

  // 转场透明度（fade in/out）→ 包一层恒等变换组
  const alpha = transitionAlpha(shot, t)
  const cam = shot.motion?.camera
  const camM = cam ? cameraMatrix(cam, shot, t, box, p) : null
  if (camM) {
    draws.push({ kind: 'transform', ...camM, alpha: alpha < 1 ? alpha : undefined, draws: inner })
  } else if (alpha < 1) {
    draws.push({ kind: 'transform', a: 1, b: 0, c: 0, d: 1, e: 0, f: 0, alpha, draws: inner })
  } else {
    draws.push(...inner)
  }
  // 转场硬特效（闪白/遮扫）不受运镜影响，画在最上
  compileTransitionFx(draws, box, shot, t)
}

// ─────────────────────────── 运镜（Motion.camera） ───────────────────────────

/**
 * 编译镜头运镜为仿射矩阵。params：
 *   zoom: [from, to]  整镜缓推/缓拉
 *   pan:  [x, y]      每秒平移（画幅比例，正 y 向下）
 *   rotate: [from, to] 度
 *   punch: number     下拍冲镜强度（scale 脉冲，指数衰减）
 * 返回 null = 恒等（不包变换组）。
 */
function cameraMatrix(
  cam: PartRef,
  shot: Shot,
  t: number,
  box: { x: number; y: number; w: number; h: number },
  p: Project,
): { a: number; b: number; c: number; d: number; e: number; f: number } | null {
  const pr = cam.params as Record<string, unknown> | undefined
  if (!pr) return null
  const dur = Math.max(0.001, shot.end - shot.start)
  const k = clamp01((t - shot.start) / dur)
  const zoom = pr.zoom as [number, number] | undefined
  const pan = pr.pan as [number, number] | undefined
  const rotate = pr.rotate as [number, number] | undefined
  const punch = (pr.punch as number) || 0

  const z = zoom ? zoom[0] + (zoom[1] - zoom[0]) * k : 1
  const beatScale = punch > 0 ? 1 + punch * Math.exp(-8 * tSinceDownbeat(p, t)) : 1
  const scale = z * beatScale
  const px = pan ? pan[0] * (t - shot.start) : 0
  const py = pan ? pan[1] * (t - shot.start) : 0
  const deg = rotate ? rotate[0] + (rotate[1] - rotate[0]) * k : 0
  const rot = (deg * Math.PI) / 180

  if (scale === 1 && px === 0 && py === 0 && rot === 0) return null

  const cx = box.x + box.w / 2
  const cy = box.y + box.h / 2
  const cos = Math.cos(rot) * scale
  const sin = Math.sin(rot) * scale
  const a = cos
  const b = sin
  const c = -sin
  const d = cos
  const e = cx - (a * cx + c * cy) + px * box.w
  const f = cy - (b * cx + d * cy) + py * box.h
  return { a, b, c, d, e, f }
}

/** 距上一个下拍（小节首拍）的时间（秒）。 */
function tSinceDownbeat(p: Project, t: number): number {
  const bpm = p.timing?.bpm || 120
  const off = p.timing?.offset || 0
  const bar = (4 * 60) / bpm
  return (t - off) % bar
}

// ─────────────────────────── 转场（Shot.transition） ───────────────────────────

/** fade in/out → 整组 alpha。flash/wipe 走 compileTransitionFx。 */
function transitionAlpha(shot: Shot, t: number): number {
  const tr = shot.transition
  if (!tr) return 1
  const pr = tr.params as Record<string, unknown> | undefined
  if (!pr) return 1
  let alpha = 1
  const inDur = (pr.inDur as number) ?? 0
  const outDur = (pr.outDur as number) ?? 0
  if (pr.in === 'fade' && inDur > 0) alpha = Math.min(alpha, clamp01((t - shot.start) / inDur))
  if (pr.out === 'fade' && outDur > 0) alpha = Math.min(alpha, clamp01((shot.end - t) / outDur))
  return alpha
}

/** flash（闪白）/ wipe（遮扫黑幕）—— 画在运镜之上，术曲PV硬切语言。 */
function compileTransitionFx(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  shot: Shot,
  t: number,
): void {
  const tr = shot.transition
  if (!tr) return
  const pr = (tr.params as Record<string, unknown> | undefined) ?? {}
  const dur = Math.max(0.001, shot.end - shot.start)

  const inDur = (pr.inDur as number) ?? 0.3
  const outDur = (pr.outDur as number) ?? 0.3
  const kindIn = (pr.in as string) || 'cut'
  const kindOut = (pr.out as string) || 'cut'

  if (kindIn === 'flash' && t - shot.start < inDur) {
    draws.push({
      kind: 'rect', x: box.x, y: box.y, w: box.w, h: box.h,
      fill: (pr.color as string) || '#ffffff',
      alpha: 1 - (t - shot.start) / inDur,
    })
  }
  if (kindIn === 'wipe' && t - shot.start < inDur) {
    const k = clamp01((t - shot.start) / inDur)
    draws.push(...wipeRect(box, 1 - k, (pr.dir as string) || 'left', (pr.color as string) || '#000000'))
  }
  if (kindOut === 'wipe' && shot.end - t < outDur) {
    const k = clamp01((shot.end - t) / outDur)
    draws.push(...wipeRect(box, 1 - k, (pr.outDir as string) || 'right', (pr.color as string) || '#000000'))
  }
  void dur
}

/** 遮扫：k=1 全遮，k=0 无遮。dir = 覆盖展开方向。 */
function wipeRect(
  box: { x: number; y: number; w: number; h: number },
  k: number,
  dir: string,
  color: string,
): Draw[] {
  if (k <= 0) return []
  const r = { x: box.x, y: box.y, w: box.w, h: box.h }
  if (dir === 'right') r.w = box.w * k
  else if (dir === 'left') { r.w = box.w * k; r.x = box.x + box.w * (1 - k) }
  else if (dir === 'up') { r.h = box.h * k; r.y = box.y + box.h * (1 - k) }
  else r.h = box.h * k
  return [{ kind: 'rect', ...r, fill: color }]
}

// ─────────────────────────── 动效装饰部件（decor） ───────────────────────────

function compileDecorPart(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
  p: Project,
): void {
  switch (part.part) {
    case 'beatRings':
      compileBeatRings(draws, box, part, t, p)
      return
    case 'bars':
      compileBars(draws, box, part, t, p)
      return
    case 'particles':
      compileParticles(draws, box, part, t)
      return
    case 'flash':
      compileDecorFlash(draws, box, part, t)
      return
    case 'frameLines':
      compileFrameLines(draws, box, part)
      return
    case 'glowDot': {
      const pr2 = (part.params as Record<string, unknown> | undefined) ?? {}
      const scale = box.h / 1080
      glowDot(
        draws,
        box.x + box.w * ((pr2.x as number) ?? 0.5),
        box.y + box.h * ((pr2.y as number) ?? 0.5),
        ((pr2.r as number) || 6) * scale,
        (pr2.color as string) || '#ffffff',
        clamp01(((pr2.k as number) ?? 0.8)),
      )
      return
    }
    default:
      compilePart(draws, box, part, t)
  }
}

/** 下拍扩散环：每个（或每第 every 拍）从中心扩散一圈，随时间淡出。 */
function compileBeatRings(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
  p: Project,
): void {
  const pr = part.params as Record<string, unknown> | undefined
  const bpm = p.timing?.bpm || 120
  const off = p.timing?.offset || 0
  const beat = 60 / bpm
  const every = ((pr?.every as number) || 4) * beat
  const life = (pr?.life as number) || every
  const maxR = ((pr?.maxR as number) || 0.75) * Math.min(box.w, box.h) * 0.5
  const color = (pr?.color as string) || '#ffffff'
  const width = (pr?.width as number) || 2.5
  const cx = box.x + box.w * ((pr?.x as number) ?? 0.5)
  const cy = box.y + box.h * ((pr?.y as number) ?? 0.5)

  const idx = Math.floor((t - off) / every)
  for (let back = 0; back < 2; back++) {
    const t0 = off + (idx - back) * every
    const k = (t - t0) / life
    if (k <= 0 || k >= 1) continue
    draws.push({
      kind: 'ring',
      cx, cy,
      r: maxR * (0.08 + 0.92 * k),
      width: Math.max(0.5, width * (1 - k)),
      color,
      alpha: (1 - k) * 0.7,
    })
  }
}

/** 节拍频谱条：确定性正弦 + 下拍脉冲（无音频依赖，可复现）。 */
function compileBars(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
  p: Project,
): void {
  const pr = part.params as Record<string, unknown> | undefined
  const n = (pr?.n as number) || 48
  const color = (pr?.color as string) || 'rgba(255,255,255,0.18)'
  const maxH = ((pr?.maxH as number) || 0.16) * box.h
  const gap = ((pr?.gap as number) || 0.15) * (box.w / n)
  const punch = ((pr?.punch as number) || 0.5)
  const seed = (pr?.seed as number) || 0
  const bw = box.w / n - gap
  const pulse = Math.exp(-8 * tSinceDownbeat(p, t)) * punch
  for (let i = 0; i < n; i++) {
    const ph = hash01(i * 7.13 + seed) * Math.PI * 2
    const sp = 1.5 + hash01(i * 3.7 + seed + 11) * 2.5
    const level = 0.18 + 0.62 * Math.abs(Math.sin(t * sp + ph)) + pulse * hash01(i * 1.9 + seed + 31)
    const h = Math.min(maxH, maxH * Math.min(1.25, level))
    draws.push({
      kind: 'rect',
      x: box.x + gap / 2 + i * (bw + gap),
      y: box.y + box.h - h,
      w: bw,
      h,
      fill: color,
    })
  }
}

/** 确定性漂浮粒子（hash01 种子 → 无随机、可复现）。 */
function compileParticles(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
): void {
  const pr = part.params as Record<string, unknown> | undefined
  const n = (pr?.n as number) || 40
  const color = (pr?.color as string) || '#ffffff'
  const size = ((pr?.size as number) || 2.5)
  const speed = ((pr?.speed as number) || 0.02)
  const seed = (pr?.seed as number) || 0
  for (let i = 0; i < n; i++) {
    const vx = (hash01(i * 5.31 + seed) - 0.5) * speed
    const vy = -(0.008 + hash01(i * 9.77 + seed + 3) * 0.03) * speed * 50
    const x = box.x + (((hash01(i * 13.7 + seed) + t * vx) % 1) + 1) % 1 * box.w
    const y = box.y + (((hash01(i * 17.3 + seed + 7) + t * vy) % 1) + 1) % 1 * box.h
    draws.push({
      kind: 'circle',
      cx: x, cy: y,
      r: size * (0.5 + hash01(i * 2.9 + seed + 5)) * (box.h / 1080) * 3,
      fill: color,
      alpha: 0.15 + hash01(i * 23.1 + seed + 9) * 0.35,
    })
  }
}

/** 定点闪白（decor 用，比转场 flash 更自由）。 */
function compileDecorFlash(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
): void {
  const pr = (part.params as Record<string, unknown> | undefined) ?? {}
  const at = (pr.at as number) ?? 0
  const dur = (pr.dur as number) || 0.2
  const k = (t - at) / dur
  if (k < 0 || k >= 1) return
  draws.push({
    kind: 'rect', x: box.x, y: box.y, w: box.w, h: box.h,
    fill: (pr.color as string) || '#ffffff',
    alpha: (1 - k) * ((pr.maxAlpha as number) ?? 1),
  })
}

/** 细线画框 + 角十字准星（术曲PV排版装饰）。 */
function compileFrameLines(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
): void {
  const pr = (part.params as Record<string, unknown> | undefined) ?? {}
  const m = ((pr.margin as number) ?? 0.055)
  const color = (pr.color as string) || 'rgba(255,255,255,0.28)'
  const lw = Math.max(1, (pr.width as number) || 2) * (box.h / 1080)
  const cross = ((pr.cross as number) ?? 14) * (box.h / 1080)
  const x0 = box.x + box.w * m
  const y0 = box.y + box.h * m
  const x1 = box.x + box.w * (1 - m)
  const y1 = box.y + box.h * (1 - m)
  const line = (x: number, y: number, w: number, h: number) =>
    draws.push({ kind: 'rect', x, y, w, h, fill: color })
  line(x0, y0, x1 - x0, lw)
  line(x0, y1 - lw, x1 - x0, lw)
  line(x0, y0, lw, y1 - y0)
  line(x1 - lw, y0, lw, y1 - y0)
  // 四角十字（框外角）
  for (const [cx, cy] of [[x0, y0], [x1, y0], [x0, y1], [x1, y1]] as const) {
    line(cx - cross / 2, cy - lw / 2, cross, lw)
    line(cx - lw / 2, cy - cross / 2, lw, cross)
  }
}

// ─────────────────────────── 部件：背景 ───────────────────────────

/** 给任意 CSS 颜色套 alpha（hex 走 hexA，rgba() 直接改末位）。 */
function withAlpha(color: string, alpha: number): string {
  const c = color.trim()
  if (c.startsWith('#')) return hexA(c, alpha)
  const m = c.match(/^rgba?\(([^)]+)\)$/i)
  if (m) {
    const p = m[1].split(',').map((s) => s.trim())
    return `rgba(${p[0]},${p[1]},${p[2]},${alpha})`
  }
  return c
}

// ─────────────────────────── 背景 / 颗粒离屏缓存（LUME 性能命门 #4，指南 §3.3） ───────────────────────────
//
// 程序化背景、胶片颗粒由几百条线/圆/渐变堆成，且移动极慢
// （grid 每帧位移 ~0.13px、bokeh ~0.0035 速度）。每帧重算几百条指令是纯浪费。
// 改为：按「背景参数指纹」缓存一张离屏 canvas，每帧只 drawImage 一次；
// 时变图层按 BG_REFRESH_SEC 低频刷新（10fps 足够顺滑，肉眼难辨慢动跳变）；
// 静态背景（gradient/solid）永不重渲。背景命中率应接近 100%。
// 缓存层只影响速度不影响输出（同 key 同内容），不破坏可复现红线。

interface BgEntry {
  canvas: HTMLCanvasElement
  /** 上次重渲时刻（用于时变图层低频刷新）。 */
  renderedAt: number
}

/** 时变图层刷新间隔（秒）。10fps 动态对极慢动效足够顺滑。 */
const BG_REFRESH_SEC = 0.1
/** 缓存槽位上限：术曲 PV 全片就几种背景 + 颗粒，12 足够覆盖当前镜头组。 */
const bgCache = new LruCache<string, BgEntry>(12)
/** 离屏 canvas 复用池（避免反复 new/GC 一张 720p 画布）。 */
const bgScratch: HTMLCanvasElement[] = []
let bgRenders = 0

function getBgCanvas(w: number, h: number): HTMLCanvasElement {
  for (const c of bgScratch) {
    if (c.width === w && c.height === h) return c
  }
  const c = document.createElement('canvas')
  c.width = w
  c.height = h
  bgScratch.push(c)
  return c
}

/** 背景 / 颗粒缓存统计（验收指标：命中率 > 95%）。 */
export function bgCacheStats(): {
  hits: number
  misses: number
  evictions: number
  entries: number
  renders: number
  hitRate: number
} {
  return {
    hits: bgCache.stats.hits,
    misses: bgCache.stats.misses,
    evictions: bgCache.stats.evictions,
    entries: bgCache.size,
    renders: bgRenders,
    hitRate: hitRate(bgCache.stats),
  }
}

/** 清空背景 / 颗粒缓存（分辨率切换 / 显存压力时调用）。 */
export function resetBgCache(): void {
  bgCache.clear()
  bgRenders = 0
}

/** 把背景图元编译进 out（本地坐标 0,0 起，w×h 尺寸）。供离屏缓存烘焙复用。 */
function buildBackgroundDraws(
  out: Draw[],
  w: number,
  h: number,
  kind: string,
  base: string,
  base2: string,
  accent: string,
  seed: number,
  angle: number,
  t: number,
  bpm: number,
): void {
  const lb = { x: 0, y: 0, w, h }
  // 节拍脉冲：每拍 1→0 的衰减，给网格/网点/放射线呼吸用
  const beat = (t * bpm) / 60
  const pulse = 0.5 + 0.5 * Math.cos(beat * Math.PI * 2)
  const R = (i: number, s = 0) => hash01(seed * 97 + i * 13.7, s)

  const rect = (fill: string) =>
    out.push({ kind: 'rect', x: lb.x, y: lb.y, w: lb.w, h: lb.h, fill })
  const grad = (c1: string, c2: string, deg = angle) =>
    out.push({ kind: 'gradient', x: lb.x, y: lb.y, w: lb.w, h: lb.h, color: c1, color2: c2, angleDeg: deg })
  const blob = (cx: number, cy: number, r: number, color: string, a: number) =>
    out.push({
      kind: 'radial', cx, cy, rInner: 0, rOuter: Math.max(1, r),
      inner: withAlpha(color, a), outer: withAlpha(color, 0),
      x: lb.x, y: lb.y, w: lb.w, h: lb.h,
    })
  const line = (x1: number, y1: number, x2: number, y2: number, width: number, color: string, alpha: number, dash?: [number, number]) =>
    out.push({ kind: 'line', x1, y1, x2, y2, width, color, alpha, dash })

  switch (kind) {
    case 'gradient':
      grad(base, base2)
      return

    case 'aurora': {
      grad(base, base2, 100)
      const n = 6
      for (let i = 0; i < n; i++) {
        const ph = R(i, 1) * Math.PI * 2
        const cx = lb.x + lb.w * (0.12 + 0.76 * R(i, 2)) + lb.w * 0.05 * Math.sin(t * 0.11 + ph)
        const cy = lb.y + lb.h * (0.10 + 0.78 * R(i, 3)) + lb.h * 0.04 * Math.cos(t * 0.09 + ph)
        const r = lb.h * (0.32 + 0.46 * R(i, 4)) * (1 + 0.04 * pulse)
        blob(cx, cy, r, i % 3 === 0 ? accent : base2, 0.14 + 0.10 * R(i, 5))
      }
      grad('rgba(0,0,0,0)', 'rgba(0,0,0,0.45)', 90)
      return
    }

    case 'bokeh': {
      rect(base)
      const n = 24
      for (let i = 0; i < n; i++) {
        const depth = 0.25 + 0.75 * R(i, 1)
        const x = lb.x + (((R(i, 2) + t * 0.0035 * depth) % 1) + 1) % 1 * lb.w
        const y = lb.y + ((((R(i, 3) - t * 0.0055 * depth) % 1) + 1) % 1) * lb.h
        blob(x, y, lb.h * (0.018 + 0.052 * depth), accent, 0.05 + 0.11 * R(i, 4) * depth)
      }
      return
    }

    case 'grid': {
      grad(base, base2, 90)
      const sp = lb.h / 9
      const off = (t * 16 * (bpm / 120)) % sp
      for (let y = lb.y - sp + off; y <= lb.y + lb.h + sp; y += sp) {
        line(lb.x, y, lb.x + lb.w, y, 1.1, accent, 0.10 + 0.05 * pulse)
      }
      const spx = lb.w / 16
      const offx = (t * 22 * (bpm / 120)) % spx
      for (let x = lb.x - spx + offx; x <= lb.x + lb.w + spx; x += spx) {
        line(x, lb.y, x, lb.y + lb.h, 1, accent, 0.05)
      }
      return
    }

    case 'rays': {
      rect(base)
      const cx = lb.x + lb.w * 0.5
      const cy = lb.y + lb.h * 0.5
      const rot = t * 0.14
      const len = Math.hypot(lb.w, lb.h) * 0.8
      const n = 30
      for (let i = 0; i < n; i++) {
        const a = (i / n) * Math.PI * 2 + rot
        const inner = lb.h * (0.10 + 0.04 * R(i, 1))
        line(
          cx + Math.cos(a) * inner, cy + Math.sin(a) * inner,
          cx + Math.cos(a) * len, cy + Math.sin(a) * len,
          i % 2 ? 2.5 : 8, accent, 0.045 + 0.035 * pulse,
        )
      }
      blob(cx, cy, lb.h * 0.85, accent, 0.10 + 0.05 * pulse)
      return
    }

    case 'stripes': {
      rect(base)
      const sp = lb.h * 0.085
      const shift = (t * 26) % sp
      for (let i = -14; i < 30; i++) {
        const x0 = lb.x + i * sp + shift
        line(x0, lb.y, x0 + lb.h * 0.6, lb.y + lb.h, lb.h * 0.014, accent, 0.055)
      }
      grad('rgba(0,0,0,0)', 'rgba(0,0,0,0.4)', 90)
      return
    }

    case 'halftone': {
      rect(base)
      const cols = 22
      const rows = 13
      for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
          const x = lb.x + ((c + 0.5) / cols) * lb.w
          const y = lb.y + ((r + 0.5) / rows) * lb.h
          const dx = (x - (lb.x + lb.w * 0.5)) / lb.w
          const dy = (y - (lb.y + lb.h * 0.5)) / lb.h
          const d = Math.min(1, Math.hypot(dx, dy) * 1.7)
          const rad = lb.h * 0.013 * (1 - d) * (0.75 + 0.55 * pulse)
          if (rad < 0.5) continue
          out.push({ kind: 'circle', cx: x, cy: y, r: rad, fill: withAlpha(accent, 0.26 * (1 - d)) })
        }
      }
      return
    }

    case 'scanlines': {
      rect(base)
      const sp = Math.max(4, lb.h / 150)
      for (let y = lb.y; y < lb.y + lb.h; y += sp) {
        line(lb.x, y, lb.x + lb.w, y, Math.max(1, sp * 0.35), '#000000', 0.16)
      }
      const sweepY = lb.y + (((t * 0.12) % 1) * lb.h)
      grad('rgba(255,255,255,0)', withAlpha(accent, 0.06), 0)
      out.push({ kind: 'rect', x: lb.x, y: sweepY, w: lb.w, h: lb.h * 0.16, fill: withAlpha(accent, 0.04) })
      return
    }

    case 'noise': {
      rect(base)
      const f = Math.floor(t * 30)
      const n = 260
      for (let i = 0; i < n; i++) {
        const x = lb.x + hash01(i * 1.7 + f * 0.37, 3) * lb.w
        const y = lb.y + hash01(i * 2.3 + f * 0.51, 7) * lb.h
        out.push({ kind: 'circle', cx: x, cy: y, r: lb.h * 0.0016, fill: withAlpha(accent, 0.07) })
      }
      return
    }

    case 'paper': {
      grad(base2 || '#f4eee4', base || '#e9e2d6', 110)
      const f = Math.floor(t * 24)
      for (let i = 0; i < 120; i++) {
        const x = lb.x + hash01(i * 2.1 + f * 0.21, 11) * lb.w
        const y = lb.y + hash01(i * 3.3 + f * 0.17, 13) * lb.h
        out.push({ kind: 'circle', cx: x, cy: y, r: lb.h * 0.0014, fill: 'rgba(0,0,0,0.05)' })
      }
      out.push({
        kind: 'vignette', x: lb.x, y: lb.y, w: lb.w, h: lb.h,
        edge: 'rgba(120,100,80,0.16)', inner: 'rgba(120,100,80,0)',
      })
      return
    }

    default:
      rect(base)
      return
  }
}

/**
 * 背景部件。kind：
 *   solid / gradient            纯色、线性渐变（基础）
 *   aurora                      极光：多枚大光斑缓慢游移（暗场氛围）
 *   bokeh                       散景：按深度视差的软光点（浅景深感）
 *   grid                        网格：随节拍滚动（科技/电子感）
 *   rays                        放射线：中心发散缓慢旋转 + 中心辉光
 *   stripes                     斜条纹：对角粗线（版式化平面感）
 *   halftone                    网点：径向尺寸衰减的印刷网点
 *   scanlines                   扫描线：细横线（CRT/录像带感）
 *   noise                       颗粒：每帧重抽的微点（胶片呼吸）
 *   paper                       纸面：浅色底 + 纤维点 + 边缘轻晕（反色段专用）
 *
 * 所有随机都走 hash01(seed)，同一 t 必然同一画面 —— 可复现是红线。
 */
function compileBackgroundPart(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t = 0,
  bpm = 120,
): void {
  const pr = part.params as Record<string, unknown>
  const kind = ((pr.kind as string) || 'solid').toLowerCase()
  const w = Math.round(box.w)
  const h = Math.round(box.h)
  // 首帧布局未就绪时 box 可能为 0：0×0 离屏画布 blit 会抛 InvalidStateError。
  // 直接跳过，等画布尺寸就绪后下一帧正常渲染。
  if (w <= 0 || h <= 0) return
  const seed = (pr.seed as number) ?? 1
  const base = (pr.color as string) || '#0a0a12'
  const base2 = (pr.color2 as string) || '#1a0a14'
  const accent = (pr.accent as string) || '#ffffff'
  const angle = (pr.angle as number) || 90
  const animated = !(kind === 'gradient' || kind === 'solid' || kind === 'default')
  // 缓存 key 不含 t：同一组背景参数即同一条缓存。时变背景按时间低频刷新。
  const key = `bg|${kind}|${seed}|${base}|${base2}|${accent}|${angle}|${w}x${h}`
  let entry = bgCache.get(key)
  const needRefresh = animated && (!entry || t - entry.renderedAt >= BG_REFRESH_SEC)
  if (!entry || needRefresh) {
    const off = entry?.canvas ?? getBgCanvas(w, h)
    const octx = off.getContext('2d')
    if (!octx) return
    octx.setTransform(1, 0, 0, 1, 0, 0)
    octx.clearRect(0, 0, w, h)
    const local: Draw[] = []
    buildBackgroundDraws(local, w, h, kind, base, base2, accent, seed, angle, t, bpm)
    renderDraws(octx, local)
    entry = { canvas: off, renderedAt: t }
    bgCache.set(key, entry)
    bgRenders++
  }
  draws.push({ kind: 'bitmap', src: entry.canvas, x: box.x, y: box.y, w: box.w, h: box.h })
}

// ─────────────────────────── 部件：覆盖（渐晕等） ───────────────────────────

function compileOverlayPart(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t = 0,
  bpm = 120,
): void {
  const pr = part.params as Record<string, unknown>
  const kind = ((pr.kind as string) || 'vignette').toLowerCase()
  if (kind === 'vignette') {
    draws.push({
      kind: 'vignette',
      x: box.x, y: box.y, w: box.w, h: box.h,
      edge: (pr.color as string) || 'rgba(0,0,0,0.55)',
      inner: (pr.inner as string) || 'rgba(0,0,0,0)',
    })
  } else if (kind === 'grain') {
    // 胶片颗粒：烘焙进离屏 tile 缓存，每帧只 blit（低频刷新，10fps 抖动肉眼无差）。
    const w = Math.round(box.w)
    const h = Math.round(box.h)
    if (w <= 0 || h <= 0) return
    const amount = (pr.amount as number) || 420
    const color = (pr.color as string) || '#ffffff'
    const a = (pr.alpha as number) ?? 0.05
    const key = `grain|${amount}|${color}|${a}|${w}x${h}`
    let entry = bgCache.get(key)
    if (!entry || t - entry.renderedAt >= BG_REFRESH_SEC) {
      const off = entry?.canvas ?? getBgCanvas(w, h)
      const octx = off.getContext('2d')
      if (octx) {
        octx.setTransform(1, 0, 0, 1, 0, 0)
        octx.clearRect(0, 0, w, h)
        const local: Draw[] = []
        const n = Math.min(700, amount)
        const f = Math.floor(t * 30)
        for (let i = 0; i < n; i++) {
          const x = hash01(i * 1.13 + f * 0.31, 21) * w
          const y = hash01(i * 2.71 + f * 0.47, 23) * h
          local.push({ kind: 'circle', cx: x, cy: y, r: h * 0.0015, fill: withAlpha(color, a) })
        }
        renderDraws(octx, local)
        entry = { canvas: off, renderedAt: t }
        bgCache.set(key, entry)
        bgRenders++
      }
    }
    if (entry) draws.push({ kind: 'bitmap', src: entry.canvas, x: box.x, y: box.y, w: box.w, h: box.h })
  } else {
    compileBackgroundPart(draws, box, part, t, bpm)
  }
}

// ─────────────────────────── 部件：静态文字 ───────────────────────────

/**
 * 静态文字部件。params 额外支持：
 *   vertical: true          竖排（逐字下排，术曲PV标志性排版）
 *   echo: {n, dx, decay}    重影拖尾（多重 alpha 递减位移）
 *   spacing: number         字距（px，画幅 1080 基准）
 */
function compileTextPart(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
  safe = 1,
): void {
  const pr = part.params as Record<string, unknown>
  const showStart = pr.showStart as number | undefined
  const showEnd = pr.showEnd as number | undefined
  if (showStart !== undefined && t < showStart) return
  if (showEnd !== undefined && t >= showEnd) return

  const text = (pr.text as string) || ''
  if (!text) return
  const scale = box.h / 1080
  let sizePx = ((pr.size as number) || 64) * scale
  const weight = (pr.weight as number) || 700
  const spacing = ((pr.spacing as number) || 0) * scale
  const fontOf = (s: number) =>
    `${weight} ${s}px "Noto Serif SC", "Microsoft YaHei", serif`

  const ops = pr.ops as OperatorSpec[] | undefined
  const useOps = ops && ops.length > 0
  const seg = useOps ? Array.from(text) : [text]

  // 安全区：整段文字必须落在 92%×运镜折减 的画宽内（竖排另有行列约束）
  const maxW = box.w * 0.92 * safe
  if (pr.vertical) {
    const chars = Array.from(text).length
    const maxH = box.h * 0.9 * safe
    if (chars * sizePx * 1.14 > maxH) sizePx = maxH / (chars * 1.14)
    for (let pass = 0; pass < 2; pass++) {
      const widest = Math.max(...Array.from(text).map((c) => layoutRun([c], fontOf(sizePx), 0).totalWidth))
      if (widest > box.w * 0.18) sizePx *= (box.w * 0.18) / widest
    }
  } else {
    for (let pass = 0; pass < 3; pass++) {
      const w = layoutRun(seg, fontOf(sizePx), spacing).totalWidth
      if (w <= maxW || w <= 0) break
      sizePx *= Math.max(0.2, maxW / w)
    }
  }
  sizePx = Math.max(9 * scale, sizePx)
  const font = fontOf(sizePx)
  const style: TextStyle = { fill: (pr.fill as string) || '#ffffff' }
  const stroke = pr.stroke as { color?: string; width?: number } | undefined
  if (stroke && stroke.color) style.stroke = { color: stroke.color, width: (stroke.width || 3) * scale }
  const shadow = pr.shadow as { blur?: number; color?: string; offset?: [number, number] } | undefined
  if (shadow && shadow.color) {
    style.shadow = {
      color: shadow.color,
      blur: (shadow.blur || 8) * scale,
      offsetX: (shadow.offset?.[0] ?? 0) * scale,
      offsetY: (shadow.offset?.[1] ?? 4) * scale,
    }
  }
  const align = (pr.align as string) || 'center'
  const anchorX = box.x + box.w * ((pr.x as number) ?? 0.5)
  const anchorY = box.y + box.h * ((pr.y as number) ?? 0.85)

  // 重影拖尾：同一文字多重 alpha 递减位移（萤火 demo 的"帰り道で"手法）
  const echo = pr.echo as { n: number; dx: number; decay: number } | undefined
  const copies = echo ? Math.max(1, Math.min(6, echo.n)) : 1

  if (pr.vertical) {
    // 竖排：逐字独立布局，沿 y 下排
    const chars = Array.from(text)
    const lh = sizePx * 1.14
    for (let i = 0; i < chars.length; i++) {
      const one = layoutRun([chars[i]], font, 0)
      const x = anchorX - one.totalWidth / 2
      const ym = anchorY + lh * i + lh / 2
      for (let c = copies - 1; c >= 0; c--) {
        const st: TextStyle = { ...style }
        if (c > 0 && echo) {
          st.alpha = Math.pow(echo.decay ?? 0.4, c)
        }
        let cs: CharState | undefined
        if (useOps) {
          const s: CharState = {}
          applyOperators(ops!, i, chars.length, t, s)
          cs = s
          if (c > 0 && echo) cs.alpha = (cs.alpha ?? 1) * (st.alpha ?? 1)
        }
        draws.push({ kind: 'text', layout: one, x, y: baselineForMiddle(font, ym), style: st, charStates: cs ? [cs] : undefined })
      }
    }
    return
  }

  const segments = useOps ? Array.from(text) : [text]
  const layout = layoutRun(segments, font, spacing)

  let x = anchorX - layout.totalWidth / 2
  if (align === 'left') x = anchorX
  else if (align === 'right') x = anchorX - layout.totalWidth
  // 横向夹进安全区（左/右对齐的注记文字也不能出框）
  x = Math.min(Math.max(x, box.x + box.w * 0.02), Math.max(box.x + box.w * 0.02, box.x + box.w * 0.98 - layout.totalWidth))

  let charStates: (CharState | undefined)[] | undefined
  if (useOps) {
    charStates = segments.map((_, i) => {
      const s: CharState = {}
      applyOperators(ops!, i, segments.length, t, s)
      return s
    })
  }

  for (let c = copies - 1; c >= 0; c--) {
    const st: TextStyle = { ...style }
    if (c > 0 && echo) {
      st.alpha = Math.pow(echo.decay ?? 0.4, c)
    }
    const cs2 = charStates?.map((s) => {
      if (!s || c === 0) return s
      const out = { ...s }
      out.alpha = (out.alpha ?? 1) * (st.alpha ?? 1)
      return out
    })
    draws.push({
      kind: 'text',
      layout,
      x: x - (echo ? (echo.dx ?? 8) * scale * c : 0),
      y: baselineForMiddle(font, anchorY),
      style: st,
      charStates: cs2,
    })
  }
}

// ─────────────────────────── 通用部件分发 ───────────────────────────

function compilePart(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  part: PartRef,
  t: number,
): void {
  // 未知部件静默跳过，保证「加了一个不认识的部件」也不会让整片渲染崩掉。
  switch (part.part) {
    case 'background':
      compileBackgroundPart(draws, box, part, t)
      break
    case 'text':
      compileTextPart(draws, box, part, t)
      break
    case 'overlay':
      compileOverlayPart(draws, box, part, t)
      break
    default:
      break
  }
}

// ─────────────────────────── 歌词（逐字卡拉OK） ───────────────────────────

/**
 * 歌词构型模式（一屏一戏的核心）。由 shot.motion.hold 携带：
 *   { part: 'lyricLayout', params: { mode, sizeScale, x, y } }
 *   mode: 'center'（默认居中横排）| 'corner'（角落小字+光点）|
 *         'giant'（巨型字占满画面）| 'vertical'（竖排）
 */
function lyricLayoutOf(
  shot: Shot,
): { mode: string; sizeScale: number; x?: number; y?: number } {
  const hold = shot.motion?.hold
  const pr = (hold?.params as Record<string, unknown> | undefined) ?? {}
  return {
    mode: (pr.mode as string) || 'center',
    sizeScale: (pr.sizeScale as number) || 1,
    x: pr.x as number | undefined,
    y: pr.y as number | undefined,
  }
}

function compileLyric(
  draws: Draw[],
  box: { x: number; y: number; w: number; h: number },
  p: Project,
  lyric: Lyric,
  t: number,
  accent: string,
  shot: Shot,
  safe = 1,
): void {
  const st = styleOf(p, lyric.style)
  const scale = box.h / 1080
  const lay = lyricLayoutOf(shot)
  let size = (st.size || 96) * scale * lay.sizeScale

  const syllables =
    lyric.syllables.length > 0
      ? lyric.syllables
      : [{ text: lyric.text, start: lyric.start, end: lyric.end }]

  // ── 安全区自适应（防出框的硬约束）──────────────────────────────
  // maxW = 92% 画幅 × 运镜折减。长行缩字号、短行按模式上限放大。
  const maxW = box.w * 0.92 * safe
  const fontAt = (s: number) =>
    `${st.weight || 900} ${s}px "${st.font || 'Noto Serif SC'}", "Microsoft YaHei", serif`
  const lsAt = (s: number) => (st.letter_spacing || 0) * s
  const measureAt = (s: number) =>
    layoutRun(syllables.map((x) => x.text), fontAt(s), lsAt(s))

  if (lay.mode === 'vertical') {
    // 竖排：先按可用高度压（n 字 × 1.16 行高），再按单字宽度兜底
    const yFrac = lay.y ?? 0.16
    const availH = box.h * Math.max(0.2, 0.92 - yFrac) * safe
    size = Math.min(size, availH / Math.max(1, syllables.length * 1.16))
    for (let pass = 0; pass < 2; pass++) {
      const widest = Math.max(...syllables.map((s) => layoutRun([s.text], fontAt(size), 0).totalWidth))
      if (widest > box.w * 0.16 * safe) size *= (box.w * 0.16 * safe) / widest
    }
  } else {
    // 横排系：宽度定量放大/缩小 + 高度上限
    const maxH = (lay.mode === 'giant' ? box.h * 0.76 : lay.mode === 'corner' ? box.h * 0.2 : box.h * 0.32) * safe
    const maxRatio = lay.mode === 'giant' ? 9 : 1 // 只有 giant 允许放大，其余只缩不涨
    for (let pass = 0; pass < 2; pass++) {
      const probe = measureAt(size)
      if (probe.totalWidth <= 0) break
      const ratio = Math.min(maxRatio, maxW / probe.totalWidth)
      if (ratio === 1) break
      size *= ratio
    }
    size = Math.min(size, maxH)
  }
  size = Math.max(14 * scale, size) // 下限：再长也不小于 14px@1080

  const ls = lsAt(size)
  const font = fontAt(size)

  const measureK = (s: { start: number; end: number }) => {
    const d = s.end - s.start
    if (d <= 0) return 1
    return clamp01((t - s.start) / d)
  }

  // 行入场（Motion.entrance）：trackIn（字距收拢）/ popUp（弹起）/ flyIn（飞入）
  const ent = shot.motion?.entrance
  const ep = (ent?.params as Record<string, unknown> | undefined) ?? undefined
  const eStyle = ep?.style as string | undefined
  const eDur = (ep?.dur as number) || 0.35
  const entranceK = eStyle ? clamp01((t - lyric.start) / eDur) : 1

  const applyEntrance = (cs: CharState, i: number, n: number): void => {
    if (!eStyle || entranceK >= 1) return
    const side = i % 2 === 0 ? -1 : 1
    if (eStyle === 'trackIn') {
      cs.dx = ((i - (n - 1) / 2) / Math.max(1, n)) * (1 - entranceK) * box.w * 0.6
      cs.alpha = entranceK
    } else if (eStyle === 'popUp') {
      const kb = ease('backOut', entranceK)
      cs.scale = (cs.scale ?? 1) * (0.55 + 0.45 * kb)
      cs.dy = (1 - entranceK) * 26 * scale
      cs.alpha = Math.min(1, entranceK * 2.5)
    } else if (eStyle === 'flyIn') {
      cs.dx = side * (1 - entranceK) * box.w * 0.45
      cs.rotation = side * (1 - entranceK) * 0.5
      cs.alpha = Math.min(1, entranceK * 1.6)
    }
  }

  const mkCharState = (i: number, n: number): CharState => {
    const k = measureK(syllables[i])
    const cs: CharState = { reveal: k }
    if (k > 0 && k < 1) {
      cs.color = accent
      cs.scale = 1.14
    }
    applyEntrance(cs, i, n)
    return cs
  }

  const fillStyle: TextStyle = { fill: st.fill || '#ffffff' }
  if (st.shadow) {
    fillStyle.shadow = {
      color: st.shadow.color || 'rgba(0,0,0,0.5)',
      blur: (st.shadow.blur || 8) * scale,
      offsetX: (st.shadow.offset?.[0] ?? 0) * scale,
      offsetY: (st.shadow.offset?.[1] ?? 4) * scale,
    }
  }
  // ghost 模板：一律用歌词**本色**低透明度（描边样式若用描边色，深底上会隐身——踩过）
  const ghostStyle: TextStyle = st.stroke
    ? { stroke: { color: hexA(st.fill || '#ffffff', 0.35), width: (st.stroke.width || 4) * scale * lay.sizeScale } }
    : { fill: hexA(st.fill || '#ffffff', 0.16) }

  const n = syllables.length

  // ── 竖排：逐字一个 draw，沿 y 下排（术曲PV标志性排版）──
  if (lay.mode === 'vertical') {
    const lh = size * 1.16
    // 整列宽度按最长单字算，横向也夹进安全区
    const widest = Math.max(...syllables.map((s) => layoutRun([s.text], font, 0).totalWidth))
    const xWant = box.x + box.w * (lay.x ?? 0.84)
    const x = Math.min(Math.max(xWant, box.x + widest * 0.6 + box.w * 0.02), box.x + box.w * 0.98 - widest * 0.6)
    const colH = lh * n
    let y0 = box.y + box.h * (lay.y ?? 0.16)
    if (y0 + colH > box.y + box.h * 0.98) y0 = Math.max(box.y + box.h * 0.02, box.y + box.h * 0.98 - colH)
    for (let i = 0; i < n; i++) {
      const one = layoutRun([syllables[i].text], font, 0)
      const ym = y0 + lh * i + lh / 2
      draws.push({
        kind: 'text', layout: one,
        x: x - one.totalWidth / 2, y: baselineForMiddle(font, ym),
        style: ghostStyle,
      })
      draws.push({
        kind: 'text', layout: one,
        x: x - one.totalWidth / 2, y: baselineForMiddle(font, ym),
        style: fillStyle, charStates: [mkCharState(i, n)],
      })
    }
    return
  }

  // ── 横排系（center / corner / giant）──
  const layout = layoutRun(
    syllables.map((s) => s.text),
    font,
    ls,
  )
  let startX: number
  let cy: number
  if (lay.mode === 'corner') {
    startX = box.x + box.w * (lay.x ?? 0.08)
    cy = box.y + box.h * (lay.y ?? 0.8)
    // 角落构图：左对齐时右端不能越过安全区
    startX = Math.min(startX, box.x + box.w * 0.92 - layout.totalWidth)
    startX = Math.max(startX, box.x + box.w * 0.04)
  } else {
    startX = box.x + box.w * (lay.x ?? 0.5) - layout.totalWidth / 2
    cy = box.y + box.h * (lay.y ?? (lay.mode === 'giant' ? 0.5 : 0.58))
  }
  // 纵向安全钳制：字身（≈ size）必须完整落在画面内
  const half = size * 0.5
  cy = Math.min(Math.max(cy, box.y + half * 1.02), box.y + box.h - half * 1.02)
  const baseline = baselineForMiddle(font, cy)

  // ghost 模板
  draws.push({ kind: 'text', layout, x: startX, y: baseline, style: ghostStyle })
  // 填充 + 逐字揭示
  draws.push({
    kind: 'text',
    layout,
    x: startX,
    y: baseline,
    style: fillStyle,
    charStates: syllables.map((_, i) => mkCharState(i, n)),
  })

  // corner 模式：配一个呼吸光点（术曲PV的极简语言）
  if (lay.mode === 'corner') {
    const dotK = 0.5 + 0.5 * Math.sin(t * (p.timing?.bpm ? (p.timing.bpm / 60) * Math.PI : 2))
    glowDot(draws, box.x + box.w * 0.85, cy - size * 0.3, 7 * scale, accent, dotK)
  }

  // 强调色下划（正在唱的那一字的进度条）—— 业务语义留在适配层，内核只看到 rect
  const uy = Math.min(cy + size * 0.62, box.y + box.h * 0.985)
  for (let i = 0; i < syllables.length; i++) {
    const k = measureK(syllables[i])
    if (k > 0 && k < 1) {
      draws.push({
        kind: 'rect',
        x: startX + layout.boxes[i].x,
        y: uy,
        w: layout.boxes[i].width * k,
        h: Math.max(2, 3 * scale),
        fill: accent,
      })
    }
  }
}

/** 发光点：多层同心圆模拟光晕 + 呼吸 alpha。 */
function glowDot(
  draws: Draw[],
  x: number,
  y: number,
  r: number,
  color: string,
  k: number,
): void {
  draws.push({ kind: 'circle', cx: x, cy: y, r: r * 4.5, fill: color, alpha: 0.06 + 0.05 * k })
  draws.push({ kind: 'circle', cx: x, cy: y, r: r * 2.4, fill: color, alpha: 0.14 + 0.08 * k })
  draws.push({ kind: 'circle', cx: x, cy: y, r, fill: color, alpha: 0.85 + 0.15 * k })
}

/** 把 `#rrggbb` / `#rrggbbaa` 转成带 alpha 的 rgba 字符串。 */
export function hexA(hex: string, alpha: number): string {
  let h = hex.replace('#', '')
  if (h.length === 3) h = h.split('').map((c) => c + c).join('')
  if (h.length === 6) h += 'ff'
  const r = parseInt(h.slice(0, 2), 16)
  const g = parseInt(h.slice(2, 4), 16)
  const b = parseInt(h.slice(4, 6), 16)
  const a = parseInt(h.slice(6, 8), 16) / 255
  return `rgba(${r},${g},${b},${(a * alpha).toFixed(3)})`
}

// ─────────────────────────── 兼容导出（统计/清理，透传 LUME） ───────────────────────────

/** 内核缓存统计（LUME 版：布局缓存 + 字形图集）。 */
export function kernelCacheStats() {
  return { engine: `LUME ${LUME_VERSION}`, ...lumeStats() }
}

/** 清空内核缓存（字体加载/切换、显存压力等场景主动调用）。 */
export function resetKernelCaches() {
  lumeReset()
}
