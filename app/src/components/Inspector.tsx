import { useEffect, useMemo, useState } from 'react'
import { FolderOpen } from 'lucide-react'
import type { HwCaps, Project } from '~/lib/api'
import { timecode } from '~/lib/api'
import { ACCENT_PRESETS } from '~/lib/appearance'
import {
  QUALITY_PRESETS,
  estimateBytes,
  humanBytes,
  qualityLabel,
  scaledSize,
  type Encoder,
  type OutputSettings,
} from '~/lib/output'
import { Field, NumInput, Panel, Section, Segmented, Select, Slider, TextInput } from './ui'

type Tab = 'comp' | 'selection' | 'output'

/**
 * 检查器 —— AE 的 Effect Controls + Output Module 合在一个面板里。
 *
 * 三段式（合成 / 选择 / 输出）而不是一次铺满所有字段：
 * 编辑时看参数，交付时看输出，两件事不在同一时刻发生。
 */
export function Inspector({
  project,
  duration,
  selectedShot,
  selectedLyric,
  output,
  onOutput,
  hw,
  onBrowse,
  onAddToQueue,
  accentId,
  onAccent,
  onSafe,
}: {
  project: Project | null
  duration: number
  selectedShot: string | null
  selectedLyric: string | null
  output: OutputSettings
  onOutput: (patch: Partial<OutputSettings>) => void
  hw: HwCaps | null
  onBrowse: () => void
  onAddToQueue: () => void
  accentId: string
  onAccent: (id: string) => void
  onSafe: (v: boolean) => void
}) {
  const [tab, setTab] = useState<Tab>('comp')
  // 选中镜头时自动跳过去：点了左边/时间轴的东西，右边必须有反应
  const sel = selectedShot ?? selectedLyric
  useEffect(() => {
    if (!sel) return
    setTab((cur) => (cur === 'comp' ? 'selection' : cur))
  }, [sel])

  const shot = project?.shots.find((s) => s.id === selectedShot) ?? null
  const lyric = project?.lyrics.find((l) => l.id === selectedLyric) ?? null

  const [ow, oh] = project ? scaledSize(project.canvas, output.scale) : [0, 0]
  const rangeSec = useMemo(() => {
    if (!project) return 0
    if (output.range === 'inout') return Math.max(0, output.outPoint - output.inPoint)
    return duration
  }, [project, output.range, output.inPoint, output.outPoint, duration])

  const est = project
    ? estimateBytes(ow, oh, output.fps, output.crf, rangeSec, output.audioBitrate)
    : 0

  return (
    <Panel
      title="检查器"
      scroll={false}
      right={
        <div className="seg" style={{ height: 17 }}>
          {(
            [
              ['comp', '合成'],
              ['selection', '选择'],
              ['output', '输出'],
            ] as const
          ).map(([k, label]) => (
            <button key={k} data-on={tab === k} onClick={() => setTab(k)} style={{ height: 16 }}>
              {label}
            </button>
          ))}
        </div>
      }
    >
      <div className="pnl-body">
        {tab === 'comp' && (
          <>
            <Section title="合成">
              <Field label="名称">
                <div className="truncate text-[12px]">{project?.meta.title || '—'}</div>
              </Field>
              <Field label="画幅">
                <div className="tnum text-[12px]">
                  {project ? `${project.canvas.width} × ${project.canvas.height}` : '—'}
                  <span className="text-muted-foreground ml-1 text-[10px]">
                    {project ? (project.canvas.width / project.canvas.height).toFixed(3) : ''}
                  </span>
                </div>
              </Field>
              <Field label="帧率">
                <div className="tnum text-[12px]">{project?.canvas.fps ?? '—'} fps</div>
              </Field>
              <Field label="时长">
                <div className="tnum text-[12px]">{timecode(duration)}</div>
              </Field>
              <Field label="总帧数">
                <div className="tnum text-[12px]">
                  {project ? Math.round(duration * project.canvas.fps) : '—'}
                </div>
              </Field>
              <Field label="背景">
                <div className="flex items-center gap-1.5">
                  <span
                    className="border-line h-3 w-3 rounded-[2px] border"
                    style={{ background: project?.canvas.background || '#000' }}
                  />
                  <span className="tnum text-[11px]">{project?.canvas.background || '—'}</span>
                </div>
              </Field>
            </Section>

            <Section title="节奏">
              <Field label="BPM">
                <div className="tnum text-[12px]">
                  {project?.timing.bpm ? project.timing.bpm.toFixed(2) : '—'}
                  <span className="text-muted-foreground ml-1.5 text-[10px]">
                    {project?.timing.source}
                  </span>
                </div>
              </Field>
              <Field label="偏移">
                <div className="tnum text-[12px]">{project?.timing.offset.toFixed(3)}s</div>
              </Field>
              <Field label="拍 / 小节">
                <div className="tnum text-[12px]">
                  {project?.timing.beats.length ?? 0} / {project?.timing.downbeats.length ?? 0}
                </div>
              </Field>
            </Section>

            <Section title="内容">
              <Field label="镜头">
                <div className="tnum text-[12px]">{project?.shots.length ?? 0}</div>
              </Field>
              <Field label="歌词行">
                <div className="tnum text-[12px]">{project?.lyrics.length ?? 0}</div>
              </Field>
              <Field label="轨道">
                <div className="tnum text-[12px]">
                  {project ? new Set(project.shots.map((s) => s.track)).size : 0}
                </div>
              </Field>
            </Section>

            <Section title="强调色">
              <div className="flex flex-wrap gap-1 px-2.5 pb-1.5">
                {ACCENT_PRESETS.map((p) => (
                  <button
                    key={p.id}
                    title={`${p.name} ${p.color}`}
                    onClick={() => onAccent(p.id)}
                    className={`h-5 w-5 rounded-[3px] border ${
                      accentId === p.id ? 'border-foreground' : 'border-transparent'
                    }`}
                    style={{ background: p.color }}
                  />
                ))}
              </div>
              <Field label="安全区">
                <button
                  className="btn w-full"
                  onClick={() => onSafe(!(project?.canvas.safe_area.enabled ?? false))}
                >
                  {project?.canvas.safe_area.enabled ? '已启用' : '已关闭'}
                </button>
              </Field>
            </Section>
          </>
        )}

        {tab === 'selection' && (
          <>
            {shot && project && (
              <>
                <Section title="镜头">
                  <Field label="ID">
                    <div className="tnum truncate text-[11.5px]">{shot.id}</div>
                  </Field>
                  <Field label="区间">
                    <div className="tnum text-[11.5px]">
                      {timecode(shot.start)} → {timecode(shot.end)}
                    </div>
                  </Field>
                  <Field label="时长">
                    <div className="tnum text-[11.5px]">
                      {(shot.end - shot.start).toFixed(3)}s ·{' '}
                      {Math.round((shot.end - shot.start) * project.canvas.fps)} 帧
                    </div>
                  </Field>
                  <Field label="轨道">
                    <div className="tnum text-[11.5px]">V{shot.track + 1}</div>
                  </Field>
                  <Field label="构图">
                    <div className="text-[11.5px]">
                      {String(
                        (shot.motion.hold?.params as Record<string, unknown> | undefined)?.mode ??
                          '—',
                      )}
                    </div>
                  </Field>
                  <Field label="Seed">
                    <div className="tnum text-[11.5px]">{shot.seed}</div>
                  </Field>
                </Section>
                <Section title="部件">
                  <PartRow label="入场" v={shot.motion.entrance} />
                  <PartRow label="保持" v={shot.motion.hold} />
                  <PartRow label="出场" v={shot.motion.exit} />
                  <PartRow label="运镜" v={shot.motion.camera} />
                  <PartRow label="转场" v={shot.transition} />
                </Section>
                <Section title="层">
                  {(['background', 'text', 'decor', 'overlay'] as const).map((k) => (
                    <div key={k} className="px-2.5 py-0.5">
                      <div className="text-muted-foreground text-[10px]">{k}</div>
                      <div className="flex flex-wrap gap-1 pt-0.5">
                        {shot.layers[k].length === 0 && (
                          <span className="text-muted-foreground text-[10.5px]">—</span>
                        )}
                        {shot.layers[k].map((p, i) => (
                          <span
                            key={i}
                            className="bg-panel-2 border-line rounded-[2px] border px-1 py-px text-[10px]"
                          >
                            {p.part}
                          </span>
                        ))}
                      </div>
                    </div>
                  ))}
                </Section>
              </>
            )}

            {lyric && (
              <>
                <Section title="歌词行">
                  <Field label="ID">
                    <div className="tnum truncate text-[11.5px]">{lyric.id}</div>
                  </Field>
                  <Field label="区间">
                    <div className="tnum text-[11.5px]">
                      {timecode(lyric.start)} → {timecode(lyric.end)}
                    </div>
                  </Field>
                  <Field label="时长">
                    <div className="tnum text-[11.5px]">{(lyric.end - lyric.start).toFixed(3)}s</div>
                  </Field>
                  <Field label="音节">
                    <div className="tnum text-[11.5px]">{lyric.syllables.length}</div>
                  </Field>
                  <Field label="样式">
                    <div className="truncate text-[11.5px]">{lyric.style}</div>
                  </Field>
                </Section>
                <Section title="文本">
                  <div className="px-2.5 pt-0.5 pb-1 text-[12px] leading-relaxed break-all">
                    {lyric.text}
                  </div>
                </Section>
                <Section title="逐字 K 轴">
                  <div className="max-h-40 overflow-auto px-2 pb-1">
                    {lyric.syllables.map((s, i) => (
                      <div key={i} className="flex gap-2 py-px text-[10.5px] tnum">
                        <span className="text-muted-foreground w-6 text-right">{i}</span>
                        <span className="w-10 flex-none">{s.text}</span>
                        <span className="text-primary">{s.start.toFixed(3)}</span>
                        <span className="text-muted-foreground">→ {s.end.toFixed(3)}</span>
                      </div>
                    ))}
                  </div>
                </Section>
              </>
            )}

            {!shot && !lyric && (
              <div className="text-muted-foreground px-3 py-6 text-center text-[11px]">
                未选中镜头 / 歌词行
              </div>
            )}
          </>
        )}

        {tab === 'output' && (
          <>
            <Section title="质量预设">
              <div className="grid grid-cols-2 gap-1 px-2 pt-0.5 pb-1">
                {QUALITY_PRESETS.map((p) => (
                  <button
                    key={p.id}
                    className="btn justify-start"
                    title={p.hint}
                    onClick={() => onOutput({ crf: p.crf })}
                    style={
                      output.crf === p.crf
                        ? { borderColor: 'var(--primary)', color: 'var(--primary)' }
                        : undefined
                    }
                  >
                    {p.label}
                  </button>
                ))}
              </div>
            </Section>

            <Section title="视频">
              <Field label="质量" hint="CRF 越小画质越好、体积越大">
                <div className="flex items-center gap-2">
                  <Slider value={34 - output.crf} min={0} max={24} onChange={(v) => onOutput({ crf: 34 - v })} />
                  <span className="tnum w-14 shrink-0 text-right text-[11px]">
                    {qualityLabel(output.crf)}
                  </span>
                </div>
              </Field>
              <Field label="CRF">
                <NumInput
                  value={output.crf}
                  min={10}
                  max={34}
                  step={1}
                  onChange={(v) => onOutput({ crf: v })}
                />
              </Field>
              <Field label="编码器">
                <Select<Encoder>
                  value={output.encoder}
                  onChange={(v) => onOutput({ encoder: v })}
                  options={[
                    { value: 'auto', label: '自动（硬件优先）' },
                    { value: 'software', label: 'libx264（软件）' },
                    { value: 'nvenc', label: 'NVENC', disabled: hw ? !hw.nvenc : false },
                    { value: 'qsv', label: 'QuickSync', disabled: hw ? !hw.qsv : false },
                    { value: 'amf', label: 'AMF', disabled: hw ? !hw.amf : false },
                  ]}
                />
              </Field>
              <Field label="分辨率">
                <Segmented
                  value={output.scale}
                  onChange={(v) => onOutput({ scale: v })}
                  options={[
                    { value: 1, label: '100%' },
                    { value: 0.75, label: '75%' },
                    { value: 0.5, label: '50%' },
                  ]}
                />
              </Field>
              <Field label="输出尺寸">
                <div className="tnum text-[11.5px]">
                  {ow} × {oh}
                </div>
              </Field>
              <Field label="帧率">
                <Select<string>
                  value={String(output.fps)}
                  onChange={(v) => onOutput({ fps: Number(v) })}
                  options={[
                    { value: String(project?.canvas.fps ?? 30), label: `项目 ${project?.canvas.fps ?? 30}` },
                    { value: '24', label: '24' },
                    { value: '30', label: '30' },
                    { value: '60', label: '60' },
                  ]}
                />
              </Field>
            </Section>

            <Section title="音频">
              <Field label="码率">
                <Select<string>
                  value={String(output.audioBitrate)}
                  onChange={(v) => onOutput({ audioBitrate: Number(v) })}
                  options={[
                    { value: '128', label: '128 kbps' },
                    { value: '192', label: '192 kbps' },
                    { value: '256', label: '256 kbps' },
                    { value: '320', label: '320 kbps' },
                  ]}
                />
              </Field>
            </Section>

            <Section title="范围">
              <Field label="导出">
                <Segmented
                  value={output.range}
                  onChange={(v) => onOutput({ range: v })}
                  options={[
                    { value: 'all', label: '整片' },
                    { value: 'inout', label: '入出点' },
                  ]}
                />
              </Field>
              {output.range === 'inout' && (
                <>
                  <Field label="入点">
                    <NumInput
                      value={output.inPoint}
                      step={0.1}
                      min={0}
                      max={output.outPoint}
                      onChange={(v) => onOutput({ inPoint: v })}
                    />
                  </Field>
                  <Field label="出点">
                    <NumInput
                      value={output.outPoint}
                      step={0.1}
                      min={output.inPoint}
                      max={duration}
                      onChange={(v) => onOutput({ outPoint: v })}
                    />
                  </Field>
                </>
              )}
              <Field label="时长">
                <div className="tnum text-[11.5px]">{timecode(rangeSec)}</div>
              </Field>
            </Section>

            <Section title="输出到">
              <div className="px-2 pb-1.5">
                <TextInput value={output.outPath} onChange={(v) => onOutput({ outPath: v })} mono />
                <div className="mt-1 flex gap-1">
                  <button className="btn flex-1" onClick={onBrowse}>
                    <FolderOpen size={12} /> 浏览…
                  </button>
                  <button className="btn btn-primary flex-1" onClick={onAddToQueue}>
                    加入队列
                  </button>
                </div>
              </div>
            </Section>

            <Section title="预计">
              <Field label="体积">
                <div className="tnum text-[11.5px]">{humanBytes(est)}</div>
              </Field>
              <Field label="帧数">
                <div className="tnum text-[11.5px]">{Math.round(rangeSec * output.fps)}</div>
              </Field>
              <div className="text-muted-foreground px-2.5 pb-1 text-[10px] leading-relaxed">
                估算按 H.264 经验 bpp 推得，纯色多会偏大、噪点多会偏小。
              </div>
            </Section>
          </>
        )}
      </div>
    </Panel>
  )
}

function PartRow({
  label,
  v,
}: {
  label: string
  v: { part: string; params: Record<string, unknown> } | null
}) {
  if (!v) {
    return (
      <div className="flex gap-2 px-2.5 py-0.5 text-[10.5px]">
        <span className="text-muted-foreground w-8 shrink-0">{label}</span>
        <span className="text-muted-foreground">—</span>
      </div>
    )
  }
  const keys = Object.keys(v.params).slice(0, 4)
  return (
    <div className="px-2.5 py-0.5">
      <div className="flex gap-2 text-[10.5px]">
        <span className="text-muted-foreground w-8 shrink-0">{label}</span>
        <span className="text-primary">{v.part}</span>
      </div>
      {keys.length > 0 && (
        <div className="tnum text-muted-foreground pl-10 text-[10px] leading-snug break-all">
          {keys.map((k) => `${k}=${fmt(v.params[k])}`).join(' · ')}
        </div>
      )}
    </div>
  )
}

function fmt(v: unknown): string {
  if (typeof v === 'number') return Number.isInteger(v) ? String(v) : v.toFixed(2)
  if (typeof v === 'string') return v.length > 14 ? `${v.slice(0, 12)}…` : v
  if (Array.isArray(v)) return `[${v.length}]`
  if (v && typeof v === 'object') return '{…}'
  return String(v)
}
