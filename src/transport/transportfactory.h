#ifndef TRANSPORTFACTORY_H
#define TRANSPORTFACTORY_H

#include <QObject>
#include "domain/portdraft.h"
#include "transport/abstracttransport.h"

// 按端点类型创建传输实例。工厂只负责"选类型"，不注册 UI、不管理
// 生命周期——实例归创建方（EndpointRuntime）所有。
class TransportFactory
{
public:
    static AbstractTransport *create(int kind, int endpointIndex, QObject *parent);
};

#endif
