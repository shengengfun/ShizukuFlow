import { useEffect, useRef } from 'react'
import { renderFrame } from '../lib/renderFrame'
import type { Project } from '../lib/api'

interface Props {
  project: Project | null
  /** 当前时间（秒），由播放头 / 播放循环驱动 */
  t: number
  accent: string
}

/**
 * 演出预览画布。
 *
 * 这是「帧 = f(t) 纯函数」架构在前端的落地：每个时间点的画面只由
 * `renderFrame(project, t)` 决定。播放 / 刮擦 / 导出抓帧共用同一入口，
 * 所以预览看到的就是导出得到的。
 */
export function Preview({ project, t, accent }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const wrapRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    const wrap = wrapRef.current
    if (!canvas || !wrap || !project) return
    const dpr = window.devicePixelRatio || 1
    const cssW = wrap.clientWidth
    const cssH = wrap.clientHeight
    if (canvas.width !== Math.floor(cssW * dpr) || canvas.height !== Math.floor(cssH * dpr)) {
      canvas.width = Math.floor(cssW * dpr)
      canvas.height = Math.floor(cssH * dpr)
    }
    const ctx = canvas.getContext('2d')!
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    renderFrame(ctx, cssW, cssH, project, t, accent)
  }, [project, t, accent])

  return (
    <div ref={wrapRef} className="relative h-full w-full overflow-hidden rounded-lg border border-line bg-[#08080b]">
      <canvas ref={canvasRef} className="absolute inset-0 h-full w-full" />
      {(!project || project.lyrics.length === 0) && (
        <div className="absolute inset-0 grid place-items-center px-6 text-center text-sm text-muted-foreground">
          还没有歌词。点上方「示例工程」载入一段演示，或导入音频后开始打轴。
        </div>
      )}
    </div>
  )
}
