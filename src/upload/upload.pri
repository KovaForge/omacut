# The upload hosts and providers, shared by the app and the test suites.
QT += network

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/hosts.h \
    $$PWD/httpjob.h \
    $$PWD/provider.h \
    $$PWD/providers.h \
    $$PWD/secretstore.h \
    $$PWD/sigv4.h \
    $$PWD/sxcu.h

SOURCES += \
    $$PWD/hosts.cpp \
    $$PWD/httpjob.cpp \
    $$PWD/provider.cpp \
    $$PWD/s3provider.cpp \
    $$PWD/secretstore.cpp \
    $$PWD/sigv4.cpp \
    $$PWD/sxcu.cpp \
    $$PWD/sxcuprovider.cpp
