/**
 * LUME 缓动注册表。
 *
 * 扩展点 #1：任何第三方 `registerEasing('myEase', fn)` 后即可在算子里用名字引用。
 * 所有缓动输入都被钳制到 [0,1]，保证动画求值的确定性。
 */

export type EasingFn = (x: number) => number

const registry = new Map<string, EasingFn>()

export function registerEasing(name: string, fn: EasingFn): void {
  registry.set(name, fn)
}

export function hasEasing(name: string): boolean {
  return registry.has(name)
}

export function easingNames(): string[] {
  return [...registry.keys()]
}

const clamp01 = (x: number) => (x < 0 ? 0 : x > 1 ? 1 : x)

/** 按名字求缓动；未知名字回退 linear（不抛错 —— 内核永不因未知项崩溃）。 */
export function ease(name: string, x: number): number {
  const fn = registry.get(name)
  if (!fn) return clamp01(x)
  return fn(clamp01(x))
}

// ─────────────────────────── 内置缓动 ───────────────────────────

registerEasing('linear', (x) => x)
registerEasing('sineInOut', (x) => -(Math.cos(Math.PI * x) - 1) / 2)
registerEasing('quadIn', (x) => x * x)
registerEasing('quadOut', (x) => 1 - (1 - x) * (1 - x))
registerEasing('quadInOut', (x) => (x < 0.5 ? 2 * x * x : 1 - Math.pow(-2 * x + 2, 2) / 2))
registerEasing('cubicIn', (x) => x * x * x)
registerEasing('cubicOut', (x) => 1 - Math.pow(1 - x, 3))
registerEasing('cubicInOut', (x) => (x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2))
registerEasing('backOut', (x) => {
  const c1 = 1.70158
  const c3 = c1 + 1
  return 1 + c3 * Math.pow(x - 1, 3) + c1 * Math.pow(x - 1, 2)
})
registerEasing('elasticOut', (x) => {
  const c4 = (2 * Math.PI) / 3
  return x === 0 ? 0 : x === 1 ? 1 : Math.pow(2, -10 * x) * Math.sin((x * 10 - 0.75) * c4) + 1
})
registerEasing('bounceOut', (x) => {
  const n1 = 7.5625
  const d1 = 2.75
  if (x < 1 / d1) return n1 * x * x
  if (x < 2 / d1) return n1 * (x -= 1.5 / d1) * x + 0.75
  if (x < 2.5 / d1) return n1 * (x -= 2.25 / d1) * x + 0.9375
  return n1 * (x -= 2.625 / d1) * x + 0.984375
})
