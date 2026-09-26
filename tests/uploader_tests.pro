QT += core network testlib
CONFIG += c++17 testcase
TARGET = uploader_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    fakehttpserver.h \
    uploadtestkit.h

SOURCES += \
    uploader_tests.cpp

include(../src/upload/upload.pri)
