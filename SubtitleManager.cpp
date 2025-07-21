#include "SubtitleManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

SubtitleManager::SubtitleManager()
    : currentSubtitleIndex(-1), hasAssSubtitle(false), assTrack(nullptr) {}

void SubtitleManager::loadSubtitle(const QString &path, ASS_Library *assLibrary, ASS_Renderer *assRenderer) {
  // 加载字幕（支持模糊匹配）
  QString basePath =
      QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName();
  QString assPath = basePath + ".ass";
  QString srtPath = basePath + ".srt";
  QString subtitlePath;
  if (QFile::exists(assPath)) {
    loadAssSubtitle(assPath, assLibrary, assRenderer);
  } else if (QFile::exists(srtPath)) {
    loadSrtSubtitle(srtPath);
  } else if (findSimilarSubtitle(path, subtitlePath)) {
    if (subtitlePath.endsWith(".ass", Qt::CaseInsensitive)) {
      loadAssSubtitle(subtitlePath, assLibrary, assRenderer);
    } else if (subtitlePath.endsWith(".srt", Qt::CaseInsensitive)) {
      loadSrtSubtitle(subtitlePath);
    }
  }
}

void SubtitleManager::loadSrtSubtitle(const QString &path) {
  subtitles.clear();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return;
  QTextStream in(&f);
  QString line;
  QRegExp timeRx(R"((\d+):(\d+):(\d+),(\d+)\s*-->\s*(\d+):(\d+):(\d+),(\d+))");
  timeRx.setPatternSyntax(QRegExp::RegExp2);
  while (!in.atEnd()) {
    line = in.readLine();
    if (line.trimmed().isEmpty())
      continue;
    QString timeLine = line;
    if (!timeRx.exactMatch(timeLine))
      continue;
    qint64 start = timeRx.cap(1).toInt() * 3600000 +
                   timeRx.cap(2).toInt() * 60000 +
                   timeRx.cap(3).toInt() * 1000 + timeRx.cap(4).toInt();
    qint64 end = timeRx.cap(5).toInt() * 3600000 +
                 timeRx.cap(6).toInt() * 60000 + timeRx.cap(7).toInt() * 1000 +
                 timeRx.cap(8).toInt();
    QString text;
    while (!in.atEnd()) {
      QString t = in.readLine();
      if (t.trimmed().isEmpty())
        break;
      if (!text.isEmpty())
        text += "\n";
      text += t;
    }
    subtitles.append({start, end, text, ""});
  }
  hasAssSubtitle = false;
}

void SubtitleManager::loadAssSubtitle(const QString &path,
                                      ASS_Library *assLibrary,
                                      ASS_Renderer *assRenderer) {
  hasAssSubtitle = false;
  if (!assLibrary || !assRenderer)
    return;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return;
  if (assTrack) {
    ass_free_track(assTrack);
    assTrack = nullptr;
  }
  assTrack = ass_read_file(assLibrary, path.toUtf8().constData(), nullptr);
  hasAssSubtitle = (assTrack != nullptr);
}

void SubtitleManager::updateSubtitleIndex(qint64 pts) {
  int subIdx = currentSubtitleIndex;
  // 先判断当前字幕是否还在区间内
  if (subIdx >= 0 && subIdx < subtitles.size() &&
      pts >= subtitles[subIdx].startTime && pts <= subtitles[subIdx].endTime) {
    // 当前字幕还在区间内，不做任何变化
    return;
  }
  // 查找新的字幕区间
  int newIdx = -1;
  for (int i = 0; i < subtitles.size(); ++i) {
    if (pts >= subtitles[i].startTime && pts <= subtitles[i].endTime) {
      newIdx = i;
      break;
    }
  }
  if (currentSubtitleIndex != newIdx) {
    currentSubtitleIndex = newIdx;
  }
}

bool SubtitleManager::findSimilarSubtitle(const QString &videoPath,
                                          QString &subtitlePath) {
  QFileInfo videoInfo(videoPath);
  QDir dir = videoInfo.absoluteDir();
  QString baseName = videoInfo.completeBaseName();
  QFileInfoList fileList = dir.entryInfoList(QDir::Files);
  double bestSimilarity = 0.0;
  QString bestMatch;
  const double similarityThreshold = 0.7;
  for (const QFileInfo &fileInfo : fileList) {
    QString fileName = fileInfo.fileName();
    if (fileName.endsWith(".ass", Qt::CaseInsensitive) ||
        fileName.endsWith(".srt", Qt::CaseInsensitive)) {
      QString subBaseName = fileInfo.completeBaseName();
      int distance = levenshteinDistance(baseName, subBaseName);
      double similarity = 1.0 - (double)distance / qMax(baseName.length(),
                                                        subBaseName.length());
      if (similarity > bestSimilarity) {
        bestSimilarity = similarity;
        bestMatch = fileInfo.absoluteFilePath();
      }
    }
  }
  if (bestSimilarity >= similarityThreshold) {
    subtitlePath = bestMatch;
    return true;
  }
  return false;
}

int SubtitleManager::levenshteinDistance(const QString &s1, const QString &s2) {
  const int len1 = s1.length();
  const int len2 = s2.length();
  QVector<QVector<int>> dp(len1 + 1, QVector<int>(len2 + 1));
  for (int i = 0; i <= len1; i++) {
    dp[i][0] = i;
  }
  for (int j = 0; j <= len2; j++) {
    dp[0][j] = j;
  }
  for (int i = 1; i <= len1; i++) {
    for (int j = 1; j <= len2; j++) {
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      dp[i][j] = qMin(qMin(dp[i - 1][j] + 1, dp[i][j - 1] + 1),
                      dp[i - 1][j - 1] + cost);
    }
  }
  return dp[len1][len2];
}

const QVector<SubtitleLine> &SubtitleManager::getSubtitles() const {
  return subtitles;
}

int SubtitleManager::getCurrentSubtitleIndex() const {
  return currentSubtitleIndex;
}

bool SubtitleManager::hasAss() const { return hasAssSubtitle; }

void SubtitleManager::reset() {
  subtitles.clear();
  currentSubtitleIndex = -1;
  hasAssSubtitle = false;
  if (assTrack) {
    ass_free_track(assTrack);
    assTrack = nullptr;
  }
}

ASS_Track *SubtitleManager::getAssTrack() const { return assTrack; }

void SubtitleManager::setAssTrack(ASS_Track *track) {
  if (assTrack) {
    ass_free_track(assTrack);
  }
  assTrack = track;
}
