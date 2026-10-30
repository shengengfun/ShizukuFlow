import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { getCurrentWindow } from '@tauri-apps/api/window'
import {
  api,
  type AudioMeta,
  type HwCaps,
  type Project,
} from '~/lib/api'
import { ACCENT_PRESETS, applyAccent, applyTheme, DEFAULT_ACCENT } from '~/lib/appearance'
import { detectRenderer, rendererLabel, type RenderCaps } from '~/lib/capabilities'
import { exportVideo, projectDuration as calcDuration } from '~/lib/exporter'
import {
  defaultOutPath,
  estimateBytes,
  scaledSize,
  type OutputSettings,
} from '~/lib/output'
import { LUME_VERSION } from '~/lume'
import { ProjectStore } from '~/state/store'
import { MenuBar, type MenuActions } from '~/components/MenuBar'
import { Toolbar } from '~/components/Toolbar'
import { PartLibrary } from '~/components/PartLibrary'
import { Monitor } from '~/components/Monitor'
import { ProjectPanel } from '~/components/ProjectPanel'
import { Inspector } from '~/components/Inspector'
import { Timeline } from '~/components/Timeline'
import { RenderQueue, type RenderJob } from '~/components/RenderQueue'
import { Segmented } from '~/components/ui'
import { timecode as tc, type Shot } from '~/lib/api'

const appWindow = getCurrentWindow()
const clamp = (v: number, a: number, b: number) => Math.max(a, Math.min(b, v))
const uid = () => Math.random().toString(36).slice(2, 10)

export default function App() {
  // ── 工程与素材 ──
  //
  // 权威工程归 ProjectStore 所有（三层架构第二层）。这里**不再**用 useState 持有工程：
  // UI 只是订阅者 + 命令发送者，改写一律走 store.dispatch —— 撤销/重做因此自动成立，
  // 而不会出现「UI 里改了、历史却不知道」的漏登记情况。
  const storeRef = useRef<ProjectStore | null>(null)
  if (!storeRef.current) storeRef.current = new ProjectStore()
  const store = storeRef.current

  // 本地镜像：只由 store 的订阅回调写，UI 事件一律不许碰它。
  // 名字里带 Mirror 就是为了让人一眼看出它不是写入通道 —— 想改工程请走 edit()。
  const [project, setProjectMirror] = useState<Project | null>(() => store.read())
  const [history, setHistory] = useState(() => store.snapshot())
  const [projectPath, setProjectPath] = useState<string | null>(null)
  const [duration, setDuration] = useState(0)
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState<string | null>(null)
  const [theme, setTheme] = useState<'light' | 'dark'>('dark')
  const [accent, setAccent] = useState(DEFAULT_ACCENT)
  const [audioPath, setAudioPath] = useState<string | null>(null)
  const [audioMeta, setAudioMeta] = useState<AudioMeta | null>(null)
  const [hw, setHw] = useState<HwCaps | null>(null)
  const [caps, setCaps] = useState<RenderCaps | null>(null)

  // ── 播放 ──
  const [playhead, setPlayhead] = useState(0)
  const [playing, setPlaying] = useState(false)

  // ── 选择与视图 ──
  const [selectedShot, setSelectedShot] = useState<string | null>(null)
  const [selectedLyric, setSelectedLyric] = useState<string | null>(null)
  const [safe, setSafe] = useState(false)
  const [grid, setGrid] = useState(false)
  const [zoom, setZoom] = useState(0)

  // ── 布局（可拖动分隔） ──
  const [leftW, setLeftW] = useState(248)
  const [rightW, setRightW] = useState(302)
  const [timelineH, setTimelineH] = useState(226)

  // ── 工作区（Pr 式预设：剪辑 / 调色 / 导出，切换面板侧重） ──
  // edit  ：项目面板 + 监视器 + 检查器 + 时间轴（全功能）
  // color ：隐藏项目面板，监视器 + 检查器（调色/主题色）占主导
  // export：隐藏项目面板，自动打开渲染队列，监视器 + 检查器 + 时间轴
  const [workspace, setWorkspace] = useState<'edit' | 'color' | 'export'>('edit')

  // ── 左 dock 开关：部件库 / 项目。Premiere 的项目面板与效果面板也是同一个 dock 的两个标签
  const [leftDock, setLeftDock] = useState<'parts' | 'project'>('parts')

  // ── 渲染 ──
  const [output, setOutput] = useState<OutputSettings>({
    container: 'mp4',
    encoder: 'auto',
    scale: 1,
    fps: 30,
    crf: 18,
    range: 'all',
    inPoint: 0,
    outPoint: 0,
    audioBitrate: 192,
    outPath: '',
  })
  const [jobs, setJobs] = useState<RenderJob[]>([])
  const [running, setRunning] = useState(false)
  const [logs, setLogs] = useState<string[]>([])
  const [renderOpen, setRenderOpen] = useState(false)
  const jobsRef = useRef<RenderJob[]>([])
  const projectRef = useRef<Project | null>(null)
  const cancelRef = useRef(false)
  const autoExportDone = useRef(false)

  const playheadRef = useRef(0)

  projectRef.current = project
  // 播放头同步进 ref：快捷键监听的 deps 里不能带 playhead（播放时每帧都变，
  // 加进去就等于每帧重新挂一次监听），但 handler 又必须读到最新值。
  playheadRef.current = playhead

  const pushLog = useCallback((line: string) => {
    const st = new Date().toLocaleTimeString('zh-CN', { hour12: false })
    setLogs((prev) => [...prev.slice(-240), `${st}  ${line}`])
  }, [])

  const patchJob = useCallback((id: string, patch: Partial<RenderJob>) => {
    setJobs((prev) => {
      const next = prev.map((j) => (j.id === id ? { ...j, ...patch } : j))
      jobsRef.current = next
      return next
    })
  }, [])

  const setJobsBoth = useCallback((next: RenderJob[]) => {
    jobsRef.current = next
    setJobs(next)
  }, [])

  // UI 是订阅者：工程一变就重渲。注意这里没有反过来的写入通道。
  useEffect(
    () =>
      store.subscribe((s) => {
        setProjectMirror(s.project)
        setHistory(s)
      }),
    [store],
  )

  const undo = useCallback(() => store.undo(), [store])
  const redo = useCallback(() => store.redo(), [store])

  // ── 初始化 ──
  useEffect(() => {
    api.projectNew().then((p) => {
      store.reset(p)
      setOutput((o) => ({ ...o, fps: p.canvas.fps }))
    })
    api.toolsHwCaps().then(setHw).catch(() => {})
    setCaps(detectRenderer())
  }, [])

  // ── CLI 自动导出：shizukuflow.exe --export <project.sf> <out.mp4> ──
  // 完成后自动关窗（脚本驱动的无人工导出路径，也供批处理/CI 复用）。
  useEffect(() => {
    if (autoExportDone.current) return
    autoExportDone.current = true
    api
      .appArgs()
      .then(async (args) => {
        const i = args.indexOf('--export')
        if (i < 0 || args.length < i + 3) return
        const sfPath = args[i + 1]
        const outPath = args[i + 2]
        const flagPath = `${outPath}.flag`
        // 先落 RUNNING 覆盖旧标记：进程若被中途杀掉，flag 会停在 RUNNING，
        // 而不是把上一轮的 OK 留在这儿冒充本次成功（踩过一次这个坑）。
        await api.invokeFlag(flagPath, false, 'RUNNING 未完成（被中断即停留在此状态）').catch(() => {})
        try {
          const p = await api.projectOpen(sfPath)
          store.reset(p)
          const result = await exportVideo(p, {
            outPath,
            accent: p.meta.accent || DEFAULT_ACCENT,
            onProgress: (done, total) => setBusy(`导出 ${done}/${total} 帧`),
            onLog: pushLog,
          })
          setBusy(null)
          await api.invokeFlag(flagPath, true, `encoder=${result.encoder_used} frames=${result.frames}`)
        } catch (e) {
          setBusy(null)
          await api.invokeFlag(flagPath, false, String(e)).catch(() => {})
        } finally {
          setTimeout(() => void appWindow.close(), 800)
        }
      })
      .catch(() => {})
  }, [pushLog])

  // 时长以后端算的为准（取音频时长、镜头结尾、歌词结尾中最大的那个）
  useEffect(() => {
    if (!project) return
    api.projectDuration().then(setDuration).catch(() => setDuration(calcDuration(project)))
  }, [project])

  useEffect(() => {
    const dark = applyTheme(theme)
    applyAccent(accent, '', dark === 'dark')
  }, [theme, accent])

  // 切到「导出」工作区时自动打开渲染队列（Pr 的 export 工作区聚焦输出）
  useEffect(() => {
    if (workspace === 'export') setRenderOpen(true)
  }, [workspace])

  // 播放循环：墙钟推进。（音频同步排在下一阶段：要接 Web Audio + 解码缓存）
  const lastTick = useRef<number | null>(null)
  useEffect(() => {
    if (!playing) {
      lastTick.current = null
      return
    }
    let raf = 0
    const tick = (now: number) => {
      if (lastTick.current == null) lastTick.current = now
      const dt = (now - lastTick.current) / 1000
      lastTick.current = now
      setPlayhead((prev) => {
        const dur = duration > 0 ? duration : 12
        let next = prev + dt
        if (next >= dur) next = next % dur
        return next
      })
      raf = requestAnimationFrame(tick)
    }
    raf = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(raf)
  }, [playing, duration])

  // ── 文件操作 ──
  const adoptProject = useCallback(
    (p: Project, path: string | null) => {
      store.reset(p)
      setProjectPath(path)
      setSelectedShot(null)
      setSelectedLyric(null)
      setPlayhead(0)
      setOutput((o) => ({
        ...o,
        fps: p.canvas.fps,
        outPoint: 0,
        outPath: o.outPath || '',
      }))
      const preset = ACCENT_PRESETS.find((a) => a.color === p.meta.accent)
      if (preset) setAccent(preset.id)
      api
        .currentProjectDir()
        .then((dir) => {
          setOutput((o) => ({ ...o, outPath: o.outPath || defaultOutPath(p.meta.title, dir) }))
        })
        .catch(() => {})
      if (p.media.audio?.path) {
        setAudioPath(p.media.audio.path)
        api.audioLoad(p.media.audio.path).then(setAudioMeta).catch(() => {})
      } else {
        setAudioPath(null)
        setAudioMeta(null)
      }
    },
    [],
  )

  const openProject = useCallback(async () => {
    const { open: openDialog } = await import('@tauri-apps/plugin-dialog')
    const picked = await openDialog({
      multiple: false,
      filters: [{ name: 'ShizukuFlow 工程', extensions: ['sf', 'sfp'] }],
    })
    if (typeof picked !== 'string') return
    setBusy('打开工程…')
    setErr('')
    try {
      adoptProject(await api.projectOpen(picked), picked)
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }, [adoptProject])

  const newProject = useCallback(async () => {
    setBusy('新建工程…')
    try {
      const p = await api.projectNew()
      store.reset(p)
      setProjectPath(null)
      setDuration(0)
      setPlayhead(0)
      setPlaying(false)
      setSelectedShot(null)
      setSelectedLyric(null)
      setOutput((o) => ({ ...o, fps: p.canvas.fps, outPath: '' }))
      setAudioPath(null)
      setAudioMeta(null)
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }, [])

  const saveProject = useCallback(async () => {
    if (!project) return
    try {
      await api.projectSave(project)
      store.markSaved()
      pushLog(`已保存 ${projectPath ?? '工程'}`)
    } catch (e) {
      // 没有路径就退化成另存为
      if (String(e).includes('另存为')) void saveProjectAs()
      else setErr(String(e))
    }
  }, [project, projectPath, pushLog])

  const saveProjectAs = useCallback(async () => {
    if (!project) return
    const { save } = await import('@tauri-apps/plugin-dialog')
    const target = await save({
      defaultPath: `${project.meta.title || 'pv'}.sf`,
      filters: [{ name: 'ShizukuFlow 工程', extensions: ['sf'] }],
    })
    if (typeof target !== 'string') return
    try {
      await api.projectSaveAs(target, project)
      setProjectPath(target)
      store.markSaved()
      pushLog(`已另存为 ${target}`)
    } catch (e) {
      setErr(String(e))
    }
  }, [project, pushLog])

  const loadDemo = useCallback(async () => {
    setBusy('载入示例…')
    setErr('')
    try {
      const p = await api.projectDemo()
      adoptProject(p, null)
      setPlaying(true)
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }, [adoptProject])

  const importAudio = useCallback(async () => {
    const { open: openDialog } = await import('@tauri-apps/plugin-dialog')
    const picked = await openDialog({
      multiple: false,
      filters: [
        { name: '音频', extensions: ['mp3', 'wav', 'ogg', 'flac', 'm4a', 'aac', 'wma'] },
      ],
    })
    if (typeof picked !== 'string') return
    setBusy('解码音频…')
    setErr('')
    try {
      const meta = await api.audioLoad(picked)
      setAudioPath(picked)
      setAudioMeta(meta)
      store.dispatchCmd({ t: 'media.audio', path: picked, duration: meta.duration })
      pushLog(`音轨 ${picked}`)
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }, [pushLog])

  // ── 渲染队列 ──
  const patchOutput = useCallback((patch: Partial<OutputSettings>) => {
    setOutput((o) => ({ ...o, ...patch }))
  }, [])

  const addJob = useCallback(() => {
    const p = projectRef.current
    if (!p) return
    const s = output
    const [w, h] = scaledSize(p.canvas, s.scale)
    const span =
      s.range === 'inout' ? Math.max(0, s.outPoint - s.inPoint) : calcDuration(p)
    const base = (p.meta.title || 'pv').replace(/[\\/:*?"<>|]/g, '_')
    const sameName = jobsRef.current.filter((j) => j.title.startsWith(base)).length
    const title = sameName > 0 ? `${base}_${String(sameName + 1).padStart(2, '0')}` : base
    const job: RenderJob = {
      id: uid(),
      title,
      outPath: s.outPath || `${title}.mp4`,
      settings: { ...s },
      canvas: { width: p.canvas.width, height: p.canvas.height },
      duration: calcDuration(p),
      status: 'queued',
      progress: 0,
      doneFrames: 0,
      totalFrames: Math.max(1, Math.ceil(span * s.fps)),
      fps: 0,
      estimatedSize: estimateBytes(w, h, s.fps, s.crf, span, s.audioBitrate),
    }
    setJobsBoth([...jobsRef.current, job])
    setRenderOpen(true)
    pushLog(`加入队列：${title}.mp4 (${w}×${h} @ ${s.fps}fps, CRF ${s.crf})`)
  }, [output, pushLog, setJobsBoth])

  const runJob = useCallback(
    async (job: RenderJob) => {
      const p = projectRef.current
      if (!p) return
      const s = job.settings
      const [w, h] = scaledSize(job.canvas, s.scale)
      const start = s.range === 'inout' ? Math.max(0, s.inPoint) : 0
      const end = s.range === 'inout' ? Math.max(start, s.outPoint) : job.duration
      patchJob(job.id, {
        status: 'rendering',
        startedAt: Date.now(),
        progress: 0,
        doneFrames: 0,
        totalFrames: Math.max(1, Math.ceil((end - start) * s.fps)),
        fps: 0,
        message: undefined,
      })
      pushLog(`> 开始渲染 ${job.title}.mp4`)
      const t0 = performance.now()
      try {
        const res = await exportVideo(p, {
          outPath: job.outPath,
          accent: p.meta.accent || ACCENT_PRESETS.find((a) => a.id === accent)?.color || DEFAULT_ACCENT,
          width: w,
          height: h,
          fps: s.fps,
          start,
          end,
          encoder: s.encoder,
          crf: s.crf,
          audioBitrate: s.audioBitrate,
          shouldAbort: () => cancelRef.current,
          onLog: (l) => pushLog(`  ${l}`),
          onProgress: (done, total) => {
            const el = (performance.now() - t0) / 1000
            patchJob(job.id, {
              progress: done / total,
              doneFrames: done,
              totalFrames: total,
              fps: done / Math.max(0.001, el),
            })
          },
        })
        patchJob(job.id, {
          status: 'done',
          progress: 1,
          finishedAt: Date.now(),
          encoderUsed: res.encoder_used,
          doneFrames: res.frames,
        })
        pushLog(`OK ${job.title}.mp4 完成（${res.encoder_used}，${res.frames} 帧）`)
      } catch (e) {
        const msg = String(e)
        const canceled = cancelRef.current
        patchJob(job.id, {
          status: canceled ? 'canceled' : 'failed',
          finishedAt: Date.now(),
          message: msg.slice(0, 160),
        })
        pushLog(
          `${canceled ? 'CANCEL' : 'FAIL'} ${job.title}.mp4 ${canceled ? '已取消' : `失败：${msg}`}`,
        )
      }
    },
    [accent, patchJob, pushLog],
  )

  const runQueue = useCallback(async () => {
    if (running) return
    cancelRef.current = false
    setRunning(true)
    setRenderOpen(true)
    try {
      for (;;) {
        const next = jobsRef.current.find((j) => j.status === 'queued')
        if (!next || cancelRef.current) break
        await runJob(next)
      }
    } finally {
      setRunning(false)
    }
  }, [running, runJob])

  const quickRender = useCallback(() => {
    const queued = jobsRef.current.some((j) => j.status === 'queued')
    if (!queued) addJob()
    // addJob 是异步 setState，这里等一拍再启动
    setTimeout(() => void runQueue(), 30)
  }, [addJob, runQueue])

  // ── 快捷键 ──
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return
      const mod = e.ctrlKey || e.metaKey
      if (mod && e.key.toLowerCase() === 'o') {
        e.preventDefault()
        void openProject()
      } else if (mod && e.key.toLowerCase() === 's') {
        e.preventDefault()
        if (e.shiftKey) void saveProjectAs()
        else void saveProject()
      } else if (mod && e.key.toLowerCase() === 'n') {
        e.preventDefault()
        void newProject()
      } else if (mod && e.key.toLowerCase() === 'i') {
        e.preventDefault()
        void importAudio()
      } else if (mod && e.key.toLowerCase() === 'm') {
        e.preventDefault()
        setRenderOpen(true)
      } else if (mod && e.key.toLowerCase() === 'z') {
        // 撤销 / 重做。改的是权威状态层，不是某个组件里的局部 state ——
        // 所以任何面板改出来的东西都能一次撤回。
        e.preventDefault()
        if (e.shiftKey) redo()
        else undo()
      } else if (mod && e.key.toLowerCase() === 'y') {
        e.preventDefault()
        redo()
      } else if (mod && e.key === 'Enter') {
        e.preventDefault()
        quickRender()
      } else if (e.key === ' ') {
        e.preventDefault()
        setPlaying((v) => !v)
      } else if (e.key === 'ArrowLeft') {
        e.preventDefault()
        setPlaying(false)
        setPlayhead((t) => Math.max(0, t - 1 / (projectRef.current?.canvas.fps || 30)))
      } else if (e.key === 'ArrowRight') {
        e.preventDefault()
        setPlaying(false)
        setPlayhead((t) =>
          Math.min(duration, t + 1 / (projectRef.current?.canvas.fps || 30)),
        )
      } else if (e.key === 'Home') {
        setPlayhead(0)
      } else if (e.key === 'End') {
        setPlayhead(duration)
      } else if (e.key === 'PageDown' || e.key === 'PageUp') {
        // 上一 / 下一编辑点（Premiere、Vegas 同一套键位）。
        // 按「所有镜头起点」走而不是按列表里相邻的那一条：播放头停在边界上时，
        // 相邻算法会跳过一个编辑点，用起来像按键失灵。
        const shots = projectRef.current?.shots
        if (!shots || shots.length === 0) return
        e.preventDefault()
        const pts = [...new Set(shots.map((s) => s.start))].sort((a, b) => a - b)
        const now = playheadRef.current
        const target =
          e.key === 'PageDown'
            ? pts.find((p) => p > now + 1e-3)
            : [...pts].reverse().find((p) => p < now - 1e-3)
        if (target !== undefined) {
          setPlaying(false)
          setPlayhead(target)
          const hit = shots.find((s) => s.start === target)
          if (hit) {
            setSelectedShot(hit.id)
            setSelectedLyric(null)
          }
        }
      } else if (e.key === 'f' || e.key === 'F') {
        setZoom(0)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [duration, importAudio, newProject, openProject, quickRender, redo, saveProject, saveProjectAs, undo])

  const browseOut = useCallback(async () => {
    const { save } = await import('@tauri-apps/plugin-dialog')
    const target = await save({
      defaultPath: output.outPath || `${project?.meta.title || 'pv'}.mp4`,
      filters: [{ name: 'MP4 视频', extensions: ['mp4'] }],
    })
    if (typeof target === 'string') patchOutput({ outPath: target })
  }, [output.outPath, patchOutput, project])

  const actions: MenuActions = useMemo(
    () => ({
      newProject: () => void newProject(),
      openProject: () => void openProject(),
      saveProject: () => void saveProject(),
      saveProjectAs: () => void saveProjectAs(),
      importAudio: () => void importAudio(),
      loadDemo: () => void loadDemo(),
      openRender: () => setRenderOpen(true),
      quickRender,
      toggleTheme: () => setTheme((t) => (t === 'dark' ? 'light' : 'dark')),
      toggleSafe: () => setSafe((v) => !v),
      toggleGrid: () => setGrid((v) => !v),
      fitView: () => setZoom(0),
      about: () =>
        setLogs((p) => [
          ...p,
          `ShizukuFlow 0.1.0 · LUME ${LUME_VERSION} · 预览/导出共用同一渲染内核`,
        ]),
    }),
    [importAudio, loadDemo, newProject, openProject, quickRender, saveProject, saveProjectAs],
  )

  const accentColor = ACCENT_PRESETS.find((p) => p.id === accent)?.color ?? DEFAULT_ACCENT
  const activeJob = jobs.find((j) => j.status === 'rendering')
  const unread = jobs.filter((j) => j.status === 'queued').length

  // 部件库与检查器的作用对象：两者始终看同一个镜头，不会出现「库里改了、检查器没动」
  const shot = useMemo(
    () =>
      selectedShot && project ? (project.shots.find((s) => s.id === selectedShot) ?? null) : null,
    [project, selectedShot],
  )

  const applyShot = useCallback(
    (next: Shot) => {
      // 走可序列化命令而不是闭包：这条编辑要能在 Qt 宿主 / WebView 面板之间往返
      store.dispatchCmd({ t: 'shot.replace', shotId: next.id, shot: next as never })
    },
    [store],
  )

  // 左 dock 的共用切换器：两个面板共用，切换状态在 App 里，不会互相唱反调
  const dockTabs = (
    <Segmented
      value={leftDock}
      onChange={setLeftDock}
      options={[
        { value: 'parts' as const, label: '部件' },
        { value: 'project' as const, label: '工程' },
      ]}
    />
  )

  return (
    <div className="bg-rail text-foreground flex h-full flex-col overflow-hidden">
      <MenuBar
        actions={actions}
        dark={theme === 'dark'}
        safe={safe}
        grid={grid}
        title={projectPath ? projectPath.split(/[\\/]/).pop()! : '未保存的工程'}
      />

      <main className="flex min-h-0 flex-1 flex-col">
        {err && (
          <div className="border-destructive/50 bg-destructive/15 text-destructive flex h-7 flex-none items-center gap-2 border-b px-3 text-[11.5px]">
            <span className="truncate">{err}</span>
            <div className="flex-1" />
            <button className="hover:underline" onClick={() => setErr('')}>
              忽略
            </button>
          </div>
        )}

        <Toolbar
          project={project}
          projectPath={projectPath}
          workspace={workspace}
          onWorkspace={setWorkspace}
          onNew={() => void newProject()}
          onOpen={() => void openProject()}
          onSave={() => void saveProject()}
          onImportAudio={() => void importAudio()}
          onDemo={() => void loadDemo()}
          onUndo={undo}
          onRedo={redo}
          undoLabel={history.undoLabel}
          redoLabel={history.redoLabel}
          dirty={history.dirty}
          theme={theme}
          onTheme={setTheme}
          onQueue={() => setRenderOpen(true)}
          onRender={quickRender}
          running={running}
        />

        <div className="flex min-h-0 flex-1">
          {workspace === 'edit' && (
            <>
              <div className="flex-none" style={{ width: leftW }}>
                {leftDock === 'parts' ? (
                  <PartLibrary project={project} shot={shot} onApply={applyShot} dock={dockTabs} />
                ) : (
                  <ProjectPanel
                    project={project}
                    duration={duration}
                    selectedShot={selectedShot}
                    onSelectShot={(id, t) => {
                      setSelectedShot(id)
                      setSelectedLyric(null)
                      setPlaying(false)
                      setPlayhead(t)
                    }}
                    selectedLyric={selectedLyric}
                    onSelectLyric={(id, t) => {
                      setSelectedLyric(id)
                      setSelectedShot(null)
                      setPlaying(false)
                      setPlayhead(t)
                    }}
                    audioName={audioPath ? audioPath.split(/[\\/]/).pop()! : null}
                    onOpenAudio={() => void importAudio()}
                    dock={dockTabs}
                  />
                )}
              </div>

              <Splitter axis="x" onDrag={(d) => setLeftW((w) => clamp(w + d, 180, 420))} />
            </>
          )}

          <div className="flex min-w-0 flex-1 flex-col">
            <Monitor
              project={project}
              t={playhead}
              accent={accentColor}
              playing={playing}
              duration={duration}
              onToggle={() => setPlaying((v) => !v)}
              onSeek={(t) => {
                setPlaying(false)
                setPlayhead(clamp(t, 0, duration))
              }}
              safe={safe}
              grid={grid}
              zoom={zoom}
              onZoom={setZoom}
              onFit={() => setZoom(0)}
              busy={busy}
            />
          </div>

          <Splitter axis="x" onDrag={(d) => setRightW((w) => clamp(w - d, 240, 480))} />

          <div className="flex-none" style={{ width: rightW }}>
            <Inspector
              project={project}
              duration={duration}
              selectedShot={selectedShot}
              selectedLyric={selectedLyric}
              output={output}
              onOutput={patchOutput}
              hw={hw}
              onBrowse={() => void browseOut()}
              onAddToQueue={addJob}
              accentId={accent}
              onAccent={setAccent}
              onSafe={(v) => {
                setSafe(v)
                store.dispatchCmd({ t: 'canvas.safe', enabled: v })
              }}
            />
          </div>
        </div>

        <Splitter axis="y" onDrag={(d) => setTimelineH((h) => clamp(h - d, 150, 460))} />

        <div className="flex-none" style={{ height: timelineH }}>
          <Timeline
            project={project}
            duration={duration}
            playhead={playhead}
            onSeek={(t) => {
              setPlaying(false)
              setPlayhead(clamp(t, 0, duration))
            }}
            selectedShot={selectedShot}
            onSelectShot={(id) => {
              setSelectedShot(id)
              setSelectedLyric(null)
            }}
            audioPath={audioPath}
            meta={audioMeta}
            playing={playing}
          />
        </div>
      </main>

      {/* ── 状态栏 ── */}
      <footer className="bg-panel-2 border-line text-muted-foreground flex h-[23px] flex-none items-center gap-3 border-t px-3 text-[10.5px]">
        <span className="text-foreground">{busy ? busy : '就绪'}</span>
        {running && activeJob && (
          <span className="text-primary tnum">
            渲染 {Math.round(activeJob.progress * 100)}% · {activeJob.fps.toFixed(1)} fps
          </span>
        )}
        <div className="flex-1" />
        {jobs.length > 0 && (
          <button
            className="hover:text-foreground flex items-center gap-1"
            onClick={() => setRenderOpen(true)}
            title="打开渲染队列"
          >
            队列 {jobs.filter((j) => j.status === 'done').length}/{jobs.length}
            {unread > 0 && <span className="text-primary">·{unread}</span>}
          </button>
        )}
        {caps && <span className="tnum">渲染 {rendererLabel(caps)}</span>}
        {hw && (
          <span className="tnum">
            硬件编码 {hw.nvenc || hw.qsv || hw.amf ? '可用' : '仅软件'}
            {hw.nvenc && ' NVENC'}
            {hw.qsv && ' QSV'}
            {hw.amf && ' AMF'}
          </span>
        )}
        <span className="tnum">LUME {LUME_VERSION}</span>
        <span className="tnum">
          {project ? `${project.lyrics.length} 行 · ${project.shots.length} 镜` : ''}
        </span>
        <span className="tnum">{tc(playhead)}</span>
      </footer>

      {renderOpen && (
        <RenderQueue
          jobs={jobs}
          running={running}
          logs={logs}
          onClose={() => setRenderOpen(false)}
          onStartAll={() => void runQueue()}
          onCancel={() => {
            cancelRef.current = true
            pushLog('CANCEL 请求取消…')
          }}
          onRemove={(id) => setJobsBoth(jobsRef.current.filter((j) => j.id !== id))}
          onClearDone={() => setJobsBoth(jobsRef.current.filter((j) => j.status !== 'done'))}
          onAdd={() => addJob()}
        />
      )}
    </div>
  )
}

/**
 * 面板分隔条。
 *
 * 用 window 级 mouse 事件而不是 pointer capture：拖过分隔条本身时
 * 光标会离开元素，element-level 的 mousemove 会断流（AE/Premiere 里
 * 拖分隔条断流是最恼人的手感问题之一）。
 */
function Splitter({ axis, onDrag }: { axis: 'x' | 'y'; onDrag: (delta: number) => void }) {
  const last = useRef(0)
  const onDown = (e: React.MouseEvent) => {
    e.preventDefault()
    last.current = axis === 'x' ? e.clientX : e.clientY
    const move = (ev: MouseEvent) => {
      const cur = axis === 'x' ? ev.clientX : ev.clientY
      const d = cur - last.current
      last.current = cur
      if (d !== 0) onDrag(d)
    }
    const up = () => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      document.body.style.cursor = ''
    }
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
    document.body.style.cursor = axis === 'x' ? 'col-resize' : 'row-resize'
  }
  return (
    <div
      className={axis === 'x' ? 'grip-x' : 'grip-y'}
      onMouseDown={onDown}
      role="separator"
      aria-orientation={axis === 'x' ? 'vertical' : 'horizontal'}
    />
  )
}
