/**
 * 输出模块设置。
 *
 * 这里刻意只有「真能落地成 ffmpeg 参数」的字段 —— 界面上不给假开关：
 * 改了分辨率就真的按比例渲，改了 CRF 就真的进编码器。
 * （导出器只认这份结构，渲染队列里存的是它的快照。）
 */

export type Encoder = 'auto' | 'software' | 'nvenc' | 'qsv' | 'amf'

export interface OutputSettings {
  /** 容器/编码，目前只有 MP4(H.264)——GUI 不该显示做不到的选项 */
  container: 'mp4'
  encoder: Encoder
  /** 相对工程分辨率的比例 */
  scale: number
  fps: number
  /** 10(近无损) ~ 34(草稿) */
  crf: number
  /** 取值范围：整片 / 入出点之间 */
  range: 'all' | 'inout'
  inPoint: number
  outPoint: number
  audioBitrate: number
  outPath: string
}

export const QUALITY_PRESETS = [
  { id: 'draft', label: '草稿（快）', crf: 30, hint: '预览用，细节糊但几分钟出片' },
  { id: 'balanced', label: '标准', crf: 23, hint: '体积与画质平衡，网络分享够用' },
  { id: 'high', label: '高质量', crf: 18, hint: '术曲 PV 交付档，噪点/渐变保得住' },
  { id: 'archive', label: '归档', crf: 14, hint: '接近无损，体积大 2~3 倍' },
] as const

export function qualityLabel(crf: number): string {
  if (crf <= 15) return '归档'
  if (crf <= 19) return '高质量'
  if (crf <= 25) return '标准'
  if (crf <= 29) return '草稿'
  return '极速'
}

export function encoderLabel(e: Encoder): string {
  switch (e) {
    case 'auto':
      return '自动（硬件优先）'
    case 'software':
      return 'libx264（软件）'
    case 'nvenc':
      return 'NVIDIA NVENC'
    case 'qsv':
      return 'Intel QuickSync'
    case 'amf':
      return 'AMD AMF'
  }
}

export function scaledSize(
  canvas: { width: number; height: number },
  scale: number,
): [number, number] {
  const even = (n: number) => Math.max(2, Math.round((n * scale) / 2) * 2)
  return [even(canvas.width), even(canvas.height)]
}

/**
 * 体积估算（只用于给用户一个量级感，不作为承诺）。
 *
 * 用 bits-per-pixel 的经验档位：H.264 在 crf 18 时约 0.06 bpp，
 * 每 +6 crf 砍一半。术曲 PV 有大面积纯色 + 噪点颗粒，实测会在这个
 * 估算的 0.8~1.4 倍之间漂移，所以区间给宽一点。
 */
export function estimateBytes(
  w: number,
  h: number,
  fps: number,
  crf: number,
  seconds: number,
  audioKbps: number,
): number {
  const bpp = 0.06 * Math.pow(2, (18 - crf) / 6)
  const video = (w * h * fps * bpp * seconds) / 8
  const audio = (audioKbps * 1000 * seconds) / 8
  return Math.max(0, video + audio)
}

export function humanBytes(n: number): string {
  if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GB`
  if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MB`
  if (n >= 1024) return `${(n / 1024).toFixed(0)} KB`
  return `${Math.round(n)} B`
}

/** 默认输出路径：与工程同名的 mp4，落在工程目录旁边 */
export function defaultOutPath(title: string, dir: string | null): string {
  const safe = (title || 'pv').replace(/[\\/:*?"<>|]/g, '_')
  const base = dir ? dir.replace(/[\\/]+$/, '') : ''
  return base ? `${base}\\${safe}.mp4` : `${safe}.mp4`
}
