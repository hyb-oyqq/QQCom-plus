# 串口发现：后台枚举服务。QtConcurrent 与 SerialPort 只在此声明，
# 复用 discovery.pri 的工程无需重复声明，不用的目标不受牵连。
QT += serialport concurrent
INCLUDEPATH += $$PWD/.. $$PWD/../..

SOURCES += $$PWD/portdiscoveryservice.cpp

HEADERS += $$PWD/portdiscoveryservice.h
