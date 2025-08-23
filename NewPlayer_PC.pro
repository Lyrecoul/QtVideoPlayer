QT       += core widgets gui multimedia

CONFIG   += c++11
TEMPLATE = app

SOURCES += main.cpp \
           VideoPlayer.cpp \
           FFMpegDecoder.cpp \
           LyricManager.cpp \
           SubtitleManager.cpp \
           LyricRenderer.cpp \
           SubtitleRenderer.cpp

HEADERS += VideoPlayer.h \
           FFMpegDecoder.h \
           LyricManager.h \
           SubtitleManager.h \
           LyricRenderer.h \
           SubtitleRenderer.h

RESOURCES += resources.qrc

QMAKE_CXXFLAGS += -Wno-deprecated-declarations

INCLUDEPATH += $$PWD/include/
LIBS += -L$$PWD/pc-libs/taglib -ltag \
        -L$$PWD/pc-libs/libass -lass -lfribidi -lharfbuzz -lunibreak \
        -L/home/lyrecoul/主机编译/ffmpeg-3.4.8/lib/ \
        -lavformat -lavcodec -lavutil -lswscale -lswresample \
        -lavdevice -lz -lasound -lavfilter \
        -lfontconfig -lfreetype -lexpat -lpng16 -lglib-2.0 -lpcre2-8 \
        -ldrm -lssl -lcrypto -lvorbisenc \
        -lvorbis -lmp3lame -lpcre -logg \
        -ltheoradec -ltheoraenc -lm -ldl -lgbm -lwayland-client \
        -lwayland-server -lffi
