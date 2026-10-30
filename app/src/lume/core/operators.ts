/**
 * LUME 逐字动画算子注册表 —— 引擎可扩展性的核心。
 *
 * 一个算子 = 一个纯函数：给定 (字符序号, 时间窗, 参数) 把 CharState 就地改写。
 * 算子只碰 CharState（变换/透明度/颜色/揭示量），**永远拿不到布局** ——
 * 这从类型系统上执行了「动画与布局分离」红线。
 *
 * 扩展点 #2：`registerOperator('我的特效', fn)` 即插即用，内核零改动。
 * 新增动画预设只改这一层（指令生成），渲染调度与缓存体系不动（指南 §6.2）。
 */

import type { CharState } from './types'
import { ease } from './easing'
import { hash01 } from './cache'

export interface OperatorSpec {
  name: string
  /** 时间窗（秒，绝对时间）；窗外不生效。 */
  start: number
  end: number
  params?: Record<string, unknown>
}

export interface OpContext {
  /** 字形序号 / 总数 */
  i: number
  n: number
  /** 绝对时间（秒） */
  t: number
  /** 算子时间窗内的进度 0..1（含 per-char 交错后的结果由算子自行计算时可用原始量） */
  local: number
  /** 算子时间窗原始量，供交错类算子使用 */
  start: number
  end: number
  /** 确定性伪随机 [0,1)（按字符序号派生，可复现） */
  rand: number
  params: Record<string, unknown>
  state: CharState
}

export type CharOperator = (ctx: OpContext) => void

const registry = new Map<string, CharOperator>()

export function registerOperator(name: string, fn: CharOperator): void {
  registry.set(name, fn)
}

export function hasOperator(name: string): boolean {
  return registry.has(name)
}

export function operatorNames(): string[] {
  return [...registry.keys()]
}

const clamp01 = (x: number) => (x < 0 ? 0 : x > 1 ? 1 : x)
const num = (v: unknown, dflt: number) => (typeof v === 'number' && Number.isFinite(v) ? v : dflt)

/**
 * 对第 i 个字形应用一组算子。
 * `state` 会被就地改写；未命中时间窗的算子完全跳过（零开销）。
 */
export function applyOperators(
  specs: readonly OperatorSpec[],
  i: number,
  n: number,
  t: number,
  state: CharState,
): void {
  for (const spec of specs) {
    if (t < spec.start || t >= spec.end) continue
    const op = registry.get(spec.name)
    if (!op) continue // 未知算子静默跳过：渲染永不因未知项崩溃
    const span = spec.end - spec.start
    const local = span <= 0 ? 1 : clamp01((t - spec.start) / span)
    op({
      i,
      n,
      t,
      local,
      start: spec.start,
      end: spec.end,
      rand: hash01(i, 1),
      params: spec.params ?? {},
      state,
    })
  }
}

// ─────────────────────────── 内置算子 ───────────────────────────

/** 整窗淡入（from 可指定起始透明度）。 */
registerOperator('fade', ({ local, params, state }) => {
  const from = clamp01(num(params.from, 0))
  state.alpha = (state.alpha ?? 1) * (from + (1 - from) * local)
})

/** 弹性放大入场（backOut，自带过冲）。 */
registerOperator('popIn', ({ local, params, state }) => {
  const e = ease((params.ease as string) ?? 'backOut', local)
  const from = num(params.from, 0.2)
  state.scale = (state.scale ?? 1) * (from + (1 - from) * e)
})

/** 从下方滑入。 */
registerOperator('slideUp', ({ local, params, state }) => {
  const e = ease((params.ease as string) ?? 'cubicOut', local)
  state.dy = (state.dy ?? 0) + (1 - e) * num(params.distance, 48)
})

/** 从上方滑入。 */
registerOperator('slideDown', ({ local, params, state }) => {
  const e = ease((params.ease as string) ?? 'cubicOut', local)
  state.dy = (state.dy ?? 0) - (1 - e) * num(params.distance, 48)
})

/** 逐字交错淡入（打字机）：字符 i 延迟 i*stagger 秒。 */
registerOperator('typewriter', ({ i, t, start, params, state }) => {
  const stagger = num(params.stagger, 0.06)
  const dur = num(params.duration, 0.18)
  const local = clamp01((t - start - i * stagger) / dur)
  state.alpha = (state.alpha ?? 1) * local
})

/** 抖动：随 local 衰减的随机位移（确定性，可复现）。 */
registerOperator('shake', ({ local, rand, params, state }) => {
  const amp = num(params.amplitude, 6) * (1 - local)
  state.dx = (state.dx ?? 0) + (rand - 0.5) * 2 * amp
  state.dy = (state.dy ?? 0) + (hash01(rand * 97) - 0.5) * 2 * amp
})

/** 正弦波浪（持续型：整窗起伏，不衰减）。 */
registerOperator('wave', ({ i, t, params, state }) => {
  const amp = num(params.amplitude, 10)
  const freq = num(params.frequency, 2.4)
  const phase = num(params.phase, 0.55) * i
  state.dy = (state.dy ?? 0) + Math.sin(t * freq + phase) * amp
})

/** 旋转入场。 */
registerOperator('spinIn', ({ local, params, state }) => {
  const e = ease((params.ease as string) ?? 'cubicOut', local)
  const turns = num(params.turns, 0.5)
  state.rotation = (state.rotation ?? 0) + (1 - e) * turns * Math.PI * 2 * (num(params.direction, 1) >= 0 ? 1 : -1)
})
