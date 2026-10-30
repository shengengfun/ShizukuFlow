/**
 * 部件目录 —— UI 唯一允许列出的部件清单。
 *
 * 这份表存在的理由：以前 UI 里写死过一批部件名，引擎并不认，点了没反应 ——
 * 界面看着很全，实际是空的。这里每一项都必须对应 renderFrame.ts 里真实存在的一条分支，
 * 也就是说：**新增部件要先在渲染器里实现，再回来这里登记**。
 *
 * 命名全部取自真实工程数据（projects/*.sf）而不是凭空拟定：
 *   motion.hold      → { part:'lyricLayout',    params:{ mode } }
 *   motion.entrance  → { part:'lyricEntrance',  params:{ style, dur } }
 *   motion.camera    → { part:'camera',         params:{ zoom, pan, rotate, punch } }
 *   shot.transition  → { part:'transition',     params:{ in, out, inDur, outDur, dir, color } }
 *   layers.background→ { part:'background',     params:{ kind, color, color2, accent, angle, seed } }
 *   layers.decor     → { part:<名>,             params:{...} }
 *   layers.overlay   → { part:'overlay',        params:{ kind, ... } }
 *
 * 刻意没有收录的两类：
 *   - motion.exit ：类型里有字段，但渲染器从未读取（点了不会有画面变化）
 *   - overlay 的 solid / gradient / paper：它们会先铺一层不透明底色，当叠加用会盖住画面
 */

import type { PartRef, Shot } from './api'

/** 部件落点。单值槽位（替换）与多值槽位（叠加/开关）行为不同。 */
export type SlotId =
  | 'layout'
  | 'entrance'
  | 'camera'
  | 'transition'
  | 'background'
  | 'decor'
  | 'overlay'

export interface PartItem {
  /** 引擎识别的子类型：background 的 kind、decor 的 part 名、lyricLayout 的 mode…… */
  id: string
  /** 界面短名。只写名称，不写解释。 */
  label: string
  /** 默认参数。applyPart 会按槽位补上 part 名。 */
  params: Record<string, unknown>
}

export interface PartGroup {
  slot: SlotId
  label: string
  /** true = 可叠加多个（再点一次移除）；false = 单值，点击即替换 */
  multi: boolean
  items: PartItem[]
}

/** 叠加层用「透明底」：底色 rect 铺好后图案才看得见，否则会盖住画面。 */
const CLEAR = 'rgba(0,0,0,0)'

export const PART_GROUPS: PartGroup[] = [
  {
    slot: 'layout',
    label: '布局',
    multi: false,
    items: [
      { id: 'center', label: '居中横排', params: { sizeScale: 1 } },
      { id: 'corner', label: '角落小字', params: { sizeScale: 1 } },
      { id: 'giant', label: '巨型字', params: { sizeScale: 1 } },
      { id: 'vertical', label: '竖排', params: { sizeScale: 1 } },
    ],
  },
  {
    slot: 'entrance',
    label: '登场',
    multi: false,
    items: [
      { id: 'trackIn', label: '字距收拢', params: { dur: 0.3 } },
      { id: 'popUp', label: '弹起', params: { dur: 0.32 } },
      { id: 'flyIn', label: '飞入', params: { dur: 0.35 } },
    ],
  },
  {
    slot: 'camera',
    label: '相机',
    multi: false,
    items: [
      { id: 'pushIn', label: '推近', params: { zoom: [1, 1.08] } },
      { id: 'pullOut', label: '拉远', params: { zoom: [1.08, 1] } },
      { id: 'panX', label: '横移', params: { pan: [0.05, 0] } },
      { id: 'tiltY', label: '纵移', params: { pan: [0, 0.05] } },
      { id: 'roll', label: '滚转', params: { rotate: [0, 3] } },
      { id: 'beatPunch', label: '节拍冲击', params: { zoom: [1.14, 1], punch: 0.06 } },
    ],
  },
  {
    slot: 'transition',
    label: '转场',
    multi: false,
    items: [
      { id: 'cut', label: '硬切', params: { in: 'cut', out: 'cut' } },
      { id: 'fadeIn', label: '淡入', params: { in: 'fade', inDur: 0.3 } },
      { id: 'fadeOut', label: '淡出', params: { out: 'fade', outDur: 0.3 } },
      { id: 'flashIn', label: '闪白', params: { in: 'flash', inDur: 0.22, color: '#ffffff' } },
      {
        id: 'wipeIn',
        label: '擦除',
        params: { in: 'wipe', inDur: 0.26, dir: 'left', color: '#000000' },
      },
    ],
  },
  {
    slot: 'background',
    label: '背景',
    multi: false,
    items: [
      { id: 'solid', label: '纯色', params: { color: '#07070b' } },
      {
        id: 'gradient',
        label: '渐变',
        params: { color: '#07070b', color2: '#140a18', angle: 100 },
      },
      { id: 'aurora', label: '极光', params: { color: '#07070b', color2: '#140a18', angle: 100 } },
      { id: 'bokeh', label: '散景', params: { color: '#07070b', angle: 100 } },
      { id: 'grid', label: '网格', params: { color: '#07070b', angle: 100 } },
      { id: 'rays', label: '放射线', params: { color: '#07070b', angle: 100 } },
      { id: 'stripes', label: '条纹', params: { color: '#07070b', color2: '#140a18', angle: 100 } },
      { id: 'halftone', label: '半调', params: { color: '#07070b', angle: 100 } },
      { id: 'scanlines', label: '扫描线', params: { color: '#07070b', angle: 100 } },
      { id: 'noise', label: '噪点', params: { color: '#07070b' } },
      { id: 'paper', label: '纸纹', params: { color: '#f4eee4', color2: '#e8dfd0' } },
    ],
  },
  {
    slot: 'decor',
    label: '装饰',
    multi: true,
    items: [
      { id: 'beatRings', label: '节拍环', params: {} },
      { id: 'bars', label: '条形律动', params: {} },
      { id: 'particles', label: '粒子', params: {} },
      { id: 'flash', label: '闪光', params: {} },
      { id: 'frameLines', label: '框线', params: {} },
      { id: 'glowDot', label: '光点', params: {} },
    ],
  },
  {
    slot: 'overlay',
    label: '画面效果',
    multi: true,
    items: [
      { id: 'vignette', label: '暗角', params: { color: 'rgba(0,0,0,0.55)' } },
      { id: 'grain', label: '颗粒', params: { amount: 500, alpha: 0.05 } },
      { id: 'scanlines', label: '扫描线', params: { color: CLEAR, angle: 100 } },
      { id: 'halftone', label: '半调网点', params: { color: CLEAR, angle: 100 } },
      { id: 'noise', label: '噪点', params: { color: CLEAR } },
    ],
  },
]

/** 槽位 → 显示名，给检查器和日志用。 */
export const SLOT_LABEL: Record<SlotId, string> = {
  layout: '布局',
  entrance: '登场',
  camera: '相机',
  transition: '转场',
  background: '背景',
  decor: '装饰',
  overlay: '画面效果',
}

// ─────────────────────────── 读取当前值（库里高亮用） ───────────────────────────

const paramsOf = (r: PartRef | null | undefined): Record<string, unknown> =>
  (r?.params as Record<string, unknown> | undefined) ?? {}

function activeIn(shot: Shot, slot: SlotId): string[] {
  switch (slot) {
    case 'layout':
      return [String(paramsOf(shot.motion?.hold).mode ?? '')].filter(Boolean)
    case 'entrance':
      return [String(paramsOf(shot.motion?.entrance).style ?? '')].filter(Boolean)
    case 'camera': {
      // 相机是按参数组合取胜的连续槽位：只有完整命中预设参数才算「在用」
      const cur = paramsOf(shot.motion?.camera)
      const hit = PART_GROUPS.find((g) => g.slot === 'camera')!.items.find((it) =>
        Object.entries(it.params).every(([k, v]) => jsonEq(cur[k], v)),
      )
      return hit ? [hit.id] : []
    }
    case 'transition': {
      const p = paramsOf(shot.transition)
      const out: string[] = []
      if (p.in && p.in !== 'cut') out.push(String(p.in === 'fade' ? 'fadeIn' : `${p.in}In`))
      if (p.out && p.out !== 'cut') out.push(String(p.out === 'fade' ? 'fadeOut' : `${p.out}Out`))
      if (out.length === 0) out.push('cut')
      return out
    }
    case 'background':
      return shot.layers.background.map((p) => String(paramsOf(p).kind ?? ''))
    case 'decor':
      return shot.layers.decor.map((p) => p.part)
    case 'overlay':
      return shot.layers.overlay.map((p) => String(paramsOf(p).kind ?? ''))
  }
}

function jsonEq(a: unknown, b: unknown): boolean {
  return JSON.stringify(a) === JSON.stringify(b)
}

/** 一次算出全部槽位的当前值，避免每个分类各遍历一遍镜头。 */
export function activeSlots(shot: Shot): Record<SlotId, Set<string>> {
  const out = {} as Record<SlotId, Set<string>>
  for (const g of PART_GROUPS) out[g.slot] = new Set(activeIn(shot, g.slot))
  return out
}

// ─────────────────────────── 写回 ───────────────────────────

export interface ApplyOpts {
  /** 工程强调色：背景/叠加层的图案色随它走 */
  accent?: string
  /** 图案随机种子，缺省沿用镜头 seed，保证可复现 */
  seed?: number
}

/**
 * 把部件落到镜头上，返回新 Shot（不改原对象）。
 *
 * 单值槽位直接替换；多值槽位（装饰 / 画面效果）是开关语义：
 * 已在用则移除，未在用则追加 —— 这样库里点击永远可逆，不需要额外的删除入口。
 */
export function applyPart(shot: Shot, slot: SlotId, item: PartItem, opts: ApplyOpts = {}): Shot {
  const accent = opts.accent ?? '#7fd4ff'
  const seed = opts.seed ?? shot.seed
  const layers = shot.layers
  const motion = shot.motion

  // 多值槽位按「子类型」去重而不是按全参数比对：
  // 工程里已有的同类部件参数未必和这里的默认值一致（比如颗粒 amount 420 vs 500），
  // 全参数比对会导致再点一次不是移除而是叠上第二份。
  const toggle = <K extends 'decor' | 'overlay'>(
    key: K,
    ref: PartRef,
    sameKind: (p: PartRef) => boolean,
  ): Shot => {
    const cur = layers[key]
    const next = cur.some(sameKind)
      ? cur.filter((p) => !sameKind(p))
      : [...cur, ref]
    return { ...shot, layers: { ...layers, [key]: next } }
  }

  switch (slot) {
    case 'layout':
      return {
        ...shot,
        motion: {
          ...motion,
          hold: { part: 'lyricLayout', params: { mode: item.id, ...item.params } },
        },
      }

    case 'entrance':
      return {
        ...shot,
        motion: {
          ...motion,
          entrance: { part: 'lyricEntrance', params: { style: item.id, ...item.params } },
        },
      }

    case 'camera':
      return { ...shot, motion: { ...motion, camera: { part: 'camera', params: { ...item.params } } } }

    case 'transition':
      return { ...shot, transition: { part: 'transition', params: { ...item.params } } }

    case 'background':
      return {
        ...shot,
        layers: {
          ...layers,
          background: [
            {
              part: 'background',
              params: { accent, seed, kind: item.id, ...item.params },
            },
          ],
        },
      }

    case 'decor':
      return toggle(
        'decor',
        { part: item.id, params: { accent, seed, ...item.params } },
        (p) => p.part === item.id,
      )

    case 'overlay':
      return toggle(
        'overlay',
        { part: 'overlay', params: { accent, seed, kind: item.id, ...item.params } },
        (p) => paramsOf(p).kind === item.id,
      )
  }
}
