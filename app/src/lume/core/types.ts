/**
 * LUME (Letter Universal Motion Engine) · 琉明
 * 核心指令 IR —— 内核唯一对外契约。
 *
 * 设计红线（GuideOfUI.md §2.3 / §6.2）：
 * - 内核不理解「歌词」「淡入」「弹跳」等业务概念，只消费 Draw 指令流；
 * - 一帧 = 一条 `Frame`，纯数据、可序列化 —— 预览 / 导出 / 增量重渲三方共享；
 * - 逐字动画只携带变换/透明度/颜色/揭示量（CharState），绝不携带布局；
 *   布局在 `text/layout.ts` 一次性算好并缓存。
 */

/** 一段文字布局后的单个字形盒（tight 宽度，x 为相对整行的偏移）。 */
export interface GlyphBox {
  text: string
  x: number
  width: number
}

/** 布局结果：与时间 t 无关，可被无限帧复用。 */
export interface TextLayout {
  font: string
  letterSpacing: number
  boxes: GlyphBox[]
  totalWidth: number
}

/** 文字绘制样式（会被烘焙进字形图集的 key）。 */
export interface TextStyle {
  fill?: string
  stroke?: { color: string; width: number }
  shadow?: { color: string; blur: number; offsetX: number; offsetY: number }
  /** 整体透明度（1 为不透明）。 */
  alpha?: number
}

/**
 * 单个字形的动画状态 —— 逐字动画的全部表达。
 * 只改变换/透明度/颜色/揭示量，**绝不触发布局重算**（GuideOfUI.md 风险 #3）。
 */
export interface CharState {
  dx?: number
  dy?: number
  scale?: number
  /** 弧度 */
  rotation?: number
  alpha?: number
  /** 卡拉OK式左→右揭示进度 0..1（1 = 完整显示）。 */
  reveal?: number
  /** 覆盖填充色（需要单独烘焙一个图集条目）。 */
  color?: string
}

/** 文字绘制指令。x/y 为整行锚点（x 左对齐起点，y 为基线）。 */
export interface TextDraw {
  kind: 'text'
  layout: TextLayout
  x: number
  y: number
  style: TextStyle
  /** 与 layout.boxes 一一对应；缺省 = 静态完整显示。 */
  charStates?: (CharState | undefined)[]
}

export interface RectDraw {
  kind: 'rect'
  x: number
  y: number
  w: number
  h: number
  fill: string
  alpha?: number
}

export interface GradientDraw {
  kind: 'gradient'
  x: number
  y: number
  w: number
  h: number
  color: string
  color2: string
  /** 角度（度），90 = 垂直。 */
  angleDeg: number
}

export interface VignetteDraw {
  kind: 'vignette'
  x: number
  y: number
  w: number
  h: number
  edge: string
  inner: string
}

export interface CircleDraw {
  kind: 'circle'
  cx: number
  cy: number
  r: number
  fill: string
  alpha?: number
}

/** 描边圆环（不填充）。width 为线宽。 */
export interface RingDraw {
  kind: 'ring'
  cx: number
  cy: number
  r: number
  width: number
  color: string
  alpha?: number
}

/**
 * 径向渐变（光斑 / 极光底 / 辉光）。
 * inner→outer 由 rInner 到 rOuter 过渡，超出 rOuter 部分按 outer 色填满矩形。
 */
export interface RadialDraw {
  kind: 'radial'
  cx: number
  cy: number
  rInner: number
  rOuter: number
  inner: string
  outer: string
  /** 限制绘制区域（默认整幅由调用方传 box）。 */
  x?: number
  y?: number
  w?: number
  h?: number
}

/** 线段（网格 / 放射线 / 扫描线 / 十字准星）。dash 为 [实, 虚] 像素。 */
export interface LineDraw {
  kind: 'line'
  x1: number
  y1: number
  x2: number
  y2: number
  width: number
  color: string
  alpha?: number
  dash?: [number, number]
}

/**
 * 嵌套变换组：对子指令流整体施加仿射变换（运镜/遮扫/卷动的地基）。
 * 矩阵为 Canvas2D 语义：x' = a*x + c*y + e；y' = b*x + d*y + f。
 * alpha 作用于整组（与子指令自身的 alpha 相乘）。
 * 递归嵌套合法 —— 内核执行器必须可重入。
 */
export interface TransformDraw {
  kind: 'transform'
  a: number
  b: number
  c: number
  d: number
  e: number
  f: number
  alpha?: number
  draws: Draw[]
}

/**
 * 位图贴片：把一张已渲染好的离屏画布（或 ImageBitmap）直接 blit 到目标矩形。
 *
 * 用途（指南 §3.3 性能命门 #4）：程序化背景、胶片颗粒这类「几百条图元堆出来的
 * 静态/近似静态图层」，与其每帧重算几百条渐变/线/圆指令，不如先烘焙进离屏
 * canvas 缓存在外，每帧只做一次 drawImage。时变图层按时间分桶低频刷新即可。
 * 这是 LUME 在 Canvas2D 后端逼近"批处理"性能的关键一招。
 *
 * 源生命周期由调用方（缓存层）持有，Draw 只持引用。
 */
export interface BitmapDraw {
  kind: 'bitmap'
  src: CanvasImageSource
  x: number
  y: number
  w: number
  h: number
  alpha?: number
  /** 九参数 drawImage 的源裁剪（用于图集/子区域复用）。 */
  sx?: number
  sy?: number
  sw?: number
  sh?: number
}

/** 内核消费的最小绘制指令集。新部件类型在这里追加，后端逐个实现。 */
export type Draw =
  | TextDraw
  | RectDraw
  | GradientDraw
  | VignetteDraw
  | CircleDraw
  | RingDraw
  | RadialDraw
  | LineDraw
  | TransformDraw
  | BitmapDraw

/** 一帧 = 一条指令流。t 仅作标注，渲染器本身无状态。 */
export interface Frame {
  t: number
  draws: Draw[]
}
