QT += core gui widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = easyDebug_ui_tests
msvc: QMAKE_CXXFLAGS += /utf-8
INCLUDEPATH += ../..
include(../../src/ui/ui.pri)
SOURCES += tst_ui.cpp ../../mainwindow.cpp
HEADERS += ../../mainwindow.h
FORMS += ../../mainwindow.ui
RESOURCES += ../../resources/resources.qrc
