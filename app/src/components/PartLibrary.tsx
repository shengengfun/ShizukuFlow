import { useMemo, useState } from 'react'
import type { ReactNode } from 'react'
import { ChevronRight } from 'lucide-react'
import type { Project, Shot } from '~/lib/api'
import {
  PART_GROUPS,
  SLOT_LABEL,
  activeSlots,
  applyPart,
  type PartItem,
  type SlotId,
} from '~/lib/parts'
import { Panel } from './ui'

/**
 * 演出部件库 —— 左侧 dock。
 *
 * 列表不是装饰：每一项都来自 parts.ts 的目录，而目录每一项背后都有一条真实的渲染分支，
 * 点击就是把 PartRef 写进选中镜头的对应槽位（写入规则见 applyPart）。
 *
 * 单值槽位（布局/登场/相机/转场/背景）点一次替换；
 * 多值槽位（装饰/画面效果）是开关，再点一次移除 —— 可叠加的东西不需要额外的删除按钮。
 */
export function PartLibrary({
  project,
  shot,
  onApply,
  dock,
}: {
  project: Project | null
  /** 目标镜头。为空时整表禁用：能落下去才给点，不做点了没反应的假按钮 */
  shot: Shot | null
  onApply: (next: Shot) => void
  dock?: ReactNode
}) {
  const [q, setQ] = useState('')
  const [only, setOnly] = useState<SlotId | null>(null)
  const [open, setOpen] = useState<Set<SlotId>>(new Set<SlotId>(['layout', 'entrance', 'camera']))

  const active = useMemo(() => (shot ? activeSlots(shot) : null), [shot])
  const accent = project?.meta.accent

  const query = q.trim().toLowerCase()
  const groups = useMemo(() => {
    return PART_GROUPS.filter((g) => !only || g.slot === only).map((g) => {
      const items = query
        ? g.items.filter(
            (it) =>
              it.label.toLowerCase().includes(query) || it.id.toLowerCase().includes(query),
          )
        : g.items
      return { group: g, items }
    })
  }, [only, query])

  const total = groups.reduce((n, r) => n + r.items.length, 0)

  const apply = (slot: SlotId, item: PartItem) => {
    if (!shot) return
    onApply(applyPart(shot, slot, item, { accent }))
  }

  return (
    <Panel
      title={dock ?? '演出部件库'}
      scroll={false}
      right={
        <span className="tnum text-muted-foreground text-[10px]">
          {shot ? `目标 ${shot.id}` : '未选镜头'}
        </span>
      }
    >
      <div className="border-line flex-none border-b p-1.5">
        <input
          className="inp"
          placeholder="搜索部件…"
          value={q}
          onChange={(e) => setQ(e.target.value)}
        />
      </div>

      <div className="flex flex-wrap gap-1 px-1.5 py-1.5">
        <Tag on={only === null} onClick={() => setOnly(null)}>
          全部
        </Tag>
        {PART_GROUPS.map((g) => (
          <Tag key={g.slot} on={only === g.slot} onClick={() => setOnly(g.slot)}>
            {g.label}
          </Tag>
        ))}
      </div>

      <div className="min-h-0 flex-1 overflow-auto pb-2">
        {total === 0 && (
          <div className="text-muted-foreground px-3 py-4 text-center text-[11px]">
            没有匹配的部件
          </div>
        )}

        {groups.map(({ group, items }) =>
          items.length === 0 ? null : (
            <div key={group.slot}>
              <button
                className="row text-muted-foreground w-full disabled:opacity-100"
                onClick={() =>
                  setOpen((prev) => {
                    const next = new Set(prev)
                    next.has(group.slot) ? next.delete(group.slot) : next.add(group.slot)
                    return next
                  })
                }
              >
                <ChevronRight
                  size={10}
                  className={`flex-none transition-transform ${open.has(group.slot) ? 'rotate-90' : ''}`}
                />
                <span className="truncate">{group.label}</span>
                <div className="flex-1" />
                <span className="tnum text-[10px] opacity-60">{items.length}</span>
              </button>

              {open.has(group.slot) &&
                items.map((it) => {
                  const on = active?.[group.slot].has(it.id) ?? false
                  return (
                    <button
                      key={it.id}
                      className="row w-full pl-6 disabled:opacity-45"
                      data-on={on}
                      disabled={!shot}
                      title={`${SLOT_LABEL[group.slot]} · ${it.id}${group.multi ? '（再点移除）' : ''}`}
                      onClick={() => apply(group.slot, it)}
                    >
                      <span
                        className="h-3 w-4 flex-none rounded-[2px]"
                        style={{
                          background: on ? 'var(--primary)' : 'var(--panel-2)',
                          outline: '1px solid color-mix(in srgb, var(--foreground) 12%, transparent)',
                        }}
                      />
                      <span className="truncate">{it.label}</span>
                    </button>
                  )
                })}
            </div>
          ),
        )}
      </div>
    </Panel>
  )
}

function Tag({
  children,
  on,
  onClick,
}: {
  children: ReactNode
  on: boolean
  onClick: () => void
}) {
  return (
    <button
      onClick={onClick}
      className={`h-[17px] rounded-[9px] px-2 text-[10.5px] transition-colors ${
        on
          ? 'bg-[color-mix(in_srgb,var(--primary)_22%,transparent)] text-primary font-semibold'
          : 'bg-panel-2 text-muted-foreground hover:text-foreground'
      }`}
    >
      {children}
    </button>
  )
}
