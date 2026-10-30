/**
 * 面板会话协议 —— 「Qt 做骨架、内核做心脏、WebView 做衣服」这条路上的通信层。
 *
 * 分工：
 *   - Qt 宿主持有 ProjectStore（权威工程）
 *   - WebView 面板（媒体库 / 导出设置 / 脚本与表达式编辑器 / 插件模板）
 *     拿到的是**会话**，不是工程本身：只能读、只能提交 EditCmd
 *
 * 关键点：这里的消息全部是**纯数据**，所以 `LocalBus`（今天：同进程）
 * 可以直接换成 Tauri IPC / Qt 的 C-ABI 通道，而面板代码一行都不用改 ——
 * 这就是把 WebView 当「衣服」而不是当「大脑」的意思。
 */

import type { Project } from '../lib/api'
import { PROTOCOL_VERSION, resolve, type EditCmd } from './commands'
import { ProjectStore, deepFreeze } from './store'

// ─────────────────────────── 线上消息 ───────────────────────────

/** 面板 → 宿主：「我要改这个」。 */
export interface ToHost {
  dir: 'to-host'
  v: number
  cmd: EditCmd
  coalesceMs?: number
}

/** 宿主 → 面板：「现在长这样」。面板据此重绘，不自己推算。 */
export interface ToView {
  dir: 'to-view'
  v: number
  version: number
  project: Project | null
  undoLabel: string | null
  redoLabel: string | null
  dirty: boolean
}

export type Wire = ToHost | ToView

export interface Transport {
  post: (msg: Wire) => void
  subscribe: (fn: (msg: Wire) => void) => () => void
}

/** 同进程总线。将来换成 IPC/Qt 通道时，只需实现同一个 Transport 接口。 */
export class LocalBus implements Transport {
  private fns = new Set<(m: Wire) => void>()

  post(msg: Wire) {
    // 复制一份再广播：模拟真实通道必然发生的序列化 ——
    // 任何「靠对象引用就能偷改宿主状态」的写法在这里会被立刻打回。
    const wire = JSON.parse(JSON.stringify(msg)) as Wire
    for (const fn of [...this.fns]) fn(wire)
  }

  subscribe(fn: (m: Wire) => void) {
    this.fns.add(fn)
    return () => this.fns.delete(fn)
  }
}

// ─────────────────────────── 宿主侧 ───────────────────────────

/** 把 Store 挂到总线上：收命令、广播状态。Qt 宿主将来就调这一个函数。 */
export function attachHost(store: ProjectStore, bus: Transport): () => void {
  const off = bus.subscribe((msg) => {
    if (msg.dir !== 'to-host') return
    if (msg.v !== PROTOCOL_VERSION) return
    store.dispatchCmd(msg.cmd, { coalesceMs: msg.coalesceMs })
  })
  const offState = store.subscribe((s) => {
    bus.post({
      dir: 'to-view',
      v: PROTOCOL_VERSION,
      version: s.version,
      project: s.project,
      undoLabel: s.undoLabel,
      redoLabel: s.redoLabel,
      dirty: s.dirty,
    })
  })
  return () => {
    off()
    offState()
  }
}

// ─────────────────────────── 面板侧 ───────────────────────────

export interface PanelView {
  project: Project | null
  undoLabel: string | null
  redoLabel: string | null
  dirty: boolean
  version: number
}

/**
 * WebView 面板拿到的东西。
 * 刻意**不暴露** store：面板既不能绕过命令改工程，也不必知道权威状态住在哪里。
 */
export interface PanelSession {
  read: () => PanelView
  subscribe: (fn: (v: PanelView) => void) => () => void
  /** 提交编辑（纯数据命令，能过进程边界）。 */
  submit: (cmd: EditCmd, coalesceMs?: number) => void
  close: () => void
}

export function createPanel(bus: Transport): PanelSession {
  let view: PanelView = {
    project: null,
    undoLabel: null,
    redoLabel: null,
    dirty: false,
    version: -1,
  }
  const fns = new Set<(v: PanelView) => void>()
  const off = bus.subscribe((msg) => {
    if (msg.dir !== 'to-view') return
    // 重新冻结：工程一旦经过序列化，冻结标记是不跟着过线的。
    // 若不在这里补上，面板拿到的是可写副本 —— 改写对本进程无效、对本视图有效，
    // 于是面板显示的与实际提交的出现偏差，而且极难排查。
    // 冻结后，「本地总线」与「将来的 Qt IPC」两种情形不变量完全一致。
    view = {
      project: msg.project ? deepFreeze(msg.project) : null,
      undoLabel: msg.undoLabel,
      redoLabel: msg.redoLabel,
      dirty: msg.dirty,
      version: msg.version,
    }
    for (const fn of [...fns]) fn(view)
  })
  return {
    read: () => view,
    subscribe: (fn) => {
      fns.add(fn)
      return () => fns.delete(fn)
    },
    submit: (cmd, coalesceMs) => {
      bus.post({ dir: 'to-host', v: PROTOCOL_VERSION, cmd, coalesceMs })
    },
    close: off,
  }
}

/** 面板侧也能在本地重放同一条命令做乐观 UI —— 但要走同一个解析器，不许各写各的。 */
export const previewResolve = resolve
