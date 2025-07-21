#pragma once
#include <QString>
#include <QVector>
#include <QRegExp>
#include <ass/ass.h>

struct SubtitleLine {
    qint64 startTime;
    qint64 endTime;
    QString text;
    QString style;
};

class SubtitleManager {
public:
    SubtitleManager();
    void loadSubtitle(const QString &path, ASS_Library *assLibrary, ASS_Renderer *assRenderer);
    void loadSrtSubtitle(const QString &path);
    void loadAssSubtitle(const QString &path, ASS_Library *assLibrary, ASS_Renderer *assRenderer);
    void updateSubtitleIndex(qint64 pts);
    bool findSimilarSubtitle(const QString &videoPath, QString &subtitlePath);
    int levenshteinDistance(const QString &s1, const QString &s2);
    const QVector<SubtitleLine>& getSubtitles() const;
    int getCurrentSubtitleIndex() const;
    bool hasAss() const;
    void reset();
    ASS_Track* getAssTrack() const;
    void setAssTrack(ASS_Track* track);
private:
    QVector<SubtitleLine> subtitles;
    int currentSubtitleIndex;
    bool hasAssSubtitle;
    ASS_Track* assTrack;
};
