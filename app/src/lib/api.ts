/**
 * 后端命令封装。
 *
 * 这里的类型定义必须与 `src-tauri/src/sf/mod.rs` 的 serde 结构**逐字段对应** ——
 * 两边是同一份工程格式的两种表达，Rust 加了字段这边忘了加，
 * 编译期不会报错，但运行时会静默丢数据。
 */

import { invoke } from '@tauri-apps/api/core'

// ─────────────────────────── 工程类型 ───────────────────────────

export interface Meta {
  title: string
  artist: string
  created: string
  modified: string
  generator: string
  /** 工程级强调色（「正在唱」的字的提示色）。缺省用全局水蓝。 */
  accent?: string
}

export interface SafeArea {
  enabled: boolean
  margin: number
}

export interface Canvas {
  width: number
  height: number
  fps: number
  background: string
  safe_area: SafeArea
}

export interface MediaRef {
  path: string
  sha1: string
  duration: number
  kind: string
}

export interface Media {
  audio: MediaRef | null
  reference: MediaRef[]
}

export interface Timing {
  bpm: number
  offset: number
  beats: number[]
  downbeats: number[]
  source: string
}

export interface Stroke {
  color: string
  width: number
}

export interface Shadow {
  offset: [number, number]
  blur: number
  color: string
}

export interface Style {
  id: string
  font: string
  weight: number
  size: number
  letter_spacing: number
  line_height: number
  fill: string
  /** 歌词行垂直锚点（画幅高度比例，缺省 0.58）——版式错落用。 */
  y?: number | null
  stroke: Stroke | null
  shadow: Shadow | null
}

export interface Syllable {
  text: string
  start: number
  end: number
}

export interface Lyric {
  id: string
  start: number
  end: number
  text: string
  syllables: Syllable[]
  style: string
}

/** 部件引用。params 是开放结构：检查器按部件声明的 schema 渲染控件。 */
export interface PartRef {
  part: string
  params: Record<string, unknown>
}

export interface Layers {
  background: PartRef[]
  text: PartRef[]
  decor: PartRef[]
  overlay: PartRef[]
}

export interface Motion {
  entrance: PartRef | null
  hold: PartRef | null
  exit: PartRef | null
  camera: PartRef | null
}

export interface Shot {
  id: string
  start: number
  end: number
  /** 时间轨编号，越大越靠上（后合成）。默认 0，数量不封顶。 */
  track: number
  lyric_refs: string[]
  layers: Layers
  motion: Motion
  transition: PartRef | null
  seed: number
  locked: boolean
}

export interface Asset {
  id: string
  kind: string
  path: string
  sha1: string
}

export interface VideoExport {
  codec: string
  crf: number
  preset: string
  /** auto / software / nvenc / qsv / amf */
  encoder: string
}

export interface GifExport {
  fps: number
  width: number
  dither: string
}

export interface ApngExport {
  fps: number
  colors: number
}

export interface ExportConfig {
  preset: string
  video: VideoExport
  gif: GifExport
  apng: ApngExport
}

export interface Project {
  format: string
  version: number
  meta: Meta
  canvas: Canvas
  media: Media
  timing: Timing
  styles: Style[]
  lyrics: Lyric[]
  shots: Shot[]
  assets: Asset[]
  export: ExportConfig
}

// ─────────────────────────── 工具链 ───────────────────────────

/** `origin`: own / borrowed / custom / missing */
export type ToolsOrigin = 'own' | 'borrowed' | 'custom' | 'missing'

export interface ToolsInfo {
  dir: string
  origin: ToolsOrigin
  missing: string[]
  found: number
}

/** 硬件编码能力（探测自 ffmpeg）。 */
export interface HwCaps {
  nvenc: boolean
  qsv: boolean
  amf: boolean
  available: string[]
}

export const ORIGIN_LABEL: Record<ToolsOrigin, string> = {
  own: 'ShizukuFlow 自带',
  borrowed: '借自岚珠工具箱',
  custom: '手动指定',
  missing: '未找到',
}

// ─────────────────────────── 音频 / 波形 ───────────────────────────

export interface AudioMeta {
  duration: number
  sample_rate: number
  channels: number
  has_audio: boolean
}

/** 声场视图。值与后端 `ChannelView` 的 serde 命名一致（小写）。 */
export type ChannelView = 'mix' | 'left' | 'right' | 'mid' | 'side'

export const CHANNEL_VIEW_LABEL: Record<ChannelView, string> = {
  mix: '混合',
  left: '左声道',
  right: '右声道',
  mid: '中置',
  side: '侧边',
}

/** 波形峰值：交错的 [min0,max0,min1,max1,...]，值已归一化到 [-1,1]。 */
export interface Waveform {
  t0: number
  t1: number
  width: number
  peaks: number[]
}

// ─────────────────────────── 命令 ───────────────────────────

export const api = {
  // 启动参数（CLI 自动导出：--export <project.sf> <out.mp4>）
  appArgs: () => invoke<string[]>('app_args'),

  // 工程
  projectNew: () => invoke<Project>('project_new'),
  projectDemo: () => invoke<Project>('project_demo'),
  projectOpen: (path: string) => invoke<Project>('project_open', { path }),
  projectSave: (project: Project) => invoke<void>('project_save', { project }),
  projectSaveAs: (path: string, project: Project) =>
    invoke<void>('project_save_as', { path, project }),
  projectMissingAssets: () => invoke<string[]>('project_missing_assets'),
  projectDuration: () => invoke<number>('project_duration'),
  currentProjectDir: () => invoke<string | null>('current_project_dir'),
  /** 自动导出流程的结果标记文件（供脚本判断成功/失败）。 */
  invokeFlag: (path: string, ok: boolean, message: string) =>
    invoke<void>('export_flag', { path, ok, message }),

  // 工具链
  toolsDir: () => invoke<ToolsInfo>('tools_dir'),
  toolsMissing: () => invoke<string[]>('tools_missing'),
  toolsList: () => invoke<string[]>('tools_list'),
  toolsRescan: (overrideDir?: string) =>
    invoke<string[]>('tools_rescan', { overrideDir: overrideDir ?? null }),
  toolsHwCaps: () => invoke<HwCaps>('tools_hw_caps'),

  // 音频 / 波形
  audioLoad: (audioPath: string) => invoke<AudioMeta>('audio_load', { audioPath }),
  audioPeaks: (
    audioPath: string,
    t0: number,
    t1: number,
    width: number,
    view: ChannelView = 'mix',
  ) => invoke<Waveform>('audio_peaks', { audioPath, t0, t1, width, view }),
}

/**
 * 时间码格式化：`mm:ss.mmm`。
 *
 * 全应用统一走这一个函数 —— Aegisub 的手感有很大一部分来自时间码的稳定显示，
 * 各处自己拼字符串一定会出现 `1:2.3` 和 `01:02.300` 混着的情况。
 */
export function timecode(sec: number): string {
  const sign = sec < 0 ? '-' : ''
  const s = Math.abs(sec)
  const m = Math.floor(s / 60)
  const rest = s - m * 60
  return `${sign}${String(m).padStart(2, '0')}:${rest.toFixed(3).padStart(6, '0')}`
}
