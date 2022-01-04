QT -= gui

CONFIG += c++11 console
CONFIG -= app_bundle

unix: LIBS += -lpulse -lX11 -lhidapi-hidraw

DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        keyboarddriver.cpp \
        main.cpp \
        pulseaudio.cpp \
        runguard.cpp \
        xhklib.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += \
    keyboarddriver.h \
    pulseaudio.h \
    runguard.h \
    xhklib.h
