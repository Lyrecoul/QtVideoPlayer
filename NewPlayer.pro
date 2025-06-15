QT       += core widgets gui multimedia

CONFIG   += c++11
TEMPLATE = app

SOURCES += main.cpp \
           VideoPlayer.cpp \
           FFMpegDecoder.cpp

HEADERS += VideoPlayer.h \
           FFMpegDecoder.h

QMAKE_CXXFLAGS += -Wno-deprecated-declarations

INCLUDEPATH += $$PWD/include/
LIBS += -L$$PWD/libs/taglib -ltag \
        -L$$PWD/libs/libass -lass -lfribidi -lharfbuzz -lunibreak \
        -L$$PWD/libs/ffmpeg-static -lavformat -lavcodec -lavutil -lswscale -lswresample \
        -L/home/lyrecoul/PenDevelopment/lib/ -lfontconfig -lz -lpng16 -lexpat -lfreetype
