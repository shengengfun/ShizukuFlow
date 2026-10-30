#!/usr/bin/env node
/**
 * fetch_lyrics.mjs —— 取同步歌词（.lrc）落到 projects/，给 K 轴用。
 *
 * 两个来源，先 LRCLIB（开放 API、纯时间轴最干净），再退网易云（收录更全）：
 *   node scripts/fetch_lyrics.mjs            # 取内置两首
 *   node scripts/fetch_lyrics.mjs --force    # 覆盖已有 .lrc
 *   node scripts/fetch_lyrics.mjs --only 余韻
 *
 * 产物被 gen_homework.mjs 直接消费（projects/<同名>.lrc）。
 * 仅供本地研究/学习：脚本只把结果写进本地文件，不打印歌词正文。
 */
import { writeFileSync, existsSync, mkdirSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..')
const OUT = join(ROOT, 'projects')

const JOBS = [
  { file: '妄想感傷代償連盟.sf', track: '妄想感傷代償連盟', artist: 'DECO*27', aliases: ['DECO*27', 'DECO27'], duration: 270.0 },
  { file: '余韻.sf', track: '余韻', artist: '小泉萌香', aliases: ['小泉萌香', 'Koizumi Moeka'], duration: 213.37 },
  // 有网易云 id 时直接按 id 取，跳过搜索（同名曲太多，搜索容易命中翻唱）
  { file: '私は雨.sf', track: '私は雨', artist: '東雲絵名', aliases: ['東雲絵名', 'アヤメミク'], duration: 123.72, id: 3360865320 },
]

const args = process.argv.slice(2)
const force = args.includes('--force')
const onlyIdx = args.indexOf('--only')
const only = onlyIdx >= 0 ? args[onlyIdx + 1] : null

const UA = 'ShizukuFlow/0.1 (research; local use only)'
const NETEASE_H = {
  'User-Agent':
    'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36',
  Referer: 'https://music.163.com/',
  Cookie: 'appver=2.0.2',
}

/** 带超时与重试的 JSON GET（这台机器到境外 CDN 偶发连不上）。 */
async function jget(url, headers, tries = 3) {
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(url, { headers, signal: AbortSignal.timeout(15000) })
      if (!r.ok) return { err: `HTTP ${r.status}` }
      return await r.json()
    } catch (e) {
      if (i === tries - 1) return { err: e.message }
    }
  }
}

// ── 来源 1：LRCLIB ──────────────────────────────────────────────
async function fromLrclib(job) {
  const qs = new URLSearchParams({ track_name: job.track, artist_name: job.artist })
  let list = await jget(`https://lrclib.net/api/search?${qs}`, { 'User-Agent': UA })
  if (Array.isArray(list) && list.length === 0) {
    list = await jget(`https://lrclib.net/api/search?q=${encodeURIComponent(`${job.track} ${job.artist}`)}`, { 'User-Agent': UA })
  }
  if (!Array.isArray(list)) return null
  let best = null
  let bestScore = -Infinity
  for (const it of list) {
    if (!it.syncedLyrics) continue
    let s = 0
    if (it.trackName === job.track) s += 40
    else if (it.trackName?.includes(job.track)) s += 15
    if (job.aliases.some((a) => (it.artistName ?? '').includes(a))) s += 20
    s -= Math.min(30, Math.abs((it.duration ?? 0) - job.duration) / 2)
    if (s > bestScore) { bestScore = s; best = it }
  }
  if (!best) return null
  return { lrc: best.syncedLyrics, note: `LRCLIB「${best.trackName}」/${best.artistName} ${(best.duration ?? 0).toFixed(1)}s` }
}

// ── 来源 2：网易云（收录更全，同步轴够用）──────────────────────
async function fromNetease(job) {
  // 已知歌曲 id：直接取歌词，不做搜索
  if (job.id) {
    const lyr = await jget(`https://music.163.com/api/song/lyric?id=${job.id}&lv=1&kv=1&tv=-1`, NETEASE_H)
    const lrc = lyr?.lrc?.lyric
    if (lrc && /\[\d{1,2}:\d{2}/.test(lrc)) return { lrc, note: `网易云 id=${job.id}` }
    return null
  }
  const s = await jget(
    `https://music.163.com/api/search/get/web?s=${encodeURIComponent(`${job.track} ${job.artist}`)}&type=1&offset=0&limit=10`,
    NETEASE_H,
  )
  const songs = s?.result?.songs ?? []
  if (!songs.length) return null
  // 时长最接近的当命中（同名曲翻唱很多，时长是最稳的指纹）
  const hit = songs
    .map((it) => ({ it, d: Math.abs(it.duration / 1000 - job.duration) }))
    .filter((x) => x.d < 12)
    .sort((a, b) => a.d - b.d)[0]
  if (!hit) return null
  const lyr = await jget(`https://music.163.com/api/song/lyric?id=${hit.it.id}&lv=1&kv=1&tv=-1`, NETEASE_H)
  const lrc = lyr?.lrc?.lyric
  if (!lrc || !/\[\d{1,2}:\d{2}/.test(lrc)) return null
  return {
    lrc,
    note: `网易云「${hit.it.name}」/${(hit.it.artists ?? []).map((a) => a.name).join('&')} ${(hit.it.duration / 1000).toFixed(1)}s`,
  }
}

// ── 清洗：去掉制作信息行（作词/作曲/编曲…），保留纯歌词行 ──────────
const CREDIT = /^(作词|作詞|作曲|编曲|編曲|混音|母带|母帶|制作人|製作人|录音|錄音|和声|和聲|吉他|贝斯|貝斯|鼓|弦乐|弦樂|监制|監製|出品|发行|發行|Lyrics?|Composer|Arranger|Mixing|Mastering|Producer|Guitar|Bass|Drums|Vocals?|Chorus)\s*[:：]/i

function clean(lrc, dur) {
  const keep = []
  for (const line of lrc.split(/\r?\n/)) {
    const text = line.replace(/\[[^\]]*\]/g, '').trim()
    if (!text) continue
    if (CREDIT.test(text)) continue
    if (/^(作词|作曲|编曲|Lyrics|Composer)\s*$/i.test(text)) continue
    keep.push(line.trim())
  }
  return keep.join('\n') + '\n'
}

const stampCount = (lrc) => (lrc.match(/\[\d{1,2}:\d{2}([.:]\d{1,3})?\]/g) || []).length

mkdirSync(OUT, { recursive: true })

for (const job of JOBS) {
  if (only && !job.file.includes(only)) continue
  const target = join(OUT, job.file.replace(/\.sf$/, '.lrc'))
  if (existsSync(target) && !force) {
    console.log(`- ${job.track}：已有 .lrc，跳过（--force 覆盖）`)
    continue
  }
  process.stdout.write(`- ${job.track}：`)
  const hit = job.id ? await fromNetease(job) : (await fromLrclib(job)) ?? (await fromNetease(job))
  if (!hit) {
    console.log('两个来源都没找到同步歌词')
    continue
  }
  const body = clean(hit.lrc, job.duration)
  writeFileSync(target, body, 'utf8')
  console.log(`OK ${stampCount(body)} 行 ← ${hit.note}`)
}
