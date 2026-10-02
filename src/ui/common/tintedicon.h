#ifndef TINTEDICON_H
#define TINTEDICON_H

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
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

#endif
