import { useMemo, useState } from 'react'
import type { ReactNode } from 'react'
import { FileAudio, Film, Layers, Type } from 'lucide-react'
import type { Project, Shot } from '~/lib/api'
import { timecode } from '~/lib/api'
import { Panel } from './ui'

/** 构图模式 → 色标。左栏那根 2px 色条是"一眼看出这一屏是什么戏"的唯一线索。 */
const MODE_COLOR: Record<string, string> = {
  giant: 'hsl(348 80% 62%)',
  quiet: 'hsl(210 12% 62%)',
  corner: 'hsl(42 90% 60%)',
  vertical: 'hsl(150 60% 52%)',
  center: 'hsl(193 90% 58%)',
}

function shotMode(s: Shot): string {
  const p = s.motion.hold?.params as Record<string, unknown> | undefined
  const m = p?.mode
  return typeof m === 'string' ? m : 'center'
}

type Tab = 'shots' | 'lyrics' | 'asset'

/**
 * 项目面板 —— 左侧导航。
 *
 * 镜头表按 `track` 分组（轨道号越大越上，与时间轴一致），
 * 这样左栏和时间轴的阅读顺序是同一个，眼睛不用来回换方向。
 */
export function ProjectPanel({
  project,
  duration,
  selectedShot,
  onSelectShot,
  selectedLyric,
  onSelectLyric,
  audioName,
  onOpenAudio,
  /** 左侧 dock 的共用切换器（部件 / 工程），由 App 注入，两个面板共用同一个 */
  dock,
}: {
  project: Project | null
  duration: number
  selectedShot: string | null
  onSelectShot: (id: string, start: number) => void
  selectedLyric: string | null
  onSelectLyric: (id: string, start: number) => void
  audioName: string | null
  onOpenAudio: () => void
  dock?: ReactNode
}) {
  const [tab, setTab] = useState<Tab>('shots')

  const tracks = useMemo(() => {
    if (!project) return []
    const m = new Map<number, Shot[]>()
    for (const s of project.shots) {
      const arr = m.get(s.track) ?? []
      arr.push(s)
      m.set(s.track, arr)
    }
    return [...m.entries()]
      .sort((a, b) => b[0] - a[0])
      .map(([track, shots]) => ({ track, shots: shots.sort((a, b) => a.start - b.start) }))
  }, [project])

  return (
    <Panel
      title={dock ?? '项目'}
      scroll={false}
      right={
        <div className="flex items-center gap-0.5">
          <TabBtn on={tab === 'shots'} onClick={() => setTab('shots')} title="镜头">
            <Film size={12} />
          </TabBtn>
          <TabBtn on={tab === 'lyrics'} onClick={() => setTab('lyrics')} title="歌词">
            <Type size={12} />
          </TabBtn>
          <TabBtn on={tab === 'asset'} onClick={() => setTab('asset')} title="素材">
            <Layers size={12} />
          </TabBtn>
        </div>
      }
    >
      {/* 工程摘要：四行，尽量紧凑 —— 面板空间要留给列表 */}
      <div className="border-line flex-none border-b px-2.5 py-2">
        {project ? (
          <>
            <div className="truncate text-[12.5px] font-semibold" title={project.meta.title}>
              {project.meta.title || '未命名工程'}
            </div>
            <div className="text-muted-foreground mt-0.5 truncate text-[11px]">
              {project.meta.artist || '未知艺术家'}
            </div>
            <div className="tnum text-muted-foreground mt-1.5 grid grid-cols-2 gap-x-2 gap-y-0.5 text-[10.5px]">
              <span>
                {project.canvas.width}×{project.canvas.height}
              </span>
              <span>{project.canvas.fps} fps</span>
              <span>{timecode(duration)}</span>
              <span>{project.timing.bpm ? `${project.timing.bpm.toFixed(1)} BPM` : '—'}</span>
            </div>
          </>
        ) : (
          <div className="text-muted-foreground text-[11px]">加载中…</div>
        )}
      </div>

      <div className="pnl-body">
        {tab === 'shots' && (
          <>
            {tracks.map((g) => (
              <div key={g.track}>
                <div className="text-muted-foreground bg-[color-mix(in_srgb,var(--foreground)_4%,transparent)] flex h-[19px] items-center px-2.5 text-[10px] font-semibold tracking-wider">
                  V{g.track + 1} · {g.shots.length}
                </div>
                {g.shots.map((s) => {
                  const mode = shotMode(s)
                  return (
                    <div
                      key={s.id}
                      className="row"
                      data-on={selectedShot === s.id}
                      onClick={() => onSelectShot(s.id, s.start)}
                    >
                      <span
                        className="h-[11px] w-[2px] flex-none rounded-full"
                        style={{ background: MODE_COLOR[mode] ?? 'hsl(210 10% 55%)' }}
                      />
                      <span className="tnum text-muted-foreground w-[52px] flex-none text-[10.5px]">
                        {timecode(s.start)}
                      </span>
                      <span className="flex-1 truncate text-[11.5px]">{mode}</span>
                      <span className="tnum text-muted-foreground text-[10px]">
                        {(s.end - s.start).toFixed(2)}s
                      </span>
                    </div>
                  )
                })}
              </div>
            ))}
            {tracks.length === 0 && <Empty text="还没有镜头" />}
          </>
        )}

        {tab === 'lyrics' && (
          <>
            {project?.lyrics.map((l) => (
              <div
                key={l.id}
                className="row"
                data-on={selectedLyric === l.id}
                onClick={() => onSelectLyric(l.id, l.start)}
              >
                <span className="tnum text-muted-foreground w-[52px] flex-none text-[10.5px]">
                  {timecode(l.start)}
                </span>
                <span className="flex-1 truncate text-[11.5px]">{l.text}</span>
                {l.syllables.length > 1 && (
                  <span className="tnum text-muted-foreground text-[9.5px]">
                    {l.syllables.length}
                  </span>
                )}
              </div>
            ))}
            {!project?.lyrics.length && <Empty text="还没有歌词" />}
          </>
        )}

        {tab === 'asset' && (
          <div className="p-2">
            <button
              onClick={onOpenAudio}
              className="border-line hover:bg-panel-2 flex w-full items-center gap-2 rounded border border-dashed px-2 py-2 text-left"
            >
              <FileAudio size={14} className="text-primary shrink-0" />
              <span className="min-w-0 flex-1">
                <span className="block truncate text-[11.5px]">{audioName ?? '未加载音频'}</span>
                <span className="text-muted-foreground block text-[10px]">
                  {audioName ? '点击更换音轨' : '点击导入音轨（波形 / 导出音轨）'}
                </span>
              </span>
            </button>
            <div className="text-muted-foreground mt-3 px-1 text-[10.5px] leading-relaxed">
              工程引用的音频以相对路径存于 <span className="tnum">.sf</span> 内，
              换机打开时按工程目录解析，不会因为盘符变化丢链接。
            </div>
          </div>
        )}
      </div>
    </Panel>
  )
}

function TabBtn({
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
    <button
      title={title}
      onClick={onClick}
      className={`grid h-[17px] w-[20px] place-items-center rounded-[2px] transition-colors ${
        on
          ? 'bg-primary text-primary-foreground'
          : 'text-muted-foreground hover:bg-[color-mix(in_srgb,var(--foreground)_12%,transparent)]'
      }`}
    >
      {children}
    </button>
  )
}

function Empty({ text }: { text: string }) {
  return <div className="text-muted-foreground px-3 py-4 text-center text-[11px]">{text}</div>
}
