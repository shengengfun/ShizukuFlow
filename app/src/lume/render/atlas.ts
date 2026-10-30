/**
 * LUME 字形图集（Glyph Atlas）。
 *
 * 指南（GuideOfUI.md §3.1）头号瓶颈的解法：同一段「字形 + 样式」全宇宙只光栅化
 * 一次，烘焙进离屏画布；之后每帧只是 drawImage 采样 —— 逐帧 fillText 成本归零。
 * key = 字体|字距|文本|样式签名，所以幽灵字/描边/阴影/换色天然是不同条目。
 *
 * 打包算法：shelf packing（行式装箱）；图集写满整盘清除重建（统计 evictions）。
 * 将来下沉 Rust/wgpu，接口原样（get → cell → drawImage 变纹理采样）。
 */

export interface AtlasCell {
  x: number
  y: number
  w: number
  h: number
  /** 基线距 cell 顶部的距离 */
  bl: number
}

const PAD = 2

export class GlyphAtlas {
  readonly canvas: HTMLCanvasElement
  private g: CanvasRenderingContext2D
  private cells = new Map<string, AtlasCell>()
  private cursorX = 0
  private cursorY = 0
  private rowH = 0

  /** 命中统计（验收指标：命中率 > 95%，GuideOfUI.md §8） */
  hits = 0
  misses = 0
  evictions = 0

  constructor(readonly size = 2048) {
    this.canvas = document.createElement('canvas')
    this.canvas.width = size
    this.canvas.height = size
    const g = this.canvas.getContext('2d')
    if (!g) throw new Error('LUME atlas: 2D context unavailable')
    this.g = g
  }

  get count(): number {
    return this.cells.size
  }

  get hitRate(): number {
    const total = this.hits + this.misses
    return total === 0 ? 1 : this.hits / total
  }

  lookup(key: string): AtlasCell | undefined {
    const cell = this.cells.get(key)
    if (cell) this.hits++
    else this.misses++
    return cell
  }

  /** 分配并烘焙一个条目。调用前应先 lookup 未命中。 */
  alloc(
    key: string,
    w: number,
    h: number,
    bl: number,
    paint: (g: CanvasRenderingContext2D, ox: number, oy: number) => void,
  ): AtlasCell {
    const width = Math.max(1, Math.ceil(w))
    const height = Math.max(1, Math.ceil(h))
    // 换行
    if (this.cursorX + width + PAD > this.size) {
      this.cursorX = 0
      this.cursorY += this.rowH + PAD
      this.rowH = 0
    }
    // 整盘满 → 清除重建（条目会按需重烘焙，miss 数自然回升）
    if (this.cursorY + height + PAD > this.size) {
      this.evict()
    }
    const cell: AtlasCell = { x: this.cursorX, y: this.cursorY, w: width, h: height, bl }
    paint(this.g, cell.x, cell.y)
    this.cursorX += width + PAD
    this.rowH = Math.max(this.rowH, height)
    this.cells.set(key, cell)
    return cell
  }

  private evict(): void {
    this.g.clearRect(0, 0, this.size, this.size)
    this.cells.clear()
    this.cursorX = 0
    this.cursorY = 0
    this.rowH = 0
    this.evictions++
  }

  clear(): void {
    this.evict()
    this.evictions = 0
    this.hits = 0
    this.misses = 0
  }
}
