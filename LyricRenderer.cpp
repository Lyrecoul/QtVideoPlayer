#include "LyricRenderer.h"
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QTimer>

LyricRenderer::LyricRenderer(LyricManager *manager) : lyricManager(manager) {}

void LyricRenderer::drawLyricsByTime(QPainter &p, const QRect &lyricRect,
                                     int overlayFontSize, qint64 currentTime) {
  // 获取当前歌词
  LyricLine currentLyric = lyricManager->getCurrentLyric(currentTime);

  if (currentLyric.text.isEmpty()) {
    return;
  }

  qint64 startTime = currentLyric.time;
  qint64 endTime = currentLyric.endTime;
  QString lyricText = currentLyric.text;
  const qint64 fadeDuration = 400; // 淡入淡出时间400ms
  int alpha = 0;

  qDebug() << "Rendering lyric at time:" << currentTime << "(" << startTime
           << "-" << endTime << "):" << lyricText;

  // 简化透明度计算
  if (currentTime < startTime) {
    // 未到显示时间
    return;
  } else if (currentTime < startTime + fadeDuration) {
    // 淡入阶段
    alpha = 255 * (currentTime - startTime) / fadeDuration;
  } else if (currentTime < endTime - fadeDuration) {
    // 完全显示阶段
    alpha = 255;
  } else if (currentTime < endTime) {
    // 淡出阶段
    alpha = 255 * (endTime - currentTime) / fadeDuration;
  } else {
    // 超过结束时间不再显示
    return;
  }

  // 绘制歌词
  QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
  p.setFont(lyricFont);
  QRect textRect = p.fontMetrics().boundingRect(
      lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
  textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
  textRect.moveCenter(lyricRect.center());

  // 绘制背景
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  QColor bgColor(0, 0, 0, 180 * alpha / 255);
  p.setPen(Qt::NoPen);
  p.setBrush(bgColor);
  p.drawRoundedRect(textRect, 12, 12);
  p.restore();

  // 绘制文本
  p.save();
  QColor textColor(255, 255, 255, alpha);
  p.setPen(textColor);
  p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
  p.restore();
}
