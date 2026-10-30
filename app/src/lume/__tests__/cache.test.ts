import { describe, it, expect } from 'vitest'
import { LruCache, hash01, hitRate } from '../core/cache'

describe('LruCache', () => {
  it('命中与未命中计数', () => {
    const c = new LruCache<string, number>(4)
    expect(c.get('a')).toBeUndefined()
    c.set('a', 1)
    expect(c.get('a')).toBe(1)
    expect(c.stats.hits).toBe(1)
    expect(c.stats.misses).toBe(1)
  })

  it('超限淘汰最旧条目（LRU 语义）', () => {
    const c = new LruCache<string, number>(3)
    c.set('a', 1)
    c.set('b', 2)
    c.set('c', 3)
    c.get('a') // a 变为最新
    c.set('d', 4) // 挤掉 b
    expect(c.get('b')).toBeUndefined()
    expect(c.get('a')).toBe(1)
    expect(c.get('c')).toBe(3)
    expect(c.get('d')).toBe(4)
    expect(c.stats.evictions).toBe(1)
  })

  it('set 覆盖同 key 不产生幽灵条目', () => {
    const c = new LruCache<string, number>(2)
    c.set('a', 1)
    c.set('a', 9)
    expect(c.size).toBe(1)
    expect(c.get('a')).toBe(9)
  })

  it('hitRate 计算', () => {
    expect(hitRate({ hits: 95, misses: 5, evictions: 0 })).toBe(0.95)
    expect(hitRate({ hits: 0, misses: 0, evictions: 0 })).toBe(1)
  })
})

describe('hash01', () => {
  it('确定性：同输入恒同输出', () => {
    for (let i = 0; i < 100; i++) {
      expect(hash01(i)).toBe(hash01(i))
    }
  })
  it('输出在 [0,1) 且不同种子可区分', () => {
    for (let i = 0; i < 1000; i++) {
      const r = hash01(i, 7)
      expect(r).toBeGreaterThanOrEqual(0)
      expect(r).toBeLessThan(1)
    }
    expect(hash01(1, 1)).not.toBe(hash01(1, 2))
  })
})
