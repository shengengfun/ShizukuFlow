import { clsx } from 'clsx'
import type { ReactNode } from 'react'

/* ------------------------------------------------------------------ *
 * 基础控件集
 *
 * 只做「AE / Vegas 里长什么样」这一件事，不含业务语义。
 * 所有输入控件都走同一套 22px 高度 —— 混排时不会出现 21/23px 的参差，
 * 这是专业软件和网页最容易被一眼看穿的差别。
 * ------------------------------------------------------------------ */

export function Panel({
  title,
  right,
  children,
  className,
  bodyClass,
  scroll = true,
}: {
  title?: ReactNode
  right?: ReactNode
  children: ReactNode
  className?: string
  bodyClass?: string
  scroll?: boolean
}) {
  return (
    <div className={clsx('pnl', className)}>
      {title !== undefined && (
        <div className="pnl-head">
          <span className="truncate">{title}</span>
          <div className="flex-1" />
          {right}
        </div>
      )}
      <div className={clsx(scroll ? 'pnl-body' : 'flex min-h-0 flex-1 flex-col', bodyClass)}>
        {children}
      </div>
    </div>
  )
}

export function Field({
  label,
  children,
  hint,
}: {
  label: ReactNode
  children: ReactNode
  hint?: string
}) {
  return (
    <label className="field" title={hint}>
      <span className="lbl">{label}</span>
      <div className="min-w-0">{children}</div>
    </label>
  )
}

export function NumInput({
  value,
  onChange,
  step = 1,
  min,
  max,
  suffix,
  disabled,
  title,
}: {
  value: number
  onChange: (v: number) => void
  step?: number
  min?: number
  max?: number
  suffix?: string
  disabled?: boolean
  title?: string
}) {
  return (
    <div className="relative">
      <input
        className="inp"
        type="number"
        value={Number.isFinite(value) ? value : 0}
        step={step}
        min={min}
        max={max}
        disabled={disabled}
        title={title}
        onChange={(e) => {
          const v = Number(e.target.value)
          if (Number.isFinite(v)) onChange(clamp(v, min, max))
        }}
        // 滚轮微调是专业软件的肌肉记忆（AE 全部数值框都吃滚轮）
        onWheel={(e) => {
          if (disabled || document.activeElement !== e.currentTarget) return
          e.preventDefault()
          onChange(clamp(value + (e.deltaY < 0 ? step : -step), min, max))
        }}
      />
      {suffix && (
        <span className="text-muted-foreground pointer-events-none absolute top-1/2 right-1.5 -translate-y-1/2 text-[10px]">
          {suffix}
        </span>
      )}
    </div>
  )
}

export function TextInput({
  value,
  onChange,
  placeholder,
  mono,
  title,
}: {
  value: string
  onChange: (v: string) => void
  placeholder?: string
  mono?: boolean
  title?: string
}) {
  return (
    <input
      className={clsx('inp', !mono && 'font-[inherit]')}
      style={mono ? undefined : { fontFamily: 'var(--font-sans)' }}
      value={value}
      placeholder={placeholder}
      title={title}
      onChange={(e) => onChange(e.target.value)}
    />
  )
}

export function Select<T extends string>({
  value,
  onChange,
  options,
  title,
  disabled,
}: {
  value: T
  onChange: (v: T) => void
  options: { value: T; label: string; disabled?: boolean }[]
  title?: string
  disabled?: boolean
}) {
  return (
    <select
      className="inp"
      value={value}
      title={title}
      disabled={disabled}
      onChange={(e) => onChange(e.target.value as T)}
    >
      {options.map((o) => (
        <option key={o.value} value={o.value} disabled={o.disabled}>
          {o.label}
        </option>
      ))}
    </select>
  )
}

export function Segmented<T extends string | number>({
  value,
  onChange,
  options,
  title,
}: {
  value: T
  onChange: (v: T) => void
  options: { value: T; label: string; title?: string }[]
  title?: string
}) {
  return (
    <div className="seg w-full" title={title}>
      {options.map((o) => (
        <button
          key={String(o.value)}
          type="button"
          data-on={o.value === value}
          title={o.title}
          onClick={() => onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}

export function Slider({
  value,
  onChange,
  min,
  max,
  step = 1,
}: {
  value: number
  onChange: (v: number) => void
  min: number
  max: number
  step?: number
}) {
  return (
    <input
      className="rng"
      type="range"
      min={min}
      max={max}
      step={step}
      value={value}
      onChange={(e) => onChange(Number(e.target.value))}
    />
  )
}

export function IconBtn({
  children,
  onClick,
  title,
  on,
  disabled,
  className,
}: {
  children: ReactNode
  onClick?: () => void
  title: string
  on?: boolean
  disabled?: boolean
  className?: string
}) {
  return (
    <button
      type="button"
      title={title}
      disabled={disabled}
      onClick={onClick}
      data-on={on}
      className={clsx(
        'grid h-[18px] w-[18px] place-items-center rounded-[3px] transition-colors',
        on ? 'text-primary' : 'text-muted-foreground hover:text-foreground',
        'hover:bg-[color-mix(in_srgb,var(--foreground)_12%,transparent)] disabled:opacity-40',
        className,
      )}
    >
      {children}
    </button>
  )
}

/** 分区小标题（检查器里把一堆字段分组） */
export function Section({
  title,
  children,
  right,
}: {
  title: string
  children: ReactNode
  right?: ReactNode
}) {
  return (
    <div className="border-line border-b last:border-b-0">
      <div className="text-muted-foreground bg-[color-mix(in_srgb,var(--foreground)_4%,transparent)] flex h-[20px] items-center px-2.5 text-[10.5px] font-semibold tracking-wider uppercase">
        {title}
        <div className="flex-1" />
        {right}
      </div>
      <div className="py-1">{children}</div>
    </div>
  )
}

export function clamp(v: number, min?: number, max?: number) {
  if (min != null && v < min) return min
  if (max != null && v > max) return max
  return v
}

/** `mm:ss:ff` —— 编辑软件用帧时间码，比毫秒更贴近"一帧"的手感 */
export function frameTime(sec: number, fps: number): string {
  const s = Math.max(0, sec)
  const total = Math.round(s * fps)
  const f = total % Math.max(1, Math.round(fps))
  const whole = Math.floor(total / Math.max(1, Math.round(fps)))
  const m = Math.floor(whole / 60)
  const ss = whole % 60
  return `${String(m).padStart(2, '0')}:${String(ss).padStart(2, '0')}:${String(f).padStart(2, '0')}`
}
