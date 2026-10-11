#ifndef PAYLOADCHECKSUM_H
#define PAYLOADCHECKSUM_H

#include <QtGlobal>

// 文件载荷校验。CRC16 为 XMODEM/CCITT（初值 0，与 YMODEM 包内算法相同，但是从文件头连续计算）。
// CRC32 为同一段字节的 IEEE 值。两者都不含协议头、填充和前置/后置指令。
// 协议层和文件预览共用这一份，避免两处各写一套结果对不上。
struct PayloadChecksum
{
    quint16 crc16 = 0;

    quint32 crc32() const { return m_crc32 ^ 0xffffffffu; }
    void feed(const char *data, int size);
    void clear();

private:
    quint32 m_crc32 = 0xffffffffu;
};

#endif
