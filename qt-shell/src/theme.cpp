#include "theme.h"

#include <QColor>
#include <QStringList>

namespace Theme {

QColor c(const char *hex, int alpha)
{
    QColor col{QString::fromLatin1(hex)};
    if (alpha < 255)
        col.setAlpha(alpha);
    return col;
}

QString rgba(const char *hex, int alpha)
{
    // QSS 认的是 #AARRGGBB（和 CSS 的 #RRGGBBAA 顺序相反），用 QColor 转最稳
    QColor col{QString::fromLatin1(hex)};
    col.setAlpha(alpha);
    return col.name(QColor::HexArgb);
}

QFont ui(int px, int weight, qreal letterSpacing)
{
    QFont f;
    f.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                   QStringLiteral("Microsoft YaHei"), QStringLiteral("Noto Sans SC")});
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    if (!qFuzzyIsNull(letterSpacing))
        f.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    return f;
}

QFont mono(int px, int weight)
{
    QFont f;
    f.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                   QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QString styleSheet()
{
    const QString bg = Col::Bg, panel = Col::Panel, panel2 = Col::Panel2, panel3 = Col::Panel3;
    const QString line = Col::Line, lineSoft = Col::LineSoft;
    const QString tx = Col::Tx, tx2 = Col::Tx2, tx3 = Col::Tx3;
    const QString accent = Col::Accent, accentInk = Col::AccentInk;

    QString s;
    s += QStringLiteral(R"(
/* ── 基础 ───────────────────────────────────────────── */
QMainWindow, QDialog { background: %1; }
QToolTip {
    background: %2; color: %3; border: 1px solid %4;
    border-radius: 4px; padding: 4px 7px;
}
)").arg(bg, panel3, tx, line);

    // ── 菜单（自建菜单栏用 QPushButton[role=menu] 承载）
    s += QStringLiteral(R"(
QPushButton[role="menu"] {
    background: transparent; border: none; border-radius: 4px;
    padding: 0 9px; color: %1; font-size: 13px;
}
QPushButton[role="menu"]:hover { background: %2; color: %3; }
QPushButton[role="menu"]::menu-indicator { image: none; width: 0; }

QMenu {
    background: %4; border: 1px solid %5; border-radius: 6px; padding: 4px;
}
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; color: %3; font-size: 13px; }
QMenu::item:selected { background: %2; }
QMenu::item:disabled { color: %6; }
QMenu::separator { height: 1px; background: %7; margin: 4px 8px; }
)").arg(tx2, panel3, tx, Col::MenuBg, line, tx3, lineSoft);

    // ── 按钮：默认 / ghost / primary / sm / seg
    s += QStringLiteral(R"(
QPushButton {
    background: %1; border: 1px solid transparent; border-radius: 5px;
    color: %2; padding: 0 11px; font-size: 13px;
}
QPushButton:hover { background: %3; }
QPushButton:pressed { background: %4; }

QPushButton[role="ghost"] { background: transparent; color: %5; }
QPushButton[role="ghost"]:hover { background: %1; color: %2; }
QPushButton[role="ghost"]:pressed { background: %4; }

QPushButton[role="primary"] {
    background: %6; color: %7; font-weight: 600; border: none;
}
QPushButton[role="primary"]:hover { background: #5adeff; }
QPushButton[role="primary"]:pressed { background: #2ec4ee; }

QPushButton[role="sm"] { padding: 0 8px; font-size: 12px; }

QPushButton[role="seg"] {
    background: transparent; border: none; border-radius: 0;
    padding: 0 9px; color: %5; font-size: 12px;
}
QPushButton[role="seg"]:hover { color: %2; }
QPushButton[role="seg"]:checked { background: %1; color: %2; }

QPushButton:disabled { color: %8; }
)").arg(panel3, tx, Col::BtnHover, Col::BtnActive, tx2, accent, accentInk, tx3);

    // ── 窗口控制按钮（自绘标题栏右侧，原型 .menubar .win）
    s += QStringLiteral(R"(
QPushButton[role="wbtn"], QPushButton[role="wclose"] {
    background: transparent; border: none; border-radius: 0;
    padding: 0; color: %1; font-size: 12px;
}
QPushButton[role="wbtn"]:hover { background: %2; color: %3; }
QPushButton[role="wbtn"]:pressed { background: %4; color: %3; }
QPushButton[role="wclose"]:hover { background: #e81123; color: #ffffff; }
QPushButton[role="wclose"]:pressed { background: #c50f1f; color: #ffffff; }
QPushButton[role="wbtn"]:disabled, QPushButton[role="wclose"]:disabled { color: %1; }
)").arg(tx3, panel3, tx, Col::BtnActive);

    // ── 输入控件
    s += QStringLiteral(R"(
QLineEdit {
    background: %1; border: 1px solid %2; border-radius: 5px;
    color: %3; padding: 0 9px; selection-background-color: %4;
    selection-color: %5;
}
QLineEdit:focus { border-color: %6; }

QComboBox {
    background: %1; border: 1px solid %2; border-radius: 4px;
    color: %3; padding: 0 7px; font-size: 12px;
}
QComboBox:hover { border-color: %7; }
QComboBox:focus { border-color: %6; }
QComboBox::drop-down { width: 16px; border: none; background: transparent; }
QComboBox QAbstractItemView {
    background: %8; border: 1px solid %2; border-radius: 4px;
    color: %3; selection-background-color: %9; outline: none; padding: 3px;
}
QComboBox QAbstractItemView::item { min-height: 22px; padding-left: 6px; }
)").arg(bg, line, tx, accent, accentInk, accent, tx3, Col::MenuBg, panel3);

    // ── 滑块：轨道 3px，把手 12px 圆，2px 面板色描边（原型同）
    s += QStringLiteral(R"(
QSlider { min-height: 12px; max-height: 12px; background: transparent; }
QSlider::groove:horizontal {
    height: 3px; background: %1; border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -5px 0;
    border-radius: 6px; background: %2; border: 2px solid %3;
}
QSlider::handle:horizontal:hover { background: #5adeff; }
)").arg(panel3, accent, panel);

    // ── 树/列表
    s += QStringLiteral(R"(
QTreeWidget, QTreeView, QListWidget {
    background: transparent; border: none; outline: none;
    alternate-background-color: transparent;
}
QTreeWidget::item, QListWidget::item { border: none; }
QHeaderView::section { background: transparent; border: none; }
)");

    // ── 滚动条：9px，圆角 5
    s += QStringLiteral(R"(
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }
QScrollBar::handle:vertical {
    background: %1; border-radius: 5px; min-height: 28px;
}
QScrollBar::handle:vertical:hover { background: %2; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

QScrollBar:horizontal { background: transparent; height: 9px; margin: 0; }
QScrollBar::handle:horizontal {
    background: %1; border-radius: 5px; min-width: 28px;
}
QScrollBar::handle:horizontal:hover { background: %2; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
)").arg(Col::Thumb, Col::ThumbHover);

    // ── 容器角色
    s += QStringLiteral(R"(
QFrame[role="panelHead"] {
    background: transparent; border: none; border-bottom: 1px solid %1;
}
QFrame[role="hairline"] { background: %1; border: none; }
QFrame[role="stage"] {
    background: %2; border: 1px solid %3; border-radius: 6px;
}
QFrame[role="segbox"] { background: %4; border: none; border-radius: 5px; }
QFrame[role="timeline"] { background: %5; border: none; border-top: 1px solid %3; }
QLabel[role="chip"] {
    background: %4; border-radius: 11px; padding: 0 8px;
    color: %6; font-size: 11px;
}
QLabel[role="tag"] {
    background: %4; border-radius: 9px; padding: 2px 7px;
    color: %7; font-size: 10px;
}
QLabel[role="tag"][on="true"] {
    background: %8; color: %9; font-weight: 600;
}
)").arg(lineSoft, Col::Stage, line, panel3, Col::TlBg, tx2, tx3,
        rgba(Col::Accent, 0x26), accent);

    return s;
}

} // namespace Theme
