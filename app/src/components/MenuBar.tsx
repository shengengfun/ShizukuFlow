import { useEffect, useRef, useState } from 'react'
import { getCurrentWindow } from '@tauri-apps/api/window'
import { ChevronDown, Minus, Square, X } from 'lucide-react'

const appWindow = getCurrentWindow()

export interface MenuActions {
  newProject: () => void
  openProject: () => void
  saveProject: () => void
  saveProjectAs: () => void
  importAudio: () => void
  loadDemo: () => void
  openRender: () => void
  quickRender: () => void
  toggleTheme: () => void
  toggleSafe: () => void
  toggleGrid: () => void
  fitView: () => void
  about: () => void
}

type Item =
  | { kind: 'sep' }
  | {
      kind: 'item'
      label: string
      kbd?: string
      run?: () => void
      disabled?: boolean
      checked?: boolean
    }

/**
 * 菜单栏（自绘）。
 *
 * 为什么不用 Tauri 原生菜单：原生菜单在 Windows 上是浅色系统控件，
 * 和深色工作区是两套语汇，AE/Vegas 也全是自绘。代价是要自己处理
 * "打开着时滑过另一项要切换"这种细节 —— 但那是老手感的来源。
 */
export function MenuBar({
  actions,
  dark,
  safe,
  grid,
  title,
}: {
  actions: MenuActions
  dark: boolean
  safe: boolean
  grid: boolean
  title: string
}) {
  const [open, setOpen] = useState<string | null>(null)
  const rootRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) setOpen(null)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(null)
    }
    window.addEventListener('mousedown', onDown)
    window.addEventListener('keydown', onKey)
    return () => {
      window.removeEventListener('mousedown', onDown)
      window.removeEventListener('keydown', onKey)
    }
  }, [open])

  const menus: { id: string; label: string; items: Item[] }[] = [
    {
      id: 'file',
      label: '文件',
      items: [
        { kind: 'item', label: '新建工程', kbd: 'Ctrl+N', run: actions.newProject },
        { kind: 'item', label: '打开工程…', kbd: 'Ctrl+O', run: actions.openProject },
        { kind: 'item', label: '保存', kbd: 'Ctrl+S', run: actions.saveProject },
        { kind: 'item', label: '另存为…', kbd: 'Ctrl+Shift+S', run: actions.saveProjectAs },
        { kind: 'sep' },
        { kind: 'item', label: '导入音频…', kbd: 'Ctrl+I', run: actions.importAudio },
        { kind: 'sep' },
        { kind: 'item', label: '退出', run: () => void appWindow.close() },
      ],
    },
    {
      id: 'edit',
      label: '编辑',
      items: [
        { kind: 'item', label: '撤销', kbd: 'Ctrl+Z', disabled: true },
        { kind: 'item', label: '重做', kbd: 'Ctrl+Shift+Z', disabled: true },
      ],
    },
    {
      id: 'view',
      label: '视图',
      items: [
        { kind: 'item', label: dark ? '切换到浅色主题' : '切换到深色主题', run: actions.toggleTheme },
        { kind: 'sep' },
        { kind: 'item', label: '字幕安全框', run: actions.toggleSafe, checked: safe },
        { kind: 'item', label: '构图网格', run: actions.toggleGrid, checked: grid },
        { kind: 'item', label: '预览适配窗口', kbd: 'F', run: actions.fitView },
      ],
    },
    {
      id: 'comp',
      label: '合成',
      items: [
        { kind: 'item', label: '载入示例工程', run: actions.loadDemo },
        { kind: 'item', label: '重新载入当前工程', run: actions.openProject },
      ],
    },
    {
      id: 'render',
      label: '渲染',
      items: [
        { kind: 'item', label: '渲染队列…', kbd: 'Ctrl+M', run: actions.openRender },
        { kind: 'item', label: '按当前设置直接渲染', kbd: 'Ctrl+Enter', run: actions.quickRender },
      ],
    },
    {
      id: 'help',
      label: '帮助',
      items: [{ kind: 'item', label: '关于 ShizukuFlow', run: actions.about }],
    },
  ]

  return (
    <div
      ref={rootRef}
      className="bg-rail border-line relative flex h-8 flex-none items-stretch border-b pl-2 select-none"
    >
      <div className="flex items-center gap-2 pr-3">
        {/* 一枚极简的菱形标记：LUME 的 "L" 收在字里行间 */}
        <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden>
          <path d="M7 1 L13 7 L7 13 L1 7 Z" fill="var(--primary)" opacity="0.9" />
          <path d="M7 4.4 L9.6 7 L7 9.6 L4.4 7 Z" fill="var(--rail)" />
        </svg>
        <span className="text-[12.5px] font-semibold tracking-wide">ShizukuFlow</span>
      </div>

      <nav className="flex items-stretch">
        {menus.map((m) => (
          <div key={m.id} className="relative flex items-stretch">
            <button
              className="menuitem"
              data-open={open === m.id}
              onMouseDown={(e) => {
                e.preventDefault()
                setOpen(open === m.id ? null : m.id)
              }}
              onMouseEnter={() => open && setOpen(m.id)}
            >
              {m.label}
            </button>
            {open === m.id && (
              <div className="dropdown">
                {m.items.map((it, i) =>
                  it.kind === 'sep' ? (
                    <div key={`s${i}`} className="sep" />
                  ) : (
                    <button
                      key={it.label}
                      disabled={it.disabled}
                      onClick={() => {
                        setOpen(null)
                        it.run?.()
                      }}
                    >
                      <span className="w-3 shrink-0 text-[10px]">{it.checked ? '✓' : ''}</span>
                      <span>{it.label}</span>
                      {it.kbd && <span className="kbd">{it.kbd}</span>}
                    </button>
                  ),
                )}
              </div>
            )}
          </div>
        ))}
      </nav>

      {/* 空白区域兼作拖动区：窗口无边框，只有这里能拖 */}
      <div data-tauri-drag-region className="flex-1" />

      <div className="flex items-center gap-2 pr-1">
        <span className="text-muted-foreground max-w-[280px] truncate text-[11px]">{title}</span>
      </div>

      <div className="border-line ml-1 flex items-center gap-px border-l pl-1">
        <WinBtn onClick={() => void appWindow.minimize()} label="最小化">
          <Minus size={13} />
        </WinBtn>
        <WinBtn onClick={() => void appWindow.toggleMaximize()} label="最大化">
          <Square size={10} />
        </WinBtn>
        <WinBtn onClick={() => void appWindow.close()} label="关闭" danger>
          <X size={14} />
        </WinBtn>
      </div>
    </div>
  )
}

function WinBtn({
  children,
  onClick,
  label,
  danger,
}: {
  children: React.ReactNode
  onClick: () => void
  label: string
  danger?: boolean
}) {
  return (
    <button
      onClick={onClick}
      title={label}
      className={`grid h-[26px] w-[34px] place-items-center rounded-[3px] text-muted-foreground transition-colors hover:text-foreground ${
        danger ? 'hover:bg-destructive hover:text-white' : 'hover:bg-[color-mix(in_srgb,var(--foreground)_12%,transparent)]'
      }`}
    >
      {children}
    </button>
  )
}

/** 未被使用但保留：将来多级菜单（如"渲染 > 预设"）需要它 */
export function Caret() {
  return <ChevronDown size={10} className="opacity-60" />
}
