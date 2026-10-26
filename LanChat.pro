QT += core widgets network sql concurrent
CONFIG += c++17 console
TARGET = LanChat
CONFIG -= app_bundle
SOURCES += src/main.cpp src/protocol.cpp src/server.cpp src/client.cpp
HEADERS += src/protocol.h src/server.h src/client.h

# 强制 MSVC 使用 UTF-8
msvc {
    QMAKE_CXXFLAGS += /utf-8
}
# 强制 MinGW 使用 UTF-8 (你当前用的编译器)
mingw {
    QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
}