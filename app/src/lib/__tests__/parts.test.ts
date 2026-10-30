/**
 * 部件目录验证 —— 证明 UI 里列出的每一项都对应一条真实渲染分支。
 *
 * 背景：早期版本在 UI 里写过一批引擎并不认识的部件名，点了没反应。
 * 这个测试把目录里的每一项都套到镜头上并真正渲染一帧，
 * 任何一项如果引擎里没有对应分支（或参数名写错），这里就会失败。
 *
 * 同时校验读回（库里高亮）与多值槽位的开关语义。
 */
import { describe, it, expect, beforeEach } from 'vitest'
import { renderFrame, resetBgCache } from '../renderFrame'
import { PART_GROUPS, SLOT_LABEL, activeSlots, applyPart } from '../parts'
import type { Project, Shot } from '../api'

/** 全方法计数桩：比 background-perf 那份更全，覆盖文字与路径类图元。 */
function ctxStub() {
  const grad = { addColorStop: () => {} }
  const c: Record<string, unknown> = {
    drawImage: () => {}, arc: () => {}, arcTo: () => {}, ellipse: () => {},
    roundRect: () => {}, rect: () => {}, stroke: () => {}, fill: () => {},
    fillRect: () => {}, strokeRect: () => {}, clearRect: () => {},
    fillText: () => {}, strokeText: () => {}, save: () => {}, restore: () => {},
    beginPath: () => {}, closePath: () => {}, moveTo: () => {}, lineTo: () => {},
    clip: () => {}, translate: () => {}, rotate: () => {}, scale: () => {},
    transform: () => {}, resetTransform: () => {}, setTransform: () => {},
    setLineDash: () => {}, quadraticCurveTo: () => {}, bezierCurveTo: () => {},
    createLinearGradient: () => grad, createRadialGradient: () => grad,
    createConicGradient: () => grad, createPattern: () => ({}),
    getImageData: () => ({ data: new Uint8ClampedArray(4) }),
    putImageData: () => {},
    measureText: (s: string) => ({
      width: String(s).length * 40,
      actualBoundingBoxAscent: 30, actualBoundingBoxDescent: 10,
      actualBoundingBoxLeft: -2, actualBoundingBoxRight: String(s).length * 40,
    }),
  }
  return new Proxy(c, {
    get: (t, k) => (k in t ? t[k as string] : 0),
    set: (t, k, v) => { t[k as string] = v; return true },
  }) as unknown as CanvasRenderingContext2D
}

function fixture(): Project {
  const shot: Shot = {
    id: 'S001', start: 0, end: 4, track: 0, lyric_refs: ['L001'],
    layers: { background: [], text: [], decor: [], overlay: [] },
    motion: { entrance: null, hold: null, exit: null, camera: null },
    transition: null, seed: 84213, locked: false,
  }
  return {
    format: 'shizukuflow', version: 1,
    meta: { title: 'parts', artist: '', created: '', modified: '', generator: '' },
    canvas: { width: 1280, height: 720, fps: 30, background: '#000000', safe_area: { enabled: false, margin: 0.1 } },
    media: { audio: null, reference: [] },
    timing: { bpm: 140, offset: 0, beats: [], downbeats: [], source: '' },
    styles: [{ id: 'main', font: 'sans-serif', weight: 900, size: 108, letter_spacing: 0, line_height: 1.2, fill: '#ffffff', stroke: null, shadow: null }],
    lyrics: [{
      id: 'L001', start: 0.3, end: 3.4,
      text: '黎明的颜色还留在窗上',
      syllables: Array.from('黎明的颜色还留在窗上').map((ch, i) => ({ text: ch, start: 0.3 + i * 0.16, end: 0.3 + (i + 1) * 0.16 })),
      style: 'main',
    }],
    shots: [shot],
    assets: [],
    export: { preset: 'mp4', video: { codec: 'h264', crf: 18, preset: 'medium', encoder: 'auto' }, gif: { fps: 30, width: 720, dither: 'sierra' }, apng: { fps: 30, colors: 256 } },
  }
}

/** 录制式桩：把每次调用的方法名与参数记下来，用来比较「这两帧画得不一样」。 */
function recordingCtx(log: string[]) {
  const grad = { addColorStop: (a: number, b: string) => log.push(`stop:${a}:${b}`) }
  const c: Record<string, unknown> = {
    createLinearGradient: (...a: unknown[]) => { log.push('linGrad:' + a.join(',')); return grad },
    createRadialGradient: (...a: unknown[]) => { log.push('radGrad:' + a.join(',')); return grad },
    createConicGradient: () => grad, createPattern: () => ({}),
    measureText: (s: string) => ({
      width: String(s).length * 40,
      actualBoundingBoxAscent: 30, actualBoundingBoxDescent: 10,
      actualBoundingBoxLeft: -2, actualBoundingBoxRight: String(s).length * 40,
    }),
    getImageData: () => ({ data: new Uint8ClampedArray(4) }),
  }
  for (const k of ['drawImage', 'arc', 'arcTo', 'ellipse', 'roundRect', 'rect', 'stroke', 'fill',
    'fillRect', 'strokeRect', 'clearRect', 'fillText', 'strokeText', 'save', 'restore',
    'beginPath', 'closePath', 'moveTo', 'lineTo', 'clip', 'translate', 'rotate', 'scale',
    'transform', 'resetTransform', 'setTransform', 'setLineDash', 'quadraticCurveTo',
    'bezierCurveTo', 'putImageData']) {
    c[k] = (name => (...a: unknown[]) => {
      log.push(name + ':' + a.map((v) => (typeof v === 'object' ? '[obj]' : String(v))).join(','))
    })(k)
  }
  return new Proxy(c, {
    get: (t, k) => (k in t ? t[k as string] : 0),
    set: (t, k, v) => { log.push(`set:${String(k)}=${String(v)}`); t[k as string] = v; return true },
  }) as unknown as CanvasRenderingContext2D
}

const withShot = (p: Project, s: Shot): Project => ({ ...p, shots: [s] })

describe('部件目录 · 真实性', () => {
  beforeEach(() => {
    resetBgCache()
    ;(globalThis as unknown as { document: unknown }).document = {
      createElement: (_tag: string) => ({ width: 0, height: 0, getContext: () => ctxStub() }),
    }
  })

  it('目录没有空分类，且 id 无重复', () => {
    for (const g of PART_GROUPS) expect(g.items.length, `${g.label} 应有部件`).toBeGreaterThan(0)
    const all = PART_GROUPS.flatMap((g) => g.items.map((i) => `${g.slot}/${i.id}`))
    expect(new Set(all).size, '存在重复 id').toBe(all.length)
    expect(PART_GROUPS.length).toBe(Object.keys(SLOT_LABEL).length)
  })

  it('每一项都能落位并渲染出一帧（不抛异常）', () => {
    const base = fixture()
    const shot = base.shots[0]
    for (const g of PART_GROUPS) {
      for (const item of g.items) {
        const next = applyPart(shot, g.slot, item, { accent: '#7fd4ff' })
        const p = withShot(base, next)
        expect(() => renderFrame(ctxStub(), 1280, 720, p, 1.2, '#7fd4ff'),
          `${g.label}/${item.label} 渲染失败`).not.toThrow()
      }
    }
  })

  it('每一项套用后画面真的发生了变化（不是静默跳过）', () => {
    /*
     * 引擎对认不出的部件是「静默跳过」，所以「不抛异常」不足以证明部件有效 ——
     * 必须比对该时刻的绘制序列是否真的变了。
     *
     * 采样点要跨整个镜头：登场只在行首 0.3s 内、入出场/转场只在镜头首尾一小段生效，
     * 只挑中间一帧会把这类部件全部误判成无效（已经踩过一次）。
     */
    const TIMES = [0.05, 0.12, 0.22, 0.4, 0.9, 1.2, 2.0, 3.6, 3.85, 3.98]
    // 按定义就应该「没变化」的两项：居中是引擎默认版式，硬切是不做转场。
    const INERT = new Set(['layout/center', 'transition/cut'])

    const base = fixture()
    const shot = base.shots[0]

    const baseline = new Map<number, string>()
    for (const t of TIMES) {
      const lg: string[] = []
      resetBgCache()
      renderFrame(recordingCtx(lg), 1280, 720, base, t, '#7fd4ff')
      baseline.set(t, lg.join('|'))
    }

    for (const g of PART_GROUPS) {
      for (const item of g.items) {
        if (INERT.has(`${g.slot}/${item.id}`)) continue
        const next = applyPart(shot, g.slot, item, { accent: '#7fd4ff' })
        let differs = false
        for (const t of TIMES) {
          const lg: string[] = []
          resetBgCache()
          renderFrame(recordingCtx(lg), 1280, 720, withShot(base, next), t, '#7fd4ff')
          if (lg.join('|') !== baseline.get(t)) { differs = true; break }
        }
        expect(differs, `${g.label}/${item.label} 全时段画面都与未套用时一致 → 引擎很可能静默跳过了它`).toBe(true)
      }
    }
  })

  it('每一项落位后都能被读回到对应槽位（库里能高亮）', () => {
    const base = fixture()
    for (const g of PART_GROUPS) {
      for (const item of g.items) {
        const next = applyPart(base.shots[0], g.slot, item, { accent: '#7fd4ff' })
        const act = activeSlots(next)[g.slot]
        // 相机按参数组合匹配，转场按 in/out 拆成两个 id，其余应精确命中
        const ok = act.has(item.id) || (g.slot === 'transition' && [...act].some((a) => a.startsWith(String(item.params.in ?? item.params.out ?? ''))))
        expect(ok, `${g.label}/${item.label} 落位后读不到 → 高亮会失效`).toBe(true)
      }
    }
  })
})

describe('部件目录 · 目标槽位', () => {
  const base = fixture()

  it('单值槽位写入正确的引擎 part 名', () => {
    const s = base.shots[0]
    const layout = applyPart(s, 'layout', { id: 'giant', label: '', params: {} })
    expect(layout.motion.hold?.part).toBe('lyricLayout')
    expect((layout.motion.hold?.params as { mode: string }).mode).toBe('giant')

    const ent = applyPart(s, 'entrance', { id: 'popUp', label: '', params: {} })
    expect(ent.motion.entrance?.part).toBe('lyricEntrance')
    expect((ent.motion.entrance?.params as { style: string }).style).toBe('popUp')

    const cam = applyPart(s, 'camera', { id: 'pushIn', label: '', params: { zoom: [1, 1.08] } })
    expect(cam.motion.camera?.part).toBe('camera')

    const tr = applyPart(s, 'transition', { id: 'flashIn', label: '', params: { in: 'flash', inDur: 0.22 } })
    expect(tr.transition?.part).toBe('transition')
  })

  it('背景是替换语义：永远是单层，不会越点越多', () => {
    let s = base.shots[0]
    for (const id of ['grid', 'aurora', 'noise']) {
      s = applyPart(s, 'background', { id, label: '', params: {} })
      expect(s.layers.background).toHaveLength(1)
      expect((s.layers.background[0].params as { kind: string }).kind).toBe(id)
    }
  })

  it('装饰 / 画面效果是开关语义：再点一次移除', () => {
    let s = base.shots[0]
    s = applyPart(s, 'decor', { id: 'beatRings', label: '', params: {} })
    expect(s.layers.decor.map((p) => p.part)).toEqual(['beatRings'])
    s = applyPart(s, 'decor', { id: 'glowDot', label: '', params: {} })
    expect(s.layers.decor.map((p) => p.part)).toEqual(['beatRings', 'glowDot'])
    s = applyPart(s, 'decor', { id: 'beatRings', label: '', params: {} })
    expect(s.layers.decor.map((p) => p.part)).toEqual(['glowDot'])

    let o = applyPart(base.shots[0], 'overlay', { id: 'grain', label: '', params: {} })
    expect(o.layers.overlay).toHaveLength(1)
    o = applyPart(o, 'overlay', { id: 'grain', label: '', params: {} })
    expect(o.layers.overlay).toHaveLength(0)
  })

  it('applyPart 不修改传入的镜头对象（React 依赖引用变化刷新）', () => {
    const s = base.shots[0]
    const before = JSON.stringify(s)
    applyPart(s, 'decor', { id: 'particles', label: '', params: {} })
    expect(JSON.stringify(s)).toBe(before)
  })
})
