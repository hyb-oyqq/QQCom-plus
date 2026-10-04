# 应用层运行时：端点生命周期与消息装配。依赖 transport 与 domain；
# 对象归属 I/O 线程，GUI 只见队列信号。
INCLUDEPATH += $$PWD/.. $$PWD/../..

SOURCES += \
    $$PWD/commandmanager.cpp \
    $$PWD/endpointruntime.cpp \
    $$PWD/receiveframer.cpp

HEADERS += \
    $$PWD/commandmanager.h \
    $$PWD/endpointruntime.h \
    $$PWD/receiveframer.h
