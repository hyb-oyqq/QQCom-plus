#ifndef PAYLOADCODEC_H
#define PAYLOADCODEC_H

#include <QByteArray>
#include <QList>
#include <QString>

// 数据显示与发送区共用的字节编解码接口：发送区把文本解析成字节（parse），
// 显示区把字节渲染成文本（render）。新增格式时在 Format 加枚举并补齐两向实现，
// 两个面板的组合框自动获得；两侧永远使用同一套规则，不会出现“能发不能看”。
namespace PayloadCodec {

enum class Format
{
    Hex,     // 01 AF / 01AF / 0x01
    Dec,     // 1 10 255，十进制字节值
    Base64,  // AUE=，忽略全部空白
    Utf8,    // 文本按 UTF-8 编解码
    Gbk,     // 文本按 GBK（GB18030 兼容）编解码
    Ascii,   // 文本限 0x00-0x7F
    Escape   // \r \n \t \0 \\ \xNN 转义，其余字符按 UTF-8
};

struct Parsed
{
    QByteArray bytes;
    QString error;
    bool valid() const { return error.isEmpty(); }
};

// 组合框顺序即此列表顺序；名称经 tr() 保留翻译能力。
QList<Format> formats();
QString formatName(Format format);

// 发送侧：文本 -> 字节。lineEnding 是“末尾”选项产生的行尾字节，由调用方传入；
// 输入超过 256 KiB 或结果超过 64 KiB 时报错。显示侧不使用该重载的第三个参数。
Parsed parse(const QString &text, Format format, const QByteArray &lineEnding = {});

// 显示侧：字节 -> 文本。渲染是容错的：非法 UTF-8/GBK 序列以替换符呈现，
// 控制字节与 ASCII 之外的字节转义为 \xNN，绝不因数据内容报错。
QString render(const QByteArray &payload, Format format);

// 显示侧的"按文本样子看"变体：UTF-8/GBK/ASCII 解码里 \r \n 保持真实换行，
// 其余控制字符仍转义（\t \xNN）。HEX/DEC/BASE64/转义字节本身没有换行概念，
// 与 render() 相同。文本控制台用它把行分隔的报文展开成真实多行。
QString renderMultiline(const QByteArray &payload, Format format);

} // namespace PayloadCodec

#endif
