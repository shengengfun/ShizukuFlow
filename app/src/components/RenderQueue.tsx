import { useEffect, useRef } from 'react'
import {
  CheckCircle2,
  CircleSlash,
  Clock3,
  Loader2,
  Play,
  Plus,
  Square,
  Trash2,
  X,
  XCircle,
} from 'lucide-react'
import { encoderLabel, humanBytes, scaledSize, type OutputSettings } from '~/lib/output'
import { timecode } from '~/lib/api'

export type JobStatus = 'queued' | 'rendering' | 'done' | 'failed' | 'canceled'

export interface RenderJob {
  id: string
  title: string
  outPath: string
  settings: OutputSettings
  canvas: { width: number; height: number }
  duration: number
  status: JobStatus
  /** 0~1 */
  progress: number
  doneFrames: number
  totalFrames: number
  fps: number
  startedAt?: number
  finishedAt?: number
  encoderUsed?: string
  message?: string
  /** 估算体积（渲染前显示，渲染后以实际文件为准） */
  estimatedSize?: number
}

/**
 * 渲染队列 —— AE 的 Render Queue 窗口。
 *
 * 为什么不是"一个导出按钮 + 一条进度条"：
 * 交付时人不会只渲一次 —— 草稿看完要出正式版，出完要出竖屏版，
 * 队列能把这些排着跑，而人可以去干别的。这是专业软件和"点一下转圈"的分界。
 */
export function RenderQueue({
  jobs,
  running,
  logs,
  onClose,
  onStartAll,
  onCancel,
  onRemove,
  onClearDone,
  onAdd,
}: {
  jobs: RenderJob[]
  running: boolean
  logs: string[]
  onClose: () => void
  onStartAll: () => void
  onCancel: () => void
  onRemove: (id: string) => void
  onClearDone: () => void
  onAdd: () => void
}) {
  const logRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const el = logRef.current
    if (el) el.scrollTop = el.scrollHeight
  }, [logs])

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !running) onClose()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose, running])

  const queued = jobs.filter((j) => j.status === 'queued').length
  const done = jobs.filter((j) => j.status === 'done').length
  const failed = jobs.filter((j) => j.status === 'failed').length
  const active = jobs.find((j) => j.status === 'rendering')

  return (
    <div className="absolute inset-0 z-50 flex items-center justify-center bg-black/55 p-6 backdrop-blur-[2px]">
      <div className="bg-panel border-line-2 flex h-full max-h-[760px] w-full max-w-[1080px] flex-col overflow-hidden rounded-md border shadow-[0_30px_80px_rgba(0,0,0,0.7)]">
        {/* 标题栏 */}
        <div className="bg-panel-2 border-line flex h-8 flex-none items-center gap-2 border-b px-3">
          <span className="text-[12.5px] font-semibold">渲染队列</span>
          <span className="text-muted-foreground text-[11px]">
            {jobs.length} 个任务
            {queued > 0 && ` · ${queued} 待渲`}
            {done > 0 && ` · ${done} 完成`}
            {failed > 0 && <span className="text-destructive"> · {failed} 失败</span>}
          </span>
          <div className="flex-1" />
          <button
            className="text-muted-foreground hover:text-foreground grid h-6 w-6 place-items-center rounded"
            onClick={onClose}
            disabled={running}
            title={running ? '渲染中不可关闭' : '关闭 (Esc)'}
          >
            <X size={14} />
          </button>
        </div>

        {/* 工具条 */}
        <div className="border-line flex h-9 flex-none items-center gap-1.5 border-b px-2">
          <button className="btn" onClick={onAdd} disabled={running}>
            <Plus size={12} /> 添加当前设置
          </button>
          <button className="btn" onClick={onClearDone} disabled={running || done === 0}>
            <Trash2 size={12} /> 清除已完成
          </button>
          <div className="flex-1" />
          {running ? (
            <button className="btn" onClick={onCancel}>
              <Square size={11} /> 取消
            </button>
          ) : (
            <button
              className="btn btn-primary"
              onClick={onStartAll}
              disabled={queued === 0 && active === undefined}
            >
              <Play size={12} /> 渲染
            </button>
          )}
        </div>

        {/* 队列列表 */}
        <div className="min-h-0 flex-1 overflow-auto">
          {jobs.length === 0 && (
            <div className="text-muted-foreground grid h-full place-items-center text-center text-[11.5px]">
              队列为空 · 检查器「输出」页加入队列
            </div>
          )}
          {jobs.map((j, i) => (
            <JobRow key={j.id} job={j} index={i + 1} onRemove={() => onRemove(j.id)} />
          ))}
        </div>

        {/* 底部：总进度 + 日志 */}
        <div className="border-line flex-none border-t">
          <div className="flex h-8 items-center gap-3 px-3 text-[11px]">
            {active ? (
              <>
                <Loader2 size={12} className="text-primary animate-spin" />
                <span className="truncate">{active.title}</span>
                <span className="tnum text-muted-foreground">
                  {active.doneFrames} / {active.totalFrames} 帧
                </span>
                <span className="tnum text-muted-foreground">
                  {active.fps > 0 ? `${active.fps.toFixed(1)} fps` : '预热中'}
                </span>
                <span className="tnum text-muted-foreground">
                  {active.fps > 0 && active.doneFrames > 0
                    ? `剩余 ${timecode(
                        (active.totalFrames - active.doneFrames) / Math.max(0.1, active.fps),
                      )}`
                    : ''}
                </span>
                <div className="flex-1" />
                <span className="tnum text-primary font-semibold">
                  {Math.round(active.progress * 100)}%
                </span>
              </>
            ) : (
              <>
                <Clock3 size={12} className="text-muted-foreground" />
                <span className="text-muted-foreground">
                  {queued > 0 ? `${queued} 个任务待渲染` : '空闲'}
                </span>
                <div className="flex-1" />
              </>
            )}
          </div>
          <div className="h-[3px] w-full bg-[color-mix(in_srgb,var(--foreground)_8%,transparent)]">
            <div
              className="bg-primary h-full transition-[width] duration-200"
              style={{ width: `${(active?.progress ?? 0) * 100}%` }}
            />
          </div>
          <div
            ref={logRef}
            className="tnum bg-rail border-line h-24 overflow-auto border-t px-2.5 py-1.5 text-[10px] leading-[1.5] text-[color-mix(in_srgb,var(--foreground)_62%,transparent)]"
          >
            {logs.length === 0 && <div className="opacity-50">— 日志 —</div>}
            {logs.map((l, i) => (
              <div key={i} className="whitespace-pre-wrap">
                {l}
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  )
}

function JobRow({
  job,
  index,
  onRemove,
}: {
  job: RenderJob
  index: number
  onRemove: () => void
}) {
  const [w, h] = scaledSize(job.canvas, job.settings.scale)
  const span =
    job.settings.range === 'inout'
      ? Math.max(0, job.settings.outPoint - job.settings.inPoint)
      : job.duration

  const icon =
    job.status === 'done' ? (
      <CheckCircle2 size={14} className="text-success" />
    ) : job.status === 'failed' ? (
      <XCircle size={14} className="text-destructive" />
    ) : job.status === 'rendering' ? (
      <Loader2 size={14} className="text-primary animate-spin" />
    ) : job.status === 'canceled' ? (
      <CircleSlash size={14} className="text-muted-foreground" />
    ) : (
      <Clock3 size={14} className="text-muted-foreground" />
    )

  const statusText =
    job.status === 'queued'
      ? '等待'
      : job.status === 'rendering'
        ? `${Math.round(job.progress * 100)}%`
        : job.status === 'done'
          ? '完成'
          : job.status === 'failed'
            ? '失败'
            : '已取消'

  return (
    <div
      className="border-line hover:bg-[color-mix(in_srgb,var(--foreground)_4%,transparent)] grid grid-cols-[24px_1fr_150px_84px_24px] items-center gap-2 border-b px-2.5 py-2"
      style={{ minHeight: 52 }}
    >
      <div className="tnum text-muted-foreground flex flex-col items-center justify-center gap-0.5 text-[9.5px]">
        <span>{String(index).padStart(2, '0')}</span>
        {icon}
      </div>

      <div className="min-w-0">
        <div className="flex items-center gap-2">
          <span className="truncate text-[12px] font-medium">{job.title}.mp4</span>
          <span
            className={`shrink-0 rounded-[2px] px-1 py-px text-[9.5px] ${
              job.status === 'done'
                ? 'bg-[color-mix(in_srgb,var(--success)_22%,transparent)] text-success'
                : job.status === 'failed'
                  ? 'bg-[color-mix(in_srgb,var(--destructive)_22%,transparent)] text-destructive'
                  : 'bg-panel-2 text-muted-foreground'
            }`}
          >
            {statusText}
          </span>
          {job.encoderUsed && (
            <span className="text-muted-foreground shrink-0 text-[9.5px]">{job.encoderUsed}</span>
          )}
        </div>
        <div className="tnum text-muted-foreground mt-0.5 truncate text-[10px]">
          {w}×{h} · {job.settings.fps}fps · CRF {job.settings.crf} ·{' '}
          {encoderLabel(job.settings.encoder)} · {timecode(span)}
          {job.settings.range === 'inout' && ` · 入出点 ${timecode(job.settings.inPoint)}`}
        </div>
        <div className="text-muted-foreground mt-0.5 truncate text-[10px]" title={job.outPath}>
          {job.outPath}
          {job.message && <span className="text-destructive"> · {job.message}</span>}
        </div>
      </div>

      <div className="min-w-0">
        <div className="h-[5px] overflow-hidden rounded-full bg-[color-mix(in_srgb,var(--foreground)_10%,transparent)]">
          <div
            className={`h-full rounded-full ${
              job.status === 'failed'
                ? 'bg-destructive'
                : job.status === 'done'
                  ? 'bg-success'
                  : 'bg-primary'
            }`}
            style={{ width: `${Math.round(job.progress * 100)}%` }}
          />
        </div>
        <div className="tnum text-muted-foreground mt-1 flex justify-between text-[9.5px]">
          <span>
            {job.doneFrames}/{job.totalFrames || '—'}
          </span>
          <span>{job.fps > 0 ? `${job.fps.toFixed(1)} fps` : ''}</span>
        </div>
      </div>

      <div className="tnum text-muted-foreground text-right text-[10px]">
        {job.status === 'done' && job.startedAt && job.finishedAt
          ? `${((job.finishedAt - job.startedAt) / 1000).toFixed(1)}s`
          : job.estimatedSize
            ? `≈${humanBytes(job.estimatedSize)}`
            : ''}
      </div>

      <button
        className="text-muted-foreground hover:text-destructive grid h-6 w-6 place-items-center rounded"
        title="移除"
        onClick={onRemove}
        disabled={job.status === 'rendering'}
      >
        <X size={13} />
      </button>
    </div>
  )
}
