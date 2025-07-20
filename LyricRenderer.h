#pragma once
#include "LyricManager.h"
#include <QElapsedTimer>
#include <QPainter>
#include <QRect>

class LyricManager; // 前置声明

class LyricRenderer {
public:
  LyricRenderer(LyricManager *manager);

  void drawLyricsByTime(QPainter &p, const QRect &lyricRect,
                        int overlayFontSize, qint64 currentTime);

private:
  LyricManager *lyricManager;
};
