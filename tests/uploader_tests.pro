QT += core network testlib
CONFIG += c++17 testcase
TARGET = uploader_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    fakehttpserver.h \
    ../src/sxcu.h \
    ../src/uploader.h

SOURCES += \
    uploader_tests.cpp \
    ../src/sxcu.cpp \
    ../src/uploader.cpp
