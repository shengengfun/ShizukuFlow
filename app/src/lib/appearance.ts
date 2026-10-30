/**
 * 外观：主题（亮/暗/跟随系统）+ 强调色 + 界面缩放。
 *
 * 思路与岚珠工具箱 / ShiorikoTrans 的 `lib/appearance.ts` 一致：
 * 界面里所有颜色都来自 CSS 变量（globals.css 的 `:root` / `.dark`），
 * 所以改外观 = 改这几个变量，不需要重新渲染 React 树，也不需要任何状态同步。
 *
 * 与岚珠的差异只有两处：
 *   - 默认强调色是**樱坂雫的水蓝**（本项目叫 ShizukuFlow）
 *   - 默认**深色**主题（视频软件长盯波形和预览，深底更省力）
 */

export interface Hsl {
  h: number
  s: number
  l: number
}

export interface AccentPreset {
  id: string
  /** 角色名（虹ヶ咲学園スクールアイドル同好会） */
  name: string
  /** 该角色的应援色，HTML 色号 */
  color: string
}

/** 默认强调色：樱坂雫 */
export const DEFAULT_ACCENT = 'shizuku'

/** 默认主题：深色 */
export const DEFAULT_THEME = 'dark'

/**
 * 虹咲 13 人配色。
 *
 * 这些是角色应援色，直接当强调色用 —— 所以**不能**预设固定的
 * `--primary-foreground`：中须霞的黄、樱坂雫的水蓝、高咲侑的黑，
 * 白字黑字完全是两个答案，得按亮度实时算（见 `prefersDarkText`）。
 */
export const ACCENT_PRESETS: AccentPreset[] = [
  { id: 'ayumu', name: '上原步梦', color: '#ED7D95' },
  { id: 'kasumi', name: '中须霞', color: '#E7D600' },
  { id: 'shizuku', name: '樱坂雫', color: '#01B7ED' },
  { id: 'karin', name: '朝香果林', color: '#485EC6' },
  { id: 'ai', name: '宫下爱', color: '#FF5800' },
  { id: 'kanata', name: '近江彼方', color: '#A664A0' },
  { id: 'setsuna', name: '优木雪菜', color: '#D81C2F' },
  { id: 'emma', name: '艾玛·维尔德', color: '#84C36E' },
  { id: 'rina', name: '天王寺璃奈', color: '#9CA5B9' },
  { id: 'shioriko', name: '三船栞子', color: '#37B484' },
  { id: 'mia', name: '米娅·泰勒', color: '#A9A898' },
  { id: 'lanzhu', name: '钟岚珠', color: '#F69992' },
  { id: 'yu', name: '高咲侑', color: '#1D1D1D' },
]

/** `#rgb` / `#rrggbb` → HSL；非法输入返回 null。 */
export function hexToHsl(hex: string): Hsl | null {
  let v = hex.trim().replace(/^#/, '')
  if (v.length === 3) {
    v = v
      .split('')
      .map((c) => c + c)
      .join('')
  }
  if (!/^[0-9a-fA-F]{6}$/.test(v)) return null
  const n = parseInt(v, 16)
  const r = ((n >> 16) & 255) / 255
  const g = ((n >> 8) & 255) / 255
  const b = (n & 255) / 255
  const max = Math.max(r, g, b)
  const min = Math.min(r, g, b)
  const l = (max + min) / 2
  let h = 0
  let s = 0
  if (max !== min) {
    const d = max - min
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
    if (max === r) h = (g - b) / d + (g < b ? 6 : 0)
    else if (max === g) h = (b - r) / d + 2
    else h = (r - g) / d + 4
    h *= 60
  }
  return { h: Math.round(h), s: Math.round(s * 100), l: Math.round(l * 100) }
}

const hsl = (c: Hsl) => `hsl(${c.h} ${c.s}% ${c.l}%)`

/** HSL → 感知亮度（0-1）。用来决定强调色上该用黑字还是白字。 */
function luma(c: Hsl): number {
  const s = c.s / 100
  const l = c.l / 100
  const k = (n: number) => (n + c.h / 30) % 12
  const a = s * Math.min(l, 1 - l)
  const f = (n: number) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)))
  return 0.2126 * f(0) + 0.7152 * f(8) + 0.0722 * f(4)
}

/** 亮底用深字、暗底用白字 —— 中须霞的黄配白字是看不见的。 */
function prefersDarkText(c: Hsl) {
  return luma(c) > 0.62
}

const DARK_INK = 'hsl(220 22% 14%)'

/** 承载"主色"的全部变量。改这些就能让按钮、焦点环、进度条、选中态一起换色。 */
const ACCENT_VARS = ['--primary', '--ring'] as const

/**
 * 把强调色应用到 `<html>`：`customHex` 非空时优先于预设。
 *
 * 暗色下会把颜色提亮一点，否则深底上会糊成一团。
 */
export function applyAccent(presetId: string, customHex: string, dark: boolean) {
  const el = document.documentElement
  for (const v of ACCENT_VARS) el.style.removeProperty(v)
  el.style.removeProperty('--primary-foreground')

  const custom = customHex.trim()
  const hex = custom || ACCENT_PRESETS.find((p) => p.id === presetId)?.color || ''
  if (!hex) return

  const base = hexToHsl(hex)
  if (!base) return

  const c: Hsl = dark
    ? { h: base.h, s: Math.min(100, base.s + 8), l: Math.min(70, base.l + 16) }
    : base

  const value = hsl(c)
  for (const v of ACCENT_VARS) el.style.setProperty(v, value)
  el.style.setProperty('--primary-foreground', prefersDarkText(c) ? DARK_INK : 'hsl(0 0% 100%)')
}

/**
 * 主题：light / dark / system。返回最终生效的色板。
 *
 * 顺带让原生控件（滚动条、输入框、颜色选择器）跟着走。
 */
export function applyTheme(theme: string): 'light' | 'dark' {
  const prefersDark =
    typeof window !== 'undefined' && window.matchMedia
      ? window.matchMedia('(prefers-color-scheme: dark)').matches
      : false
  const dark = theme === 'dark' || (theme === 'system' && prefersDark)
  document.documentElement.classList.toggle('dark', dark)
  document.documentElement.style.colorScheme = dark ? 'dark' : 'light'
  return dark ? 'dark' : 'light'
}

/** 界面缩放。Chromium 的 `zoom` 会连布局视口一起缩放，所以整体比例是安全的。 */
export function applyUiScale(scale: number) {
  const z = Number.isFinite(scale) && scale > 0.5 && scale < 1.6 ? scale : 1
  document.documentElement.style.zoom = z === 1 ? '' : String(z)
}

/**
 * 当前强调色的十六进制值。
 *
 * 时间轴与预览是 canvas 画的，拿不到 CSS 变量，
 * 所以要把 `hsl(h s% l%)` 反解成 `RRGGBB` —— 这样波形颜色会自动跟随主题。
 */
export function accentHex(fallback = '01B7ED') {
  if (typeof window === 'undefined') return fallback
  const raw = getComputedStyle(document.documentElement).getPropertyValue('--primary').trim()
  const m = raw.match(/hsl\(\s*([\d.]+)\s*[, ]\s*([\d.]+)%\s*[, ]\s*([\d.]+)%/)
  if (!m) return fallback
  const h = Number(m[1])
  const s = Number(m[2]) / 100
  const l = Number(m[3]) / 100
  const c = (1 - Math.abs(2 * l - 1)) * s
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1))
  const mm = l - c / 2
  const seg = Math.floor(h / 60) % 6
  const rgb: [number, number, number] =
    seg === 0
      ? [c, x, 0]
      : seg === 1
        ? [x, c, 0]
        : seg === 2
          ? [0, c, x]
          : seg === 3
            ? [0, x, c]
            : seg === 4
              ? [x, 0, c]
              : [c, 0, x]
  return rgb
    .map((v) => Math.round((v + mm) * 255).toString(16).padStart(2, '0'))
    .join('')
    .toUpperCase()
}

/** `#rrggbb` → [r, g, b]；解析不了返回 null。 */
export function hexToRgb(hex: string): [number, number, number] | null {
  let v = hex.trim().replace(/^#/, '')
  if (v.length === 3) {
    v = v
      .split('')
      .map((c) => c + c)
      .join('')
  }
  if (!/^[0-9a-fA-F]{6}$/.test(v)) return null
  const n = parseInt(v, 16)
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255]
}

/** [r, g, b] → `#RRGGBB`（越界自动夹住）。 */
export function rgbToHex(rgb: [number, number, number]) {
  const c = (v: number) => Math.max(0, Math.min(255, Math.round(v)))
  return `#${((c(rgb[0]) << 16) | (c(rgb[1]) << 8) | c(rgb[2])).toString(16).padStart(6, '0').toUpperCase()}`
}

/** 给某个底色挑一个能看清的文字色（色块上的勾、对比色判断都用得上）。 */
export function inkOn(hex: string) {
  const h = hexToHsl(hex)
  if (!h) return '#FFFFFF'
  return prefersDarkText(h) ? '#1D1D1D' : '#FFFFFF'
}
