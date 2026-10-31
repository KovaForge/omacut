QT += core gui qml quick quickcontrols2 multimedia dbus

CONFIG += c++17 release
TARGET = omacut
TEMPLATE = app

HEADERS += \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/edit.h \
    src/ffmpeg.h \
    src/thumbprovider.h \
    src/timeline.h \
    src/backend.h

SOURCES += \
    src/main.cpp \
    src/portalfilepicker.cpp \
    src/edit.cpp \
    src/ffmpeg.cpp \
    src/thumbprovider.cpp \
    src/timeline.cpp \
    src/backend.cpp

RESOURCES += src/resources.qrc
