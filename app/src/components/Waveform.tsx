import { useCallback, useEffect, useRef, useState } from 'react'
import {
  api,
  timecode,
  type AudioMeta,
  type ChannelView,
  type Waveform,
  CHANNEL_VIEW_LABEL,
} from '../lib/api'

interface Props {
  audioPath: string | null
  meta: AudioMeta | null
  accent: string
  /** 播放头位置（秒），由外部播放器驱动；null 表示没有 */
  playhead: number | null
  /** 用户刮擦时回调，告知新位置（秒） */
  onSeek?: (t: number) => void
}

const MIN_SPAN = 0.05 // 最大放大：视野跨度不小于 50ms

/**
 * 波形时间轴 —— 复刻 Aegisub 的手感。
 *
 * 关键点：波形数据全部来自后端 `audio_peaks`，那是多级峰值金字塔的聚合结果，
 * 所以无论缩放到「整首歌」还是「50ms 一帧」，取数都只跟画布宽度成正比，
 * 缩放/刮擦永远顺滑。前端只负责画，不碰 PCM。
 */
export function Waveform({ audioPath, meta, accent, playhead, onSeek }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const wrapRef = useRef<HTMLDivElement>(null)
  const [view, setView] = useState<[number, number]>([0, 1])
  const [channelView, setChannelView] = useState<ChannelView>('mix')
  const [loading, setLoading] = useState(false)
  const peaksRef = useRef<Waveform | null>(null)
  const viewRef = useRef<[number, number]>([0, 1])
  const channelRef = useRef<ChannelView>('mix')
  const rafRef = useRef<number | null>(null)
  const draggingRef = useRef(false)

  // 音轨或时长变化时重置视野
  useEffect(() => {
    if (meta && meta.duration > 0) {
      const v: [number, number] = [0, meta.duration]
      setView(v)
      viewRef.current = v
      peaksRef.current = null
    }
  }, [audioPath, meta])

  const fetchPeaks = useCallback(
    async (v: [number, number], ch: ChannelView) => {
      if (!audioPath) return
      setLoading(true)
      try {
        const w = Math.max(64, Math.floor(wrapRef.current?.clientWidth ?? 800))
        const wf = await api.audioPeaks(audioPath, v[0], v[1], w, ch)
        peaksRef.current = wf
        scheduleDraw()
      } catch (e) {
        console.error('取波形失败', e)
      } finally {
        setLoading(false)
      }
    },
    [audioPath],
  )

  const draw = useCallback(() => {
    const canvas = canvasRef.current
    const wrap = wrapRef.current
    if (!canvas || !wrap) return
    const dpr = window.devicePixelRatio || 1
    const cssW = wrap.clientWidth
    const cssH = wrap.clientHeight
    if (canvas.width !== cssW * dpr || canvas.height !== cssH * dpr) {
      canvas.width = cssW * dpr
      canvas.height = cssH * dpr
    }
    const ctx = canvas.getContext('2d')!
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, cssW, cssH)

    const mid = cssH / 2
    const [t0, t1] = viewRef.current
    const span = Math.max(1e-6, t1 - t0)

    // 背景
    ctx.fillStyle = '#1b1b22'
    ctx.fillRect(0, 0, cssW, cssH)
    // 中线
    ctx.strokeStyle = '#33333d'
    ctx.lineWidth = 1
    ctx.beginPath()
    ctx.moveTo(0, mid)
    ctx.lineTo(cssW, mid)
    ctx.stroke()

    // 刻度尺（每格尽量是 0.1/0.5/1/5/10s 之一）
    drawRuler(ctx, t0, t1, cssW, cssH, accent)

    // 波形
    const wf = peaksRef.current
    if (wf && wf.peaks.length >= 2) {
      const n = wf.peaks.length / 2
      const colW = cssW / n
      ctx.fillStyle = accent // 用强调色画波形，呼应 Shizuku 水蓝
      ctx.globalAlpha = 0.92
      for (let i = 0; i < n; i++) {
        const mn = wf.peaks[i * 2]
        const mx = wf.peaks[i * 2 + 1]
        const x = i * colW
        const yTop = mid - mx * mid * 0.96
        const yBot = mid - mn * mid * 0.96
        const h = Math.max(1, yBot - yTop)
        ctx.fillRect(x, yTop, Math.max(0.6, colW - 0.2), h)
      }
      ctx.globalAlpha = 1
    }

    // 播放头
    if (playhead != null && playhead >= t0 && playhead <= t1) {
      const x = ((playhead - t0) / span) * cssW
      ctx.strokeStyle = '#ff5d7a'
      ctx.lineWidth = 1.5
      ctx.beginPath()
      ctx.moveTo(x, 0)
      ctx.lineTo(x, cssH)
      ctx.stroke()
      ctx.fillStyle = '#ff5d7a'
      ctx.beginPath()
      ctx.moveTo(x - 5, 0)
      ctx.lineTo(x + 5, 0)
      ctx.lineTo(x, 7)
      ctx.closePath()
      ctx.fill()
    }
  }, [accent, playhead])

  // 重画（带 rAF 节流）
  const scheduleDraw = useCallback(() => {
    if (rafRef.current != null) return
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = null
      draw()
    })
  }, [draw])

  // view / 声道视图变化 → 取数 + 重画
  useEffect(() => {
    viewRef.current = view
    channelRef.current = channelView
    if (audioPath) fetchPeaks(view, channelView)
    else scheduleDraw()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [view, channelView, audioPath])

  // 仅播放头 / 窗口尺寸变化 → 只重画
  useEffect(() => {
    scheduleDraw()
  }, [playhead, scheduleDraw])

  useEffect(() => {
    const onResize = () => scheduleDraw()
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [scheduleDraw])

  // 缩放：以光标处时间为锚
  const onWheel = useCallback(
    (e: React.WheelEvent) => {
      if (!meta || meta.duration <= 0) return
      e.preventDefault()
      const wrap = wrapRef.current!
      const rect = wrap.getBoundingClientRect()
      const x = e.clientX - rect.left
      const [t0, t1] = viewRef.current
      const span = t1 - t0
      const anchor = t0 + (x / rect.width) * span
      const factor = e.deltaY > 0 ? 1.18 : 1 / 1.18
      let newSpan = span * factor
      const maxSpan = meta.duration
      if (newSpan > maxSpan) newSpan = maxSpan
      if (newSpan < MIN_SPAN) newSpan = MIN_SPAN
      let n0 = anchor - ((x / rect.width) * newSpan)
      let n1 = n0 + newSpan
      if (n0 < 0) {
        n0 = 0
        n1 = newSpan
      }
      if (n1 > meta.duration) {
        n1 = meta.duration
        n0 = n1 - newSpan
      }
      setView([n0, n1])
    },
    [meta],
  )

  // 刮擦：点击/拖动设置播放头
  const seekFromEvent = useCallback(
    (clientX: number) => {
      const wrap = wrapRef.current!
      const rect = wrap.getBoundingClientRect()
      const [t0, t1] = viewRef.current
      const r = (clientX - rect.left) / rect.width
      const t = Math.max(t0, Math.min(t1, t0 + r * (t1 - t0)))
      onSeek?.(t)
      scheduleDraw()
    },
    [onSeek, scheduleDraw],
  )

  const onDown = (e: React.MouseEvent) => {
    draggingRef.current = true
    seekFromEvent(e.clientX)
  }
  const onMove = (e: React.MouseEvent) => {
    if (draggingRef.current) seekFromEvent(e.clientX)
  }
  const onUp = () => {
    draggingRef.current = false
  }

  const zoomLabel = `${(view[1] - view[0]).toFixed(2)}s`

  return (
    <div className="flex flex-col h-full">
      <div className="flex items-center justify-between gap-3 px-3 py-1.5 text-xs text-muted-foreground border-b border-line">
        <div className="flex items-center gap-2 min-w-0">
          <span className="shrink-0">波形 · 峰值金字塔</span>
          {meta && meta.channels >= 2 && (
            <div className="flex items-center gap-0.5 rounded bg-panel-2 p-0.5">
              {(Object.keys(CHANNEL_VIEW_LABEL) as ChannelView[]).map((cv) => (
                <button
                  key={cv}
                  onClick={() => setChannelView(cv)}
                  className={`rounded px-1.5 py-0.5 text-[10px] leading-none transition-colors ${
                    channelView === cv
                      ? 'bg-primary text-primary-foreground'
                      : 'text-muted-foreground hover:text-foreground'
                  }`}
                  title={
                    cv === 'mid'
                      ? '中置 (L+R)/2'
                      : cv === 'side'
                        ? '侧边 (L-R)/2'
                        : undefined
                  }
                >
                  {CHANNEL_VIEW_LABEL[cv]}
                </button>
              ))}
            </div>
          )}
        </div>
        <span className="tnum shrink-0">
          {meta ? `${meta.channels}ch · ` : ''}
          视野 {timecode(view[0])} – {timecode(view[1])} · 跨度 {zoomLabel}
          {loading && <span className="ml-2 text-accent">取数中…</span>}
        </span>
      </div>
      <div
        ref={wrapRef}
        className="relative flex-1 min-h-0 cursor-text select-none"
        onWheel={onWheel}
        onMouseDown={onDown}
        onMouseMove={onMove}
        onMouseUp={onUp}
        onMouseLeave={onUp}
      >
        <canvas ref={canvasRef} className="absolute inset-0 w-full h-full" />
        {!audioPath && (
          <div className="text-muted-foreground absolute inset-0 grid place-items-center text-[12px]">
            未载入音轨
          </div>
        )}
      </div>
    </div>
  )
}

function drawRuler(
  ctx: CanvasRenderingContext2D,
  t0: number,
  t1: number,
  w: number,
  h: number,
  accent: string,
) {
  const span = Math.max(1e-6, t1 - t0)
  const candidates = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60, 120, 300]
  const targetPx = 90 // 目标刻度间距（px）
  let step = candidates[candidates.length - 1]
  for (const c of candidates) {
    if ((c / span) * w >= targetPx) {
      step = c
      break
    }
  }
  ctx.strokeStyle = '#2a2a33'
  ctx.fillStyle = '#6a6a78'
  ctx.font = '10px ui-monospace, monospace'
  ctx.lineWidth = 1
  const first = Math.ceil(t0 / step) * step
  for (let t = first; t <= t1; t += step) {
    const x = ((t - t0) / span) * w
    ctx.beginPath()
    ctx.moveTo(x + 0.5, h - 12)
    ctx.lineTo(x + 0.5, h)
    ctx.stroke()
    ctx.fillText(timecode(t), x + 3, h - 3)
  }
  // 顶部强调线
  ctx.strokeStyle = accent
  ctx.globalAlpha = 0.35
  ctx.beginPath()
  ctx.moveTo(0, 0.5)
  ctx.lineTo(w, 0.5)
  ctx.stroke()
  ctx.globalAlpha = 1
}
