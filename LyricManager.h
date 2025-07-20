#pragma once
#include <QElapsedTimer>
#include <QRegExp>
#include <QString>
#include <QVector>

struct LyricLine {
  qint64 time;
  QString text;
};

class LyricManager {
public:
  LyricManager();
  void loadLyrics(const QString &path);
  void parseLyrics(const QString &lyricText, const QRegExp &rx);

  // 基于时间的歌词控制
  LyricLine getCurrentLyric(qint64 currentTime) const;
  LyricLine getLastLyric(qint64 currentTime) const;
  qint64 getCurrentLyricStartTime() const;
  qint64 getLastLyricStartTime() const;

  const QVector<LyricLine> &getLyrics() const;
  void reset();

private:
  QVector<LyricLine> lyrics;
  int currentLyricIndex;
  int lastLyricIndex;
  qint64 currentLyricTime;        // 当前显示歌词的开始时间
  qint64 lastLyricTime;           // 上一个显示歌词的开始时间
  QElapsedTimer lyricChangeTimer; // 用于记录歌词切换时间
};
