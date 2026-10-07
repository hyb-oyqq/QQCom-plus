QT += core gui widgets
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = QQCom_command_refresh_perf
msvc: QMAKE_CXXFLAGS += /utf-8
# 与 tests/ui/ui_smoke.pro 同构：直接编译真实面板，测的就是出货代码。
include(../../src/ui/ui.pri)
include(../../src/services.pri)
SOURCES += command_refresh_perf.cpp
RESOURCES += ../../resources/resources.qrc
