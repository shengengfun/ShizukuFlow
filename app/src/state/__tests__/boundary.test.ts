/**
 * 架构边界守卫。
 *
 * 三层分离（UI 壳层 / 工程状态层 / LUME 渲染内核）是一条会被日常开发慢慢磨掉的约束：
 * 某天有人图省事在状态层里 `useState`、在渲染内核里 `invoke(...)`，边界就消失了。
 * 这个文件把边界写成测试 —— 一旦破线就红，而不是靠人记得。
 *
 * 之所以能这样写：**状态层与内核在设计上就是零框架依赖的**，
 * 它们不认识 React、不认识 Tauri。所以今天它们可以住在 WebView 里，
 * 明天整体搬进原生宿主进程时不需要改一行逻辑 —— 这正是移植的前提。
 */

/// <reference types="vite/client" />

import { describe, expect, it } from 'vitest'

/**
 * 源码快照：用 Vite 的 glob 在构建期把源码文本抓进来，
 * 避免为了一个测试去引入 @types/node。
 */
const SRC = import.meta.glob('/src/**/*.{ts,tsx}', {
  query: '?raw',
  import: 'default',
  eager: true,
}) as Record<string, string>

const pick = (prefix: string) =>
  Object.entries(SRC)
    .filter(([k]) => k.startsWith(prefix) && !k.includes('__tests__'))
    .map(([, v]) => v)

const read = (rel: string) => SRC[rel] ?? ''

/** 抽出源码里的模块引用（含 type import —— 边界守卫对两者都从严）。 */
function importsOf(code: string): string[] {
  const out: string[] = []
  for (const m of code.matchAll(/from\s+['"]([^'"]+)['"]/g)) out.push(m[1])
  for (const m of code.matchAll(/import\(\s*['"]([^'"]+)['"]\s*\)/g)) out.push(m[1])
  return out
}

/** 运行时引用 —— type import 编译后会被擦除，不构成真实依赖。 */
function runtimeImportsOf(code: string): string[] {
  const out: string[] = []
  for (const m of code.matchAll(/^\s*import\s+(?!type\b)[\s\S]*?from\s+['"]([^'"]+)['"]/gm)) {
    out.push(m[1])
  }
  return out
}

const UI_FRAMEWORKS = ['react', 'react-dom', '@tauri-apps']

describe('分层边界', () => {
  it('LUME 渲染内核不得依赖任何 UI / 宿主框架', () => {
    const list = pick('/src/lume/')
    expect(list.length).toBeGreaterThan(0)
    for (const code of list) {
      for (const dep of [...UI_FRAMEWORKS, '../components', '~/components', '../state', '~/state']) {
        expect(importsOf(code)).not.toContain(dep)
      }
    }
  })

  it('工程状态层不得依赖任何 UI / 宿主框架（这是它能被搬进原生进程的前提）', () => {
    const list = pick('/src/state/')
    expect(list.length).toBeGreaterThan(0)
    for (const code of list) {
      for (const dep of [...UI_FRAMEWORKS, '../components', '~/components']) {
        expect(importsOf(code)).not.toContain(dep)
      }
      // 只允许「类型」引用 api —— 运行时依赖会把状态层和 IO 焊死
      for (const d of runtimeImportsOf(code)) {
        expect(d).not.toMatch(/lib\/api/)
      }
    }
  })

  it('状态层不触碰浏览器 API（DOM / 存储），才能在任意宿主里跑', () => {
    const list = pick('/src/state/')
    expect(list.length).toBeGreaterThan(0)
    for (const code of list) {
      expect(code).not.toMatch(/\bdocument\.|\bwindow\./)
      expect(code).not.toMatch(/\blocalStorage\b/)
    }
  })

  it('UI 壳层不再直接持有工程 —— 不得出现改写工程的 setProject', () => {
    const code = read('/src/App.tsx')
    expect(code.length).toBeGreaterThan(0)
    // 工程由 ProjectStore 拥有；UI 只能读、只能 dispatch
    expect(code).not.toMatch(/const \[project, setProject\] = useState/)
    expect(code).toMatch(/ProjectStore/)
    // UI 不允许出现任何 setProject 写入调用：工程只有一个写入口 —— dispatchCmd
    expect((code.match(/setProject\(/g) ?? []).length).toBe(0)
  })

  it('UI 只提交可序列化命令，不得用闭包绕过进程边界', () => {
    const code = read('/src/App.tsx')
    expect(code.length).toBeGreaterThan(0)
    // 闭包版 dispatch 过不了 Qt ⇄ WebView 的进程边界，UI 一律走 dispatchCmd
    expect((code.match(/store\.dispatch\(/g) ?? []).length).toBe(0)
    expect(code).toMatch(/store\.dispatchCmd\(/)
  })

  it('渲染内核不反向依赖 UI 层目录', () => {
    for (const code of [...pick('/src/lume/'), read('/src/lib/renderFrame.ts')]) {
      for (const dep of importsOf(code)) {
        expect(dep).not.toMatch(/components/)
      }
    }
  })
})
