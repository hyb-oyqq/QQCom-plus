#include "payloadcodec.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QTextCodec>

namespace PayloadCodec {
namespace {

constexpr int MaxInputChars = 256 * 1024;
constexpr int MaxPayloadBytes = 64 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("PayloadCodec", text);
}

// GBK/GB18030 编解码来自 QtCore5Compat 的 QTextCodec；解析失败时给出可读错误而不是崩溃。
QTextCodec *gbkCodec()
{
    static QTextCodec *codec = QTextCodec::codecForName("GBK");
    return codec;
}

// 控制字符以 \r \n \t \xNN 转义呈现；escapeHighBytes 决定 >=0x80 的字符是否同样转义
// （ASCII 渲染必须转义，UTF-8/GBK 渲染里它们是正文）。keepLineBreaks 为真时
// \r \n 保持真实字符——"按文本样子看"的视图要按报文里的行分隔真实断行。
// keepEsc 为真时 ESC(0x1B) 同样保持真实字符：监视区要靠它识别 ANSI 颜色标识，
// 其余控制字符仍转义，避免不可见字节悄悄改变显示。
QString escapeControlChars(const QString &text, bool escapeHighBytes, bool keepLineBreaks = false,
    bool keepEsc = false)
{
    QString out;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (u == '\r') {
            if (keepLineBreaks) out += c;
            else out += QLatin1String("\\r");
        } else if (u == '\n') {
            if (keepLineBreaks) out += c;
            else out += QLatin1String("\\n");
        } else if (u == '\t') out += QLatin1String("\\t");
        else if (u == 0x1B && keepEsc) out += c;
        else if (u < 32 || u == 127 || (escapeHighBytes && u > 127)) {
            out += QLatin1String("\\x");
            out += QString::number(u, 16).rightJustified(2, QLatin1Char('0')).toUpper();
        } else
            out += c;
    }
    return out;
}

// Escape 渲染是解析的严格逆变换：任何字节序列都能无损还原。
QString escapeBytes(const QByteArray &payload)
{
    QString out;
    for (const char raw : payload) {
        const quint8 byte = quint8(raw);
        switch (byte) {
        case '\r': out += QLatin1String("\\r"); break;
        case '\n': out += QLatin1String("\\n"); break;
        case '\t': out += QLatin1String("\\t"); break;
        case '\0': out += QLatin1String("\\0"); break;
        case '\\': out += QLatin1String("\\\\"); break;
        default:
            if (byte >= 0x20 && byte <= 0x7E) {
                out += QChar(byte);
            } else {
                out += QLatin1String("\\x");
                out += QString::number(byte, 16).rightJustified(2, QLatin1Char('0')).toUpper();
            }
        }
    }
    return out;
}

void parseHex(const QString &text, Parsed &result)
{
    static const QRegularExpression whitespace("\\s+");
    static const QRegularExpression hex("^[0-9A-Fa-f]+$");
    const QStringList tokens = text.split(whitespace, Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        const bool prefixed = token.startsWith("0x", Qt::CaseInsensitive);
        const QString digits = prefixed ? token.mid(2) : token;
        if (!hex.match(digits).hasMatch() || digits.size() % 2 || (prefixed && digits.size() != 2)) {
            result.error = tr("无效 HEX：%1。请输入完整字节，如 01 AF 或 01AF。").arg(token.left(20));
            break;
        }
        result.bytes.append(QByteArray::fromHex(digits.toLatin1()));
    }

}

void parseDec(const QString &text, Parsed &result)
{
    static const QRegularExpression separators("[\\s,;]+");
    static const QRegularExpression digits("^\\d+$");
    const QStringList tokens = text.split(separators, Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (!digits.match(token).hasMatch()) {
            result.error = tr("无效 DEC：%1。请输入 0～255 的十进制数，如 1 10 255。").arg(token.left(20));
            break;
        }
        bool ok = false;
        const int value = token.toInt(&ok);
        if (!ok || value > 255) {
            result.error = tr("DEC 超出范围：%1。每个字节 0～255。").arg(token.left(20));
            break;
        }
        result.bytes.append(char(value));
    }

}

void parseBase64(const QString &text, Parsed &result)
{
    QString compact;
    for (const QChar c : text) {
        if (!c.isSpace())
            compact += c;
    }
    // 长度 %4==1 的输入没有任何合法解读；缺失的填充按常见工具的习惯自动补齐。
    if (compact.size() % 4 == 1) {
        result.error = tr("无效 BASE64。请输入合法的 Base64 文本，如 AUE=。");
        return;
    }
    while (compact.size() % 4)
        compact += QLatin1Char('=');
    const auto decoded = QByteArray::fromBase64Encoding(compact.toLatin1(),
        QByteArray::Base64Option::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
        result.error = tr("无效 BASE64。请输入合法的 Base64 文本，如 AUE=。");
        return;
    }
    result.bytes = decoded.decoded;
}

void parseEscapes(const QString &text, Parsed &result)
{
    // \r \n \t \0 \\ \xNN 之外的字符按 UTF-8 文本编码，与显示侧的转义渲染互为逆变换。
    const auto appendText = [&result](const QString &part) { result.bytes.append(part.toUtf8()); };
    qsizetype textStart = 0;
    for (qsizetype i = 0; i < text.size() && result.valid(); ++i) {
        if (text.at(i) != QLatin1Char('\\'))
            continue;
        appendText(text.mid(textStart, i - textStart));
        if (!result.valid())
            break;
        const qsizetype escapeStart = i;
        if (++i >= text.size()) {
            result.error = tr("第 %1 个字符：转义未完成").arg(escapeStart + 1);
            break;
        }
        const QChar c = text.at(i);
        if (c == 'r') result.bytes.append('\r');
        else if (c == 'n') result.bytes.append('\n');
        else if (c == 't') result.bytes.append('\t');
        else if (c == '0') result.bytes.append('\0');
        else if (c == '\\') result.bytes.append('\\');
        else if (c == 'x') {
            const auto digits = text.mid(i + 1, 2);
            static const QRegularExpression hexByte("^[0-9A-Fa-f]{2}$");
            if (!hexByte.match(digits).hasMatch()) {
                result.error = tr("第 %1 个字符：\\x 后需要两位十六进制数").arg(escapeStart + 1);
                break;
            }
            result.bytes.append(QByteArray::fromHex(digits.toLatin1()));
            i += 2;
        } else {
            result.error = tr("第 %1 个字符：不支持的转义").arg(escapeStart + 1);
        }
        textStart = i + 1;
    }
    if (result.valid())
        appendText(text.mid(textStart));

}

} // namespace

QList<Format> formats()
{
    return {Format::Hex, Format::Dec, Format::Base64,
            Format::Utf8, Format::Gbk, Format::Ascii, Format::Escape};
}

QString formatName(Format format)
{
    switch (format) {
    case Format::Hex: return QStringLiteral("HEX");
    case Format::Dec: return QStringLiteral("DEC");
    case Format::Base64: return QStringLiteral("BASE64");
    case Format::Utf8: return QStringLiteral("UTF-8");
    case Format::Gbk: return QStringLiteral("GBK");
    case Format::Ascii: return QStringLiteral("ASCII");
    case Format::Escape: return tr("转义字节");
    }
    return {};
}

Parsed parse(const QString &text, Format format, const QByteArray &lineEnding)
{
    Parsed result;
    if (text.size() > MaxInputChars) {
        result.error = tr("输入过长，单次最多 256 KiB");
        return result;
    }
    switch (format) {
    case Format::Hex: parseHex(text, result); break;
    case Format::Dec: parseDec(text, result); break;
    case Format::Base64: parseBase64(text, result); break;
    case Format::Utf8: result.bytes = text.toUtf8(); break;
    case Format::Gbk:
        if (QTextCodec *codec = gbkCodec())
            result.bytes = codec->fromUnicode(text);
        else
            result.error = tr("当前平台缺少 GBK 编解码支持");
        break;
    case Format::Ascii:
        for (const QChar c : text) {
            if (c.unicode() > 127) {
                result.error = tr("内容包含 ASCII（0～127）无法表示的字符");
                break;
            }
        }
        if (result.valid())
            result.bytes = text.toLatin1();
        break;
    case Format::Escape: parseEscapes(text, result); break;
    }
    if (result.valid()) {
        result.bytes.append(lineEnding);
        if (result.bytes.size() > MaxPayloadBytes)
            result.error = tr("内容超过 64 KiB，请缩短发送内容");
    }
    if (!result.valid())
        result.bytes.clear();
    return result;
}

QString render(const QByteArray &payload, Format format)
{
    switch (format) {
    case Format::Hex:
        return QString::fromLatin1(payload.toHex(' ').toUpper());
    case Format::Dec: {
        QString out;
        for (const char raw : payload) {
            if (!out.isEmpty())
                out += QLatin1Char(' ');
            out += QString::number(quint8(raw));
        }
        return out;
    }
    case Format::Base64:
        return QString::fromLatin1(payload.toBase64());
    case Format::Utf8:
        return escapeControlChars(QString::fromUtf8(payload), false);
    case Format::Gbk:
        if (QTextCodec *codec = gbkCodec())
            return escapeControlChars(codec->toUnicode(payload), false);
        return tr("（当前平台缺少 GBK 编解码支持）");
    case Format::Ascii:
        return escapeControlChars(QString::fromLatin1(payload), true);
    case Format::Escape:
        return escapeBytes(payload);
    }
    return {};
}

QString renderMultiline(const QByteArray &payload, Format format)
{
    switch (format) {
    case Format::Utf8:
        return escapeControlChars(QString::fromUtf8(payload), false, true, true);
    case Format::Gbk:
        if (QTextCodec *codec = gbkCodec())
            return escapeControlChars(codec->toUnicode(payload), false, true, true);
        return render(payload, format);
    case Format::Ascii:
        return escapeControlChars(QString::fromLatin1(payload), true, true, true);
    default:
        return render(payload, format);
    }
}

} // namespace PayloadCodec
