#include "VideoPlayer.h"
#include "qdebug.h"
#include "qglobal.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess> // 新增
#include <QTextStream>
#include <ass/ass.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/xiphcomment.h>

VideoPlayer::VideoPlayer(QWidget *parent) : QWidget(parent) {
  setAttribute(Qt::WA_AcceptTouchEvents);
  setWindowFlags(Qt::FramelessWindowHint);

  // AudioOutput
  QAudioFormat format;
  format.setSampleRate(44100);
  format.setChannelCount(2);
  format.setSampleSize(16);
  format.setCodec("audio/pcm");
  format.setByteOrder(QAudioFormat::LittleEndian);
  format.setSampleType(QAudioFormat::SignedInt);

  audioOutput =
      new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(), format);
  audioIO = audioOutput->start();

  // Decoder
  decoder = new FFMpegDecoder(this);
  connect(decoder, &FFMpegDecoder::frameReady, this, &VideoPlayer::onFrame);
  connect(decoder, &FFMpegDecoder::audioReady, this, &VideoPlayer::onAudioData);
  connect(decoder, &FFMpegDecoder::durationChanged, this,
          [&](qint64 d) { duration = d; });
  connect(decoder, &FFMpegDecoder::positionChanged, this,
          &VideoPlayer::onPositionChanged);

  // Overlay 更新
  overlayTimer = new QTimer(this);
  overlayTimer->setInterval(200);
  connect(overlayTimer, &QTimer::timeout, this, &VideoPlayer::updateOverlay);

  // 新增：进度条和媒体信息显示定时器
  overlayBarTimer = new QTimer(this);
  overlayBarTimer->setSingleShot(true);
  connect(overlayBarTimer, &QTimer::timeout, this, [this]() {
    showOverlayBar = false;
    update();
  });
  showOverlayBar = false;

  // 文件名滚动
  scrollTimer = new QTimer(this);
  scrollTimer->setInterval(40); // 25fps
  scrollPause = false;
  scrollPauseTimer = new QTimer(this);
  scrollPauseTimer->setSingleShot(true);
  connect(scrollTimer, &QTimer::timeout, this, [this]() {
    if (scrollPause)
      return;
    scrollOffset += 2;
    // 需要 infoText 宽度，延后到 paintEvent 处理
    update();
  });
  connect(scrollPauseTimer, &QTimer::timeout, this, [this]() {
    scrollPause = false;
    scrollOffset = 0;
    update();
  });
  scrollOffset = 0;

  // 新增：字幕定时检查定时器
  subtitleCheckTimer = new QTimer(this);
  subtitleCheckTimer->setInterval(1000);
  connect(subtitleCheckTimer, &QTimer::timeout, this, [this]() {
    if (!subtitles.isEmpty()) {
      bool found = false;
      for (int i = 0; i < subtitles.size(); ++i) {
        if (currentPts >= subtitles[i].startTime &&
            currentPts <= subtitles[i].endTime) {
          found = true;
          break;
        }
      }
      if (!found && currentSubtitleIndex != -1) {
        currentSubtitleIndex = -1;
        update();
      }
    }
  });

  // 新增：libass 初始化
  assLibrary = ass_library_init();
  assRenderer = assLibrary ? ass_renderer_init(assLibrary) : nullptr;
  if (assRenderer) {
    ass_set_fonts(assRenderer, nullptr, "Microsoft YaHei", 1, nullptr, 1);
  }
  assTrack = nullptr;
  hasAssSubtitle = false;

  // 新增：屏幕状态文件监听
  screenStatusWatcher = new QFileSystemWatcher(this);
  QString screenStatusPath = "/tmp/screen_status";
  QString screenStatusDir = QFileInfo(screenStatusPath).absolutePath();
  screenStatusWatcher->addPath(screenStatusDir);
  connect(screenStatusWatcher, &QFileSystemWatcher::directoryChanged, this,
          [screenStatusPath]() {
            if (QFile::exists(screenStatusPath)) {
              // 屏幕关闭，自动启动音频输出
              QTimer::singleShot(3000, []() {
                QProcess::execute("ubus",
                                  QStringList()
                                      << "call" << "eq_drc_process.output.rpc"
                                      << "control" << R"({"action":"Open"})");
              });
            }
          });
}

VideoPlayer::~VideoPlayer() {
  decoder->stop();
  audioOutput->stop();
  scrollTimer->stop();
  subtitleCheckTimer->stop();

  // 新增：libass 资源释放
  if (assTrack) {
    ass_free_track(assTrack);
    assTrack = nullptr;
  }
  if (assRenderer) {
    ass_renderer_done(assRenderer);
    assRenderer = nullptr;
  }
  if (assLibrary) {
    ass_library_done(assLibrary);
    assLibrary = nullptr;
  }

  // 关闭音频输出（使用 startDetached，避免不执行）
  // QProcess::startDetached("ubus", QStringList() << "call" <<
  // "eq_drc_process.output.rpc" << "control" << R"({"action":"Close"})");
}

void VideoPlayer::play(const QString &path) {
  // 启动音频输出
  QProcess::execute("ubus", QStringList()
                                << "call" << "eq_drc_process.output.rpc"
                                << "control" << R"({"action":"Open"})");

  loadLyrics(path);
  currentLyricIndex = 0; // 播放新文件时重置歌词下标

  // 新增：加载同名 ass 字幕（优先于 srt）
  subtitles.clear();
  currentSubtitleIndex = -1;
  hasAssSubtitle = false;
  if (assTrack) {
    ass_free_track(assTrack);
    assTrack = nullptr;
  }
  QString assPath = QFileInfo(path).absolutePath() + "/" +
                    QFileInfo(path).completeBaseName() + ".ass";
  if (QFile::exists(assPath)) {
    loadAssSubtitle(assPath);
  } else {
    // 没有 ass 再尝试 srt
    QString srtPath = QFileInfo(path).absolutePath() + "/" +
                      QFileInfo(path).completeBaseName() + ".srt";
    if (QFile::exists(srtPath)) {
      loadSrtSubtitle(srtPath);
    }
  }

  // 读取视频/音频信息
  videoInfoLabel.clear();
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, path.toUtf8().constData(), nullptr,
                          nullptr) == 0) {
    if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
      int vid_idx = -1, aid_idx = -1;
      for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0)
          vid_idx = i;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
          aid_idx = i;
      }
      if (vid_idx >= 0) {
        AVCodecParameters *vpar = fmt_ctx->streams[vid_idx]->codecpar;
        videoInfoLabel +=
            QString("视频: %1x%2  ").arg(vpar->width).arg(vpar->height);
      }
      if (aid_idx >= 0) {
        AVCodecParameters *apar = fmt_ctx->streams[aid_idx]->codecpar;
        videoInfoLabel += QString("音频: %1Hz  ")
                              .arg(apar->sample_rate);
      }
      if (fmt_ctx->duration > 0) {
        int sec = fmt_ctx->duration / AV_TIME_BASE;
        int min = sec / 60;
        sec = sec % 60;
        videoInfoLabel += QString("时长: %1:%2  ")
                              .arg(min, 2, 10, QChar('0'))
                              .arg(sec, 2, 10, QChar('0'));
      }
    }
    avformat_close_input(&fmt_ctx);
  }
  // 新增：保存文件名
  currentFileName = QFileInfo(path).fileName();

  // 重置滚动
  scrollOffset = 0;
  scrollTimer->stop();

  decoder->start(path);
  show();
  showOverlayBarForSeconds(5); // 播放时显示 overlay

  // 启动滚动定时器
  scrollTimer->start();

  // 启动字幕定时检查
  subtitleCheckTimer->start();
}

void VideoPlayer::loadLyrics(const QString &path) {
  lyrics.clear();
  bool embeddedLyricLoaded = false;

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  QByteArray header = file.peek(16);
  file.close();

  if (header.startsWith("ID3") ||
      header.mid(0, 2) == QByteArray::fromHex("FFFB")) {
    // MP3: TagLib 读取封面和内嵌歌词
    TagLib::MPEG::File mp3File(path.toUtf8().constData());
    if (mp3File.isValid() && mp3File.ID3v2Tag()) {
      auto *id3 = mp3File.ID3v2Tag();
      // 读取内嵌歌词（USLT 帧）
      auto usltFrames = id3->frameListMap()["USLT"];
      if (!usltFrames.isEmpty()) {
        for (auto *frame : usltFrames) {
          auto *uslt =
              dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frame);
          if (uslt) {
            QString lyricText =
                QString::fromStdWString(uslt->text().toWString());
            QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
            QStringList lines = lyricText.split('\n');
            QMap<qint64, QString> lyricMap;
            for (const QString &line : lines) {
              int pos = 0;
              QList<qint64> times;
              while ((pos = rx.indexIn(line, pos)) != -1) {
                qint64 t = rx.cap(1).toInt() * 60000 +
                           int(rx.cap(2).toDouble() * 1000);
                times.append(t);
                pos += rx.matchedLength();
              }
              QString text = line;
              text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
              text = text.trimmed();
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
            if (lyricMap.isEmpty() && !lyricText.trimmed().isEmpty()) {
              lyrics.append({0, lyricText.trimmed()});
            } else {
              for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd();
                   ++it) {
                lyrics.append({it.key(), it.value()});
              }
              std::sort(lyrics.begin(), lyrics.end(),
                        [](const LyricLine &a, const LyricLine &b) {
                          return a.time < b.time;
                        });
            }
            embeddedLyricLoaded = !lyrics.isEmpty();
            if (embeddedLyricLoaded)
              break;
          }
        }
      }
    }
  }
  if (header.startsWith("fLaC")) {
    // FLAC: TagLib 读取 Vorbis Comment 的 LYRICS 字段
    TagLib::FLAC::File flacFile(path.toUtf8().constData());
    if (flacFile.isValid() && flacFile.xiphComment()) {
      auto *comment = flacFile.xiphComment();
      if (comment->contains("LYRICS")) {
        // 修正：先 toString，再转 QString
        QString lyricText = QString::fromUtf8(
            comment->fieldListMap()["LYRICS"].toString().toCString(true));
        QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
        QStringList lines = lyricText.split('\n');
        QMap<qint64, QString> lyricMap;
        for (const QString &line : lines) {
          int pos = 0;
          QList<qint64> times;
          while ((pos = rx.indexIn(line, pos)) != -1) {
            qint64 t =
                rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
            times.append(t);
            pos += rx.matchedLength();
          }
          QString text = line;
          text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
          text = text.trimmed();
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
        if (lyricMap.isEmpty() && !lyricText.trimmed().isEmpty()) {
          lyrics.append({0, lyricText.trimmed()});
        } else {
          for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd();
               ++it) {
            lyrics.append({it.key(), it.value()});
          }
          std::sort(lyrics.begin(), lyrics.end(),
                    [](const LyricLine &a, const LyricLine &b) {
                      return a.time < b.time;
                    });
        }
        embeddedLyricLoaded = !lyrics.isEmpty();
      }
    }
  }
  // 其他类型或未读取到内嵌歌词，尝试读取同名 .lrc
  if (!embeddedLyricLoaded) {
    QString lrc = QFileInfo(path).absolutePath() + "/" +
                  QFileInfo(path).completeBaseName() + ".lrc";
    if (QFile::exists(lrc)) {
      QFile f(lrc);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        QMap<qint64, QString> lyricMap;
        while (!in.atEnd()) {
          QString line = in.readLine();
          QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
          int pos = 0;
          QList<qint64> times;
          while ((pos = rx.indexIn(line, pos)) != -1) {
            qint64 t =
                rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
            times.append(t);
            pos += rx.matchedLength();
          }
          QString text = line;
          text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
          text = text.trimmed();
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
        std::sort(lyrics.begin(), lyrics.end(),
                  [](const LyricLine &a, const LyricLine &b) {
                    return a.time < b.time;
                  });
      }
    }
  }
}

void VideoPlayer::loadSrtSubtitle(const QString &path) {
  subtitles.clear();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return;
  QTextStream in(&f);
  QString line;
  QRegExp timeRx(R"((\d+):(\d+):(\d+),(\d+)\s*-->\s*(\d+):(\d+):(\d+),(\d+))");
  while (!in.atEnd()) {
    // 跳过序号行
    line = in.readLine();
    if (line.trimmed().isEmpty())
      continue;
    // 时间行
    QString timeLine = line;
    if (!timeRx.exactMatch(timeLine))
      continue;
    qint64 start = timeRx.cap(1).toInt() * 3600000 +
                   timeRx.cap(2).toInt() * 60000 +
                   timeRx.cap(3).toInt() * 1000 + timeRx.cap(4).toInt();
    qint64 end = timeRx.cap(5).toInt() * 3600000 +
                 timeRx.cap(6).toInt() * 60000 + timeRx.cap(7).toInt() * 1000 +
                 timeRx.cap(8).toInt();
    // 字幕内容
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
}

void VideoPlayer::loadAssSubtitle(const QString &path) {
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

void VideoPlayer::onFrame(const QImage &frame) {
  currentFrame = frame.copy();
  update();
}

void VideoPlayer::onAudioData(const QByteArray &data) { audioIO->write(data); }

void VideoPlayer::onPositionChanged(qint64 pts) {
  // 拖动 seeking 时不更新进度条进度
  if (isSeeking) {
    return;
  }
  currentPts = pts;
  // 修正歌词下标同步，支持快退
  int idx = 0;
  while (idx + 1 < lyrics.size() && lyrics[idx + 1].time <= pts) {
    idx++;
  }
  if (currentLyricIndex != idx) {
    lastLyricIndex = currentLyricIndex;
    currentLyricIndex = idx;
    lyricFadeTimer.restart();
    lyricOpacity = 0.0;
    overlayTimer->start();
  }

  // SRT 字幕同步
  int subIdx = currentSubtitleIndex; // 保持上一次索引，避免闪烁
  for (int i = 0; i < subtitles.size(); ++i) {
    if (pts >= subtitles[i].startTime && pts <= subtitles[i].endTime) {
      subIdx = i;
      break;
    }
  }
  // 仅当字幕实际切换时才更新
  if (currentSubtitleIndex != subIdx) {
    currentSubtitleIndex = subIdx;
    update();
  }

  update(); // 确保进度条和歌词/字幕刷新
}

void VideoPlayer::mousePressEvent(QMouseEvent *e) {
  pressed = true;
  pressPos = e->pos();
  // pressTimer.start();
  // 移除长按关闭窗口逻辑
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *) {
  pressed = false;
  if (isSeeking) {
    isSeeking = false;
    // seek 前检查 duration 是否有效
    if (duration > 0 && currentPts >= 0 && currentPts <= duration) {
      decoder->seek(currentPts);
    }
    showOverlayBarForSeconds(5);
  } else {
    decoder->togglePause();
    // 判断当前是否为暂停状态
    if (decoder->isPaused()) {
      // 暂停时一直显示 overlay
      overlayBarTimer->stop();
      showOverlayBar = true;
      update();
    } else {
      // 播放时显示 5 秒 overlay
      showOverlayBarForSeconds(5);
    }
  }
}

// 改为双击关闭窗口
void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *) { close(); }

void VideoPlayer::mouseMoveEvent(QMouseEvent *e) {
  if (!pressed)
    return;
  int dx = e->pos().x() - pressPos.x();
  // 优化：实时响应滑动，无需大于20像素
  isSeeking = true;
  seekByDelta(dx);
  // seeking 时一直显示 overlay，不自动隐藏
  overlayBarTimer->stop();
  showOverlayBar = true;
  update(); // 实时刷新进度条
}

void VideoPlayer::seekByDelta(int dx) {
  // 动态调整每像素对应的毫秒数，随视频时长自适应
  // 例如：每像素调整为总时长的1/500，限制最小20ms，最大2000ms
  qint64 msPerPx = 20;
  if (duration > 0) {
    msPerPx = qBound<qint64>(20, duration / 5000, 2000);
  }
  qint64 delta = dx * msPerPx;
  qint64 target = qBound<qint64>(0, currentPts + delta, duration);
  currentPts = target;
}

void VideoPlayer::resizeEvent(QResizeEvent *) {
  // 移除 videoWidget 相关代码
}

void VideoPlayer::paintEvent(QPaintEvent *) {
  QPainter p(this);
  // 填充黑色背景，防止残影
  p.fillRect(rect(), Qt::black);

  // 如果有视频帧则绘制视频帧，否则绘制封面
  if (!currentFrame.isNull()) {
    // 保持比例居中显示
    QSize imgSize = currentFrame.size();
    QSize widgetSize = size();
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), imgSize);
    targetRect.moveCenter(rect().center());
    p.drawImage(targetRect, currentFrame);
  }

  // 只在 showOverlayBar 为 true 时绘制媒体信息和进度条
  if (showOverlayBar) {
    // 左上角视频信息标签
    if (!videoInfoLabel.isEmpty() || !currentFileName.isEmpty()) {
      QFont infoFont("Microsoft YaHei", overlayFontSize,
                     QFont::Bold); // 使用统一字号
      p.setFont(infoFont);
      p.setPen(Qt::white);
      QRect infoRect = QRect(10, 10, width() / 1.5, 22);
      p.setBrush(QColor(0, 0, 0, 128));
      p.setRenderHint(QPainter::Antialiasing, true);
      p.drawRoundedRect(infoRect.adjusted(-4, -2, 4, 2), 6, 6);

      // 拼接文件名和媒体信息
      QString infoText = currentFileName;
      if (!videoInfoLabel.isEmpty()) {
        infoText += "  |  " + videoInfoLabel;
      }

      QFontMetrics fm(infoFont);
      int textWidth = fm.horizontalAdvance(infoText);
      int rectWidth = infoRect.width() - 10;
      int x = infoRect.left() + 5;
      int y = infoRect.top();
      int availableWidth = rectWidth;

      if (textWidth > availableWidth) {
        int totalScroll = textWidth + 40;
        int offset = scrollOffset % totalScroll;
        int drawX = x - offset;
        p.setClipRect(infoRect.adjusted(2, 2, -2, -2));
        p.drawText(drawX, y + infoRect.height() - 8, infoText);
        // 循环补尾
        if (textWidth - offset < availableWidth) {
          p.drawText(drawX + totalScroll, y + infoRect.height() - 8, infoText);
        }
        p.setClipping(false);

        // 滚动到末尾时停顿 3 秒
        if (!scrollPause && offset + 2 >= totalScroll - 2) {
          scrollPause = true;
          scrollPauseTimer->start(3000);
        }
      } else {
        p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, infoText);
      }
    }

    // 进度条
    double pct = duration > 0 ? double(currentPts) / duration : 0;
    int barMargin = 20;
    int barWidth = width() - barMargin * 2;
    int barHeight = 10;
    int barY = height() - 30;
    QRect bar(barMargin, barY, barWidth, barHeight);

    // 圆角矩形进度条
    int radius = barHeight / 2;
    p.setRenderHint(QPainter::Antialiasing, true);

    // 阴影
    {
      QPainterPath shadowPath;
      shadowPath.addRoundedRect(bar.adjusted(-2, 2, 2, 6), radius + 2,
                                radius + 2);
      QColor shadowColor(0, 0, 0, 80);
      p.save();
      p.setPen(Qt::NoPen);
      p.setBrush(shadowColor);
      p.drawPath(shadowPath);
      p.restore();
    }

    // 背景
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawRoundedRect(bar, radius, radius);

    // 已播放部分
    int playedWidth = int(bar.width() * pct);
    if (playedWidth > 0) {
      QRect playedRect =
          QRect(bar.left(), bar.top(), playedWidth, bar.height());
      p.setBrush(Qt::white);
      p.drawRoundedRect(playedRect, radius, radius);
      // 修正：如果已播放部分比半径宽度小，避免圆角溢出
      if (playedWidth < bar.height()) {
        // 只画左侧半圆
        QPainterPath path;
        path.moveTo(bar.left(), bar.top() + bar.height() / 2.0);
        path.arcTo(bar.left(), bar.top(), bar.height(), bar.height(), 90, 180);
        path.closeSubpath();
        p.fillPath(path, Qt::white);
      }
    }

    // 边框
    p.setPen(QPen(Qt::white, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(bar, radius, radius);
  }

  // 歌词
  QRect lyricRect = rect().adjusted(0, height() - 70, 0, -10);
  // SRT 字幕绘制（优先于歌词，样式同歌词）
  {
    // 渐变动画（出现和消失）
    static int lastSubIdx = -2;
    static QElapsedTimer subFadeTimer;
    static bool fadingOut = false;
    static qreal fadeOpacity = 1.0;
    static QString lastSubText;
    qreal opacity = 1.0;
    QString subText;

    if (currentSubtitleIndex >= 0 && currentSubtitleIndex < subtitles.size()) {
      subText = subtitles[currentSubtitleIndex].text;
      if (lastSubIdx != currentSubtitleIndex) {
        // 字幕切换，启动淡入
        subFadeTimer.restart();
        fadingOut = false;
        lastSubIdx = currentSubtitleIndex;
        fadeOpacity = 1.0;
        lastSubText = subText;
      }
    } else {
      // 字幕消失，启动淡出
      if (!fadingOut && lastSubIdx != -1) {
        subFadeTimer.restart();
        fadingOut = true;
        fadeOpacity = 1.0;
      }
      subText = lastSubText;
    }

    if (!subText.isEmpty()) {
      if (!fadingOut) {
        // 淡入
        if (subFadeTimer.isValid()) {
          qint64 elapsed = subFadeTimer.elapsed();
          if (elapsed < 400) {
            opacity = qMin(1.0, elapsed / 400.0);
            QTimer::singleShot(16, this, [this] { update(); });
          } else {
            opacity = 1.0;
          }
        }
        fadeOpacity = opacity;
      } else {
        // 淡出
        if (subFadeTimer.isValid()) {
          qint64 elapsed = subFadeTimer.elapsed();
          if (elapsed < 400) {
            opacity = fadeOpacity * (1.0 - elapsed / 400.0);
            QTimer::singleShot(16, this, [this] { update(); });
          } else {
            opacity = 0.0;
            fadingOut = false;
            lastSubIdx = -1;
            lastSubText.clear();
          }
        }
      }

      if (opacity > 0.01) {
        QFont subFont("Microsoft YaHei", overlayFontSize - 2,
                      QFont::Bold); // 使用统一字号
        p.setFont(subFont);

        QRect textRect = p.fontMetrics().boundingRect(
            lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
        textRect.moveCenter(lyricRect.center());

        // 半透明黑色背景
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor bgColor(0, 0, 0, int(180 * opacity));
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor);
        p.drawRoundedRect(textRect, 12, 12);
        p.restore();

        // 白色文字
        p.save();
        QColor textColor(255, 255, 255, int(255 * opacity));
        p.setPen(textColor);
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        p.restore();
      }
      // SRT 绘制后直接 return，避免歌词和字幕重叠
      if (opacity > 0.01)
        return;
    }
  }

  // 歌词渐变动画
  qreal opacity = lyricOpacity;
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      opacity = qMin(1.0, elapsed / 400.0);
    } else {
      opacity = 1.0;
    }
  }
  // 当前歌词
  if (currentLyricIndex < lyrics.size()) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize - 2,
                    QFont::Bold); // 使用统一字号
    p.setFont(lyricFont);

    // Youtube 风格：黑色半透明背景，白色文字
    QString lyricText = lyrics[currentLyricIndex].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
    textRect.moveCenter(lyricRect.center());

    // 半透明黑色背景
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * opacity));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();

    // 白色文字
    p.save();
    QColor textColor(255, 255, 255, int(255 * opacity));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }
  // 上一行歌词淡出（可选）
  if (lastLyricIndex >= 0 && lastLyricIndex < lyrics.size() &&
      lyricOpacity < 1.0) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize - 2,
                    QFont::Bold); // 使用统一字号
    p.setFont(lyricFont);

    QString lyricText = lyrics[lastLyricIndex].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
    textRect.moveCenter(lyricRect.center());

    // 半透明黑色背景
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * (1.0 - opacity) * 0.7));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();

    // 白色文字
    p.save();
    QColor textColor(255, 255, 255, int(255 * (1.0 - opacity) * 0.7));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }

  // --- 最后绘制 ASS 字幕，保证 UI 不被遮挡 ---
  if (hasAssSubtitle && assTrack && assRenderer) {
    int w = width(), h = height();
    ass_set_frame_size(assRenderer, w, h);
    long long now = currentPts;
    // 如果 currentPts 单位为微秒，需改为：long long now = currentPts / 1000;
    int detectChange = 0;
    ASS_Image *img =
        ass_render_frame(assRenderer, assTrack, now, &detectChange);
    for (; img; img = img->next) {
      QImage qimg((const uchar *)img->bitmap, img->w, img->h, img->stride,
                  QImage::Format_Alpha8);
      QColor color;
      color.setRgba(qRgba((img->color >> 24) & 0xFF, (img->color >> 16) & 0xFF,
                          (img->color >> 8) & 0xFF, 255 - (img->color & 0xFF)));
      QImage colored(qimg.size(), QImage::Format_ARGB32_Premultiplied);
      colored.fill(Qt::transparent);
      QPainter qp(&colored);
      qp.setCompositionMode(QPainter::CompositionMode_Source);
      qp.fillRect(qimg.rect(), color);
      qp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
      qp.drawImage(0, 0, qimg);
      qp.end();
      p.drawImage(img->dst_x, img->dst_y, colored);
    }
  }
}

void VideoPlayer::updateOverlay() {
  // 歌词渐变动画刷新
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      lyricOpacity = qMin(1.0, elapsed / 400.0);
      update();
    } else {
      lyricOpacity = 1.0;
      lastLyricIndex = -1;
      overlayTimer->stop();
      update();
    }
  }
}

void VideoPlayer::showOverlayBarForSeconds(int seconds) {
  showOverlayBar = true;
  overlayBarTimer->start(seconds * 1000);
  update();
}
