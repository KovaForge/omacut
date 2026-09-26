QT += core network testlib
CONFIG += c++17 testcase
TARGET = uploader_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/sxcu.h

SOURCES += \
    uploader_tests.cpp \
    ../src/sxcu.cpp
