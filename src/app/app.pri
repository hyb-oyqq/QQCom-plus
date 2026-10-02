# 应用层运行时：端点生命周期与消息装配。依赖 transport 与 domain；
# 对象归属 I/O 线程，GUI 只见队列信号。
INCLUDEPATH += $$PWD/.. $$PWD/../..

SOURCES += $$PWD/endpointruntime.cpp
HEADERS += $$PWD/endpointruntime.h
