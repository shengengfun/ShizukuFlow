/**
 * 生成《妄想感傷代償連盟》/《余韻》两份 .sf 工程 —— 术曲PV语法 v3：「一屏一戏」。
 *
 * 对照真实参考（vocaloid-style-mv-pipeline《萤火》/《残光》分镜、JIZURA 预览）确立的语法：
 *   - 每句歌词一次全新的构图：角落小字+光点 / 居中 / 巨型字占满全屏 / 竖排 —— 版式不重复
 *   - 字即画面：巨型♪、发光、描边、重影拖尾
 *   - 极简克制：大段近黑底 + 一行字 + 一个光点；装饰只做点缀，不做常驻铺满
 *   - 副歌爆发：整屏反色 + punch 冲镜 + 闪白硬切
 *   - 标题卡是海报：大字 + UI 注记（BPM/规格）当装饰
 *   - 片尾署名卷动
 *
 * 歌词：存在 projects/<同名>.lrc 则导入（逐字均分 K 轴 + 竖排构图可用），
 * 否则 ♪ 占位。间奏卡由 ffmpeg silencedetect 从真实音频提取。
 *
 * 用法：node scripts/gen_homework.mjs
 */

import { writeFileSync, mkdirSync, existsSync, readFileSync } from 'node:fs'
import { spawnSync } from 'node:child_process'

const ROOT = 'D:/Project/ShizukuFlow'
const OUT = `${ROOT}/projects`
const FFMPEG = `${ROOT}/app/src-tauri/tools/ffmpeg.exe`
mkdirSync(OUT, { recursive: true })

// ─────────────────────────── .lrc 解析 ───────────────────────────

function parseLrc(file, audioDur) {
  const raw = readFileSync(file, 'utf8').split(/\r?\n/)
  const out = []
  const re = /\[(\d{1,2}):(\d{1,2})(?:[.:](\d{1,3}))?\]/g
  for (const line of raw) {
    const tags = [...line.matchAll(re)].map((m) => {
      const frac = m[3] ? +(`0.${m[3].padEnd(3, '0').slice(0, 3)}`) : 0
      return +m[1] * 60 + +m[2] + frac
    })
    const text = line.replace(re, '').trim()
    if (tags.length === 0 || !text) continue
    for (const s of tags) out.push({ start: s, text })
  }
  out.sort((a, b) => a.start - b.start)
  const lines = []
  for (let i = 0; i < out.length; i++) {
    const start = out[i].start
    const end = Math.min(out[i + 1]?.start ?? audioDur, start + 12, audioDur)
    if (end - start < 0.15) continue
    lines.push({ start, end, text: out[i].text })
  }
  return lines
}

function splitSyllables(line) {
  const chars = Array.from(line.text)
  if (chars.length <= 1) return [{ text: line.text, start: line.start, end: line.end }]
  const dur = line.end - line.start
  const unit = dur / chars.length
  return chars.map((c, i) => ({
    text: c,
    start: line.start + unit * i,
    end: line.start + unit * (i + 1) + (i === chars.length - 1 ? unit * 0.5 : 0),
  }))
}

// ─────────────────────────── 静音检测 ───────────────────────────

function detectSilences(audioPath, audioDur) {
  try {
    const r = spawnSync(FFMPEG, [
      '-i', audioPath,
      '-af', 'silencedetect=noise=-30dB:d=1.5',
      '-f', 'null', '-',
    ], { encoding: 'utf8', timeout: 120000 })
    const err = `${r.stderr ?? ''}`
    const starts = [...err.matchAll(/silence_start:\s*([\d.]+)/g)].map((m) => +m[1])
    const ends = [...err.matchAll(/silence_end:\s*([\d.]+)/g)].map((m) => +m[1])
    const sil = []
    for (let i = 0; i < starts.length; i++) {
      const s = starts[i]
      const e = ends[i] ?? audioDur
      if (e - s >= 1.5) sil.push([s, e])
    }
    return sil
  } catch {
    return []
  }
}

// ─────────────────────────── 构图库（一屏一戏） ───────────────────────────

/**
 * 横排系构图轮换：同一首歌里版式不重复。
 *  center   居中常规（trackIn 入场）
 *  quiet    角落小字 + 光点（极简谦让）
 *  small    居中小字偏下
 *  giant    巨型字占满画面（爆发）
 *  vertical 竖排（仅 lrc 多字时）
 */
const MODE_CYCLE = ['center', 'quiet', 'small', 'giant', 'center', 'quiet', 'vertical', 'giant']

function layoutMode(seq, kind, hasMultiChar) {
  // 副歌不再整段巨型字：连着四五个满屏巨字会迅速疲劳，冲击力反而被稀释。
  // 改成巨字与居中大字交替，重音才落在该落的地方。
  if (kind === 'C')
    return seq % 3 === 0 ? { mode: 'giant', sizeScale: 2.4 } : { mode: 'center', sizeScale: 1.35 }
  if (kind === 'I') return { mode: 'center', sizeScale: 0.9 }
  const m = MODE_CYCLE[seq % MODE_CYCLE.length]
  if (m === 'vertical' && !hasMultiChar) return { mode: 'quiet', sizeScale: 1 }
  if (m === 'giant') return { mode: 'giant', sizeScale: 2.2 }
  if (m === 'small') return { mode: 'small', sizeScale: 0.62 }
  return { mode: m, sizeScale: 1 }
}

/**
 * 背景语汇按段落轮换（同一段内相邻镜头不重样），避免"整片一个底色"。
 * 暗场段走 bokeh/grid/noise/stripes/aurora；副歌反色段走纸面/网点/斜条。
 */
const BG_CYCLE = {
  A: ['bokeh', 'grid', 'noise', 'stripes', 'aurora'],
  B: ['aurora', 'halftone', 'rays', 'scanlines'],
  C: ['paper', 'halftone', 'stripes'],
  I: ['rays', 'grid', 'aurora'],
}

/** 每种模式的镜头编排：背景、运镜、转场、装饰（克制）。 */
function shotDesign(mode, kind, seq, job, portrait = null, grp = null) {
  const A = job.accent
  const pool = BG_CYCLE[kind] || BG_CYCLE.A
  const bgKind = pool[seq % pool.length]
  const seed = 1000 + seq * 13

  let bgParams
  if (kind === 'C') {
    const pal = job.chorusPalettes[seq % job.chorusPalettes.length]
    // 反色段：浅底 + 深墨色图形（acent 用墨色，网点/斜条才看得见而不脏）
    bgParams = { kind: bgKind, color: pal[0], color2: pal[1], accent: job.chorusText, seed, angle: 100 }
  } else {
    const dark = seq % 3 === 0 ? job.darkPalettes[seq % job.darkPalettes.length] : null
    bgParams = {
      kind: bgKind,
      color: dark ? dark[0] : '#060609',
      color2: dark ? dark[1] : '#0b0a12',
      accent: A,
      seed,
      angle: 95,
    }
  }
  const bg = [{ part: 'background', params: bgParams }]
  // 底图：压一层图片当画布，文字仍在它之上（background 层最先绘制）。
  // 素材按段落分组：主歌用雨夜/城市（暗），发展段用和风/水面，副歌用浅纸（反色段要浅底）。
  // 不同素材亮度差得多，同一 alpha 会让深色水墨等于没加、亮色雨窗又盖住字 —— 按素材定。
  const PLATE_ALPHA = {
    'bg_rain_window.png': 0.42,
    'bg_ink_wash.png': 0.7,
    'bg_wet_street.png': 0.52,
    'bg_ripples.png': 0.55,
    'bg_shoji.png': 0.45,
    'bg_city.png': 0.6,
    'bg_washi.png': 0.3,
  }
  // 真场景底图：铺满画幅的一张插画（和室/走廊/学园/晚霞），
  // 比"渐变 + 薄薄一层贴图"更接近 PV 该有的样子。副歌仍走浅纸反色段，
  // 一场戏是全片唯一的"抽离"时刻，全用照片会把那个对比抹平。
  const sceneList = (job.scenes && job.scenes[kind]) || []
  const scene = sceneList.length ? sceneList[seq % sceneList.length] : null
  if (scene) {
    bg.push({
      part: 'image',
      params: {
        path: `../assets/${scene}`,
        fit: 'cover',
        w: 1.04,
        h: 1.04,
        alpha: 1,
      },
    })
  }

  // 场景图之后的压暗衬底：亮背景上白歌词会糊成一片，尤其走廊/晚霞这种高光段。
  // 只压歌词那一侧（左→右渐隐），人物站的位置基本不受影响。
  if (scene) {
    bg.push({
      part: 'background',
      params: {
        kind: 'gradient',
        color: 'rgba(0,0,0,0.40)',
        color2: 'rgba(0,0,0,0)',
        accent: '#000000',
        seed: 11,
        angle: 0,
      },
    })
  }

  const platePool = scene ? [] : (job.platePools && job.platePools[kind]) || []
  if (platePool.length) {
    const plate = platePool[seq % platePool.length]
    bg.push({
      part: 'image',
      params: {
        path: `../assets/${plate}`,
        fit: 'cover',
        w: 1.06,
        h: 1.06,
        alpha: PLATE_ALPHA[plate] ?? 0.5,
      },
    })
  }

  // 角色立绘：排在底图之后（数组按序绘制，立绘要压在照片上、又在歌词之下）。
  // at/outAt 先留空，等全部镜头生成完再按"组"回填绝对时间，入场只发生一次。
  if (portrait) {
    // 允许按段位不同配置：副歌（浅纸反色）上人物要收小一档、降一点不透明度，
    // 否则亮色和服在浅底上会像贴纸；暗段则可以站满画幅。
    const p = typeof portrait === 'string' ? { file: portrait } : portrait
    bg.push({
      part: 'portrait',
      params: {
        path: `../assets/${p.file}`,
        x: p.x ?? 0.775,
        y: 1.0,
        h: p.h ?? 0.96,
        alpha: p.alpha ?? 1,
        sway: 0.005, swaySpeed: 0.5,
        beat: 0.010,
        shadow: 0.8,
        glow: p.glow ?? 0.3,
        glowColor: job.portraitGlow ?? '#9fe0ff',
        // 染色把人和景缝在一起：原图是中性冷调，站进夕阳走廊里不打光就像贴纸
        ...(p.tint ? { tint: p.tint, tintAmount: p.tintAmount ?? 0.16 } : {}),
        seed: 0.6,
        grp,
      },
    })
  }

  const camera = mode === 'giant'
    ? { zoom: [1.14, 1.0], punch: kind === 'C' ? 0.06 : 0 }
    : mode === 'quiet'
      ? { zoom: [1.0, 1.03] }
      : { zoom: [1.02, 1.0] }

  // ── 分层转场 ──
  // 全片一套 fade 会让每一刀都变成 PPT 翻页。按段落配重：
  //   副歌进 = flash 硬闪（前一秒还是暗底，闪白切进反色段冲击力最大）
  //   副歌出 = 慢 fade 0.45（爆发之后要收得住，硬切会把余韵砍断）
  //   巨字   = wipe 擦除，方向跟运镜走：推镜(zoom 1.14→1.0)从右擦入，拉镜从左擦入
  //   主歌   = 短 fade 0.16；安静段放慢到 0.3，跟极简画面一起呼吸
  const pullBack = (camera.zoom?.[0] ?? 1) - (camera.zoom?.[1] ?? 1) > 0.02
  // 擦除方向跟运镜同向：拉镜(1.14→1.0)黑幕向左退、画面自右露；推镜反之。
  // 进/出两刀朝同一侧扫，前后才连得上，不会一刀向合一刀向分。
  const wipeDir = pullBack ? 'right' : 'left'
  const wipeBack = pullBack ? 'left' : 'right'
  const transition = kind === 'C'
    ? { in: 'flash', inDur: 0.22, color: '#ffffff' }
    : mode === 'giant'
      ? { in: 'wipe', inDur: 0.26, dir: wipeDir, out: 'wipe', outDur: 0.18, outDir: wipeBack, color: '#000000' }
      : mode === 'quiet'
        ? { in: 'fade', inDur: 0.3 }
        : { in: 'fade', inDur: 0.16 }

  const decor = []
  if (mode === 'giant' && kind !== 'C') {
    decor.push({ part: 'glowDot', params: { x: 0.16, y: 0.22, r: 5, color: A } })
  }
  if (kind === 'C') {
    decor.push({ part: 'beatRings', params: { every: 4, color: A, maxR: 0.85, width: 3, life: 1.8 } })
    decor.push({ part: 'frameLines', params: { margin: 0.045, color: job.chorusFrameColor, width: 3 } })
  } else if (mode === 'vertical' || seq % 4 === 1) {
    // 竖排/轮换位加细线画框，给"版式感"
    decor.push({ part: 'frameLines', params: { margin: 0.055, color: `rgba(255,255,255,0.14)`, width: 1.4 } })
  }
  if (mode === 'quiet') {
    decor.push({ part: 'glowDot', params: { x: 0.86, y: 0.3, r: 4, color: 'rgba(255,255,255,0.9)' } })
  }

  // 叠层：胶片颗粒（暗场加质感；反色段纸面已有颗粒，跳过）
  const overlay = kind === 'C'
    ? []
    : [{ part: 'overlay', params: { kind: 'grain', amount: 420, alpha: 0.045 } }]

  const entrance = mode === 'giant'
    ? { style: 'popUp', dur: 0.32 }
    : mode === 'quiet' || mode === 'small'
      ? null
      : { style: 'trackIn', dur: 0.3 }

  return { bg, camera, transition, decor, overlay, entrance }
}

function makeShot(id, mode, kind, seq, start, end, lyricRefs, job, hasMultiChar, extraText = [],
                  portrait = null, grp = null) {
  const d = shotDesign(mode, kind, seq, job, portrait, grp)
  const shot = {
    id,
    start,
    end,
    track: 0,
    lyric_refs: lyricRefs,
    layers: {
      background: d.bg,
      text: extraText,
      decor: d.decor,
      overlay: d.overlay,
    },
    motion: {
      entrance: d.entrance ? { part: 'lyricEntrance', params: d.entrance } : null,
      hold: lyricRefs.length
        ? {
            part: 'lyricLayout',
            params: {
              ...layoutMode(seq, kind, hasMultiChar),
              // 小字/安静段轻轻歪一点：整行一起转，像手写批注，不是逐字乱抖
              ...(mode === 'quiet' ? { x: 0.07, y: 0.8, rotate: seq % 2 ? -4.5 : 3.5 } : {}),
              ...(mode === 'small' ? { y: 0.66, rotate: seq % 2 ? 2.5 : -3.5 } : {}),
            },
          }
        : null,
      exit: null,
      camera: { part: 'camera', params: d.camera },
    },
    transition: { part: 'transition', params: d.transition },
    seed: 1000 + seq * 7919 + kind.charCodeAt(0),
    locked: false,
  }

  // 巨字段改用关键帧运镜：两段式 zoom 只能匀速推，观感像"镜头在拉伸"；
  // 三个点（起点压住 → 中段推到底 → 收尾回一点）才有"推进去再稳住"的手感。
  if (mode === 'giant') {
    const mid = start + (end - start) * 0.62
    shot.motion.camera.params = {
      keys: [
        { t: start, zoom: 1.14, x: 0.5, y: 0.5 },
        { t: mid, zoom: 1.02, x: kind === 'C' ? 0.54 : 0.5, y: 0.47 },
        { t: end, zoom: 1.0, x: 0.5, y: 0.5 },
      ],
      punch: kind === 'C' ? 0.06 : 0,
    }
  }

  // 有立绘的镜头，歌词让到左半边：人物站右侧，字压脸上就废了。
  // 居中排 + 收一档字号，长句也不会顶到人物身上。
  if (portrait && shot.motion.hold) {
    shot.motion.hold.params = {
      ...shot.motion.hold.params,
      mode: 'center',
      sizeScale: 0.95,
      x: 0.325,
      y: 0.54,
    }
  }
  return shot
}

// ─────────────────────────── 主生成 ───────────────────────────

const gen = (job) => {
  const { title, artist, audioPath, audioDuration, bpm, fileName } = job
  const bar = (4 * 60) / bpm
  const D = audioDuration

  const lrcFile = `${OUT}/${fileName.replace(/\.sf$/, '.lrc')}`
  const hasLrc = existsSync(lrcFile)
  const lrcLines = hasLrc ? parseLrc(lrcFile, D) : []
  const silences = detectSilences(audioPath, D)
  const interludes = silences
    .filter(([s, e]) => s > 20 && e < D - 12)
    .map(([s, e]) => [Math.max(0, s - 0.3), Math.min(D, e + 0.3)])
  const hasMultiChar = lrcLines.length > 0

  const lyrics = []
  const shots = []
  let shotN = 0
  let lyricN = 0
  const nextId = (p) => `${p}${String(++shotN).padStart(3, '0')}`

  // ── 片头卡：海报式 —— 大字标题 + 发光点 + BPM/规格 UI 注记 ──
  const introEnd = lrcLines.length
    ? Math.min(Math.max(lrcLines[0].start, 2 * bar), 8 * bar)
    : 4 * bar
  shots.push({
    id: nextId('S'),
    start: 0,
    end: introEnd,
    track: 0,
    lyric_refs: [],
    layers: {
      background: [{ part: 'background', params: { kind: 'aurora', color: '#07070b', color2: '#140a18', accent: job.accent, seed: 91, angle: 100 } }],
      text: [
        {
          part: 'text',
          params: {
            text: title, x: 0.5, y: 0.46, size: 108, weight: 900, fill: '#ffffff',
            stroke: { color: '#000000', width: 2 },
            shadow: { blur: 26, color: '#000000cc', offset: [0, 6] },
            spacing: 10,
            ops: [{ name: 'typewriter', start: 0.35, end: 0.35 + title.length * 0.1 + 0.3, params: { stagger: 0.1, duration: 0.26 } }],
          },
        },
        { part: 'text', params: { text: artist, x: 0.5, y: 0.6, size: 26, weight: 500, fill: 'rgba(255,255,255,0.6)', spacing: 6 } },
        { part: 'text', params: { text: `BPM ${bpm}`, x: 0.06, y: 0.9, size: 16, weight: 500, fill: 'rgba(255,255,255,0.4)', align: 'left' } },
        { part: 'text', params: { text: '1280x720 / 30FPS', x: 0.94, y: 0.9, size: 16, weight: 500, fill: 'rgba(255,255,255,0.4)', align: 'right' } },
      ],
      decor: [
        { part: 'glowDot', params: { x: 0.5, y: 0.28, r: 4, color: job.accent } },
        { part: 'frameLines', params: { margin: 0.05, color: 'rgba(255,255,255,0.12)', width: 1.2 } },
      ],
      overlay: [{ part: 'overlay', params: { kind: 'grain', amount: 500, alpha: 0.05 } }],
    },
    motion: {
      entrance: null, hold: null, exit: null,
      camera: { part: 'camera', params: { zoom: [0.97, 1.0] } },
    },
    transition: { part: 'transition', params: { out: 'fade', outDur: 0.3 } },
    seed: 101,
    locked: false,
  })

  // ── 正文 ──
  const outroStart = lrcLines.length
    ? Math.min(Math.max(lrcLines[lrcLines.length - 1].end + bar, introEnd + 12), D - 5)
    : D - 6 * bar
  const inInterlude = (t) => interludes.some(([s, e]) => t >= s && t < e)

  if (lrcLines.length > 0) {
    let seq = 0
    let lineNo = 0
    let cursor = introEnd
    for (const line of lrcLines) {
      if (line.start >= outroStart) break
      const lineEnd = Math.min(line.end, outroStart)
      if (!inInterlude((line.start + lineEnd) / 2)) {
        const start = cursor
        const block = Math.floor((start + lineEnd) / 2 / (8 * bar))
        const kind = ['A', 'A', 'B', 'C'][block % 4]
        const lyricId = `L${String(++lyricN).padStart(3, '0')}`
        lyrics.push({
          id: lyricId, start: line.start, end: lineEnd, text: line.text,
          syllables: splitSyllables({ ...line, end: lineEnd }),
          style: kind === 'C' ? 'chorus' : 'main',
        })
        // 长行拆镜：一行歌词停太久，画面会"死"在那里。
        // 每约 2.6s 换一次构图（引用同一行歌词，文字会重新入场，视觉上是重排强调）。
        const span = lineEnd - start
        const segCount = Math.max(1, Math.min(4, Math.round(span / 2.6)))
        let segStart = start
        for (let k = 0; k < segCount; k++) {
          const segEnd = k === segCount - 1 ? lineEnd : start + (span * (k + 1)) / segCount
          if (segEnd - segStart < 0.4) { segStart = segEnd; continue }
          const mode = layoutMode(seq, kind, hasMultiChar)
          // 立绘节奏：每 4 行开 2 行（第 1、2 行），"有人在唱"和"只有字"交替出现。
          // 一直挂着立绘会变成幻灯片；一直不挂又浪费了素材。
          const portraitOn = job.portraits && (lineNo % 4 === 1 || lineNo % 4 === 2)
          const portrait = portraitOn ? job.portraits[kind] ?? null : null
          const grp = portrait ? `P${Math.floor(lineNo / 4)}${kind}` : null
          shots.push(makeShot(nextId('S'), mode.mode, kind, seq, segStart, segEnd, [lyricId], job,
                              hasMultiChar, [], portrait, grp))
          seq++
          segStart = segEnd
        }
      }
      cursor = lineEnd
      lineNo++
    }
  } else {
    // ♪ 占位：2/1 小节交替行长，卡节拍切镜
    let s = introEnd
    let seq = 0
    while (s < outroStart - 0.01) {
      const mid = s + bar
      if (interludes.some(([a, b]) => mid >= a && mid < b)) {
        const iv = interludes.find(([a, b]) => mid >= a && mid < b)
        const start = Math.max(s, iv[0])
        const end = Math.min(outroStart, iv[1])
        if (end - start > 1.2) {
          shots.push(makeShot(nextId('S'), 'center', 'I', seq, start, end, [], job, false, [
            { part: 'text', params: { text: '♪', x: 0.5, y: 0.5, size: 54, weight: 700, fill: 'rgba(255,255,255,0.4)' } },
          ]))
          seq++
        }
        s = end
        continue
      }
      const block = Math.floor(s / (8 * bar)) % 4
      const kind = ['A', 'A', 'B', 'C'][block]
      const lineDur = Math.min((seq % 2 === 0 ? 2 : 1) * bar, outroStart - s)
      const mode = layoutMode(seq, kind, false)
      const lyricId = `L${String(++lyricN).padStart(3, '0')}`
      lyrics.push({
        id: lyricId, start: s, end: s + lineDur, text: '♪',
        syllables: [{ text: '♪', start: s, end: s + lineDur }],
        style: kind === 'C' ? 'chorus' : 'main',
      })
      shots.push(makeShot(nextId('S'), mode.mode, kind, seq, s, s + lineDur, [lyricId], job, false))
      seq++
      s += lineDur
    }
  }

  // 间奏卡（lrc 模式）
  if (lrcLines.length > 0) {
    let seq = 0
    for (const [s, e] of interludes) {
      if (s < introEnd || e > outroStart) continue
      if (e - s < 1.5) continue
      shots.push(makeShot(nextId('S'), 'center', 'I', seq++, Math.max(s, introEnd), Math.min(e, outroStart), [], job, false, [
        { part: 'text', params: { text: '♪', x: 0.5, y: 0.5, size: 50, weight: 700, fill: 'rgba(255,255,255,0.4)' } },
      ]))
    }
  }

  // ── 片尾：署名卷动 ──
  const credits = [
    { text: title, size: 46, weight: 800, fill: '#ffffff', y: 1.12 },
    { text: artist, size: 26, weight: 600, fill: 'rgba(255,255,255,0.75)', y: 1.24 },
    { text: 'Rendered with ShizukuFlow', size: 30, weight: 700, fill: '#ffffff', y: 1.45 },
    { text: 'LUME · Letter Universal Motion Engine（琉明）', size: 22, weight: 500, fill: 'rgba(255,255,255,0.6)', y: 1.56 },
    { text: hasLrc ? 'Lyrics: .lrc imported' : 'Lyrics: ♪ placeholder（放入 .lrc 后重渲）', size: 20, weight: 500, fill: 'rgba(255,255,255,0.5)', y: 1.76 },
  ]
  shots.push({
    id: nextId('S'),
    start: outroStart,
    end: D,
    track: 0,
    lyric_refs: [],
    layers: {
      background: [{ part: 'background', params: { kind: 'aurora', color: '#050508', color2: '#0e1018', accent: job.accent, seed: 617, angle: 90 } }],
      text: credits.map((c) => ({
        part: 'text',
        params: { text: c.text, x: 0.5, y: c.y, size: c.size, weight: c.weight, fill: c.fill },
      })),
      decor: [{ part: 'frameLines', params: { margin: 0.06, color: 'rgba(255,255,255,0.10)', width: 1.2 } }],
      overlay: [{ part: 'overlay', params: { kind: 'grain', amount: 480, alpha: 0.05 } }],
    },
    motion: {
      entrance: null, hold: null, exit: null,
      camera: { part: 'camera', params: { pan: [0, -0.115] } },
    },
    transition: { part: 'transition', params: { in: 'fade', inDur: 0.5 } },
    seed: 999983,
    locked: false,
  })

  // 立绘横跨多个镜头：入场/退场各只发生一次。
  // 每个镜头各放一份同样的 portrait 部件，把 at/outAt 回填成这一组的绝对区间，
  // 切镜时人物一动不动 —— 观感是"一个人在画里"，而不是每刀重新淡入。
  {
    const groups = new Map()
    for (const s of shots) {
      for (const part of s.layers?.background ?? []) {
        if (part.part !== 'portrait' || !part.params?.grp) continue
        const g = groups.get(part.params.grp) ?? { start: Infinity, end: -Infinity, parts: [] }
        g.start = Math.min(g.start, s.start)
        g.end = Math.max(g.end, s.end)
        g.parts.push(part.params)
        groups.set(part.params.grp, g)
      }
    }
    for (const g of groups.values()) {
      const span = g.end - g.start
      for (const params of g.parts) {
        // 组本身很短时（一个镜头就是一组），淡入别占满整组
        params.at = g.start
        params.inDur = Math.min(0.7, span * 0.45)
        params.outAt = g.end
        params.outDur = Math.min(0.45, span * 0.3)
        delete params.grp
      }
    }
  }

  // 副歌只在自己的"最后一镜"慢 fade 收：副歌内部每镜之间都淡出的话，
  // 每两秒就陷一次黑，爆发力全被吃掉 —— 内部保持硬切/闪白，只收段尾。
  {
    const body = shots.filter((s) => s.track === 0).sort((a, b) => a.start - b.start)
    for (let i = 0; i + 1 < body.length; i++) {
      const cur = body[i]
      const nxt = body[i + 1]
      if (cur.transition?.params?.in === 'flash' && nxt.transition?.params?.in !== 'flash')
        cur.transition.params = { ...cur.transition.params, out: 'fade', outDur: 0.45 }
    }
  }

  // ── 覆盖轨：仅渐晕（克制）──
  shots.push({
    id: 'S900',
    start: 0,
    end: D,
    track: 1,
    lyric_refs: [],
    layers: {
      background: [],
      text: [],
      decor: [],
      overlay: [{ part: 'overlay', params: { kind: 'vignette', color: 'rgba(0,0,0,0.45)', inner: 'rgba(0,0,0,0)' } }],
    },
    motion: {}, transition: null, seed: 7, locked: false,
  })

  const styles = [
    {
      id: 'main', font: 'Noto Serif SC', weight: 900, size: 76, letter_spacing: 0.06, line_height: 1.1,
      fill: '#ffffff', y: 0.58,
      // 描边 6：底下压了图片之后，4px 在亮部会断，字会"融进"雨滴里
      stroke: { color: '#000000', width: 6 },
      shadow: { offset: [0, 4], blur: 10, color: '#00000080' },
    },
    {
      id: 'chorus', font: 'Noto Serif SC', weight: 900, size: 84, letter_spacing: 0.04, line_height: 1.1,
      fill: job.chorusText, y: 0.5,
      stroke: { color: '#000000', width: 2 },
      shadow: { offset: [0, 4], blur: 12, color: 'rgba(0,0,0,0.25)' },
    },
  ]

  return {
    format: 'ShizukuFlow',
    version: 1,
    meta: { title, artist, created: new Date().toISOString(), modified: '', generator: 'gen_homework.mjs v4', accent: job.accent },
    canvas: { width: 1280, height: 720, fps: 30, background: '#050508', safe_area: { enabled: true, margin: 0.05 } },
    media: {
      audio: { path: audioPath, sha1: '', duration: audioDuration, kind: 'audio' },
      reference: [],
    },
    timing: { bpm, offset: 0, beats: [], downbeats: [], source: 'estimated' },
    styles,
    lyrics,
    shots,
    assets: [],
    export: {
      preset: 'web-720p',
      video: { codec: 'h264', crf: 18, preset: 'medium', encoder: 'auto' },
      gif: { fps: 12, width: 640, dither: 'sierra2_4a' },
      apng: { fps: 24, colors: 256 },
    },
  }
}

const jobs = [
  {
    title: '妄想感傷代償連盟',
    artist: 'DECO*27 feat. 初音ミク',
    audioPath: `${ROOT}/DECO27,初音ミク - 妄想感傷代償連盟.mp3`,
    audioDuration: 270.0,
    bpm: 150,
    accent: '#ff5c8a',
    darkPalettes: [
      ['#12060c', '#2a0a18'],
      ['#0c0410', '#1e0a28'],
    ],
    chorusPalettes: [
      ['#f2ece2', '#d9c8bc'],
      ['#efe4da', '#cbb6ac'],
    ],
    chorusText: '#16161c',
    chorusFrameColor: 'rgba(224,32,64,0.65)',
    fileName: '妄想感傷代償連盟.sf',
  },
  {
    title: '余韻',
    artist: '小泉萌香',
    audioPath: `${ROOT}/小泉萌香 - 余韻.mp3`,
    audioDuration: 213.373333,
    bpm: 79,
    accent: '#7fd4ff',
    darkPalettes: [
      ['#060a14', '#0e2036'],
      ['#080e1a', '#122a40'],
    ],
    chorusPalettes: [
      ['#e8f0f8', '#c2d4e4'],
      ['#e4eef6', '#bad0e2'],
    ],
    chorusText: '#0a1628',
    chorusFrameColor: 'rgba(26,111,212,0.55)',
    // 三张立绘（assets/chara_*.png，从 lihui/ 里 2048x1260 的字卡裁出人物、
    // 底部烘焙了透明渐隐）：A 主歌 = 纯（月白和服）、B 发展 = 静（深绿）、
    // C 副歌 = 笑（浅葱）—— 浅纸反色段配暖绿，人物跟画面一起亮起来。
    portraits: {
      // tint 按段落的光源走：和室/走廊是暖橙、学园/晚霞是金橙、浅纸段是米白
      A: { file: 'chara_pure.png', h: 0.96, tint: '#ffcf9e', tintAmount: 0.16 },
      B: { file: 'chara_cool.png', h: 0.96, tint: '#ffd6b4', tintAmount: 0.14 },
      C: { file: 'chara_smile.png', h: 1.06, x: 0.885, glow: 0.1, tint: '#ffe9c8', tintAmount: 0.12 },
    },
    portraitGlow: '#9fe0ff',
    // 场景底图：主歌在室内（和室 ↔ 走廊），发展段走到室外（学园 ↔ 晚霞）。
    // 副歌不给场景 —— 浅纸反色段是全片唯一的抽离，留着它才对比得出来。
    scenes: {
      A: ['scene_room.png', 'scene_hall.png'],
      B: ['scene_school.png', 'scene_sky.png'],
    },
    // 余韻 79BPM 慢歌：底图只做"薄薄一层景"，不要压过立绘
    platePools: {
      A: ['bg_shoji.png', 'bg_ink_wash.png'],
      B: ['bg_ripples.png'],
      C: ['bg_washi.png'],
      I: ['bg_ripples.png'],
    },
    fileName: '余韻.sf',
  },
  {
    title: '私は雨',
    artist: '東雲絵名, アヤメミク',
    audioPath: `${ROOT}/東雲絵名,アヤメミク - 私は雨 (feat.東雲絵名).flac`,
    audioDuration: 123.715938,
    bpm: 143, // scripts/estimate_bpm.mjs 自相关最强峰 142.9
    accent: '#7aa9ff',
    darkPalettes: [
      ['#050914', '#0b1830'],
      ['#070b18', '#122036'],
    ],
    chorusPalettes: [
      ['#e9f0fb', '#c3d6ea'],
      ['#e4eefa', '#b9cfe6'],
    ],
    chorusText: '#0b1a2e',
    chorusFrameColor: 'rgba(58,120,220,0.55)',
    // 按段落分池：A 主歌（雨夜/城市，暗）、B 发展（和风/水面）、
    // C 副歌（浅纸，反色段要浅底）、I 间奏
    platePools: {
      A: ['bg_rain_window.png', 'bg_city.png', 'bg_ink_wash.png'],
      B: ['bg_shoji.png', 'bg_ripples.png', 'bg_wet_street.png'],
      C: ['bg_washi.png'],
      I: ['bg_ripples.png', 'bg_city.png'],
    },
    fileName: '私は雨.sf',
  },
]

const onlyIdx = process.argv.indexOf('--only')
const only = onlyIdx >= 0 ? process.argv[onlyIdx + 1] : null

for (const j of jobs) {
  if (only && !j.title.includes(only)) continue
  const p = gen(j)
  const file = `${OUT}/${j.fileName}`
  writeFileSync(file, JSON.stringify(p, null, 2), 'utf8')
  const lrc = existsSync(`${OUT}/${j.fileName.replace(/\.sf$/, '.lrc')}`) ? '真歌词' : '♪占位'
  const modes = {}
  for (const s of p.shots) {
    const m = s.motion?.hold?.params?.mode ?? 'other'
    modes[m] = (modes[m] || 0) + 1
  }
  console.log(`${file}: ${lrc} ${p.lyrics.length} 行 · 镜头 ${p.shots.length}`)
  console.log(`  构图分布: ${JSON.stringify(modes)}`)
}
console.log('done')
