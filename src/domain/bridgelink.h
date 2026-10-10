#ifndef BRIDGELINK_H
#define BRIDGELINK_H

#include <QMetaType>

// A/B 桥接的方向。索引与路由下拉框一致：0 只把 A 转到 B，1 只把 B 转到 A，
// 2 双向。转发发生在原始接收字节上，不经过显示组帧改写边界。
enum class BridgeDirection
{
    AtoB = 0,
    BtoA = 1,
    Both = 2
};

struct BridgeLink
{
    bool enabled = false;
    int direction = int(BridgeDirection::Both);

    bool forwards(int fromIndex) const
    {
        if (!enabled || fromIndex < 0 || fromIndex > 1) return false;
        if (direction == int(BridgeDirection::Both)) return true;
        return direction == fromIndex;
    }

    int targetOf(int fromIndex) const
    {
        return forwards(fromIndex) ? 1 - fromIndex : -1;
    }
};

Q_DECLARE_METATYPE(BridgeLink)

#endif
