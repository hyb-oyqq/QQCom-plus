# 传输层：接口与四种端点实现。只依赖 domain 值类型与 QtNetwork/SerialPort，
# 不依赖 UI；复用方（主工程、核心测试）无需重复声明模块依赖。
QT += network
# UdpTransport 在 Windows 上调用 WSAIoctl(SIO_UDP_CONNRESET)。QtNetwork 是 DLL，
# 这个导入不会跟着进来，主工程和核心测试都要显式链接。
win32: LIBS += -lws2_32

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
