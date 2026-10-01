#include "payloadpreview.h"

#include <QRegularExpression>
#include <QObject>

PayloadPreview encodePreview(const QString &input, int mode, int encoding, int suffix)
{
    PayloadPreview result;
    if (input.size() > 256 * 1024)
        return {{}, QObject::tr("输入过长，单次发送最多 64 KiB")};

    auto appendText = [&result, encoding](const QString &text) {
        if (encoding == 1) {
            for (QChar c : text) {
                if (c.unicode() > 255) {
                    result.error = QObject::tr("内容包含 Latin-1 无法表示的字符，请使用 UTF-8");
                    return;
                }
            }
            result.bytes.append(text.toLatin1());
        } else {
            result.bytes.append(text.toUtf8());
        }
    };

    if (mode == 0) {
        const QStringList tokens = input.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        static const QRegularExpression hex("^[0-9A-Fa-f]+$");
        for (const QString &token : tokens) {
            const bool prefixed = token.startsWith("0x", Qt::CaseInsensitive);
            const QString digits = prefixed ? token.mid(2) : token;
            if (!hex.match(digits).hasMatch() || digits.size() % 2 || (prefixed && digits.size() != 2)) {
                result.error = QObject::tr("无效 HEX：%1。请输入完整字节，如 01 AF 或 01AF。")
                                   .arg(token.left(20));
                break;
            }
            result.bytes.append(QByteArray::fromHex(digits.toLatin1()));
        }
    } else if (mode == 1) {
        appendText(input);
    } else {
        qsizetype textStart = 0;
        for (qsizetype i = 0; i < input.size() && result.valid(); ++i) {
            if (input.at(i) != QLatin1Char('\\'))
                continue;
            appendText(input.mid(textStart, i - textStart));
            if (!result.valid())
                break;
            const qsizetype escapeStart = i;
            if (++i >= input.size()) {
                result.error = QObject::tr("第 %1 个字符：转义未完成").arg(escapeStart + 1);
                break;
            }
            const QChar c = input.at(i);
            if (c == 'r') result.bytes.append('\r');
            else if (c == 'n') result.bytes.append('\n');
            else if (c == 't') result.bytes.append('\t');
            else if (c == '0') result.bytes.append('\0');
            else if (c == '\\') result.bytes.append('\\');
            else if (c == 'x') {
                const auto digits = input.mid(i + 1, 2);
                static const QRegularExpression hexByte("^[0-9A-Fa-f]{2}$");
                if (!hexByte.match(digits).hasMatch()) {
                    result.error = QObject::tr("第 %1 个字符：\\x 后需要两位十六进制数").arg(escapeStart + 1);
                    break;
                }
                result.bytes.append(QByteArray::fromHex(digits.toLatin1()));
                i += 2;
            } else {
                result.error = QObject::tr("第 %1 个字符：不支持的转义").arg(escapeStart + 1);
            }
            textStart = i + 1;
        }
        if (result.valid())
            appendText(input.mid(textStart));
    }
    if (result.valid()) {
        if (suffix == 1) result.bytes.append('\r');
        else if (suffix == 2) result.bytes.append('\n');
        else if (suffix == 3) result.bytes.append("\r\n");
        if (result.bytes.size() > 64 * 1024)
            result.error = QObject::tr("内容超过 64 KiB，请缩短发送内容");
    }
    if (!result.valid())
        result.bytes.clear();
    return result;
}
