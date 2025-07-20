#include "LyricManager.h"
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
  for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
    lyrics.append({it.key(), it.value()});
  }
  std::sort(
      lyrics.begin(), lyrics.end(),
      [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });
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
  // 如果没有歌词，返回空行
  if (lyrics.isEmpty()) {
    return {0, ""};
  }

  // 查找当前应该显示的歌词（时间小于等于当前时间的最新歌词）
  int currentIdx = -1;
  for (int i = 0; i < lyrics.size(); i++) {
    if (lyrics[i].time <= currentTime) {
      currentIdx = i;
    } else {
      break; // 找到第一个时间大于当前时间的歌词，结束循环
    }
  }

  // 如果找不到符合条件的歌词（所有歌词时间都大于当前时间）
  if (currentIdx < 0) {
    return {0, ""};
  }

  // 返回找到的最新歌词
  return lyrics[currentIdx];
}

LyricLine LyricManager::getLastLyric(qint64 currentTime) const {
  // 如果没有歌词，返回空行
  if (lyrics.isEmpty()) {
    return {0, ""};
  }

  // 找到当前应显示的歌词（时间小于等于当前时间的最新歌词）
  int currentIdx = -1;
  for (int i = 0; i < lyrics.size(); i++) {
    if (lyrics[i].time <= currentTime) {
      currentIdx = i;
    } else {
      break; // 找到第一个时间大于当前时间的歌词，结束循环
    }
  }

  // 如果找不到当前歌词或者当前是第一行歌词，返回空行
  if (currentIdx <= 0) {
    return {0, ""};
  }

  // 返回当前歌词的上一行
  return lyrics[currentIdx - 1];
}

qint64 LyricManager::getCurrentLyricStartTime() const {
  return currentLyricTime;
}

qint64 LyricManager::getLastLyricStartTime() const { return lastLyricTime; }
