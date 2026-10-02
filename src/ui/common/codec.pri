# 字节编解码单一真源（PayloadCodec）。GBK 依赖 QtCore5Compat 的
# QTextCodec（Qt6 的 QStringConverter 不带 GBK），该模块声明收敛在此：
# 不含 codec 的目标不 include 本文件，即不引入 Core5Compat 依赖。
QT += core5compat
INCLUDEPATH += $$PWD/../.. $$PWD/../../..

SOURCES += $$PWD/payloadcodec.cpp

HEADERS += $$PWD/payloadcodec.h
