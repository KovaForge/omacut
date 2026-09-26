QT += core network testlib
CONFIG += c++17 testcase
TARGET = provider_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    fakehttpserver.h \
    uploadtestkit.h

SOURCES += \
    provider_tests.cpp

include(../src/upload/upload.pri)
