#pragma once
#include <QString>
#include <QPalette>
#include <QColor>
#include <QMap>

// Светлая/тёмная тема: единая точка цветов для глобального QSS, палитры,
// контекстных меню и кастомной отрисовки (CheckStyle). Тема переключается
// на лету: MainWindow::applyTheme() перечитывает всё отсюда.
namespace Theme {

inline bool dark = false;

// Общие опции приложения (доступны из LiveView/VideoWall без связи с MainWindow)
namespace Opt {
inline bool confirmCloseAll = false;   // спрашивать перед «закрыть все камеры/видео»
}

struct C { const char* light; const char* darkv; };
inline QString c(const C& p) { return QString::fromLatin1(dark ? p.darkv : p.light); }

// палитра токенов: light / dark
inline const QMap<QString, C>& tokens() {
    static const QMap<QString, C> t = {
        { "bg",        { "#e9edf0", "#1b1f24" } },   // фон страниц/шапки
        { "card",      { "#ffffff", "#23282e" } },   // карточки/панели
        { "text",      { "#2b2f36", "#d6dbe1" } },   // основной текст
        { "muted",     { "#5a6270", "#9aa3ae" } },   // вторичный текст
        { "hint",      { "#8a92a0", "#7d8791" } },   // подсказки
        { "hint2",     { "#7a8290", "#828c96" } },   // описания карточек
        { "border",    { "#dfe3e8", "#333a42" } },   // рамки карточек
        { "border2",   { "#d3d9e0", "#3a424b" } },   // рамки полей ввода
        { "input",     { "#ffffff", "#1e2329" } },   // фон полей ввода
        { "hover",     { "#f4f6f9", "#2a3037" } },   // hover карточек
        { "hoverBar",  { "#dfe4ea", "#2e353d" } },   // hover пунктов футера
        { "winHover",  { "#dce0e6", "#2e353d" } },   // hover кнопок окна
        { "sel",       { "#d5e3f7", "#31404f" } },   // выделение в дереве
        { "selText",   { "#1f4e8f", "#9cc4ef" } },
        { "accent",    { "#1f6fd6", "#4f8fdd" } },   // акцент (активная вкладка, кнопки)
        { "accentH",   { "#1a60ba", "#66a0e6" } },
        { "disBg",     { "#eef1f4", "#262b31" } },   // неактивные поля
        { "disText",   { "#a7adb5", "#5f6873" } },
        { "tabxBg",    { "#c4cad3", "#3a424b" } },   // крестик вкладки
        { "grid",      { "#eef1f4", "#2a3037" } },   // линии таблиц
        { "headBg",    { "#f4f6f9", "#262c33" } },   // заголовки таблиц
        { "menuBrd",   { "#c6ccd4", "#3a424b" } },
        { "treeHov",   { "#eef3fa", "#2a323b" } },
        { "tileOff",   { "#f0f2f4", "#20252b" } },   // плитка офлайн-рега
        { "checkText", { "#3a414b", "#c6cdd5" } },
        { "green",     { "#3ca35a", "#4db370" } },
        { "red",       { "#e2574c", "#e2574c" } },
    };
    return t;
}

inline QString apply(QString qss) {
    const auto& t = tokens();
    for (auto it = t.begin(); it != t.end(); ++it)
        qss.replace("@" + it.key() + "@", c(it.value()));
    return qss;
}

// глобальный QSS главного окна (все страницы)
inline QString mainQss() {
    return apply(QString::fromUtf8(R"(
        QMainWindow, #root { background:@bg@; }
        QWidget { color:@text@; font-family:'Segoe UI'; font-size:13px; }
        QStackedWidget { background:@bg@; }
        #topbar { background:@bg@; }
        #logo   { color:@text@; font-size:17px; font-weight:700; }
        QPushButton#tab { border:none; background:transparent; padding:0 12px; height:44px;
                          font-size:15px; color:@muted@; }
        QPushButton#tab:hover { color:@accent@; }
        QPushButton#tab[active="true"] { color:@accent@; font-weight:600; }
        QPushButton#tabx { border:none; border-radius:8px; background:@tabxBg@; color:#ffffff;
                           font-size:9px; min-width:16px; max-width:16px; min-height:16px; max-height:16px; padding:0; }
        QPushButton#tabx:hover { background:#e2574c; }
        QPushButton#win, QPushButton#winClose { border:none; background:transparent;
                          min-width:42px; max-width:42px; height:44px; font-size:15px; color:@muted@; }
        QPushButton#win:hover { background:@winHover@; }
        QPushButton#winClose:hover { background:#e81123; color:#ffffff; }
        #homePage { background:@bg@; }
        #homeCard { background:@card@; border-radius:8px; }
        #card { background:transparent; border:none; border-radius:6px; }
        #card:hover { background:@hover@; }
        #cardTitle { font-size:19px; font-weight:700; color:@text@; }
        #cardDesc  { font-size:13px; color:@hint2@; }
        #mgmtBar   { background:@bg@; }
        #mgmtTitle { color:@text@; font-size:14px; font-weight:700; }
        #mgmt { background:transparent; border:none; border-radius:6px; }
        #mgmt:hover { background:@hoverBar@; }
        #mgmtLbl { font-size:14px; font-weight:600; color:@text@; }
        QCheckBox { color:@checkText@; spacing:8px; }
        QCheckBox:disabled { color:@disText@; }
        QSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled {
            background:@disBg@; color:@disText@; border:1px solid @border@; }
        QMenu { background:@card@; border:1px solid @menuBrd@; color:@text@; }
        QMenu::item { padding:6px 24px 6px 20px; }
        QMenu::item:selected { background:@sel@; color:@accent@; }
        QMenu::item:disabled { color:@disText@; background:transparent; }
        QMenu::separator { height:1px; background:@border@; margin:4px 10px; }
        QPushButton#opbtn { border:none; background:transparent; min-width:22px; max-width:22px; height:22px; }
        QPushButton#opbtn:hover { background:@hoverBar@; border-radius:3px; }
        QTableWidget { background:@card@; border:1px solid @border@; gridline-color:@grid@; }
        QHeaderView::section { background:@headBg@; border:none; border-right:1px solid @grid@;
                               border-bottom:1px solid @border@; padding:6px; color:@muted@; }
        #toolbar { background:@headBg@; border-bottom:1px solid @border@; }
        QPushButton#lay { border:1px solid @border2@; background:@card@; border-radius:3px;
                          min-width:34px; height:26px; }
        QPushButton#lay:hover { border-color:@accent@; color:@accent@; }
        QPushButton#tool { background:@card@; border:1px solid @border2@; border-radius:3px; padding:6px 14px; }
        QPushButton#tool:hover { border-color:@accent@; color:@accent@; }
        #addpanel { background:@card@; border:1px solid @menuBrd@; border-radius:8px; }
        #addTitle { font-size:14px; font-weight:600; color:@text@; }
        #fieldLbl { color:@muted@; }
        QLineEdit, QComboBox { border:1px solid @border2@; border-radius:3px; padding:2px 8px;
                               min-height:20px; max-height:22px; background:@input@; color:@text@; }
        QLineEdit:focus, QComboBox:focus { border-color:@accent@; }
        QComboBox QAbstractItemView { background:@card@; color:@text@;
                                      selection-background-color:@accent@; selection-color:#ffffff; }
        QSpinBox { border:1px solid @border2@; border-radius:3px; padding:2px 4px;
                   background:@input@; color:@text@; }
        QPushButton#primary { background:@accent@; color:#ffffff; border:none; border-radius:3px; padding:7px 18px; }
        QPushButton#primary:hover { background:@accentH@; }
        QPushButton#ghost { background:@card@; border:1px solid @border2@; border-radius:3px;
                            padding:7px 18px; color:@text@; }

        /* --- страница «Просмотр»: левая панель --- */
        #orgpanel { background:@bg@; }
        #orgBox   { background:@card@; border:1px solid @border@; border-radius:4px; }
        #orgSearch { border:1px solid @border2@; border-radius:3px; padding:2px 8px; background:@input@;
                     min-height:20px; max-height:22px; }
        QTreeWidget { background:@card@; border:none; outline:0; font-size:12px; color:@checkText@; }
        QTreeWidget::item { height:24px; }
        QTreeWidget::item:hover { background:@treeHov@; }
        QTreeWidget::item:selected { background:@sel@; color:@selText@; }

        /* --- нижняя панель видеостены --- */
        #livebar { background:@bg@; border-top:1px solid @border@; }
        QPushButton#lbtn { border:none; background:transparent; min-width:28px; max-width:28px; height:24px; }
        #pagelbl { color:@muted@; font-size:12px; min-width:42px; }
        #livebar QComboBox { background:@input@; color:@text@; border:1px solid @border2@; border-radius:3px;
                             padding:0px 6px; min-height:18px; max-height:20px; min-width:96px; font-size:12px; }
        #livebar QComboBox QAbstractItemView { background:@card@; color:@text@; selection-background-color:@accent@; }

        /* --- страница выбора регистратора: плитки --- */
        #selPage  { background:@bg@; }
        #selTitle { font-size:14px; font-weight:700; color:@text@; }
        #devTile  { background:@card@; border:1px solid @border@; border-radius:6px; }
        #devTile:hover { border-color:@accent@; }
        #devTile[off="true"] { background:@tileOff@; }
        #devTile[off="true"]:hover { border-color:@border@; }
        #devTileName { font-size:14px; font-weight:600; color:@text@; }
        #devTileIp   { font-size:12px; color:@hint@; }
        #devTileOn   { font-size:12px; color:@green@; }
        #devTileOff  { font-size:12px; color:@red@; }
        #devTileWait { font-size:12px; color:@hint@; }

        /* --- страница «Настройки» --- */
        #setCard    { background:@card@; border:1px solid @border@; border-radius:6px; }
        #setSection { font-size:14px; font-weight:700; color:@text@; }
        #setHint    { font-size:12px; color:@hint@; }
        QScrollBar:vertical { background:transparent; width:10px; }
        QScrollBar::handle:vertical { background:@border2@; border-radius:5px; min-height:30px; }
        QScrollBar::add-line, QScrollBar::sub-line { height:0; }
    )"));
}

// контекстные меню поверх тёмной видеостены (свой стиль, не наследует фон стены)
inline QString menuQss() {
    return apply(QString::fromUtf8(
        "QMenu{background:@card@;border:1px solid @menuBrd@;color:@text@;}"
        "QMenu::item{padding:6px 24px 6px 20px;}"
        "QMenu::item:selected{background:@sel@;color:@accent@;}"
        "QMenu::item:disabled{color:@disText@;background:transparent;}"
        "QMenu::separator{height:1px;background:@border@;margin:4px 10px;}"));
}

// всплывашка ЦПУ/ОЗУ
inline QString perfQss() {
    return apply(QString::fromUtf8(
        "#perfPopup { background:@card@; border:1px solid @border@; border-radius:6px; }"
        "QLabel { color:@checkText@; font-size:13px; } "
        "#perfBar { background:@disBg@; border-radius:3px; } "
        "#perfFill { background:@green@; border-radius:3px; }"));
}

// маленькие кнопки пользовательских сеток на панели просмотра
inline QString customBtnQss() {
    return apply(QString::fromUtf8(
        "QPushButton { border:1px solid @hint@; border-radius:2px; background:transparent;"
        "  color:@muted@; font-size:11px; padding:0;"
        "  min-width:18px; max-width:18px; min-height:16px; max-height:16px; }"
        "QPushButton:hover { color:@accent@; border-color:@accent@; }"));
}

inline QPalette palette() {
    QPalette p;
    auto col = [](const char* l, const char* d){ return QColor(QString::fromLatin1(Theme::dark ? d : l)); };
    p.setColor(QPalette::Window,          col("#eef1f5", "#1b1f24"));
    p.setColor(QPalette::Base,            col("#ffffff", "#1e2329"));
    p.setColor(QPalette::AlternateBase,   col("#f6f8fa", "#23282e"));
    p.setColor(QPalette::WindowText,      col("#2b2f36", "#d6dbe1"));
    p.setColor(QPalette::Text,            col("#2b2f36", "#d6dbe1"));
    p.setColor(QPalette::Button,          col("#ffffff", "#23282e"));
    p.setColor(QPalette::ButtonText,      col("#2b2f36", "#d6dbe1"));
    p.setColor(QPalette::Highlight,       col("#1f6fd6", "#4f8fdd"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase,     col("#ffffff", "#23282e"));
    p.setColor(QPalette::ToolTipText,     col("#2b2f36", "#d6dbe1"));
    p.setColor(QPalette::PlaceholderText, col("#9aa1ab", "#6a7480"));
    return p;
}

} // namespace Theme
