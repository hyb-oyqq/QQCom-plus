#ifndef PORTDRAFT_H
#define PORTDRAFT_H

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
};

#endif
