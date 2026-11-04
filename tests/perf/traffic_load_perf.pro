QT += core gui widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
msvc: QMAKE_CXXFLAGS += /utf-8
include(../../src/ui/ui.pri)
include(../../src/services.pri)
TARGET = QQCom_traffic_load_perf
SOURCES += $$PWD/traffic_load_perf.cpp
RESOURCES += ../../resources/resources.qrc
