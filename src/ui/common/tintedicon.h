#ifndef TINTEDICON_H
#define TINTEDICON_H

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

// Glyphs on light surfaces such as combo items: the stylesheet's muted text colour, since
// both the popup and the closed combo draw on white.
inline QIcon mutedIcon(const QString &path, int size)
{
    QIcon icon;
    for (const int scale : {1, 2})
        icon.addPixmap(tintedPixmap(path, QColor(QStringLiteral("#647590")), size * scale));
    return icon;
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
    const QPixmap src = tintedPixmap(path, QColor(QStringLiteral("#647590")), size);
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
    const QColor &off = QColor(0x64, 0x75, 0x90), const QColor &on = QColor(0x7a, 0x3f, 0x00))
{
    const QIcon offIcon = tintedIcon(path, off, size);
    const QIcon onIcon = tintedIcon(path, on, size);
    const auto refresh = [button, offIcon, onIcon](bool checked) {
        button->setIcon(checked ? onIcon : offIcon);
    };
    QObject::connect(button, &QPushButton::toggled, button, refresh);
    refresh(button->isChecked());
}

#endif
