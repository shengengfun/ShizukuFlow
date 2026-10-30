/**
 * 导出管线（前端侧）。
 *
 * 数据流：LUME renderFrame 逐帧渲染（离屏画布，目标分辨率）
 *   → JPEG 编码（术曲 PV 不需要 alpha 通道，JPEG 体积小一个量级，IPC 快得多）
 *   → 二进制（Uint8Array）经 IPC 写盘（export_write，Tauri v2 直接收发，无 base64 膨胀）
 *   → ffmpeg 合成 MP4（export_mux：硬件编码优先，自动回退 libx264）
 *
 * 与预览共用同一个 renderFrame —— 预览即所得（GuideOfUI.md §4.5）。
 *
 * 缩放策略：直接把目标分辨率交给 renderFrame 排版，而不是先渲大图再缩。
 * 矢量文字在目标尺寸上光栅化，比缩图锐利；代价是 50% 预览版的小字
 * 会按小尺寸重新排版（行数可能不同）—— 但预览版本来就只看节奏。
 */

import { invoke } from '@tauri-apps/api/core'
import { api, type Project } from './api'
import { renderFrame } from './renderFrame'

export interface ExportResult {
  output: string
  encoder_used: string
  frames: number
  has_audio: boolean
  log_tail: string
}

/** 相对工程目录的音频路径 → 绝对路径。 */
async function resolveAudio(project: Project): Promise<string | null> {
  const a = project.media.audio
  if (!a || !a.path) return null
  if (/^[a-zA-Z]:[\\/]/.test(a.path)) return a.path
  const dir = await api.currentProjectDir()
  if (!dir) return null
  return `${dir.replace(/[\\/]+$/, '')}\\${a.path.replace(/\//g, '\\')}`
}

/** Blob → Uint8Array（Tauri v2 二进制 IPC 直接收发，省去 base64 膨胀）。 */
async function blobToBytes(blob: Blob): Promise<Uint8Array> {
  const buf = await blob.arrayBuffer()
  return new Uint8Array(buf)
}

export interface ExportOptions {
  outPath: string
  accent: string
  /** 目标分辨率（默认工程分辨率） */
  width?: number
  height?: number
  /** 目标帧率（默认工程帧率） */
  fps?: number
  /** 导出区间（秒），默认整片 */
  start?: number
  end?: number
  encoder?: string
  crf?: number
  audioBitrate?: number
  onProgress?: (done: number, total: number) => void
  shouldAbort?: () => boolean
  /** 每阶段日志（给渲染队列的日志窗用） */
  onLog?: (line: string) => void
}

/** 工程总时长（与后端 project_duration 同口径：音频 / 镜头 / 歌词最大者） */
export function projectDuration(project: Project): number {
  let d = 0
  for (const s of project.shots) d = Math.max(d, s.end)
  for (const l of project.lyrics) d = Math.max(d, l.end)
  if (project.media.audio?.duration) d = Math.max(d, project.media.audio.duration)
  return d
}

/**
 * 导出整片 MP4。帧率 / 分辨率 / 编码器 / CRF 由调用方给，缺省取工程配置。
 */
export async function exportVideo(project: Project, opts: ExportOptions): Promise<ExportResult> {
  const fps = opts.fps || project.canvas.fps || 30
  const W = Math.max(2, Math.round((opts.width ?? project.canvas.width) / 2) * 2)
  const H = Math.max(2, Math.round((opts.height ?? project.canvas.height) / 2) * 2)

  const full = projectDuration(project)
  const start = Math.max(0, Math.min(opts.start ?? 0, full))
  const end = Math.max(start, Math.min(opts.end ?? full, full))
  const dur = end - start
  if (dur <= 0) throw new Error('导出区间为空，没有可导出的内容')

  const total = Math.max(1, Math.ceil(dur * fps))
  const slug = (project.meta.title || 'project').replace(/[^\w-]+/g, '_').slice(0, 40)

  const dir = await invoke<string>('export_begin', { name: slug })
  opts.onLog?.(`帧目录 ${dir}`)
  opts.onLog?.(`目标 ${W}×${H} @ ${fps}fps · ${total} 帧 · ${dur.toFixed(2)}s`)

  const canvas = document.createElement('canvas')
  canvas.width = W
  canvas.height = H
  const ctx = canvas.getContext('2d')
  if (!ctx) throw new Error('无法创建离屏画布（导出用）')

  const t0 = performance.now()
  for (let i = 0; i < total; i++) {
    if (opts.shouldAbort?.()) throw new Error('已取消')
    const t = start + i / fps
    // 每帧先铺底色：JPEG 无 alpha，不铺会露出上一帧残影
    ctx.save()
    ctx.setTransform(1, 0, 0, 1, 0, 0)
    ctx.fillStyle = project.canvas.background || '#000'
    ctx.fillRect(0, 0, W, H)
    ctx.restore()
    renderFrame(ctx, W, H, project, t, opts.accent)
    const blob = await new Promise<Blob | null>((res) => canvas.toBlob(res, 'image/jpeg', 0.94))
    if (!blob) throw new Error(`第 ${i} 帧编码失败`)
    const bytes = await blobToBytes(blob)
    await invoke('export_write', {
      dir,
      name: `f${String(i).padStart(6, '0')}.jpg`,
      data: bytes,
    })
    if (i % 5 === 0 || i === total - 1) opts.onProgress?.(i + 1, total)
    // 让出主线程，避免 UI 完全冻结（也防止 IPC 队列饿死）
    if (i % 30 === 0) await new Promise((r) => setTimeout(r, 0))
  }
  opts.onProgress?.(total, total)
  const elapsed = (performance.now() - t0) / 1000
  opts.onLog?.(`渲染完成 ${total} 帧，用时 ${elapsed.toFixed(1)}s（${(total / elapsed).toFixed(1)} 帧/秒）`)

  const audio = await resolveAudio(project)
  opts.onLog?.(`合成 MP4 · 编码器=${opts.encoder ?? project.export.video.encoder ?? 'auto'}`)
  const res = await invoke<ExportResult>('export_mux', {
    framesDir: dir,
    audioPath: audio,
    audioStart: start,
    audioBitrate: opts.audioBitrate ?? 192,
    outPath: opts.outPath,
    fps,
    encoder: opts.encoder ?? project.export.video.encoder ?? 'auto',
    crf: opts.crf ?? project.export.video.crf ?? 18,
  })
  opts.onLog?.(`完成 · ${res.encoder_used} · ${res.frames} 帧 · ${res.output}`)
  return res
}
