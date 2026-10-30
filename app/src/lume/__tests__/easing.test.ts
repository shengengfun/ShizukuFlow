import { describe, it, expect } from 'vitest'
import { ease, registerEasing, hasEasing, easingNames } from '../core/easing'

describe('easing registry', () => {
  it('内置缓动端点恒为 0/1', () => {
    for (const name of easingNames()) {
      expect(ease(name, 0)).toBeCloseTo(0, 6)
      expect(ease(name, 1)).toBeCloseTo(1, 6)
    }
  })

  it('输入钳制到 [0,1]，不抛错', () => {
    expect(ease('cubicOut', -5)).toBe(0)
    expect(ease('cubicOut', 5)).toBe(1)
  })

  it('未知缓动回退 linear（内核永不因未知项崩溃）', () => {
    expect(hasEasing('不存在')).toBe(false)
    expect(ease('不存在', 0.5)).toBe(0.5)
  })

  it('registerEasing 即插即用', () => {
    registerEasing('testDouble', (x) => x * 2)
    expect(ease('testDouble', 0.4)).toBe(0.8)
    // 输入仍被钳制
    expect(ease('testDouble', 2)).toBe(2)
  })

  it('backOut 具备过冲特性（>1）', () => {
    let peak = 0
    for (let i = 0; i <= 100; i++) peak = Math.max(peak, ease('backOut', i / 100))
    expect(peak).toBeGreaterThan(1)
  })
})
