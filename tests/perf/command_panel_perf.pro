QT += core gui widgets
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = QQCom_command_panel_perf
msvc: QMAKE_CXXFLAGS += /utf-8
isEmpty(COMMAND_SOURCE_ROOT): COMMAND_SOURCE_ROOT = $$PWD/../../src
INCLUDEPATH += $$COMMAND_SOURCE_ROOT $$PWD/../../src
SOURCES += command_panel_perf.cpp \
    $$COMMAND_SOURCE_ROOT/ui/panels/commands/commandpanel.cpp \
    $$COMMAND_SOURCE_ROOT/ui/panels/commands/commandtablewidget.cpp \
    $$COMMAND_SOURCE_ROOT/ui/panels/commands/exportgroupsdialog.cpp \
    $$COMMAND_SOURCE_ROOT/ui/common/apptheme.cpp \
    $$COMMAND_SOURCE_ROOT/ui/common/styledialogs.cpp \
    $$COMMAND_SOURCE_ROOT/app/commandmanager.cpp \
    $$COMMAND_SOURCE_ROOT/app/commandformats.cpp \
    $$COMMAND_SOURCE_ROOT/domain/commanditem.cpp \
    $$COMMAND_SOURCE_ROOT/domain/commandset.cpp \
    $$COMMAND_SOURCE_ROOT/domain/networkaddress.cpp \
    $$COMMAND_SOURCE_ROOT/persistence/preferencesstore.cpp
HEADERS += $$COMMAND_SOURCE_ROOT/ui/panels/commands/commandpanel.h \
    $$COMMAND_SOURCE_ROOT/ui/panels/commands/commandtablewidget.h \
    $$COMMAND_SOURCE_ROOT/ui/panels/commands/exportgroupsdialog.h \
    $$COMMAND_SOURCE_ROOT/ui/common/apptheme.h \
    $$COMMAND_SOURCE_ROOT/app/commandmanager.h \
    $$COMMAND_SOURCE_ROOT/persistence/preferencesstore.h
exists($$COMMAND_SOURCE_ROOT/app/commandsendscheduler.cpp) {
    SOURCES += $$COMMAND_SOURCE_ROOT/app/commandsendscheduler.cpp
    HEADERS += $$COMMAND_SOURCE_ROOT/app/commandsendscheduler.h
}
RESOURCES += ../../resources/resources.qrc
