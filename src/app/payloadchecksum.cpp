#include "payloadchecksum.h"

namespace {

const quint32 *crc32Table()
{
    static quint32 table[256];
    static const bool ready = [] {
        for (int index = 0; index < 256; ++index) {
            quint32 value = quint32(index);
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (value >> 1) ^ 0xedb88320u : value >> 1;
            }
            table[index] = value;
        }
        return true;
    }();
    Q_UNUSED(ready);
    return table;
}

} // namespace

void PayloadChecksum::clear()
{
    crc16 = 0;
    m_crc32 = 0xffffffffu;
}

void PayloadChecksum::feed(const char *data, int size)
{
    if (!data || size <= 0) return;
    const quint32 *table = crc32Table();
    for (int i = 0; i < size; ++i) {
        const quint8 byte = quint8(data[i]);
        crc16 ^= quint16(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc16 = (crc16 & 0x8000)
                ? quint16((crc16 << 1) ^ 0x1021)
                : quint16(crc16 << 1);
        }
        m_crc32 = table[(m_crc32 ^ byte) & 0xff] ^ (m_crc32 >> 8);
    }
}
