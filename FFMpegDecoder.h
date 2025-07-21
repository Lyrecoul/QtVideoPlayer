#pragma once
#include "qdebug.h"
#include <QImage>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QtDebug>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 智能指针管理 AVFrame
template <typename T, void (*FreeFunc)(T **)> struct FFmpegDeleter {
  void operator()(T *ptr) const {
    if (ptr) {
      FreeFunc(&ptr);
    }
  }
};

using AVFramePtr =
    std::unique_ptr<AVFrame, FFmpegDeleter<AVFrame, av_frame_free>>;
using AVPacketPtr =
    std::unique_ptr<AVPacket, FFmpegDeleter<AVPacket, av_packet_free>>;
using AVCodecContextPtr =
    std::unique_ptr<AVCodecContext,
                    FFmpegDeleter<AVCodecContext, avcodec_free_context>>;
using AVFormatContextPtr =
    std::unique_ptr<AVFormatContext,
                    FFmpegDeleter<AVFormatContext, avformat_close_input>>;

static const int OUT_SAMPLE_RATE = 44100;
static const int OUT_CHANNELS = 2;
static const AVSampleFormat OUT_SAMPLE_FMT = AV_SAMPLE_FMT_S16;

// ===== 音频解码循环相关类 =====
class AudioSynchronizer {
public:
  void reset(double speed) {
    m_first = true;
    m_speed = speed;
  }

  void sync(int64_t ptsMs, double speed) {
    if (fabs(speed - m_speed) > 0.1) {
      reset(speed);
    }

    if (m_first) {
      m_refTime = std::chrono::steady_clock::now();
      m_refPts = ptsMs;
      m_first = false;
      return;
    }

    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - m_refTime)
                          .count();
    double diff = ((ptsMs - m_refPts) / speed) - elapsed;
    if (diff > 10) {
      std::this_thread::sleep_for(std::chrono::milliseconds(int(diff * 0.8)));
    }
  }

private:
  bool m_first = true;
  double m_speed = 1.0;
  int64_t m_refPts = 0;
  std::chrono::steady_clock::time_point m_refTime;
};

class SwrBuffer {
public:
  SwrBuffer() = default;
  ~SwrBuffer() { cleanup(); }

  void cleanup() {
    if (m_ctx)
      swr_free(&m_ctx);
    if (m_buf) {
      av_freep(&m_buf[0]);
      av_freep(&m_buf);
    }
  }

  bool init(AVCodecContext *actx) {
    cleanup();
    m_ctx = swr_alloc_set_opts(
        nullptr, av_get_default_channel_layout(OUT_CHANNELS), OUT_SAMPLE_FMT,
        OUT_SAMPLE_RATE, actx->channel_layout, actx->sample_fmt,
        actx->sample_rate, 0, nullptr);
    if (!m_ctx || swr_init(m_ctx) < 0) {
      swr_free(&m_ctx);
      return false;
    }
    return true;
  }

  uint8_t **getBuffer(int requiredSamples) {
    if (requiredSamples > m_bufSamples) {
      if (m_buf) {
        av_freep(&m_buf[0]);
        av_freep(&m_buf);
      }
      av_samples_alloc_array_and_samples(&m_buf, nullptr, OUT_CHANNELS,
                                         requiredSamples, OUT_SAMPLE_FMT, 0);
      m_bufSamples = requiredSamples;
    }
    return m_buf;
  }

  SwrContext *ctx() const { return m_ctx; }

private:
  SwrContext *m_ctx = nullptr;
  uint8_t **m_buf = nullptr;
  int m_bufSamples = 0;
};

// ===== 解码器主类 =====
class FFMpegDecoder : public QObject {
  Q_OBJECT
public:
  explicit FFMpegDecoder(QObject *parent = nullptr);
  ~FFMpegDecoder();

  // path: 文件路径
  void start(const QString &path); // 移除 rate 参数
  void stop();
  void seek(qint64 ms);
  void togglePause();
  bool isPaused() const; // 新增：判断是否暂停

  // 音轨切换
  void setAudioTrack(int index); // index=-1 为静音
  int audioTrackCount() const;
  int currentAudioTrack() const;
  QString audioTrackName(int idx) const;

  // 视频轨道切换
  void setVideoTrack(int index); // index=-1 为无视频
  int videoTrackCount() const;
  int currentVideoTrack() const;
  QString videoTrackName(int idx) const;

  // 倍速支持
  void setPlaybackSpeed(float speed);

signals:
  void frameReady(const QSharedPointer<QImage> &img);
  void audioReady(const QByteArray &pcm);
  void durationChanged(qint64 ms);
  void positionChanged(qint64 ms);
  void errorOccurred(const QString &message); // 新增：错误信号

private:
  // 线程与同步
  std::thread m_videoThread;
  std::thread m_audioThread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_pause{false};
  std::atomic<bool> m_seeking{false};
  std::atomic<qint64> m_seekTarget{0};
  std::mutex m_mutex;
  std::condition_variable m_cond;

  // seek 同步标志
  bool m_videoSeekHandled = false;
  bool m_audioSeekHandled = false;

  // 播放结束标志
  std::atomic<bool> m_eof{false}; // 新增

  // 音频时钟（ms）
  std::atomic<qint64> m_audioClockMs{0};

  // 播放参数
  QString m_path;

  // 解码主循环
  void videoDecodeLoop();
  void audioDecodeLoop();

  // 音频解码循环相关
  AVFormatContextPtr m_fmtCtx;
  bool openInputFile(AVFormatContextPtr &m_fmtCtx);
  void scanAudioStreams(AVFormatContextPtr &m_fmtCtx);
  bool initDecoder(int streamIndex, AVCodecContextPtr &actx,
                   SwrBuffer &resampler, AVRational &timeBase);
  bool handlePauseOrSeek();
  void emitSilence();
  void handleEOF();
  int getCurrentAudioStream();

  int m_audioTrackIndex = 0;                       // -1为静音
  mutable std::vector<int> m_audioStreamIndices;   // 存储所有音频流索引
  mutable std::vector<QString> m_audioStreamNames; // 存储音轨描述

  int m_videoTrackIndex = 0; // -1为无视频
  mutable std::vector<int> m_videoStreamIndices;
  mutable std::vector<QString> m_videoStreamNames;

  // 倍速支持
  std::atomic<float> m_playbackSpeed{1.0f};
};