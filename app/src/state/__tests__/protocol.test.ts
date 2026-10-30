/**
 * 面板会话协议测试 —— 验证「WebView 做衣服」这条能真的穿上去。
 *
 * 模拟真实的三方场景：一个宿主持权威工程，两个独立面板（比如媒体库 + 导出设置）
 * 挂在同一条只传数据的线上。要证明的是：
 *   1. 面板收不到任何可写句柄（想偷改工程会被拒绝）
 *   2. 任一面板的改动，所有面板会收敛到一致
 *   3. 线上消息全是纯数据 → 换成 Qt 的 IPC 通道时不用改面板代码
 */

import { describe, expect, it, vi } from 'vitest'
import { EditCmd, serializabilityIssue } from '../commands'
import { LocalBus, PanelSession, attachHost, createPanel } from '../protocol'
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

function setup(): { store: ProjectStore; bus: LocalBus; a: PanelSession; b: PanelSession } {
  const store = new ProjectStore()
  const bus = new LocalBus()
  attachHost(store, bus)
  const a = createPanel(bus)
  const b = createPanel(bus)
  store.reset(project())
  return { store, bus, a, b }
}

describe('面板会话（Qt 宿主 ⇄ WebView 面板）', () => {
  it('面板提交命令后，宿主工程确实变了', () => {
    const { store, a } = setup()
    a.submit({ t: 'canvas.safe', enabled: true })
    expect(store.get()!.canvas.safe_area.enabled).toBe(true)
  })

  it('两个独立面板会收敛到同一份状态', () => {
    const { a, b } = setup()
    const seen = vi.fn()
    b.subscribe(seen)

    a.submit({ t: 'shot.replace', shotId: 'S002', shot: shot({ seed: 777 }) as never })

    expect(a.read().project?.shots[1].seed).toBe(777)
    expect(b.read().project?.shots[1].seed).toBe(777)
    expect(JSON.stringify(a.read().project)).toBe(JSON.stringify(b.read().project))
    expect(seen).toHaveBeenCalled()
  })

  it('面板拿不到可写句柄：想偷改工程会被拒绝', () => {
    const { store, a } = setup()
    const p = a.read().project!
    expect(() => {
      ;(p.shots[0] as Shot).seed = 999999
    }).toThrow()
    expect(store.get()!.shots[0].seed).toBe(84213)
  })

  it('面板只能提交命令，拿不到 store', () => {
    const { a } = setup()
    // 会话接口刻意最小化：没有 dispatch 闭包入口，只有纯数据 submit
    expect(Object.keys(a).sort()).toEqual(['close', 'read', 'submit', 'subscribe'])
  })

  it('总线上的每条消息都是纯数据（换成真实 IPC 时不需要改面板）', () => {
    const { bus } = setup()
    bus.subscribe((msg) => {
      expect(serializabilityIssue(msg)).toBeNull()
    })
    bus.post({ dir: 'to-host', v: 1, cmd: { t: 'canvas.safe', enabled: true } })
  })

  it('版本不同的消息会被忽略，不会带崩宿主', () => {
    const { store, bus } = setup()
    const before = JSON.stringify(store.get())
    bus.post({ dir: 'to-host', v: 999, cmd: { t: 'canvas.safe', enabled: true } })
    expect(JSON.stringify(store.get())).toBe(before)
  })

  it('撤销标签能跨线传回面板（供面板禁用「撤销」按钮）', () => {
    const { store, a } = setup()
    a.submit({ t: 'canvas.safe', enabled: true })
    expect(a.read().undoLabel).toBe('画面安全框')
    store.undo()
    expect(a.read().redoLabel).toBe('画面安全框')
  })

  it('面板关闭后退订，不再收到广播', () => {
    const { a, b } = setup()
    const fn = vi.fn()
    b.subscribe(fn)
    b.close()
    a.submit({ t: 'canvas.safe', enabled: true })
    expect(fn).not.toHaveBeenCalled()
  })

  it('线上往返后命令语义不变（跨进程不失真）', () => {
    const { store, bus } = setup()
    const cmd: EditCmd = { t: 'shot.replace', shotId: 'S001', shot: shot({ seed: 4321 }) as never }
    bus.post({ dir: 'to-host', v: 1, cmd })
    expect(store.get()!.shots[0].seed).toBe(4321)
  })
})
