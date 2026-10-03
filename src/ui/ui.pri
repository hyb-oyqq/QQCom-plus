# UI 层：面板与复用控件。include 路径以项目根为基准（ui/...、domain/...），
# 不再使用跨目录相对路径；INCLUDEPATH 由本文件统一提供，复用方无需自带。
INCLUDEPATH += $$PWD/.. $$PWD/../..

# 编解码（含 core5compat 模块声明）独立成块，不含它的目标可以只取面板部分。
include($$PWD/common/codec.pri)

SOURCES += \
    $$PWD/panels/portselector/portselectorwidget.cpp \
    $$PWD/panels/traffic/trafficpanel.cpp \
    $$PWD/panels/traffic/traffictextview.cpp \
    $$PWD/panels/sending/sendpanel.cpp \
    $$PWD/panels/routing/routeeditorwidget.cpp \
    $$PWD/common/endpointformat.cpp

HEADERS += \
    $$PWD/common/tintedicon.h \
    $$PWD/common/endpointformat.h \
    $$PWD/common/combometrics.h \
    $$PWD/panels/portselector/portselectorwidget.h \
    $$PWD/panels/traffic/trafficpanel.h \
    $$PWD/panels/traffic/traffictextview.h \
    $$PWD/panels/sending/sendpanel.h \
    $$PWD/panels/routing/routeeditorwidget.h
