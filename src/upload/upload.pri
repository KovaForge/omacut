# The upload hosts and providers, shared by the app and the test suites.
QT += network

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/hosts.h \
    $$PWD/httpjob.h \
    $$PWD/provider.h \
    $$PWD/providers.h \
    $$PWD/secretstore.h \
    $$PWD/sxcu.h

SOURCES += \
    $$PWD/hosts.cpp \
    $$PWD/httpjob.cpp \
    $$PWD/provider.cpp \
    $$PWD/secretstore.cpp \
    $$PWD/sxcu.cpp \
    $$PWD/sxcuprovider.cpp
