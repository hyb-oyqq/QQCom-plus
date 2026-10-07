QT += core gui widgets testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = QQCom_ui_tests
msvc: QMAKE_CXXFLAGS += /utf-8
# include 路径（ui/...、domain/...、项目根）由 ui.pri / services.pri 自带提供。
include(../../src/ui/ui.pri)
include(../../src/services.pri)
SOURCES += tst_ui.cpp ../../mainwindow.cpp
HEADERS += ../../mainwindow.h
FORMS += ../../mainwindow.ui
RESOURCES += ../../resources/resources.qrc
