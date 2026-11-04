# UI 层：面板与复用控件。include 路径以项目根为基准（ui/...、domain/...），
# 不再使用跨目录相对路径；INCLUDEPATH 由本文件统一提供，复用方无需自带。
INCLUDEPATH += $$PWD/.. $$PWD/../..

# 编解码（含 core5compat 模块声明）独立成块，不含它的目标可以只取面板部分。
include($$PWD/common/codec.pri)

SOURCES += \
    $$PWD/panels/commands/commandpanel.cpp \
    $$PWD/panels/commands/commandtablewidget.cpp \
    $$PWD/panels/commands/exportgroupsdialog.cpp \
    $$PWD/panels/portselector/portselectorwidget.cpp \
    $$PWD/panels/traffic/trafficpanel.cpp \
    $$PWD/panels/traffic/trafficmodel.cpp \
    $$PWD/panels/traffic/trafficfilter.cpp \
    $$PWD/panels/traffic/traffictextview.cpp \
    $$PWD/panels/traffic/filterbar.cpp \
    $$PWD/panels/traffic/filterflags.cpp \
    $$PWD/panels/traffic/viewtoggles.cpp \
    $$PWD/panels/sending/sendpanel.cpp \
    $$PWD/panels/sending/filetransferwidget.cpp \
    $$PWD/panels/routing/routeeditorwidget.cpp \
    $$PWD/common/apptheme.cpp \
    $$PWD/common/endpointalertstate.cpp \
    $$PWD/common/endpointformat.cpp \
    $$PWD/common/styledialogs.cpp

HEADERS += \
    $$PWD/common/apptheme.h \
    $$PWD/common/endpointalertstate.h \
    $$PWD/common/tintedicon.h \
    $$PWD/common/stayopenmenu.h \
    $$PWD/common/endpointformat.h \
    $$PWD/common/bytesformat.h \
    $$PWD/common/combometrics.h \
    $$PWD/common/buttonmetrics.h \
    $$PWD/common/rowmetrics.h \
    $$PWD/common/stylehelpers.h \
    $$PWD/common/styledialogs.h \
    $$PWD/panels/commands/commandpanel.h \
    $$PWD/panels/commands/commandtablewidget.h \
    $$PWD/panels/commands/exportgroupsdialog.h \
    $$PWD/panels/portselector/portselectorwidget.h \
    $$PWD/panels/traffic/trafficpanel.h \
    $$PWD/panels/traffic/trafficmodel.h \
    $$PWD/panels/traffic/trafficfilter.h \
    $$PWD/panels/traffic/traffictextview.h \
    $$PWD/panels/traffic/filterbar.h \
    $$PWD/panels/traffic/filterflags.h \
    $$PWD/panels/traffic/viewtoggles.h \
    $$PWD/panels/sending/sendpanel.h \
    $$PWD/panels/sending/filetransferwidget.h \
    $$PWD/panels/routing/routeeditorwidget.h
