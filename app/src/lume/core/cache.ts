/**
 * LUME 缓存基座：带统计的 LRU。
 *
 * 指南（GuideOfUI.md §3.2 / §8）把「布局缓存命中率 > 90%、字形图集命中率 > 95%」
 * 列为验收指标 —— 所以缓存的命中/未命中/淘汰必须可观测，不是可选项。
 * 将来内核下沉 Rust，`LruCache` 的语义原样平移。
 */

export interface CacheStats {
  hits: number
  misses: number
  evictions: number
}

export function emptyStats(): CacheStats {
  return { hits: 0, misses: 0, evictions: 0 }
}

export function hitRate(s: CacheStats): number {
  const total = s.hits + s.misses
  return total === 0 ? 1 : s.hits / total
}

/** 插入序即访问序的 LRU（Map 重插刷新）。条目数超限淘汰最旧者。 */
export class LruCache<K, V> {
  private map = new Map<K, V>()
  readonly stats: CacheStats = emptyStats()

  constructor(readonly max: number) {
    if (max < 1) throw new Error('LruCache.max must be >= 1')
  }

  get size(): number {
    return this.map.size
  }

  get(key: K): V | undefined {
    const v = this.map.get(key)
    if (v === undefined) {
      this.stats.misses++
      return undefined
    }
    this.stats.hits++
    // 刷新到最新（近似 LRU）
    this.map.delete(key)
    this.map.set(key, v)
    return v
  }

  set(key: K, value: V): void {
    if (this.map.has(key)) this.map.delete(key)
    this.map.set(key, value)
    while (this.map.size > this.max) {
      const oldest = this.map.keys().next().value
      if (oldest === undefined) break
      this.map.delete(oldest)
      this.stats.evictions++
    }
  }

  clear(): void {
    this.map.clear()
  }
}

/** 确定性伪随机（同输入恒同输出 —— 可复现性红线，禁 Math.random）。 */
export function hash01(i: number, seed = 0): number {
  const x = Math.sin(i * 12.9898 + seed * 78.233) * 43758.5453
  return x - Math.floor(x)
}
