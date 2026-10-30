const H = {
  'User-Agent':
    'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36',
  Referer: 'https://music.163.com/',
  Cookie: 'appver=2.0.2',
}

async function jget(url, tries = 3) {
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(url, { headers: H, signal: AbortSignal.timeout(15000) })
      if (!r.ok) return { err: 'HTTP ' + r.status }
      return await r.json()
    } catch (e) {
      if (i === tries - 1) return { err: e.message }
    }
  }
}

const q = encodeURIComponent('余韻 小泉萌香')
const s = await jget(`https://music.163.com/api/search/get/web?s=${q}&type=1&offset=0&limit=10`)
if (s.err) {
  console.log('search err', s.err)
} else {
  const songs = s?.result?.songs ?? []
  console.log('songs:', songs.length)
  for (const it of songs.slice(0, 8))
    console.log('  ', it.id, JSON.stringify(it.name), '/', (it.artists || []).map((a) => a.name).join('&'), (it.duration / 1000).toFixed(1) + 's')
  const first = songs[0]
  if (first) {
    const lyr = await jget(`https://music.163.com/api/song/lyric?id=${first.id}&lv=1&kv=1&tv=-1`)
    if (lyr.err) console.log('lyric err', lyr.err)
    else {
      const lrc = lyr?.lrc?.lyric ?? ''
      const kly = lyr?.klyric?.lyric ?? ''
      const stamps = (lrc.match(/\[\d{1,2}:\d{2}([.:]\d{1,3})?\]/g) || []).length
      console.log('lyric: chars=', lrc.length, 'stamps=', stamps, 'wordLevel=', kly.length)
    }
  }
}
