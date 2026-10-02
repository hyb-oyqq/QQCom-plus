#ifndef CONTENTSCROLLAREA_H
#define CONTENTSCROLLAREA_H

#include <QEvent>
#include <QScrollArea>

// Prefer the current content height; only scroll when the parent runs out of space.
class ContentScrollArea final : public QScrollArea
{
public:
    explicit ContentScrollArea(QWidget *parent = nullptr) : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    QSize sizeHint() const override
    {
        return widget() ? widget()->sizeHint() + QSize(2 * frameWidth(), 2 * frameWidth())
                        : QScrollArea::sizeHint();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const bool result = QScrollArea::eventFilter(watched, event);
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();
        return result;
    }
};

#endif
