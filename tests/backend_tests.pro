QT += core gui quick quickcontrols2 multimedia testlib dbus network
CONFIG += c++17 testcase
TARGET = backend_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    fakehttpserver.h \
    ../src/backend.h \
    ../src/sxcu.h \
    ../src/uploader.h \
    ../src/ffmpeg.h \
    ../src/filepicker.h \
    ../src/portalfilepicker.h \
    ../src/thumbprovider.h \
    ../src/thumbworker.h

SOURCES += \
    backend_tests.cpp \
    ../src/backend.cpp \
    ../src/sxcu.cpp \
    ../src/uploader.cpp \
    ../src/ffmpeg.cpp \
    ../src/portalfilepicker.cpp \
    ../src/thumbprovider.cpp \
    ../src/thumbworker.cpp
