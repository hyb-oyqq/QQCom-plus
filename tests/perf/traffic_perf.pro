QT += core gui widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = QQCom_traffic_perf
msvc: QMAKE_CXXFLAGS += /utf-8
# 与 tests/ui/ui_smoke.pro 同构：直接编译真实的面板与组合根，测的就是出货代码。
include(../../src/ui/ui.pri)
include(../../src/services.pri)
SOURCES += traffic_perf.cpp ../../mainwindow.cpp
HEADERS += ../../mainwindow.h
FORMS += ../../mainwindow.ui
RESOURCES += ../../resources/resources.qrc
