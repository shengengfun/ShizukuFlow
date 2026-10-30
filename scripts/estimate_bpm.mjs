#!/usr/bin/env node
/**
 * estimate_bpm.mjs —— 从音频估算 BPM（自相关法）。
 *
 * 为什么需要它：切镜节奏由 BPM 决定。BPM 猜错半拍，整片都会"踩不准"，
 * 比没有节拍更难看。与其拍脑袋填一个数，不如从音频算。
 *
 * 做法：
 *   1. ffmpeg 解码成 8kHz 单声道 s16le
 *   2. 每 10ms 算一次 RMS → 能量包络
 *   3. 取正向差分得到 onset 强度
 *   4. 对 onset 做自相关，在 60~200 BPM 范围找峰
 *
 * 用法：
 *   node scripts/estimate_bpm.mjs <音频文件> [分析秒数，默认 90]
 *
 * 输出的是候选值（含倍频），最终由人确认 —— 自相关分不清 70 和 140。
 */
import { spawnSync } from 'node:child_process'

const ROOT = new URL('..', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')
const FFMPEG = `${ROOT}/app/src-tauri/tools/ffmpeg.exe`

const file = process.argv[2]
const seconds = +(process.argv[3] || 90)
if (!file) {
  console.error('用法: node scripts/estimate_bpm.mjs <音频文件> [秒数]')
  process.exit(2)
}

const SR = 8000
const HOP = 80 // 10ms

const r = spawnSync(
  FFMPEG,
  ['-v', 'error', '-i', file, '-t', String(seconds), '-ac', '1', '-ar', String(SR), '-f', 's16le', '-'],
  { maxBuffer: 512 * 1024 * 1024 },
)
if (r.status !== 0 || !r.stdout?.length) {
  console.error('解码失败：', String(r.stderr || '').slice(0, 400))
  process.exit(1)
}

const buf = r.stdout
const total = Math.floor(buf.length / 2)

// ── 能量包络 ──
const env = []
for (let i = 0; i + HOP <= total; i += HOP) {
  let s = 0
  for (let j = 0; j < HOP; j++) {
    const v = buf.readInt16LE((i + j) * 2) / 32768
    s += v * v
  }
  env.push(Math.sqrt(s / HOP))
}

// ── onset 强度：正向差分（只保留"能量上升"）──
const on = new Float64Array(env.length)
for (let i = 1; i < env.length; i++) on[i] = Math.max(0, env[i] - env[i - 1])

// 去均值，避免低频漂移主导自相关
let mean = 0
for (const v of on) mean += v
mean /= on.length
for (let i = 0; i < on.length; i++) on[i] -= mean

// ── 自相关 ──
const framesPerSec = 1000 / 10
const lagMin = Math.round((60 / 200) * framesPerSec)
const lagMax = Math.round((60 / 60) * framesPerSec)
const scores = []
for (let lag = lagMin; lag <= lagMax; lag++) {
  let c = 0
  const n = on.length - lag
  for (let i = 0; i < n; i++) c += on[i] * on[i + lag]
  scores.push({ lag, bpm: (60 * framesPerSec) / lag, score: c / n })
}
scores.sort((a, b) => b.score - a.score)

// 倍频归并：同一条节奏会在 1x / 2x / 0.5x 都出峰，只保留一组
const picked = []
for (const s of scores) {
  const dup = picked.some((p) => {
    const ratio = s.bpm / p.bpm
    return Math.abs(ratio - 1) < 0.035 || Math.abs(ratio - 2) < 0.06 || Math.abs(ratio - 0.5) < 0.03
  })
  if (!dup) picked.push(s)
  if (picked.length >= 4) break
}

console.log(`文件   ${file}`)
console.log(`分析   ${seconds}s @ ${SR}Hz  帧数 ${env.length}`)
console.log('候选 BPM（自相关峰，需人判断哪个是真正的拍）：')
for (const p of picked) {
  const conf = ((p.score / picked[0].score) * 100).toFixed(0)
  console.log(`  ${p.bpm.toFixed(1).padStart(6)}  相对强度 ${conf}%`)
}
console.log('\n提示：Vocaloid / 术曲常见 130~180。若最大峰是 2x，取一半往往才是主拍。')
