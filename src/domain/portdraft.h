#ifndef PORTDRAFT_H
#define PORTDRAFT_H

#include "receiveframing.h"
#include <QVariantMap>
#include <QUuid>

// 端点草稿：界面上一份可编辑的端口配置值对象。只有类型与参数，没有运行时状态——
// 真实传输对象与持久化配置是独立类型。领域类型独立成头，避免每个只想引用它的
// 单元被迫包含 UI 控件声明。
struct PortDraft
{
    // 端点类型，同时也是类型下拉框的索引；非限定枚举可隐式转 int。
    enum Kind
    {
        Serial = 0,
        TcpClient,
        TcpServer,
        Udp
    };

    QUuid id = QUuid::createUuid();
    int kind = Serial; // PortDraft::Kind
    QVariantMap values;
    // 接收组帧策略。贴在草稿上而不是运行时里：它随端点配置一起编辑、随 open 一起下发，
    // 且只影响字节流端点（UDP 数据报自带边界，不经过组帧器）。
    ReceiveFraming framing;
};

// 跨线程命令（GUI → I/O 线程的 openEndpoint）按值携带草稿，需要元类型。
Q_DECLARE_METATYPE(PortDraft)

#endif
