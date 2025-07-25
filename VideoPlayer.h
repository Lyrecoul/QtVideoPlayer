#pragma once
#include <QAction>
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QSharedPointer>
#include <QTimer>
#include <QWidget>
#include <ass/ass.h>

#include "FFMpegDecoder.h"
#include "LyricRenderer.h"
#include "SubtitleRenderer.h"

class VideoPlayer : public QWidget {
  Q_OBJECT
public:
  explicit VideoPlayer(QWidget *parent = nullptr);
  ~VideoPlayer();
  void play(const QString &path);

protected:
  // 手势/点击处理（双击关闭窗口）
  void mousePressEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;

  void resizeEvent(QResizeEvent *e) override;
  void paintEvent(QPaintEvent *e) override;

private slots:
  void onFrame(const QSharedPointer<QImage> &frame);
  void onAudioData(const QByteArray &data);
  void onPositionChanged(qint64 pts);
  void updateOverlay();

private:
  QAudioOutput *audioOutput;
  QIODevice *audioIO;
  FFMpegDecoder *decoder;
  QTimer *overlayTimer;
  QTimer *frameRateTimer; // 帧率控制定时器

  // 音频输出参数（补充声明）
  int audioSampleRate = 44100;
  int audioChannels = 2;
  int audioSampleSize = 16;

  // 状态管理
  bool pressed = false;
  QPoint pressPos;
  // QElapsedTimer pressTimer;
  bool isSeeking = false;
  qint64 duration = 0;
  qint64 currentPts = 0;

  // 长按 2 倍速播放相关
  QTimer *speedPressTimer = nullptr;
  bool isSpeedPressed = false;
  float normalPlaybackSpeed = 1.0f;

  LyricRenderer *lyricRenderer = nullptr;
  SubtitleRenderer *subtitleRenderer = nullptr;

  // libass 相关
  ASS_Library *assLibrary = nullptr;
  ASS_Renderer *assRenderer = nullptr;

  QSharedPointer<QImage> currentFrame;
  // 进度条显示控制
  bool showOverlayBar = false;
  QTimer *overlayBarTimer = nullptr;

  // 统一 overlay 字号
  int overlayFontSize = 10;

  void seekByDelta(int dx);
  void showOverlay(bool visible);
  void drawProgressBar(QPainter &p);
  void drawSubtitlesAndLyrics(QPainter &p);
  void updateOverlayVisibility();
  void scheduleUpdate(); // 控制帧率的更新调度

  QFileSystemWatcher *screenStatusWatcher;

  // 新增：错误提示
  QString errorMessage;
  QTimer *errorShowTimer = nullptr;

  LyricManager *lyricManager;
  SubtitleManager *subtitleManager;

  // 音轨/视频轨道切换按钮和菜单
  QPushButton *trackButton = nullptr;
  QElapsedTimer *trackButtonTimer;
  QMenu *audioMenu = nullptr;
  QMenu *videoMenu = nullptr;

  // 帧率控制
  qint64 lastScrollUpdateTime; // 上次滚动更新时间
  bool updatePending = false;  // 是否有未处理的更新请求
  qint64 lastUpdateTime = 0;   // 上次更新时间

  // 顶部土司消息
  QString toastMessage;
  QTimer *toastTimer;
  void drawToastMessage(QPainter &p);
  void showToastMessage(const QString &message);

  int toastElapsedMs = 0;
  qreal toastOpacity = 0.0;
  int toastSlideOffset = -30;

  const int toastTotalDuration = 2100;

  // 手动管理的土司消息
  QString manualToastMessage;
  qreal manualToastOpacity = 0.8;
  bool manualToastVisible = false;

  void showManualToast(const QString &msg);
  void clearManualToast();

  // 字幕开关
  QPushButton *subtitleButton = nullptr;
  bool subtitlesEnabled = true;

  // 截屏
  void doScreenShot();
};
