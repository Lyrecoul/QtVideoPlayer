#include "VideoPlayer.h"
#include "LyricManager.h"
#include "LyricRenderer.h"
#include "SubtitleManager.h"
#include "SubtitleRenderer.h"
#include "qglobal.h"
#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QSharedPointer>
#include <QTextStream>
#include <QTimer>
#include <ass/ass.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/xiphcomment.h>

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent), lastScrollUpdateTime(0), updatePending(false),
      lastUpdateTime(0) {
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

  // 错误提示
  errorShowTimer = new QTimer(this);
  errorShowTimer->setSingleShot(true);
  connect(errorShowTimer, &QTimer::timeout, this, [this]() {
    errorMessage.clear();
    scheduleUpdate();
  });
  connect(decoder, &FFMpegDecoder::errorOccurred, this,
          [this](const QString &msg) {
            errorMessage = msg;
            errorShowTimer->start(3000); // 显示 3 秒
            scheduleUpdate();
          });

  // 顶部土司消息
  toastTimer = new QTimer(this);
  toastTimer->setInterval(16); // 每帧刷新 (~60 FPS)
  connect(toastTimer, &QTimer::timeout, this, [this]() {
    toastElapsedMs += 16;
    if (toastElapsedMs >= toastTotalDuration) {
      toastTimer->stop();
      toastMessage.clear();
    }
    scheduleUpdate();
  });

  // 初始化长按 2 倍速播放定时器
  speedPressTimer = new QTimer(this);
  speedPressTimer->setSingleShot(true);
  connect(speedPressTimer, &QTimer::timeout, this, [this]() {
    if (pressed && !isSeeking) {
      normalPlaybackSpeed = 1.0f; // 保存原始速度
      isSpeedPressed = true;
      decoder->setPlaybackSpeed(2.0f); // 设置 2 倍速

      // 显示 2 倍速提示，使用土司消息
      showManualToast("x2 ▶▶");
    }
  });

  // Overlay 更新
  overlayTimer = new QTimer(this);
  overlayTimer->setInterval(200);
  connect(overlayTimer, &QTimer::timeout, this, &VideoPlayer::updateOverlay);

  // 进度条显示定时器
  overlayBarTimer = new QTimer(this);
  overlayBarTimer->setSingleShot(true);
  connect(overlayBarTimer, &QTimer::timeout, this, [this]() {
    showOverlayBar = false;
    updateOverlayVisibility();
    scheduleUpdate();
  });
  showOverlayBar = false;

  // 帧率控制定时器 - 60fps (约 16.67 ms)
  frameRateTimer = new QTimer(this);
  frameRateTimer->setInterval(16); // ~60fps
  connect(frameRateTimer, &QTimer::timeout, this, [this]() {
    if (updatePending) {
      updatePending = false;
      QWidget::update();
    }
  });

  // 启动定时器 - 集中启动以提高代码清晰度
  frameRateTimer->start();

  // 重置状态变量
  updatePending = false;

  // libass 初始化
  assLibrary = ass_library_init();
  if (assLibrary) {
    assRenderer = ass_renderer_init(assLibrary);
    if (assRenderer) {
      ass_set_fonts(assRenderer, nullptr, "Microsoft YaHei", 1, nullptr, 1);
    }
  }

  // 屏幕状态文件监听
  screenStatusWatcher = new QFileSystemWatcher(this);
  QString screenStatusPath = "/tmp/screen_status";
  QString screenStatusDir = QFileInfo(screenStatusPath).absolutePath();
  screenStatusWatcher->addPath(screenStatusDir);
  connect(screenStatusWatcher, &QFileSystemWatcher::directoryChanged, this,
          [screenStatusPath]() {
            if (QFile::exists(screenStatusPath)) {
              QTimer::singleShot(3000, []() {
                QProcess::execute("ubus",
                                  QStringList()
                                      << "call" << "eq_drc_process.output.rpc"
                                      << "control" << R"({"action":"Open"})");
              });
            }
          });

  lyricManager = new LyricManager();
  subtitleManager = new SubtitleManager();
  lyricRenderer = new LyricRenderer(lyricManager);
  subtitleRenderer = new SubtitleRenderer(subtitleManager);

  // 轨道切换按钮和菜单
  trackButton = new QPushButton(this);
  trackButton->setGeometry(10, 10, 32, 32);         // 圆形按钮：宽高相等
  trackButton->setIcon(QIcon(":/icons/track.png")); // 使用 qrc 资源路径
  trackButton->setIconSize(QSize(24, 24));          // 图标大小适当缩小
  trackButton->setStyleSheet("QPushButton {"
                             "  background-color: rgba(30,30,30,180);"
                             "  border: none;"
                             "  border-radius: 16px;" // 半径为宽/2 实现圆形
                             "}"
                             "QPushButton:hover {"
                             "  background-color: rgba(50,50,50,200);"
                             "}");
  trackButton->setToolTip("轨道切换");
  trackButton->raise();
  connect(trackButton, &QPushButton::clicked, this, [this]() {
    QMenu menu;
    QActionGroup *audioGroup = new QActionGroup(&menu);
    audioGroup->setExclusive(true);
    int acnt = decoder->audioTrackCount();
    for (int i = 0; i < acnt; ++i) {
      QAction *act = menu.addAction(decoder->audioTrackName(i));
      act->setCheckable(true);
      act->setChecked(decoder->currentAudioTrack() == i);
      audioGroup->addAction(act);
      connect(act, &QAction::triggered, this, [this, i]() {
        decoder->setAudioTrack(i);
        showToastMessage(tr("音轨: %1").arg(decoder->audioTrackName(i)));
        scheduleUpdate();
      });
    }
    // QAction *muteAct = menu.addAction(tr("静音轨道"));
    // muteAct->setCheckable(true);
    // muteAct->setChecked(decoder->currentAudioTrack() == -1);
    // audioGroup->addAction(muteAct);
    // connect(muteAct, &QAction::triggered, this, [this]() {
    //   decoder->setAudioTrack(-1);
    //   errorMessage = tr("切换音轨: 静音轨道");
    //   errorShowTimer->start(2000);
    //   scheduleUpdate();
    // });
    menu.addSeparator();
    QActionGroup *videoGroup = new QActionGroup(&menu);
    videoGroup->setExclusive(true);
    int vcnt = decoder->videoTrackCount();
    for (int i = 0; i < vcnt; ++i) {
      QAction *act = menu.addAction(decoder->videoTrackName(i));
      act->setCheckable(true);
      act->setChecked(decoder->currentVideoTrack() == i);
      videoGroup->addAction(act);
      connect(act, &QAction::triggered, this, [this, i]() {
        decoder->setVideoTrack(i);
        showToastMessage(
            tr("切换视频轨道: %1").arg(decoder->videoTrackName(i)));
        scheduleUpdate();
      });
    }
    QAction *noVideoAct = menu.addAction(tr("无视频轨道"));
    noVideoAct->setCheckable(true);
    noVideoAct->setChecked(decoder->currentVideoTrack() == -1);
    videoGroup->addAction(noVideoAct);
    connect(noVideoAct, &QAction::triggered, this, [this]() {
      decoder->setVideoTrack(-1);
      showToastMessage("视频轨道: 无");
      scheduleUpdate();
    });
    menu.exec(trackButton->mapToGlobal(QPoint(0, trackButton->height())));
  });

  // 字幕开关按钮
  subtitleButton = new QPushButton(this);
  subtitleButton->setGeometry(52, 10, 32, 32);             // 位置右移，避免重叠
  subtitleButton->setIcon(QIcon(":/icons/subtitles.png")); // 初始启用状态图标
  subtitleButton->setIconSize(QSize(24, 24));
  subtitleButton->setStyleSheet("QPushButton {"
                                "  background-color: rgba(30,30,30,180);"
                                "  border: none;"
                                "  border-radius: 16px;"
                                "}"
                                "QPushButton:hover {"
                                "  background-color: rgba(50,50,50,200);"
                                "}");
  subtitleButton->setToolTip("字幕开关");
  subtitleButton->raise();
  connect(subtitleButton, &QPushButton::clicked, this, [this]() {
    subtitlesEnabled = !subtitlesEnabled;
    subtitleButton->setIcon(QIcon(subtitlesEnabled
                                      ? ":/icons/subtitles.png"
                                      : ":/icons/subtitles_off.png"));
    showToastMessage(subtitlesEnabled ? "字幕已开启" : "字幕已关闭");
    scheduleUpdate();
  });
}

VideoPlayer::~VideoPlayer() {
  // 停止所有定时器
  if (frameRateTimer)
    frameRateTimer->stop();
  if (speedPressTimer)
    speedPressTimer->stop();
  if (toastTimer)
    toastTimer->stop();
  if (errorShowTimer)
    errorShowTimer->stop();
  if (overlayTimer)
    overlayTimer->stop();
  if (overlayBarTimer)
    overlayBarTimer->stop();

  // 停止解码器和音频输出
  if (decoder)
    decoder->stop();
  if (audioOutput)
    audioOutput->stop();

  // libass 资源释放 - 先释放渲染器，再释放库
  if (assRenderer) {
    ass_renderer_done(assRenderer);
    assRenderer = nullptr;
  }
  if (assLibrary) {
    ass_library_done(assLibrary);
    assLibrary = nullptr;
  }

  // 释放对象资源 - 使用 nullptr 检查增加安全性
  delete lyricRenderer;
  lyricRenderer = nullptr;
  delete subtitleRenderer;
  subtitleRenderer = nullptr;
  delete lyricManager;
  lyricManager = nullptr;
  delete subtitleManager;
  subtitleManager = nullptr;
}

void VideoPlayer::play(const QString &path) {
  // 启动音频输出
  QProcess::execute("ubus", QStringList()
                                << "call" << "eq_drc_process.output.rpc"
                                << "control" << R"({"action":"Open"})");

  lyricManager->loadLyrics(path);
  subtitleManager->reset();

  subtitleManager->loadSubtitle(path, assLibrary, assRenderer);

  decoder->start(path);
  show();
  showOverlayBar = true;
  overlayBarTimer->start(5 * 1000);
  updateOverlayVisibility();
  scheduleUpdate();
}

void VideoPlayer::onFrame(const QSharedPointer<QImage> &frame) {
  currentFrame = frame;
  scheduleUpdate();
}

void VideoPlayer::onAudioData(const QByteArray &data) { audioIO->write(data); }

void VideoPlayer::onPositionChanged(qint64 pts) {
  // 拖动 seeking 时不更新进度条进度
  if (isSeeking) {
    return;
  }

  // 更新当前播放时间
  currentPts = pts;

  // 更新歌词和字幕索引
  subtitleManager->updateSubtitleIndex(pts);

  // 更新界面
  scheduleUpdate();
}

void VideoPlayer::mousePressEvent(QMouseEvent *e) {
  pressed = true;
  pressPos = e->pos();

  // 启动长按检测定时器，500ms 后触发 2 倍速播放
  if (speedPressTimer) {
    speedPressTimer->start(500); // 500ms 长按触发
  }
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *) {
  pressed = false;

  // 取消长按定时器
  if (speedPressTimer) {
    speedPressTimer->stop();
  }

  // 如果是在 2 倍速状态，恢复正常速度
  if (isSpeedPressed) {
    decoder->setPlaybackSpeed(1.0f);
    isSpeedPressed = false;
    clearManualToast();
    return;
  }

  if (isSeeking) {
    isSeeking = false;
    // seek 前检查 duration 是否有效
    if (duration > 0 && currentPts >= 0 && currentPts <= duration) {
      decoder->seek(currentPts);
    }
    showOverlayBar = true;
    overlayBarTimer->start(5 * 1000);
    updateOverlayVisibility();
    scheduleUpdate();
  } else {
    decoder->togglePause();
    // 根据暂停状态调整控件可见性
    if (decoder->isPaused()) {
      // 暂停后一直显示叠加层
      overlayBarTimer->stop();
      showOverlayBar = true;
      updateOverlayVisibility();
    } else {
      // 暂停后显示 5 秒叠加层
      showOverlayBar = true;
      overlayBarTimer->start(5 * 1000);
      updateOverlayVisibility();
    }
  }
}

// 改为双击关闭窗口
void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *) { close(); }

void VideoPlayer::mouseMoveEvent(QMouseEvent *e) {
  if (!pressed || e == nullptr)
    return;

  int dx = e->pos().x() - pressPos.x();
  isSeeking = true;
  seekByDelta(dx);

  overlayBarTimer->stop();
  showOverlayBar = true;
  scheduleUpdate();
}

void VideoPlayer::seekByDelta(int dx) {
  // 动态调整每像素对应的毫秒数，随视频时长自适应
  // 例如：每像素调整为总时长的 1/500，限制最小 20ms，最大 2000ms
  qint64 msPerPx = 20;
  if (duration > 0) {
    msPerPx = qBound<qint64>(20, duration / 10000, 2000);
  }
  qint64 delta = dx * msPerPx;
  qint64 target = qBound<qint64>(0, currentPts + delta, duration);
  currentPts = target;
}

void VideoPlayer::resizeEvent(QResizeEvent *) {}

void VideoPlayer::paintEvent(QPaintEvent *) {
  // 绘制视频帧
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  if (currentFrame && !currentFrame->isNull()) {
    QSize imgSize = currentFrame->size();
    QSize widgetSize = size();
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), imgSize);
    targetRect.moveCenter(rect().center());
    p.drawImage(targetRect, *currentFrame);
  }

  // 绘制字幕和歌词
  drawSubtitlesAndLyrics(p);
  if (subtitleManager->hasAss() && subtitleManager->getAssTrack() &&
      assRenderer) {
    subtitleRenderer->setAssRenderer(assRenderer);
    subtitleRenderer->drawAssSubtitles(p, width(), height(), currentPts);
  }

  // 绘制顶部土司消息
  drawToastMessage(p);

  // 绘制错误消息
  if (!errorMessage.isEmpty()) {
    // 使用静态字体对象，避免频繁创建
    static QFont errFont("Microsoft YaHei", 0, QFont::Bold);
    errFont.setPointSize(overlayFontSize + 4);

    QString msg = errorMessage;

    // 保存当前绘图状态
    p.save();

    p.setFont(errFont);
    QFontMetrics fm(errFont);
    int textWidth = fm.horizontalAdvance(msg);
    int textHeight = fm.height();
    QRect boxRect((width() - textWidth) / 2 - 30,
                  (height() - textHeight) / 2 - 16, textWidth + 60,
                  textHeight + 32);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 180));
    p.drawRoundedRect(boxRect, 18, 18);
    p.setPen(QColor(220, 40, 40));
    p.drawText(boxRect, Qt::AlignCenter, msg);

    // 恢复绘图状态
    p.restore();
  }

  if (showOverlayBar) {
    drawProgressBar(p);
  }
}

void VideoPlayer::drawProgressBar(QPainter &p) {
  double pct = (duration > 0) ? double(currentPts) / duration : 0.0;

  // 样式参数
  const int barHeight = 4;
  const int radius = 2;  // 圆角半径
  const int marginX = 0;
  const int barWidth = width() - marginX * 2;
  const int barY = height() - barHeight;

  QRectF fullBar(marginX, barY, barWidth, barHeight);
  QRectF playedBar = fullBar;
  playedBar.setWidth(fullBar.width() * pct);

  p.setRenderHint(QPainter::Antialiasing, true);

  // 背景轨道（浅灰色）
  p.setBrush(QColor(80, 80, 80, 180));  // 半透明深灰
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(fullBar, radius, radius);

  // 播放进度（红色）
  if (playedBar.width() > 0.5) {
    p.setBrush(QColor(255, 60, 60));  // 柔和红色
    p.drawRoundedRect(playedBar, radius, radius);
  }
}

void VideoPlayer::drawSubtitlesAndLyrics(QPainter &p) {
  QRect lyricRect = rect().adjusted(0, height() - 70, 0, -10);
  if (subtitlesEnabled) {
    subtitleRenderer->drawSrtSubtitles(p, lyricRect, overlayFontSize,
                                       currentPts);
    lyricRenderer->drawLyricsByTime(p, lyricRect, overlayFontSize, currentPts);
  }
}

void VideoPlayer::updateOverlay() { scheduleUpdate(); }

void VideoPlayer::scheduleUpdate() {
  // 只有在未标记待更新时才设置更新标记
  if (!updatePending) {
    // 获取当前时间戳
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

    // 如果距离上次更新时间超过 16ms (约 60 fps)，立即更新界面
    if (currentTime - lastUpdateTime > 16) {
      lastUpdateTime = currentTime;
      QWidget::update();
    } else {
      // 设置更新标记，由 frameRateTimer 定时处理
      updatePending = true;
    }
  }
}

void VideoPlayer::showToastMessage(const QString &msg) {
  toastMessage = msg;
  toastElapsedMs = 0;
  toastOpacity = 0.0;
  toastSlideOffset = -30;
  toastTimer->start();
  update();
}

void VideoPlayer::drawToastMessage(QPainter &p) {
  p.setRenderHint(QPainter::Antialiasing);

  // ------- 自动 Toast -------
  if (!toastMessage.isEmpty()) {
    const int fadeDuration = 300;
    const int displayDuration = 1500;

    if (toastElapsedMs < fadeDuration) {
      toastOpacity = toastElapsedMs / double(fadeDuration);
      toastSlideOffset = -30 + int(30 * toastOpacity);
    } else if (toastElapsedMs < fadeDuration + displayDuration) {
      toastOpacity = 1.0;
      toastSlideOffset = 0;
    } else {
      qreal progress = (toastElapsedMs - fadeDuration - displayDuration) /
                       double(fadeDuration);
      toastOpacity = 1.0 - progress;
      toastSlideOffset = int(30 * progress);
    }

    static QFont toastFont("Microsoft YaHei", 0, QFont::Bold);
    toastFont.setPointSize(overlayFontSize + 2);
    p.setFont(toastFont);

    QFontMetrics fm(toastFont);
    int textWidth = fm.horizontalAdvance(toastMessage);
    int textHeight = fm.height();
    QRect toastRect((width() - textWidth) / 2 - 20, 20 + toastSlideOffset,
                    textWidth + 40, textHeight + 16);

    p.save();
    p.setOpacity(toastOpacity);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 200));
    p.drawRoundedRect(toastRect, 12, 12);
    p.setPen(Qt::white);
    p.drawText(toastRect, Qt::AlignCenter, toastMessage);
    p.restore();
  }

  // ------- 手动 Toast -------
  if (manualToastVisible && !manualToastMessage.isEmpty()) {
    static QFont manualFont("Microsoft YaHei", 0, QFont::Bold);
    manualFont.setPointSize(overlayFontSize);
    p.setFont(manualFont);

    QFontMetrics fm(manualFont);
    int textWidth = fm.horizontalAdvance(manualToastMessage);
    int textHeight = fm.height();
    QRect toastRect((width() - textWidth) / 2 - 20,
                    height() / 2 - textHeight / 2, // 居中显示
                    textWidth + 20, textHeight + 8);

    p.save();
    p.setOpacity(manualToastOpacity); // 支持透明度动画
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 20, 220));
    p.drawRoundedRect(toastRect, 12, 12);
    p.setPen(Qt::white);
    p.drawText(toastRect, Qt::AlignCenter, manualToastMessage);
    p.restore();
  }
}

void VideoPlayer::showManualToast(const QString &msg) {
  manualToastMessage = msg;
  manualToastVisible = true;
  manualToastOpacity = 1.0; // 可选：初始时从0渐变
  update();
}

void VideoPlayer::clearManualToast() {
  manualToastMessage.clear();
  manualToastVisible = false;
  update();
}

void VideoPlayer::updateOverlayVisibility() {
  trackButton->setVisible(showOverlayBar);
  subtitleButton->setVisible(showOverlayBar);
  trackButton->raise();
  subtitleButton->raise();
}