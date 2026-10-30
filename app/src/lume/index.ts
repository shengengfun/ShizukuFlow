/**
 * LUME (Letter Universal Motion Engine) · 琉明
 * Text2PV 渲染内核 —— 公共 API。
 *
 * 定位：UI 无关（零 React / 零 Tauri 依赖）、后端可换（Canvas2D 现在，WebGL2/wgpu 未来）、
 * 预览与导出唯一管线。业务语义（歌词/卡拉OK/镜头）不属于内核，
 * 由上层「工程 → Draw 指令流」的编译器负责（见 app/src/lib/renderFrame.ts 适配层）。
 *
 * 三条性能命门（GuideOfUI.md §3 / §4.1）全部内建：
 * 1. 布局缓存      —— text/layout.ts（动画与布局分离）
 * 2. 字形图集      —— render/atlas.ts（光栅化一次，逐帧只采样）
 * 3. 动画算子与布局彻底隔离 —— core/operators.ts（类型系统上拿不到布局）
 *
 * 可复现性红线：除确定性 hash01 外全引擎无随机、无时钟、无全局可变状态
 * （缓存只影响速度，不影响输出）。
 */

import { layoutStats, resetLayoutCache } from './text/layout'
import { atlasStats, resetAtlas } from './render/canvas2d'

export const LUME_VERSION = '0.1.0'

// 契约
export type {
  Draw,
  Frame,
  TextDraw,
  TextLayout,
  TextStyle,
  CharState,
  RectDraw,
  GradientDraw,
  VignetteDraw,
  CircleDraw,
  RingDraw,
  RadialDraw,
  LineDraw,
  TransformDraw,
  GlyphBox,
} from './core/types'

// 缓存基座
export { LruCache, hash01, hitRate, type CacheStats } from './core/cache'

// 扩展点
export { registerEasing, ease, hasEasing, easingNames, type EasingFn } from './core/easing'
export {
  registerOperator,
  applyOperators,
  hasOperator,
  operatorNames,
  type OperatorSpec,
  type OpContext,
  type CharOperator,
} from './core/operators'

// 文字管线
export { layoutRun, layoutStats, resetLayoutCache, baselineForMiddle, measureContext } from './text/layout'

// 渲染后端
export { renderDraws, atlasStats, resetAtlas } from './render/canvas2d'

/** 全内核缓存统计（供状态栏 / 基准测试 / 验收指标采集）。 */
export function lumeStats(): {
  version: string
  layout: { hits: number; misses: number; evictions: number }
  atlas: { hits: number; misses: number; evictions: number; entries: number; hitRate: number }
} {
  return { version: LUME_VERSION, layout: layoutStats(), atlas: atlasStats() }
}

/** 清空全内核缓存（字体切换 / 显存压力 / 基准冷启动时调用）。 */
export function lumeReset(): void {
  resetLayoutCache()
  resetAtlas()
}
