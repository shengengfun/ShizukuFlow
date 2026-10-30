#pragma once

#include <QColor>
#include <QFont>
#include <QString>

// ─────────────────────────────────────────────────────────────────────────────
// 设计 token —— 全 UI 的唯一真源
//
// 所有数值逐条摘自 prototype/gui.html（:root 与 html.dark 两套变量 + 各处
// 写死的尺寸），这里落地深色套（原型默认）。其它源文件禁止再出现字面量颜色，
// 一律引用本文件，这样"照搬原型"是结构性的，不靠自觉。
//
// 将来接主题切换时：把 Col 换成运行时结构体、styleSheet() 接收一套 Palette，
// 调用方一行不改。
// ─────────────────────────────────────────────────────────────────────────────
namespace Theme {

// ── 颜色（对应 gui.html 第 21–29 行的 html.dark）
namespace Col {
constexpr const char *Bg         = "#16161b"; // --bg        窗口底
constexpr const char *Panel      = "#1e1e25"; // --panel     左右侧栏
constexpr const char *Panel2     = "#24242d"; // --panel-2   工具栏
constexpr const char *Panel3     = "#2b2b35"; // --panel-3   按钮/输入底
constexpr const char *Line       = "#35353f"; // --line      主描边
constexpr const char *LineSoft   = "#2a2a33"; // --line-soft 次描边
constexpr const char *Tx         = "#e6e6ec"; // --tx        主文字
constexpr const char *Tx2        = "#9a9aa8"; // --tx-2      次文字
constexpr const char *Tx3        = "#6a6a78"; // --tx-3      弱文字
constexpr const char *Accent     = "#3dd5ff"; // --accent
constexpr const char *AccentInk  = "#0d1b21"; // --accent-ink
constexpr const char *Beat       = "#5fd8e8"; // --beat      节拍标记
constexpr const char *Stage      = "#08080b"; // --stage     预览舞台（恒深色）
constexpr const char *Ok         = "#5ad19a"; // --ok
constexpr const char *Warn       = "#e8b64c"; // --warn

// ── 原型里散落在各处、未 token 化的颜色
constexpr const char *BtnHover   = "#34343f"; // .btn:hover
constexpr const char *BtnActive  = "#3c3c48"; // .btn:active
constexpr const char *TlBg       = "#1a1a20"; // .timeline（恒深色）
constexpr const char *Thumb      = "#3a3a46"; // 滚动条滑块
constexpr const char *ThumbHover = "#4a4a58";
constexpr const char *MenuBg     = "#1c1c23"; // 下拉菜单底
} // namespace Col

// ── 尺寸（像素）。原型是 4px 栅格 + 少量例外，这里全部锁死
namespace M {
constexpr int MenuH      = 32;  // .menubar
constexpr int ToolbarH   = 42;  // .toolbar
constexpr int PanelHeadH = 30;  // .panel-hd
constexpr int BtnH       = 28;  // .btn
constexpr int BtnSmH     = 24;  // .btn.sm
constexpr int ChipH      = 22;  // .chip
constexpr int InputH     = 24;  // select / input
constexpr int InputTallH = 26;  // 搜索框
constexpr int LeftW      = 216; // .library
constexpr int RightW     = 268; // .inspector
// 32(head) + 230(轨道：含胶片条 28 + 部件轨四层 44) + 24(foot) = 286，再留 8px 给 DPI 舍入，
// 否则最后一行会被裁掉（实测刚好卡满会把特效轨切掉）。
constexpr int TimelineH  = 294;
constexpr int TlHeadH    = 32;  // .tl-head
constexpr int TlFootH    = 24;  // .tl-foot
constexpr int TlLabelW   = 96;  // .tl-labels
constexpr int StatusH    = 24;  // .statusbar
constexpr int Gutter     = 10;  // 工具栏/面板横向内边距
constexpr int Gap        = 6;   // 工具栏元素间距
constexpr int PadSection = 14;  // .preview-wrap 四周

// 圆角分级：4 / 5 / 6 / 9 / 11
constexpr int RSm   = 4;
constexpr int RMd   = 5;
constexpr int RLg   = 6;
constexpr int RTag  = 9;
constexpr int RPill = 11;
} // namespace M

// ── 字号（像素）。原型是 10.5 / 11 / 11.5 / 12 / 12.5 / 13 六级，
//    Qt 只吃整像素，就近归并成 10 / 11 / 12 / 13 四级
namespace F {
constexpr int Body  = 13; // 13 / 12.5
constexpr int Small = 12; // 12 / 11.5
constexpr int Tiny  = 11; // 11 / 10.5
constexpr int Micro = 10; // 计数、角标
constexpr int Mono  = 11; // 数值列
} // namespace F

// ── 字体
QFont ui(int px, int weight = QFont::Normal, qreal letterSpacing = 0.0);
QFont mono(int px, int weight = QFont::Normal);

// ── 工具
QColor c(const char *hex, int alpha = 255);
QString rgba(const char *hex, int alpha); // 带 alpha 的 #AARRGGBB，QSS 用

// ── 全局样式表
QString styleSheet();

} // namespace Theme
