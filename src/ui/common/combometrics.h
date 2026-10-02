#ifndef COMBOMETRICS_H
#define COMBOMETRICS_H

#include <QComboBox>
#include <QSizePolicy>

// Let the style size the box from its own items: QStyleSheetStyle::sizeFromContents() adds the
// stylesheet chrome, which is far wider than the item text because the arrow is drawn inside
// the combo's right padding. Fixed then asks the layout for exactly this hint, so the box
// follows font, DPI and translation changes instead of a frozen pixel width.
inline void fitComboToItems(QComboBox *box)
{
    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    box->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

#endif
