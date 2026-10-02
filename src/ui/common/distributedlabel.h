#ifndef DISTRIBUTEDLABEL_H
#define DISTRIBUTEDLABEL_H

#include <QLabel>
#include <QPainter>
#include <QTextBoundaryFinder>

// Distribute graphemes instead of inserting spaces into translated/accessibility text.
class DistributedLabel final : public QLabel
{
public:
    explicit DistributedLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStringList glyphs;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text());
        int start = 0;
        for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) {
            glyphs.append(text().mid(start, end - start));
            start = end;
        }
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       QPalette::WindowText));
        const QFontMetricsF metrics(font());
        const auto area = contentsRect();
        qreal textWidth = 0;
        for (const auto &glyph : glyphs) textWidth += metrics.horizontalAdvance(glyph);
        const qreal gap = glyphs.size() > 1 ? qMax(0.0, (area.width() - textWidth) / (glyphs.size() - 1)) : 0;
        qreal x = area.left();
        const qreal baseline = area.top() + (area.height() - metrics.height()) / 2 + metrics.ascent();
        for (const auto &glyph : glyphs) {
            painter.drawText(QPointF(x, baseline), glyph);
            x += metrics.horizontalAdvance(glyph) + gap;
        }
    }
};

#endif
