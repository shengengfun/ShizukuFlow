import { describe, it, expect } from 'vitest'
import {
  applyOperators,
  registerOperator,
  hasOperator,
  operatorNames,
} from '../core/operators'
import type { CharState } from '../core/types'

const spec = (name: string, start: number, end: number, params?: Record<string, unknown>) => ({
  name,
  start,
  end,
  ...(params ? { params } : {}),
})

describe('operator registry', () => {
  it('时间窗外完全跳过（零开销，不改状态）', () => {
    const s: CharState = { alpha: 1 }
    applyOperators([spec('fade', 10, 20)], 0, 1, 5, s)
    expect(s.alpha).toBe(1)
    applyOperators([spec('fade', 10, 20)], 0, 1, 20, s) // end 为开区间
    expect(s.alpha).toBe(1)
  })

  it('fade：窗内线性淡入', () => {
    const s: CharState = {}
    applyOperators([spec('fade', 0, 2)], 0, 1, 1, s)
    expect(s.alpha).toBeCloseTo(0.5, 6)
  })

  it('typewriter：逐字交错', () => {
    const st = (i: number, t: number) => {
      const s: CharState = {}
      applyOperators([spec('typewriter', 0, 5, { stagger: 0.1, duration: 0.2 })], i, 10, t, s)
      return s.alpha ?? 1
    }
    expect(st(0, 0.05)).toBeGreaterThan(0) // 第 0 字已开始
    expect(st(5, 0.05)).toBe(0) // 第 5 字还没轮到
    expect(st(5, 0.55)).toBeGreaterThan(0) // 第 5 字进行中
    expect(st(5, 0.75)).toBe(1) // 第 5 字完成（0.5 + 0.2 = 0.7 后）
  })

  it('wave：确定性（同 t 同结果，可复现）', () => {
    const run = (t: number) => {
      const s: CharState = {}
      applyOperators([spec('wave', 0, 100, { amplitude: 8 })], 3, 10, t, s)
      return s.dy
    }
    expect(run(1.234)).toBe(run(1.234))
    expect(run(1.234)).not.toBe(run(2.345))
  })

  it('popIn：scale 过冲后回到 1（时间窗为开区间，终点前一刻收敛）', () => {
    const s: CharState = {}
    applyOperators([spec('popIn', 0, 1)], 0, 1, 0.5, s)
    expect((s.scale ?? 0)).toBeGreaterThan(1) // backOut 过冲
    const done: CharState = {}
    applyOperators([spec('popIn', 0, 1)], 0, 1, 1 - 1e-9, done)
    expect(done.scale).toBeCloseTo(1, 6)
  })

  it('未知算子静默跳过（渲染永不因未知项崩溃）', () => {
    expect(hasOperator('不存在的算子')).toBe(false)
    const s: CharState = {}
    expect(() => applyOperators([spec('不存在的算子', 0, 1)], 0, 1, 0.5, s)).not.toThrow()
    expect(s.alpha).toBeUndefined()
  })

  it('registerOperator 即插即用，内核零改动', () => {
    registerOperator('testShift', (ctx) => {
      ctx.state.dx = ctx.local * 100
    })
    expect(hasOperator('testShift')).toBe(true)
    expect(operatorNames()).toContain('testShift')
    const s: CharState = {}
    applyOperators([spec('testShift', 0, 2)], 0, 1, 1, s)
    expect(s.dx).toBeCloseTo(50, 6)
  })

  it('多算子叠加按顺序生效', () => {
    const s: CharState = {}
    applyOperators([spec('fade', 0, 1), spec('slideUp', 0, 1)], 0, 1, 0.5, s)
    expect(s.alpha).toBeCloseTo(0.5, 6)
    expect((s.dy ?? 0)).toBeGreaterThan(0)
  })
})
