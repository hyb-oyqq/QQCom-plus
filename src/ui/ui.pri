# GBK 编解码（payloadcodec.cpp 用 QTextCodec）来自 QtCore5Compat；
# 由本 .pri 声明依赖，复用 ui.pri 的工程无需重复声明。
QT += core5compat

SOURCES += \
    $$PWD/panels/portselector/portselectorwidget.cpp \
    $$PWD/panels/traffic/trafficpanel.cpp \
    $$PWD/panels/sending/sendpanel.cpp \
    $$PWD/panels/routing/routeeditorwidget.cpp \
    $$PWD/common/payloadcodec.cpp

HEADERS += \
    $$PWD/common/elidedlabel.h \
    $$PWD/common/contentscrollarea.h \
    $$PWD/common/distributedlabel.h \
    $$PWD/common/tintedicon.h \
    $$PWD/common/payloadcodec.h \
    $$PWD/panels/portselector/portselectorwidget.h \
    $$PWD/panels/traffic/trafficpanel.h \
    $$PWD/panels/sending/sendpanel.h \
    $$PWD/panels/routing/routeeditorwidget.h
