#include "LyricManager.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <algorithm>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/xiphcomment.h>

LyricManager::LyricManager()
    : currentLyricIndex(0), lastLyricIndex(-1), currentLyricTime(0),
      lastLyricTime(0) {
  lyricChangeTimer.start();
}

void LyricManager::loadLyrics(const QString &path) {
  lyrics.clear();
  currentLyricIndex = 0;
  lastLyricIndex = -1;
  bool embeddedLyricLoaded = false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  QByteArray header = file.peek(16);
  file.close();
  QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
  rx.setPatternSyntax(QRegExp::RegExp2);
  if (header.startsWith("ID3") ||
      header.mid(0, 2) == QByteArray::fromHex("FFFB")) {
    TagLib::MPEG::File mp3File(path.toUtf8().constData());
    if (mp3File.isValid() && mp3File.ID3v2Tag()) {
      auto *id3 = mp3File.ID3v2Tag();
      auto usltFrames = id3->frameListMap()["USLT"];
      if (!usltFrames.isEmpty()) {
        for (auto *frame : usltFrames) {
          auto *uslt =
              dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frame);
          if (uslt) {
            QString lyricText =
                QString::fromStdWString(uslt->text().toWString());
            parseLyrics(lyricText, rx);
            embeddedLyricLoaded = !lyrics.isEmpty();
            if (embeddedLyricLoaded)
              break;
          }
        }
      }
    }
  }
  if (header.startsWith("fLaC")) {
    TagLib::FLAC::File flacFile(path.toUtf8().constData());
    if (flacFile.isValid() && flacFile.xiphComment()) {
      auto *comment = flacFile.xiphComment();
      if (comment->contains("LYRICS")) {
        QString lyricText = QString::fromUtf8(
            comment->fieldListMap()["LYRICS"].toString().toCString(true));
        parseLyrics(lyricText, rx);
        embeddedLyricLoaded = !lyrics.isEmpty();
      }
    }
  }
  if (!embeddedLyricLoaded) {
    QString lrc = QFileInfo(path).absolutePath() + "/" +
                  QFileInfo(path).completeBaseName() + ".lrc";
    if (QFile::exists(lrc)) {
      QFile f(lrc);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        QString allLyrics = in.readAll();
        parseLyrics(allLyrics, rx);
      }
    }
  }
}

void LyricManager::parseLyrics(const QString &lyricText, const QRegExp &rx) {
  QStringList lines = lyricText.split('\n');
  QHash<qint64, QString> lyricMap;
  for (const QString &line : lines) {
    int pos = 0;
    QList<qint64> times;
    while ((pos = rx.indexIn(line, pos)) != -1) {
      qint64 t = rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
      times.append(t);
      pos += rx.matchedLength();
    }
    QString text = line;
    text = text.remove(rx).trimmed();
    if (!times.isEmpty() && !text.isEmpty()) {
      for (qint64 t : times) {
        if (lyricMap.contains(t)) {
          lyricMap[t] += "\n" + text;
        } else {
          lyricMap[t] = text;
        }
      }
    }
  }

  // 先收集所有歌词
  for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
    lyrics.append({it.key(), 0, it.value()});
  }

  // 按时间排序
  std::sort(
      lyrics.begin(), lyrics.end(),
      [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });

  // 计算每句歌词的结束时间
  if (!lyrics.isEmpty()) {
    for (int i = 0; i < lyrics.size(); i++) {
      if (i < lyrics.size() - 1) {
        lyrics[i].endTime = lyrics[i + 1].time;
      } else {
        // 最后一句歌词显示 5 秒
        lyrics[i].endTime = lyrics[i].time + 5000;
      }
    }
  }
}

const QVector<LyricLine> &LyricManager::getLyrics() const { return lyrics; }

void LyricManager::reset() {
  lyrics.clear();
  currentLyricIndex = 0;
  lastLyricIndex = -1;
  currentLyricTime = 0;
  lastLyricTime = 0;
  lyricChangeTimer.restart();
}

LyricLine LyricManager::getCurrentLyric(qint64 currentTime) const {
  if (lyrics.isEmpty()) {
    return {0, 0, ""};
  }

  // 添加调试输出
  // qDebug() << "Current time:" << currentTime;
  // for (const auto &lyric : lyrics) {
  //   qDebug() << "Lyric time:" << lyric.time << "-" << lyric.endTime << ":" <<
  //   lyric.text;
  // }

  int currentIdx = -1;
  for (int i = 0; i < lyrics.size(); i++) {
    if (lyrics[i].time <= currentTime) {
      currentIdx = i;
    } else {
      break;
    }
  }

  if (currentIdx < 0) {
    return {0, 0, ""};
  }

  return lyrics[currentIdx];
}

LyricLine LyricManager::getLastLyric(qint64 currentTime) const {
  if (lyrics.isEmpty()) {
    return {0, 0, ""};
  }

  int currentIdx = -1;
  for (int i = 0; i < lyrics.size(); i++) {
    if (lyrics[i].time <= currentTime) {
      currentIdx = i;
    } else {
      break;
    }
  }

  if (currentIdx <= 0) {
    return {0, 0, ""};
  }

  return lyrics[currentIdx - 1];
}

qint64 LyricManager::getCurrentLyricStartTime() const {
  return currentLyricTime;
}

qint64 LyricManager::getLastLyricStartTime() const { return lastLyricTime; }
