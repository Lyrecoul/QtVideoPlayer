#include "LyricRenderer.h"
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QTimer>

LyricRenderer::LyricRenderer(LyricManager *manager) : lyricManager(manager) {}

void LyricRenderer::drawLyricsByTime(QPainter &p, const QRect &lyricRect,
                                     int overlayFontSize, qint64 currentTime) {
  // 获取当前和上一个歌词
  LyricLine currentLyric = lyricManager->getCurrentLyric(currentTime);
  LyricLine lastLyric = lyricManager->getLastLyric(currentTime);

  // 获取歌词时间 - 直接从LyricLine获取
  qint64 currentLyricTime = currentLyric.time;

  // 当前歌词渲染
  if (!currentLyric.text.isEmpty()) {
    QString lyricText = currentLyric.text;

    // 设置淡入淡出持续时间（毫秒）
    const qint64 fadeDuration = 400;

    // 基础透明度
    int alpha = 255; // 最大透明度值

    // 计算当前歌词透明度 - 基于当前时间和歌词开始时间
    qint64 elapsed = currentTime - currentLyricTime;

    // 确保当前时间大于歌词开始时间
    if (currentTime >= currentLyricTime) {
      if (elapsed < fadeDuration) {
        // 淡入效果
        alpha = static_cast<int>(alpha * qMin(1.0, elapsed / double(fadeDuration)));
      }
    } else {
      // 如果当前时间小于歌词开始时间，设置为完全透明（不显示）
      alpha = 0;
    }

    QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
    p.setFont(lyricFont);
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
    textRect.moveCenter(lyricRect.center());

    // 绘制背景
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * alpha / 255.0));
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

  // 上一行歌词淡出
  if (!lastLyric.text.isEmpty() && lastLyric.text != currentLyric.text) {
    QString lyricText = lastLyric.text;

    // 设置淡出持续时间（毫秒）
    const qint64 fadeDuration = 600;
    int alpha = 0;

    // 计算淡出透明度 - 基于上一行歌词的时间和当前歌词的时间差
    qint64 lastLyricTime = lastLyric.time;
    qint64 elapsed = currentTime - currentLyricTime;
    // 只有当当前时间在当前歌词显示时间和当前歌词显示时间+淡出时间之间时才显示上一行歌词
    if (elapsed < fadeDuration && currentLyricTime > lastLyricTime) {
      alpha = static_cast<int>(255 * qMax(0.0, 1.0 - elapsed / double(fadeDuration)));
    }

    if (alpha > 0) {
      QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
      p.setFont(lyricFont);
      QRect textRect = p.fontMetrics().boundingRect(
          lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
      textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
      textRect.moveCenter(lyricRect.center());

      // 绘制背景
      p.save();
      p.setRenderHint(QPainter::Antialiasing, true);
      QColor bgColor(0, 0, 0, int(180 * alpha / 255.0));
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
  }
}