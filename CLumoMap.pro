QT += network
QT += core widgets gui
QT += concurrent
QT += serialport


# install
 INSTALLS += widget

HEADERS += \
    CCloudPoints.h \
    CLumoMap.h \
    CComm.h \
    CMainWin.h \
    CProtocol.h \
    crc16.h \
    CCopyTableWidget.h
SOURCES += \
           CLumoMap.cpp \
           CComm.cpp \
           CMainWin.cpp \
           main.cpp
FORMS +=
QMAKE_CXXFLAGS += /utf-8
CONFIG += c++11

CONFIG(debug, debug|release) {
    DESTDIR = antiGravity/debug
}
CONFIG(release, debug|release) {
    DESTDIR = antiGravity/release
}
