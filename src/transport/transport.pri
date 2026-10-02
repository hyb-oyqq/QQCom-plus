# 传输层：接口与四种端点实现。只依赖 domain 值类型与 QtNetwork/SerialPort，
# 不依赖 UI；复用方（主工程、核心测试）无需重复声明模块依赖。
QT += network

INCLUDEPATH += $$PWD/.. $$PWD/../..

SOURCES += \
    $$PWD/abstracttransport.cpp \
    $$PWD/serialtransport.cpp \
    $$PWD/tcpclienttransport.cpp \
    $$PWD/tcpservertransport.cpp \
    $$PWD/udptransport.cpp \
    $$PWD/transportfactory.cpp

HEADERS += \
    $$PWD/transporttypes.h \
    $$PWD/abstracttransport.h \
    $$PWD/serialtransport.h \
    $$PWD/tcpclienttransport.h \
    $$PWD/tcpservertransport.h \
    $$PWD/udptransport.h \
    $$PWD/transportfactory.h
