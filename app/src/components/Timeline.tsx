import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { AudioLines, Film, Type, ZoomIn, ZoomOut } from 'lucide-react'
import type { AudioMeta, Project } from '~/lib/api'
import { api, timecode as tc, type Waveform } from '~/lib/api'

const RULER_H = 20
const ROW_H = 26
const MIN_SPAN = 0.35

type Row =
  | { kind: 'shot'; track: number; shots: Project['shots'] }
  | { kind: 'lyric' }
  | { kind: 'audio' }

const MODE_COLOR: Record<string, string> = {
  giant: '#e05a7d',
  quiet: '#8d94a3',
  corner: '#d9a441',
  vertical: '#4fae7c',
  center: '#3fb6d8',
}

function shotMode(m: Project['shots'][number]): string {
  const p = m.motion.hold?.params as Record<string, unknown> | undefined
  return typeof p?.mode === 'string' ? p.mode : 'center'
}

/**
 * 时间轴 —— Vegas / AE 的主编辑区。
 *
 * 全部用一张 canvas 画（刻度、镜头块、歌词块、波形、播放头）：
 *  - 100 多个块的 DOM 会在每次播放头移动时触发上百个 style 重算，canvas 不会；
 *  - 波形本来就得画在 canvas 上，混排 DOM 反而要做坐标对齐；
 *  - 命中测试只有「哪一行 + 哪个时间」，手写比事件委托更直接。
 *
 * 坐标只有一条真理：`x = (t - view[0]) / span * W`，绘制与命中共用。
 */
export function Timeline({
  project,
  duration,
  playhead,
  onSeek,
  onSelectShot,
  selectedShot,
  audioPath,
  meta,
  playing,
}: {
  project: Project | null
  duration: number
  playhead: number
  onSeek: (t: number) => void
  onSelectShot: (id: string, t: number) => void
  selectedShot: string | null
  audioPath: string | null
  meta: AudioMeta | null
  playing: boolean
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const wrapRef = useRef<HTMLDivElement>(null)
  const innerRef = useRef<HTMLDivElement>(null)
  const [view, setView] = useState<[number, number]>([0, 1])
  const [width, setWidth] = useState(900)
  const [peaks, setPeaks] = useState<Waveform | null>(null)
  const viewRef = useRef<[number, number]>([0, 1])
  const peaksRef = useRef<Waveform | null>(null)
  const dragRef = useRef<'seek' | null>(null)
  const rafRef = useRef<number | null>(null)
  const followRef = useRef(true)

  // 轨道清单：镜头轨按 track 降序（号大在上），其后是歌词轨与音频轨
  const rows = useMemo<Row[]>(() => {
    if (!project) return []
    const tracks = [...new Set(project.shots.map((s) => s.track))].sort((a, b) => b - a)
    const out: Row[] = tracks.map((t) => ({
      kind: 'shot' as const,
      track: t,
      shots: project.shots.filter((s) => s.track === t).sort((a, b) => a.start - b.start),
    }))
    out.push({ kind: 'lyric' })
    if (meta || audioPath) out.push({ kind: 'audio' })
    return out
  }, [project, meta, audioPath])

  const contentH = RULER_H + rows.length * ROW_H

  // 工程时长变化时把视野重置为全片
  useEffect(() => {
    const d = Math.max(0.5, duration || meta?.duration || 1)
    const v: [number, number] = [0, d]
    setView(v)
    viewRef.current = v
  }, [duration, meta?.duration])

  // 宽度跟随容器
  useEffect(() => {
    const el = wrapRef.current
    if (!el) return
    const ro = new ResizeObserver(() => setWidth(Math.max(120, el.clientWidth)))
    ro.observe(el)
    setWidth(Math.max(120, el.clientWidth))
    return () => ro.disconnect()
  }, [])

  // 取波形（缩放松手 120ms 后再取，避免每一帧滚轮都打后端）
  useEffect(() => {
    if (!audioPath) {
      setPeaks(null)
      peaksRef.current = null
      return
    }
    let alive = true
    const id = window.setTimeout(() => {
      api
        .audioPeaks(audioPath, view[0], view[1], Math.max(64, width), 'mix')
        .then((wf) => {
          if (!alive) return
          setPeaks(wf)
          peaksRef.current = wf
        })
        .catch(() => {})
    }, 120)
    return () => {
      alive = false
      window.clearTimeout(id)
    }
  }, [audioPath, view, width])

  // ── 绘制 ──
  const draw = useCallback(() => {
    const canvas = canvasRef.current
    if (!canvas || !project) return
    const dpr = window.devicePixelRatio || 1
    const W = width
    const H = contentH
    if (canvas.width !== Math.floor(W * dpr) || canvas.height !== Math.floor(H * dpr)) {
      canvas.width = Math.floor(W * dpr)
      canvas.height = Math.floor(H * dpr)
    }
    canvas.style.width = `${W}px`
    canvas.style.height = `${H}px`
    const ctx = canvas.getContext('2d')!
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, W, H)

    const [t0, t1] = viewRef.current
    const span = Math.max(1e-6, t1 - t0)
    const x = (t: number) => ((t - t0) / span) * W

    // 底
    ctx.fillStyle = '#16161b'
    ctx.fillRect(0, 0, W, H)

    // 行底纹：交替深浅，读起来像轨道而不是一片黑
    rows.forEach((r, i) => {
      const y = RULER_H + i * ROW_H
      ctx.fillStyle = r.kind === 'audio' ? '#12151a' : i % 2 ? '#191920' : '#16161b'
      ctx.fillRect(0, y, W, ROW_H)
      ctx.strokeStyle = '#22222b'
      ctx.lineWidth = 1
      ctx.beginPath()
      ctx.moveTo(0, y + ROW_H - 0.5)
      ctx.lineTo(W, y + ROW_H - 0.5)
      ctx.stroke()
    })

    // 刻度步长：目标 90px 一格
    const steps = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300]
    let grid = steps[steps.length - 1]
    for (const s of steps) {
      if ((s / span) * W >= 90) {
        grid = s
        break
      }
    }

    // 竖向网格 + 刻度文字
    ctx.font = '10px var(--font-mono, monospace)'
    ctx.textBaseline = 'middle'
    const first = Math.ceil(t0 / grid) * grid
    for (let t = first; t <= t1; t += grid) {
      const px = Math.round(x(t)) + 0.5
      ctx.strokeStyle = '#23232c'
      ctx.beginPath()
      ctx.moveTo(px, RULER_H)
      ctx.lineTo(px, H)
      ctx.stroke()
      ctx.fillStyle = '#5d5d6b'
      ctx.fillText(tc(t), px + 3, RULER_H / 2)
      ctx.strokeStyle = '#33333f'
      ctx.beginPath()
      ctx.moveTo(px, RULER_H - 4)
      ctx.lineTo(px, RULER_H)
      ctx.stroke()
    }

    // 节拍线：下拍亮一档，副歌卡点靠它对齐
    const bpm = project.timing.bpm
    if (bpm && bpm > 20) {
      const beat = 60 / bpm
      const downs = new Set(project.timing.downbeats.map((d) => Math.round(d * 100)))
      const bf = Math.ceil(t0 / beat) * beat
      for (let t = bf; t <= t1; t += beat) {
        const px = Math.round(x(t)) + 0.5
        const isDown = downs.has(Math.round(t * 100))
        ctx.strokeStyle = isDown ? 'rgba(90,220,230,0.22)' : 'rgba(120,140,160,0.10)'
        ctx.beginPath()
        ctx.moveTo(px, RULER_H)
        ctx.lineTo(px, H)
        ctx.stroke()
      }
    }

    // ── 各行内容 ──
    rows.forEach((r, i) => {
      const y = RULER_H + i * ROW_H
      const mid = y + ROW_H / 2

      if (r.kind === 'shot') {
        for (const s of r.shots) {
          const a = x(s.start)
          const b = x(s.end)
          if (b < -20 || a > W + 20) continue
          const w = Math.max(2, b - a - 1)
          const col = MODE_COLOR[shotMode(s)] ?? '#6b7280'
          const on = selectedShot === s.id
          roundRect(ctx, a + 0.5, y + 3, w, ROW_H - 6, 2)
          ctx.fillStyle = hexA(col, on ? 0.40 : 0.20)
          ctx.fill()
          ctx.strokeStyle = hexA(col, on ? 0.95 : 0.55)
          ctx.lineWidth = on ? 1.5 : 1
          ctx.stroke()
          // 左侧色条，宽块拉满行高
          if (w > 5) {
            ctx.fillStyle = hexA(col, 0.9)
            ctx.fillRect(a + 1.5, y + 5, 2, ROW_H - 10)
          }
          if (w > 44) {
            ctx.save()
            ctx.beginPath()
            ctx.rect(a + 5, y, w - 8, ROW_H)
            ctx.clip()
            ctx.fillStyle = on ? '#ffffff' : 'rgba(235,238,245,0.82)'
            ctx.font = '10.5px var(--font-sans, sans-serif)'
            ctx.fillText(shotMode(s), a + 6, mid)
            ctx.restore()
          }
        }
      }

      if (r.kind === 'lyric') {
        for (const l of project.lyrics) {
          const a = x(l.start)
          const b = x(l.end)
          if (b < -10 || a > W + 10) continue
          const w = Math.max(1.5, b - a - 1)
          roundRect(ctx, a + 0.5, y + 7, w, ROW_H - 14, 1.5)
          ctx.fillStyle = 'rgba(150,120,220,0.55)'
          ctx.fill()
          if (w > 60) {
            ctx.save()
            ctx.beginPath()
            ctx.rect(a + 3, y, w - 6, ROW_H)
            ctx.clip()
            ctx.fillStyle = 'rgba(20,18,26,0.92)'
            ctx.font = '9.5px var(--font-sans, sans-serif)'
            ctx.fillText(l.text, a + 4, mid + 0.5)
            ctx.restore()
          }
        }
      }

      if (r.kind === 'audio') {
        const wf = peaksRef.current
        ctx.strokeStyle = '#2c3340'
        ctx.beginPath()
        ctx.moveTo(0, mid)
        ctx.lineTo(W, mid)
        ctx.stroke()
        if (wf && wf.peaks.length >= 2) {
          const n = wf.peaks.length / 2
          // 波形按自己的时间范围映射回视图坐标（请求窗口可能比当前视图旧）
          const ax = (t: number) => ((t - t0) / span) * W
          const colW = (ax(wf.t1) - ax(wf.t0)) / n
          ctx.fillStyle = '#5fc9d8'
          for (let k = 0; k < n; k++) {
            const mn = wf.peaks[k * 2]
            const mx = wf.peaks[k * 2 + 1]
            const px = ax(wf.t0) + k * colW
            const h = Math.max(1, (mx - mn) * (ROW_H - 8) * 0.5)
            ctx.fillRect(px, mid - h, Math.max(0.7, colW - 0.3), h * 2)
          }
        } else {
          ctx.fillStyle = 'rgba(120,140,160,0.35)'
          ctx.font = '10px var(--font-sans, sans-serif)'
          ctx.fillText('无音轨', 8, mid)
        }
      }
    })

    // ruler 底色压在内容上方（滚动的块不会爬到刻度上）
    ctx.fillStyle = '#1c1c22'
    ctx.fillRect(0, 0, W, RULER_H)
    ctx.strokeStyle = '#2c2c36'
    ctx.beginPath()
    ctx.moveTo(0, RULER_H - 0.5)
    ctx.lineTo(W, RULER_H - 0.5)
    ctx.stroke()
    for (let t = first; t <= t1; t += grid) {
      const px = Math.round(x(t)) + 0.5
      ctx.fillStyle = '#7b7b8a'
      ctx.font = '10px var(--font-mono, monospace)'
      ctx.fillText(tc(t), px + 3, RULER_H / 2 - 1)
      ctx.strokeStyle = '#43434f'
      ctx.beginPath()
      ctx.moveTo(px, RULER_H - 5)
      ctx.lineTo(px, RULER_H)
      ctx.stroke()
    }

    // 视野外的暗角，提示"还有内容没看到"
    const gradL = ctx.createLinearGradient(0, 0, 14, 0)
    gradL.addColorStop(0, 'rgba(0,0,0,0.35)')
    gradL.addColorStop(1, 'rgba(0,0,0,0)')
    ctx.fillStyle = gradL
    ctx.fillRect(0, RULER_H, 14, H - RULER_H)
    const gradR = ctx.createLinearGradient(W - 14, 0, W, 0)
    gradR.addColorStop(0, 'rgba(0,0,0,0)')
    gradR.addColorStop(1, 'rgba(0,0,0,0.35)')
    ctx.fillStyle = gradR
    ctx.fillRect(W - 14, RULER_H, 14, H - RULER_H)

    // 播放头
    const px = Math.round(x(playhead)) + 0.5
    if (px >= -2 && px <= W + 2) {
      ctx.strokeStyle = '#ff3f6e'
      ctx.lineWidth = 1.5
      ctx.beginPath()
      ctx.moveTo(px, 0)
      ctx.lineTo(px, H)
      ctx.stroke()
      ctx.fillStyle = '#ff3f6e'
      ctx.beginPath()
      ctx.moveTo(px - 5, 0)
      ctx.lineTo(px + 5, 0)
      ctx.lineTo(px, 7)
      ctx.closePath()
      ctx.fill()
      // 顶部时间码小旗
      ctx.font = '9.5px var(--font-mono, monospace)'
      const label = tc(playhead)
      const tw = ctx.measureText(label).width + 8
      const bx = Math.min(Math.max(0, px + 3), W - tw)
      ctx.fillStyle = 'rgba(255,63,110,0.92)'
      roundRect(ctx, bx, 1, tw, 12, 2)
      ctx.fill()
      ctx.fillStyle = '#fff'
      ctx.fillText(label, bx + 4, 7.5)
    }
  }, [project, rows, width, contentH, playhead, selectedShot])

  const scheduleDraw = useCallback(() => {
    if (rafRef.current != null) return
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = null
      draw()
    })
  }, [draw])

  useEffect(() => {
    viewRef.current = view
    scheduleDraw()
  }, [view, peaks, scheduleDraw])

  useEffect(() => {
    scheduleDraw()
  }, [playhead, scheduleDraw])

  // 播放头跟随：越过视野 82% 时推进视野（用户手动滚动后暂停跟随）
  useEffect(() => {
    if (!playing || !followRef.current) return
    const [t0, t1] = viewRef.current
    const span = t1 - t0
    if (playhead < t0 || playhead > t0 + span * 0.82) {
      const n0 = Math.max(0, playhead - span * 0.15)
      setView([n0, n0 + span])
    }
  }, [playhead, playing])

  // ── 交互 ──
  const seekAt = useCallback(
    (clientX: number) => {
      const el = innerRef.current
      if (!el) return
      const r = el.getBoundingClientRect()
      const [t0, t1] = viewRef.current
      const ratio = Math.min(1, Math.max(0, (clientX - r.left) / r.width))
      onSeek(t0 + ratio * (t1 - t0))
    },
    [onSeek],
  )

  const hitShot = useCallback(
    (clientX: number, clientY: number) => {
      const el = innerRef.current
      if (!el || !project) return null
      const r = el.getBoundingClientRect()
      const y = clientY - r.top - RULER_H
      const rowIdx = Math.floor(y / ROW_H)
      const row = rows[rowIdx]
      if (!row || row.kind !== 'shot') return null
      const [t0, t1] = viewRef.current
      const t = t0 + ((clientX - r.left) / r.width) * (t1 - t0)
      return row.shots.find((s) => t >= s.start && t <= s.end) ?? null
    },
    [project, rows],
  )

  const onDown = (e: React.MouseEvent) => {
    followRef.current = false
    dragRef.current = 'seek'
    const shot = hitShot(e.clientX, e.clientY)
    if (shot) onSelectShot(shot.id, shot.start)
    seekAt(e.clientX)
  }

  const onWheel = (e: React.WheelEvent) => {
    e.preventDefault()
    const el = innerRef.current
    if (!el) return
    const r = el.getBoundingClientRect()
    const [t0, t1] = viewRef.current
    const span = t1 - t0
    const maxSpan = Math.max(0.6, duration || 1)
    const anchorRatio = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width))
    if (e.shiftKey) {
      // Shift + 滚轮 = 水平平移（Vegas 的肌肉记忆）
      const dt = (e.deltaY / 400) * span
      let n0 = t0 + dt
      n0 = Math.max(0, Math.min(maxSpan - span, n0))
      followRef.current = false
      setView([n0, n0 + span])
      return
    }
    // 滚轮 = 缩放，锚在光标
    const anchor = t0 + anchorRatio * span
    const factor = e.deltaY > 0 ? 1.16 : 1 / 1.16
    let ns = Math.min(maxSpan, Math.max(MIN_SPAN, span * factor))
    let n0 = anchor - anchorRatio * ns
    n0 = Math.max(0, Math.min(duration - ns, n0))
    followRef.current = false
    setView([n0, n0 + ns])
  }

  const zoomBy = (f: number) => {
    const [t0, t1] = viewRef.current
    const span = t1 - t0
    const anchor = playhead
    let ns = Math.min(Math.max(0.6, duration || 1), Math.max(MIN_SPAN, span * f))
    let n0 = Math.max(0, Math.min(duration - ns, anchor - ns / 2))
    setView([n0, n0 + ns])
    followRef.current = false
  }

  const fit = () => {
    const v: [number, number] = [0, Math.max(0.6, duration || 1)]
    setView(v)
    followRef.current = true
  }

  const span = view[1] - view[0]

  return (
    <div className="pnl bg-timeline">
      <div className="pnl-head justify-between">
        <span>时间轴</span>
        <div className="flex items-center gap-1">
          <span className="tnum text-muted-foreground mr-1 text-[10px]">
            {span.toFixed(2)}s / 屏
          </span>
          <button className="btn" style={{ height: 17, padding: '0 5px' }} onClick={() => zoomBy(1.5)} title="缩小">
            <ZoomOut size={11} />
          </button>
          <button className="btn" style={{ height: 17, padding: '0 5px' }} onClick={() => zoomBy(1 / 1.5)} title="放大">
            <ZoomIn size={11} />
          </button>
          <button className="btn" style={{ height: 17, padding: '0 5px' }} onClick={fit} title="适配全片">
            适配
          </button>
        </div>
      </div>

      <div className="flex min-h-0 flex-1">
        {/* 轨道头 */}
        <div className="bg-panel-2 border-line w-[132px] flex-none border-r">
          <div className="border-line text-muted-foreground flex items-center px-2 text-[9.5px] tracking-wider border-b" style={{ height: RULER_H }}>
            轨道
          </div>
          {rows.map((r, i) => (
            <div
              key={i}
              className="border-line flex items-center gap-1.5 border-b px-2"
              style={{ height: ROW_H }}
            >
              <span className="text-muted-foreground">
                {r.kind === 'shot' ? <Film size={11} /> : r.kind === 'lyric' ? <Type size={11} /> : <AudioLines size={11} />}
              </span>
              <span className="flex-1 truncate text-[10.5px]">
                {r.kind === 'shot' ? `V${r.track + 1} 镜头` : r.kind === 'lyric' ? 'L1 歌词' : 'A1 音频'}
              </span>
              <span className="tnum text-muted-foreground text-[9.5px]">
                {r.kind === 'shot' ? r.shots.length : r.kind === 'lyric' ? project?.lyrics.length ?? 0 : ''}
              </span>
            </div>
          ))}
          {rows.length === 0 && (
            <div className="text-muted-foreground px-2 py-3 text-[10.5px]">空</div>
          )}
        </div>

        {/* 画布区 */}
        <div ref={wrapRef} className="relative min-w-0 flex-1 overflow-hidden">
          <div
            ref={innerRef}
            className="absolute inset-0 overflow-hidden"
            onMouseDown={onDown}
            onMouseMove={(e) => dragRef.current === 'seek' && seekAt(e.clientX)}
            onMouseUp={() => (dragRef.current = null)}
            onMouseLeave={() => (dragRef.current = null)}
            onWheel={onWheel}
            style={{ cursor: 'text' }}
          >
            <canvas ref={canvasRef} className="block select-none" />
            {!project && (
              <div className="text-muted-foreground absolute inset-0 grid place-items-center text-[11px]">
                没有工程
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}

function roundRect(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  r: number,
) {
  const rr = Math.min(r, h / 2, w / 2)
  ctx.beginPath()
  ctx.moveTo(x + rr, y)
  ctx.arcTo(x + w, y, x + w, y + h, rr)
  ctx.arcTo(x + w, y + h, x, y + h, rr)
  ctx.arcTo(x, y + h, x, y, rr)
  ctx.arcTo(x, y, x + w, y, rr)
  ctx.closePath()
}

/** #rrggbb + alpha → rgba() */
function hexA(hex: string, a: number) {
  const h = hex.replace('#', '')
  const n = parseInt(h.length === 3 ? h.split('').map((c) => c + c).join('') : h, 16)
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`
}
