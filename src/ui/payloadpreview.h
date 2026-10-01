#ifndef PAYLOADPREVIEW_H
#define PAYLOADPREVIEW_H

#include <QByteArray>
#include <QString>

struct PayloadPreview
{
    QByteArray bytes;
    QString error;
    bool valid() const { return error.isEmpty(); }
};

// Modes: 0 HEX, 1 text, 2 escaped bytes. Encoding: 0 UTF-8, 1 Latin-1.
PayloadPreview encodePreview(const QString &input, int mode, int encoding, int suffix);

#endif
