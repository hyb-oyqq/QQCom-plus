QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = easyDebug_core_tests
msvc: QMAKE_CXXFLAGS += /utf-8
# 无 UI 核心测试：只链接服务层（domain/transport/app/discovery/persistence），
# 模块依赖（network/serialport/concurrent）与 include 路径都由各 .pri 自带。
include(../../src/services.pri)
SOURCES += tst_transport.cpp
