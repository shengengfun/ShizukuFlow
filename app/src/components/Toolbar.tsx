import type { Project } from '~/lib/api'

/**
 * 工具栏。
 *
 * 这里每一个按钮都必须真的能干活 —— 上一个版本有过一批按下去什么也没发生的
 * 「演示按钮」，那是最容易被一眼看穿的地方。所以本文件只放已接线的动作。
 * 撤销/重做已由 `src/state/store` 的命令历史提供，因此不再是空按钮。
 */
export function Toolbar({
  project,
  projectPath,
  workspace,
  onWorkspace,
  onNew,
  onOpen,
  onSave,
  onImportAudio,
  onDemo,
  onUndo,
  onRedo,
  undoLabel,
  redoLabel,
  dirty,
  theme,
  onTheme,
  onQueue,
  onRender,
  running,
}: {
  project: Project | null
  projectPath: string | null
  workspace: 'edit' | 'color' | 'export'
  onWorkspace: (w: 'edit' | 'color' | 'export') => void
  onNew: () => void
  onOpen: () => void
  onSave: () => void
  onImportAudio: () => void
  onDemo: () => void
  onUndo: () => void
  onRedo: () => void
  undoLabel: string | null
  redoLabel: string | null
  /** 有未保存改动 —— 专业软件里表现为标题旁的一颗星。 */
  dirty: boolean
  theme: 'light' | 'dark'
  onTheme: (t: 'light' | 'dark') => void
  onQueue: () => void
  onRender: () => void
  running: boolean
}) {
  const name = projectPath ? projectPath.split(/[\\/]/).pop()! : '未保存'
  const res = project ? `${project.canvas.width}×${project.canvas.height}` : '—'
  const fps = project ? project.canvas.fps : 0

  return (
    <div className="bg-panel-2 border-line flex h-[30px] flex-none items-center gap-1 overflow-hidden border-b px-2">
      <button className="btn h-[20px] px-2" onClick={onOpen} title="打开工程 (Ctrl+O)">
        打开
      </button>
      <button className="btn h-[20px] px-2" onClick={onSave} title="保存 (Ctrl+S)">
        保存
      </button>
      <button className="btn h-[20px] px-2" onClick={onNew} title="新建工程 (Ctrl+N)">
        新建
      </button>
      <button className="btn h-[20px] px-2" onClick={onImportAudio} title="导入音轨 (Ctrl+I)">
        音轨
      </button>
      <button className="btn h-[20px] px-2" onClick={onDemo} title="载入内置示例工程">
        示例
      </button>

      <div className="tb-sep" />

      <button
        className="btn h-[20px] px-2"
        onClick={onUndo}
        disabled={!undoLabel}
        title={undoLabel ? `撤销：${undoLabel} (Ctrl+Z)` : '没有可撤销的操作'}
      >
        撤销
      </button>
      <button
        className="btn h-[20px] px-2"
        onClick={onRedo}
        disabled={!redoLabel}
        title={redoLabel ? `重做：${redoLabel} (Ctrl+Shift+Z)` : '没有可重做的操作'}
      >
        重做
      </button>

      <div className="tb-sep" />

      <span className="tb-label">叠层</span>
      <div className="seg h-[20px]">
        <WorkspaceBtn on={workspace === 'edit'} onClick={() => onWorkspace('edit')} title="剪辑：部件库 + 合成 + 检查器 + 时间轴">
          剪辑
        </WorkspaceBtn>
        <WorkspaceBtn on={workspace === 'color'} onClick={() => onWorkspace('color')} title="调色：收起部件库，画面区域最大化">
          调色
        </WorkspaceBtn>
        <WorkspaceBtn on={workspace === 'export'} onClick={() => onWorkspace('export')} title="导出：收起部件库并打开渲染队列">
          导出
        </WorkspaceBtn>
      </div>

      <div className="flex-1" />

      <span className="chip tnum">
        工程 <b>{name}</b>
        {dirty && '*'}
        <span
          className="h-1.5 w-1.5 rounded-full"
          style={{ background: dirty ? 'var(--warn, var(--accent))' : 'var(--success)' }}
          title={dirty ? '有未保存的改动' : '已保存'}
        />
      </span>
      <span className="chip tnum">
        <b>{res}</b> · {fps}fps
      </span>

      <div className="tb-sep" />

      <div className="seg h-[20px]">
        <WorkspaceBtn on={theme === 'dark'} onClick={() => onTheme('dark')} title="深色">
          深
        </WorkspaceBtn>
        <WorkspaceBtn on={theme === 'light'} onClick={() => onTheme('light')} title="浅色">
          浅
        </WorkspaceBtn>
      </div>

      <div className="tb-sep" />

      <button className="btn h-[20px] px-2" onClick={onQueue} title="渲染队列 (Ctrl+M)">
        队列
      </button>
      <button
        className="btn btn-primary h-[20px] px-2.5"
        onClick={onRender}
        disabled={running}
        title="加入队列并渲染 (Ctrl+Enter)"
      >
        {running ? '渲染中' : '渲染'}
      </button>
    </div>
  )
}

function WorkspaceBtn({
  children,
  on,
  onClick,
  title,
}: {
  children: React.ReactNode
  on: boolean
  onClick: () => void
  title: string
}) {
  return (
    <button data-on={on} onClick={onClick} title={title}>
      {children}
    </button>
  )
}
