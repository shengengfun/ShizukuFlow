import {
  Maximize2,
  Pause,
  Play,
  Repeat,
  SkipBack,
  SkipForward,
  StepBack,
  StepForward,
} from 'lucide-react'
import type { Project } from '~/lib/api'
import { timecode } from '~/lib/api'
import { Preview } from './Preview'
import { Panel } from './ui'

/**
 * 监视器 —— AE 的 Composition 面板。
 *
 * 关键：画面按画布宽高比等比缩放居中，任何窗口尺寸下都不裁切、不拉伸。
 * 安全框与网格是 DOM 叠加层（不是画进 canvas），所以开关它们不会触发重渲染，
 * 拖动播放头时也不会因为它们闪烁。
 */
export function Monitor({
  project,
  t,
  accent,
  playing,
  duration,
  onToggle,
  onSeek,
  safe,
  grid,
  zoom,
  onZoom,
  onFit,
  busy,
}: {
  project: Project | null
  t: number
  accent: string
  playing: boolean
  duration: number
  onToggle: () => void
  onSeek: (t: number) => void
  safe: boolean
  grid: boolean
  zoom: number // 0 = 适配
  onZoom: (z: number) => void
  onFit: () => void
  busy?: string | null
}) {
  const fps = project?.canvas.fps || 30
  const ratio = project ? project.canvas.width / project.canvas.height : 16 / 9
  const frame = Math.round(t * fps)
  const totalFrames = Math.max(0, Math.round(duration * fps))
  const resLabel = project ? `${project.canvas.width}×${project.canvas.height}` : '—'

  const step = (n: number) => onSeek(Math.max(0, Math.min(duration, t + n / fps)))

  return (
    <Panel
      className="h-full"
      title={
        <span className="flex items-center gap-1.5">
          <span className="opacity-60">合成</span>
          <span className="text-foreground font-medium">
            {project?.meta.title || '未命名'}
          </span>
        </span>
      }
      scroll={false}
      bodyClass="bg-stage"
      right={
        <div className="flex items-center gap-1.5">
          <span className="tnum text-muted-foreground text-[10px]">{resLabel}</span>
          <select
            className="text-muted-foreground h-[16px] rounded-[2px] border border-transparent bg-transparent px-0.5 text-[10px] hover:border-line"
            value={zoom}
            onChange={(e) => onZoom(Number(e.target.value))}
            title="预览缩放"
          >
            <option value={0}>适配</option>
            <option value={0.25}>25%</option>
            <option value={0.5}>50%</option>
            <option value={1}>100%</option>
          </select>
          <button
            onClick={onFit}
            title="适配窗口 (F)"
            className="text-muted-foreground hover:text-foreground grid h-[17px] w-[18px] place-items-center"
          >
            <Maximize2 size={11} />
          </button>
        </div>
      }
    >
      {/* 画布区：垂直居中、等比 */}
      <div className="relative flex min-h-0 flex-1 items-center justify-center overflow-hidden p-3">
        <div
          className="relative shadow-[0_0_0_1px_rgba(255,255,255,0.07),0_18px_50px_rgba(0,0,0,0.6)]"
          style={{
            aspectRatio: String(ratio),
            height: zoom === 0 ? '100%' : undefined,
            width: zoom === 0 ? 'auto' : `${zoom * 100}%`,
            maxHeight: '100%',
            maxWidth: '100%',
          }}
        >
          <Preview project={project} t={t} accent={accent} />

          {/* 安全框：90% 动作安全 + 80% 字幕安全，术曲 PV 的字必须活在里面 */}
          {safe && (
            <div className="pointer-events-none absolute inset-0">
              <div className="absolute inset-[5%] border border-[color-mix(in_srgb,var(--primary)_55%,transparent)]" />
              <div className="absolute inset-[10%] border border-dashed border-[color-mix(in_srgb,var(--primary)_30%,transparent)]" />
              <div className="absolute top-1/2 left-0 h-px w-full bg-[color-mix(in_srgb,var(--primary)_18%,transparent)]" />
              <div className="absolute top-0 left-1/2 h-full w-px bg-[color-mix(in_srgb,var(--primary)_18%,transparent)]" />
            </div>
          )}
          {/* 三分构图网格 */}
          {grid && (
            <div className="pointer-events-none absolute inset-0">
              {[1, 2].map((i) => (
                <div
                  key={`v${i}`}
                  className="absolute top-0 h-full w-px bg-white/12"
                  style={{ left: `${(i * 100) / 3}%` }}
                />
              ))}
              {[1, 2].map((i) => (
                <div
                  key={`h${i}`}
                  className="absolute left-0 h-px w-full bg-white/12"
                  style={{ top: `${(i * 100) / 3}%` }}
                />
              ))}
            </div>
          )}

          {busy && (
            <div className="absolute inset-0 grid place-items-center bg-black/45 backdrop-blur-[1px]">
              <div className="text-center">
                <div className="border-primary mx-auto h-6 w-6 animate-spin rounded-full border-2 border-t-transparent" />
                <div className="mt-2 text-[11px] text-white/80">{busy}</div>
              </div>
            </div>
          )}
        </div>

        {/* 左下角：当前帧的「规格注记」，AE 里对应的是色彩空间/位深那行小字 */}
        <div className="tnum pointer-events-none absolute bottom-1.5 left-2.5 text-[10px] text-white/35">
          {timecode(t)} · f{frame}
        </div>
        {project?.meta.accent && (
          <div className="pointer-events-none absolute right-2.5 bottom-1.5 flex items-center gap-1 text-[10px] text-white/35">
            <span
              className="inline-block h-2 w-2 rounded-full"
              style={{ background: project.meta.accent }}
            />
            accent
          </div>
        )}
      </div>

      {/* 传输控制条 */}
      <div className="bg-panel-2 border-line flex h-8 flex-none items-center gap-1 border-t px-2">
        <TButton onClick={() => onSeek(0)} title="回到开头 (Home)">
          <SkipBack size={13} />
        </TButton>
        <TButton onClick={() => step(-1)} title="上一帧 (←)">
          <StepBack size={13} />
        </TButton>
        <TButton onClick={onToggle} title="播放/暂停 (空格)" wide active={playing}>
          {playing ? <Pause size={14} /> : <Play size={14} />}
        </TButton>
        <TButton onClick={() => step(1)} title="下一帧 (→)">
          <StepForward size={13} />
        </TButton>
        <TButton onClick={() => onSeek(duration)} title="跳到结尾 (End)">
          <SkipForward size={13} />
        </TButton>

        <div className="tnum border-line ml-1.5 flex items-center gap-2 border-l pl-2.5">
          <span className="text-[12px] font-semibold text-primary">{timecode(t)}</span>
          <span className="text-muted-foreground text-[10.5px]">/ {timecode(duration)}</span>
        </div>

        <div className="flex-1" />

        <span className="tnum text-muted-foreground text-[10px]">
          {frame} / {totalFrames} 帧
        </span>
        <TButton onClick={() => onSeek(0)} title="回到起点" className="ml-1">
          <Repeat size={12} />
        </TButton>
      </div>
    </Panel>
  )
}

function TButton({
  children,
  onClick,
  title,
  wide,
  active,
  className,
}: {
  children: React.ReactNode
  onClick: () => void
  title: string
  wide?: boolean
  active?: boolean
  className?: string
}) {
  return (
    <button
      onClick={onClick}
      title={title}
      className={`grid h-[22px] place-items-center rounded-[3px] transition-colors ${
        wide ? 'w-8' : 'w-[22px]'
      } ${
        active
          ? 'bg-primary text-primary-foreground'
          : 'text-muted-foreground hover:bg-[color-mix(in_srgb,var(--foreground)_12%,transparent)] hover:text-foreground'
      } ${className ?? ''}`}
    >
      {children}
    </button>
  )
}
