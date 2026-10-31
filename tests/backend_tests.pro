QT += core gui quick quickcontrols2 multimedia testlib dbus
CONFIG += c++17 testcase
TARGET = backend_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/backend.h \
    ../src/edit.h \
    ../src/ffmpeg.h \
    ../src/filepicker.h \
    ../src/portalfilepicker.h \
    ../src/thumbprovider.h \
    ../src/timeline.h \

SOURCES += \
    backend_tests.cpp \
    ../src/backend.cpp \
    ../src/edit.cpp \
    ../src/ffmpeg.cpp \
    ../src/portalfilepicker.cpp \
    ../src/thumbprovider.cpp \
    ../src/timeline.cpp \
