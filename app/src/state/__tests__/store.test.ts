/**
 * 工程状态层的性质测试。
 *
 * 这里验证的不是「方法能调用」，而是重构报告 §3.2 要求的三条硬约束：
 *   1. 状态层是唯一权威所有者 → UI 拿不到可写句柄（见 snapshot 只读性测试）
 *   2. 撤销/重走用增量 diff → 改一个参数只产生一条叶子级补丁（见「叶子级」测试）
 *   3. 命令可逆 → undo 后必须与改之前**逐字节一致**（不是「看起来对」）
 */

import { describe, expect, it, vi } from 'vitest'
import { ProjectStore, diff } from '../store'
import type { Lyric, Project, Shot } from '../../lib/api'

// ── 夹具：结构贴近真实工程（含 shots / lyrics / 嵌套 params） ──

function shot(over: Partial<Shot> = {}): Shot {
  return {
    id: 'S001',
    start: 0,
    end: 4,
    track: 0,
    lyric_refs: ['L1'],
    layers: { background: [], overlay: [], decor: [], text: [] },
    motion: { entrance: null, hold: null, exit: null, camera: null },
    transition: null,
    seed: 12345,
    locked: false,
    ...over,
  } as Shot
}

function lyric(over: Partial<Lyric> = {}): Lyric {
  return { id: 'L1', text: 'テスト', start: 0.5, end: 1.5, style_ref: 'default', syllables: [], ...over } as Lyric
}

function project(over: Partial<Project> = {}): Project {
  return {
    format: 'ShizukuFlow',
    version: 1,
    meta: { title: 'P', artist: 'A', accent: '#7fd4ff' },
    canvas: { width: 1280, height: 720, fps: 30, background: '#000' },
    media: { audio: null },
    timing: { bpm: 120, offset: 0, downbeats: [] },
    styles: [],
    lyrics: [lyric()],
    shots: [shot(), shot({ id: 'S002', start: 4, end: 8, seed: 999 })],
    assets: [],
    export: { preset: 'mp4', video: { codec: 'h264', crf: 20, preset: 'medium', encoder: 'auto' }, gif: { fps: 15, width: 480, dither: 'none' }, apng: { fps: 15, colors: 128 } },
    ...over,
  } as unknown as Project
}

describe('diff —— 增量补丁', () => {
  it('改一个深层参数只产出一条叶子级补丁', () => {
    const a = project()
    const b = structuredClone(a) as Project
    b.shots[0].motion.camera = { part: 'camera', params: { kind: 'zoom', amount: 1.2 } }

    const patches = diff(a, b, [])
    expect(patches).toHaveLength(1)
    expect(patches[0].kind).toBe('set')
    // 路径必须精确落到被改的那一片叶子，而不是整棵 shots
    expect(patches[0].path).toEqual(['shots', 0, 'motion', 'camera'])
    expect(patches.every((p) => p.path.length >= 2)).toBe(true)
  })

  it('未改动的字段不出现在补丁里', () => {
    const a = project()
    const b = structuredClone(a) as Project
    ;(b.meta as { title: string }).title = '改过的标题'
    const patches = diff(a, b, [])
    expect(patches).toHaveLength(1)
    expect(patches[0].path).toEqual(['meta', 'title'])
  })

  it('数组整体内容相同时不产出补丁', () => {
    const a = project()
    const b = structuredClone(a) as Project
    expect(diff(a, b, [])).toHaveLength(0)
  })

  it('增删镜头（id 序列变化）退化为数组级替换，而不是整个工程', () => {
    const a = project()
    const b = structuredClone(a) as Project
    b.shots = [a.shots[1]]
    const patches = diff(a, b, [])
    expect(patches).toHaveLength(1)
    expect(patches[0].kind).toBe('array')
    expect(patches[0].path).toEqual(['shots'])
  })
})

describe('ProjectStore —— 命令 / 撤销 / 重走', () => {
  it('undo 后工程与改之前逐字节一致', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)

    store.dispatch({
      label: '镜头 S001 改用推近',
      apply: (d) => {
        d.shots[0].motion.camera = { part: 'camera', params: { kind: 'zoom', amount: 1.2 } }
      },
    })
    expect(store.get()).not.toEqual(before)

    expect(store.undo()).toBe(true)
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })

  it('redo 恢复到 undo 之前的样子', () => {
    const store = new ProjectStore()
    store.reset(project())
    store.dispatch({ label: '改标题', apply: (d) => { (d.meta as { title: string }).title = 'X' } })
    const edited = JSON.stringify(store.get())
    store.undo()
    expect(store.redo()).toBe(true)
    expect(JSON.stringify(store.get())).toBe(edited)
  })

  it('连续 20 次不同编辑 → 全部撤销后回到原点', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)
    for (let i = 0; i < 20; i++) {
      store.dispatch({ label: `第 ${i} 次`, apply: (d) => { d.shots[0].seed = 1000 + i } })
    }
    expect(store.history()).toHaveLength(20)
    for (let i = 0; i < 20; i++) store.undo()
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
    expect(store.snapshot().canUndo).toBe(false)
  })

  it('新编辑会让重走栈失效', () => {
    const store = new ProjectStore()
    store.reset(project())
    store.dispatch({ label: 'A', apply: (d) => { d.shots[0].seed = 1 } })
    store.undo()
    expect(store.snapshot().canRedo).toBe(true)
    store.dispatch({ label: 'B', apply: (d) => { d.shots[0].seed = 2 } })
    expect(store.snapshot().canRedo).toBe(false)
    expect(store.redo()).toBe(false)
  })

  it('空编辑不产生历史记录', () => {
    const store = new ProjectStore()
    store.reset(project())
    const ok = store.dispatch({ label: '没改任何东西', apply: () => {} })
    expect(ok).toBe(false)
    expect(store.history()).toHaveLength(0)
  })

  it('dispatch 不修改旧工程对象（不可变）', () => {
    const store = new ProjectStore()
    const before = project()
    const frozenMirror = JSON.stringify(before)
    store.reset(before)
    store.dispatch({ label: '改', apply: (d) => { d.shots[0].seed = 777 } })
    // UI 若还持有旧引用，不应该看到它被就地改写
    expect(JSON.stringify(before)).toBe(frozenMirror)
    expect(store.get()).not.toBe(before)
  })
})

describe('ProjectStore —— 合并与通知', () => {
  it('滑块连续拖动只产生一条历史，撤销直接回到起点', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)
    for (let i = 1; i <= 15; i++) {
      store.dispatch({
        label: 'CRF',
        apply: (d) => { d.export.video.crf = 20 + i },
        coalesceKey: 'export.crf',
      })
    }
    expect(store.history()).toHaveLength(1)
    store.undo()
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })

  it('合并窗口过期后会开一条新历史', async () => {
    const store = new ProjectStore(10)
    store.reset(project())
    const realNow = Date.now
    let clock = 1_000_000
    Date.now = () => clock
    try {
      store.dispatch({ label: 'A', apply: (d) => { d.shots[0].seed = 1 }, coalesceKey: 'k' })
      clock += 5_000 // 超出 600ms 窗口
      store.dispatch({ label: 'B', apply: (d) => { d.shots[0].seed = 2 }, coalesceKey: 'k' })
      expect(store.history()).toHaveLength(2)
    } finally {
      Date.now = realNow
    }
  })

  it('订阅者能收到变更通知，退订后不再收到', () => {
    const store = new ProjectStore()
    store.reset(project())
    const fn = vi.fn()
    const off = store.subscribe(fn)
    store.dispatch({ label: 'A', apply: (d) => { d.shots[0].seed = 1 } })
    expect(fn).toHaveBeenCalledTimes(1)
    expect(fn.mock.calls[0][1]).toBe('command')
    off()
    store.dispatch({ label: 'B', apply: (d) => { d.shots[0].seed = 2 } })
    expect(fn).toHaveBeenCalledTimes(1)
  })

  it('快照是只读投影：UI 绕过 dispatch 直接改写会被拒绝', () => {
    const store = new ProjectStore()
    store.reset(project())
    const snap = store.snapshot()
    // 权威状态只允许经 dispatch 变更；直接写必须在运行时挡住，而不是静默生效
    expect(() => {
      ;(snap.project as Project).shots[0].seed = 424242
    }).toThrow()
    expect(store.get()!.shots[0].seed).toBe(12345)
  })

  it('append 到冻结数组同样被拒绝（防止 UI 绕过命令增删镜头）', () => {
    const store = new ProjectStore()
    store.reset(project())
    const snap = store.snapshot()
    expect(() => {
      snap.project!.shots.push(shot({ id: 'SNEAK' }))
    }).toThrow()
    expect(store.get()!.shots).toHaveLength(2)
  })

  it('reset 会清空历史（跨工程撤销没有意义）', () => {
    const store = new ProjectStore()
    store.reset(project())
    store.dispatch({ label: 'A', apply: (d) => { d.shots[0].seed = 1 } })
    store.reset(project())
    expect(store.snapshot().canUndo).toBe(false)
    expect(store.snapshot().canRedo).toBe(false)
  })

  it('dirty 标记：改了就脏，保存后可清', () => {
    const store = new ProjectStore()
    store.reset(project())
    expect(store.snapshot().dirty).toBe(false)
    store.dispatch({ label: 'A', apply: (d) => { d.shots[0].seed = 1 } })
    expect(store.snapshot().dirty).toBe(true)
    store.markSaved()
    expect(store.snapshot().dirty).toBe(false)
  })
})

describe('补丁的可逆性（随机性质测试）', () => {
  it('随机 100 次编辑，全部撤销后必定回到初始状态', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)

    let rng = 20261030
    const rand = () => ((rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff)

    const edits: Array<(d: Project) => void> = [
      (d) => { d.shots[0].seed = Math.floor(rand() * 1e6) },
      (d) => { d.shots[0].end = 1 + rand() * 8 },
      (d) => { d.shots[0].motion.camera = { part: 'camera', params: { amount: rand() } } },
      (d) => { (d.meta as { title: string }).title = 'T' + Math.floor(rand() * 100) },
      (d) => { d.lyrics[0].text = 'あ' + Math.floor(rand() * 10) },
      (d) => { d.export.video.crf = 10 + Math.floor(rand() * 20) },
      (d) => { d.canvas.fps = [24, 30, 60][Math.floor(rand() * 3)] },
      (d) => { d.shots.push(shot({ id: 'S' + Math.floor(rand() * 1e6), seed: 1 })) },
      (d) => { d.shots.pop() },
    ]

    let applied = 0
    for (let i = 0; i < 100; i++) {
      const e = edits[Math.floor(rand() * edits.length)]
      if (store.dispatch({ label: `rnd-${i}`, apply: e })) applied++
    }
    expect(applied).toBeGreaterThan(50)

    let guard = 0
    while (store.snapshot().canUndo && guard++ < 500) store.undo()
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })
})
