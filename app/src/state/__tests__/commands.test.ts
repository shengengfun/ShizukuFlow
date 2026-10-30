/**
 * 可序列化命令的性质测试。
 *
 * 三方架构（Qt 骨架 / LUME 内核 / WebView 面板）能成立的前提就是一条：
 * **命令可以脱离 JS 进程、被任何宿主原样重放。**
 * 所以这里验证的不是"命令有没效果"，而是：
 *   1. 每条命令都能 JSON 往返且结果一致（丢得了进程，丢不了语义）
 *   2. 提交命令后撤销，工程逐字节回到原样
 *   3. 载荷里不准混进跨不过边界的东西（函数 / undefined / Map / 类实例）
 */

import { describe, expect, it } from 'vitest'
import {
  COMMANDS,
  EditCmd,
  coalesceKeyOf,
  isEditCmd,
  isJsonRoundTrippable,
  resolve,
  serializabilityIssue,
} from '../commands'
import { ProjectStore } from '../store'
import type { Project, Shot } from '../../lib/api'

function shot(over: Partial<Shot> = {}): Shot {
  return {
    id: 'S001', start: 0, end: 4, track: 0, lyric_refs: [],
    layers: { background: [], overlay: [], decor: [], text: [] },
    motion: { entrance: null, hold: null, exit: null, camera: null },
    transition: null, seed: 84213, locked: false,
    ...over,
  } as Shot
}

function project(): Project {
  return {
    format: 'ShizukuFlow', version: 1,
    meta: { title: 'P', artist: '', created: '', modified: '', generator: '' },
    canvas: { width: 1280, height: 720, fps: 30, background: '#000000', safe_area: { enabled: false, margin: 0.1 } },
    media: { audio: null, reference: [] },
    timing: { bpm: 140, offset: 0, beats: [], downbeats: [], source: '' },
    styles: [], lyrics: [],
    shots: [shot(), shot({ id: 'S002', start: 4, end: 8 })],
    assets: [],
    export: { preset: 'mp4', video: { codec: 'h264', crf: 18, preset: 'medium', encoder: 'auto' }, gif: { fps: 30, width: 720, dither: 'sierra' }, apng: { fps: 30, colors: 256 } },
  } as unknown as Project
}

/** 冒号后的 Sample 命令 —— 覆盖全部类型，避免新增命令时漏掉测试。 */
function sampleCommands(): EditCmd[] {
  return [
    { t: 'shot.replace', shotId: 'S001', shot: shot({ seed: 555 }) as never },
    { t: 'canvas.safe', enabled: true },
    { t: 'media.audio', path: 'D:\\x\\a.mp3', duration: 213.4 },
  ]
}

describe('命令可序列化（跨进程的前提）', () => {
  it('每种命令都能 JSON 往返且不失真', () => {
    for (const cmd of sampleCommands()) {
      expect(isJsonRoundTrippable(cmd)).toBe(true)
      // 模拟过一次真正的「进程边界」：字符串化再解析回来
      const revived = JSON.parse(JSON.stringify(cmd)) as EditCmd
      expect(revived).toEqual(cmd)
      expect(isEditCmd(revived)).toBe(true)
    }
  })

  it('往返后的命令与原命令产出完全一致的工程', () => {
    for (const cmd of sampleCommands()) {
      const a = new ProjectStore()
      a.reset(project())
      a.dispatchCmd(cmd)

      const b = new ProjectStore()
      b.reset(project())
      b.dispatchCmd(JSON.parse(JSON.stringify(cmd)) as EditCmd)

      expect(JSON.stringify(b.get())).toBe(JSON.stringify(a.get()))
    }
  })

  it('命令类型清单与实现保持一致（新增命令必须同时补这里）', () => {
    const realized = new Set(sampleCommands().map((c) => c.t))
    realized.add('patches')
    for (const t of COMMANDS) expect(realized.has(t)).toBe(true)
  })

  it('函数混进载荷必须当场被拒 —— 它会静默丢键，不会报 JSON 错误', () => {
    const bad = { t: 'shot.replace', shot: { id: 'S1', glowing: () => 1 } } as never
    // 这是最容易踩的坑：JSON 层完全不报错，往返比较也"通过"，但内容已经丢了
    expect(isJsonRoundTrippable(bad)).toBe(true)
    expect(JSON.parse(JSON.stringify(bad)).shot.glowing).toBeUndefined()
    // 所以真正的守卫是深检查，而不是往返
    expect(serializabilityIssue(bad)).toMatch(/函数无法跨进程序列化/)
    expect(() => (new ProjectStore()).dispatchCmd(bad)).toThrow(/不可跨端序列化/)
  })

  it('NaN / Infinity / Date / Map 同样被拒（都是静默失真的重灾区）', () => {
    expect(serializabilityIssue({ a: NaN })).toMatch(/NaN/)
    expect(serializabilityIssue({ a: Infinity })).toMatch(/Infinity/)
    expect(serializabilityIssue({ a: new Date() })).toMatch(/非纯对象/)
    expect(serializabilityIssue({ a: new Map() })).toMatch(/非纯对象/)
    expect(serializabilityIssue({ a: undefined })).toMatch(/undefined 会在跨端后消失/)
    expect(serializabilityIssue({ a: 1, b: 'x', c: [true, null] })).toBeNull()
  })

  it('正常命令通过深检查', () => {
    for (const cmd of sampleCommands()) {
      expect(serializabilityIssue(cmd)).toBeNull()
    }
  })

  it('结构性校验挡住畸形命令', () => {
    expect(isEditCmd({ t: 'shot.replace', shotId: 'S1' })).toBe(false)
    expect(isEditCmd({ t: '不存在的命令' })).toBe(false)
    expect(isEditCmd(null)).toBe(false)
  })

  it('未知命令会被拒绝而不是静默吞掉', () => {
    expect(() => resolve({ t: 'nope' } as unknown as EditCmd)).toThrow(/未知/)
  })
})

describe('命令 + 状态层协同', () => {
  it('提交命令后可撤销回原样（逐字节）', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)

    store.dispatchCmd({ t: 'shot.replace', shotId: 'S001', shot: shot({ seed: 555 }) as never })
    expect(store.get()!.shots[0].seed).toBe(555)

    store.undo()
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })

  it('canvas.safe 与 media.audio 也能原样撤回', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)

    store.dispatchCmd({ t: 'canvas.safe', enabled: true })
    expect(store.get()!.canvas.safe_area.enabled).toBe(true)
    store.dispatchCmd({ t: 'media.audio', path: 'a.mp3', duration: 12 })
    expect(store.get()!.media.audio?.path).toBe('a.mp3')

    store.undo()
    expect(store.get()!.media.audio).toBeNull()
    store.undo()
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })

  it('对不存在的镜头下发替换命令不会误伤其它数据', () => {
    const store = new ProjectStore()
    const before = project()
    store.reset(before)
    const ok = store.dispatchCmd({ t: 'shot.replace', shotId: 'NOPE', shot: shot() as never })
    expect(ok).toBe(false) // 无变化 → 不进历史
    expect(JSON.stringify(store.get())).toBe(JSON.stringify(before))
  })

  it('同一控件的连续命令会被合并成一条历史', () => {
    const store = new ProjectStore()
    store.reset(project())
    for (let i = 0; i < 8; i++) {
      store.dispatchCmd({ t: 'shot.replace', shotId: 'S001', shot: shot({ seed: i }) as never })
    }
    expect(store.history()).toHaveLength(1)
    expect(coalesceKeyOf({ t: 'shot.replace', shotId: 'S001', shot: shot() as never })).toBe(
      'shot:S001',
    )
  })

  it('不同控件的命令不会被错误合并', () => {
    const store = new ProjectStore()
    store.reset(project())
    store.dispatchCmd({ t: 'canvas.safe', enabled: true })
    store.dispatchCmd({ t: 'shot.replace', shotId: 'S002', shot: shot({ seed: 1 }) as never })
    expect(store.history()).toHaveLength(2)
  })
})
