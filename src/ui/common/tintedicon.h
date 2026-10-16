#ifndef TINTEDICON_H
#define TINTEDICON_H

#include "ui/common/apptheme.h"

#include <QAbstractButton>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>

// Material Design Icons ship a single flat glyph with no fill, so the raster is recoloured
// instead of rendered black; the SVG image plugin is deployed with the application.
inline QPixmap tintedPixmap(const QString &path, const QColor &color, int size)
{
    QPixmap pixmap = QIcon(path).pixmap(QSize(size, size));
    // Without the plugin the glyph cannot be rendered; the caller then falls back to text
    // instead of painting onto a null device.
    if (pixmap.isNull()) return pixmap;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    return pixmap;
}

// Glyphs on coloured buttons: white while enabled, which sits on the blue and the red
// stylesheet backgrounds alike, and the stylesheet's disabled text colour when disabled.
// Cached per path and size so frequent updates do not re-rasterise the SVGs.
inline QIcon actionStateIcon(const QString &path, int size)
{
    static QHash<QString, QIcon> cache;
    const QString key = QString::number(size) + QLatin1Char('/') + path;
    const auto found = cache.constFind(key);
    if (found != cache.constEnd()) return found.value();
    QIcon icon;
    for (const int scale : {1, 2}) {
        icon.addPixmap(tintedPixmap(path, QColor(QStringLiteral("#ffffff")), size * scale),
                       QIcon::Normal);
        icon.addPixmap(tintedPixmap(path, QColor(QStringLiteral("#f0f3fa")), size * scale),
                       QIcon::Disabled);
    }
    cache.insert(key, icon);
    return icon;
}

// 静音字形跟当前主题的次要文字走。浅色是 #647590，深色是 #8b95a8。
// 字形是栅格，样式表的 color 染不到它，所以这里按主题取色。
inline QColor mutedInk()
{
    return AppTheme::isDark() ? QColor(QStringLiteral("#8b95a8")) : QColor(QStringLiteral("#647590"));
}

inline QColor checkedInk()
{
    return AppTheme::isDark() ? QColor(QStringLiteral("#fef3c7")) : QColor(QStringLiteral("#7a3f00"));
}

inline QColor accentInk()
{
    return AppTheme::isDark() ? QColor(QStringLiteral("#8eb6ff")) : QColor(QStringLiteral("#1d4ed8"));
}

// Glyphs on light surfaces such as combo items: the stylesheet's muted text colour.
inline QIcon mutedIcon(const QString &path, int size)
{
    QIcon icon;
    for (const int scale : {1, 2})
        icon.addPixmap(tintedPixmap(path, mutedInk(), size * scale));
    return icon;
}

// 按钮上的静音图标。系统切换浅色/深色时按当前主题重画，避免深色按钮上仍是深灰字形。
inline void setMutedIcon(QAbstractButton *button, const QString &path, int size)
{
    if (!button) return;
    const auto apply = [button, path, size] {
        button->setIcon(mutedIcon(path, size));
    };
    QObject::connect(AppTheme::instance(), &AppTheme::schemeChanged, button, apply);
    apply();
}

// 单色字形（自定颜色）：用于需要随状态换色的勾选型按钮。QPushButton 的图标状态位跟的是
// 按下而不是勾选，所以"灰→琥珀"这类切换必须由调用方按 toggled 显式换图标。
inline QIcon tintedIcon(const QString &path, const QColor &color, int size)
{
    QIcon icon;
    for (const int scale : {1, 2})
        icon.addPixmap(tintedPixmap(path, color, size * scale));
    return icon;
}

// 「上移」版静音图标：QLineEdit 的 leading action 图标由 Qt 落进内容矩形，实测比同行
// 文字的视觉中线低约 2 个逻辑像素（用户反馈"图标偏左下角"；改 SVG viewBox 补偿在 Qt 的
// SVG 渲染下不生效——preserveAspectRatio 的对齐把平移吃掉了），所以在像素层面直接把
// 图形上移。顶部被裁掉的是图标自身四周的透明边距（MDI 字形上缘距 viewBox 顶约 3/24），
// liftPx=2 时没有可见损失。
inline QIcon liftedMutedIcon(const QString &path, int size, int liftPx = 2)
{
    const QPixmap src = tintedPixmap(path, mutedInk(), size);
    QPixmap dst(size, size);
    dst.fill(Qt::transparent);
    QPainter painter(&dst);
    painter.drawPixmap(0, -liftPx, src);
    painter.end();
    QIcon icon;
    icon.addPixmap(dst);
    return icon;
}

// 勾选型按钮的图标随勾选态换色：未勾选用静音灰，勾选用 QSS 选中态文字色（深琥珀）。
// 每个勾选按钮都要这三行，抽出来只有一份——"琥珀底上出现灰字"这个错只需要防一次。
inline void bindToggleIcon(QPushButton *button, const QString &path, int size,
    const QColor &off = QColor(), const QColor &on = QColor())
{
    const auto apply = [button, path, size, off, on] {
        const QColor offColor = off.isValid() ? off : mutedInk();
        const QColor onColor = on.isValid() ? on : checkedInk();
        button->setIcon(button->isChecked() ? tintedIcon(path, onColor, size)
                                            : tintedIcon(path, offColor, size));
    };
    QObject::connect(button, &QPushButton::toggled, button, [apply](bool) { apply(); });
    QObject::connect(AppTheme::instance(), &AppTheme::schemeChanged, button, apply);
    apply();
}

#endif
