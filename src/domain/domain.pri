# 领域值类型：纯 QtCore，无 UI 与硬件依赖。只需要配置语义的目标
# （未来的核心测试、SessionStore）include 本文件即可，不带入 SerialPort。
INCLUDEPATH += $$PWD/.. $$PWD/../..

SOURCES += \
    $$PWD/commanditem.cpp \
    $$PWD/commandset.cpp \
    $$PWD/networkaddress.cpp \
    $$PWD/trafficmessage.cpp

HEADERS += \
    $$PWD/commanditem.h \
    $$PWD/commandset.h \
    $$PWD/networkaddress.h \
    $$PWD/portdraft.h \
    $$PWD/receiveframing.h \
    $$PWD/serialportdescriptor.h \
    $$PWD/trafficmessage.h
