#ifndef BYTESFORMAT_H
#define BYTESFORMAT_H

#include <QObject>
#include <QString>

// 字节数的唯一格式化：GB 两位小数、KB/MB 一位小数、B 保持整数——状态栏要的是
// "一眼量级"。主窗口的 RX/TX 速率与监视区的日志大小共用同一份实现，
// 免得同一串数字在状态栏和菜单里长得不一样。
inline QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QObject::tr(" GB");
    if (bytes >= 1024 * 1024)
        return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + QObject::tr(" MB");
    if (bytes >= 1024)
        return QString::number(double(bytes) / 1024.0, 'f', 1) + QObject::tr(" KB");
    return QString::number(bytes) + QObject::tr(" B");
}

#endif
