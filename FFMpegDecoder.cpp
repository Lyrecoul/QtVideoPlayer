#include "FFMpegDecoder.h"
#include "qdebug.h"
#include <QtDebug>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

static const int OUT_SAMPLE_RATE = 44100;
static const int OUT_CHANNELS = 2;
static const AVSampleFormat OUT_SAMPLE_FMT = AV_SAMPLE_FMT_S16;

FFMpegDecoder::FFMpegDecoder(QObject *parent) : QObject(parent) {
  avformat_network_init();
}

FFMpegDecoder::~FFMpegDecoder() { stop(); }

void FFMpegDecoder::start(const QString &path) {
  stop(); // 如已有线程，先停止
  m_path = path;
  m_stop = false;
  m_pause = false;
  m_seeking = false;
  m_videoSeekHandled = false; // 新增
  m_audioSeekHandled = false; // 新增
  m_eof = false; // 新增

  m_videoThread = std::thread(&FFMpegDecoder::videoDecodeLoop, this);
  m_audioThread = std::thread(&FFMpegDecoder::audioDecodeLoop, this);
}

void FFMpegDecoder::stop() {
  m_stop = true;
  m_eof = false; // 新增
  m_cond.notify_all();
  if (m_videoThread.joinable())
    m_videoThread.join();
  if (m_audioThread.joinable())
    m_audioThread.join();
}

void FFMpegDecoder::seek(qint64 ms) {
  m_seekTarget = ms;
  m_seeking = true;
  m_videoSeekHandled = false; // 新增
  m_audioSeekHandled = false; // 新增
  m_eof = false; // 新增
  m_cond.notify_all(); // 唤醒所有线程
}

void FFMpegDecoder::togglePause() {
  m_pause = !m_pause;
  if (!m_pause)
    m_cond.notify_all();
}

bool FFMpegDecoder::isPaused() const { return m_pause; }

// 拆分后的视频解码循环
void FFMpegDecoder::videoDecodeLoop() {
  AVFormatContext *fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0); // 1MB
  av_dict_set(&opts, "analyzeduration", "1000000", 0); // 1秒
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr, &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }

  int vid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0)
      vid_idx = i;
  }
  if (vid_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return;
  }



  AVCodecParameters *vcodecpar = fmt_ctx->streams[vid_idx]->codecpar;
  const AVCodec *vcodec = avcodec_find_decoder(vcodecpar->codec_id);
  if (!vcodec) {
      qWarning() << "Video decoder not found for codec id:" << vcodecpar->codec_id;
      return;
  }
  AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
  if (!vctx) {
    qWarning() << "Failed to allocate video decoder context";
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_parameters_to_context(vctx, fmt_ctx->streams[vid_idx]->codecpar) <
      0) {
    qWarning() << "Failed to copy video decoder parameters";
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
    qWarning() << "Failed to open video decoder";
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  int vwidth = vctx->width;
  int vheight = vctx->height;
  AVRational vtime_base = fmt_ctx->streams[vid_idx]->time_base;
  int sws_src_pix_fmt = -1;
  SwsContext *sws_ctx = nullptr;

  // 视频输出 buffer
  int rgb_stride = vwidth * 3;
  uint8_t *rgb_buf = nullptr;
  int rgb_buf_size = 0;
  if (vwidth && vheight) {
    rgb_buf_size =
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
    rgb_buf = (uint8_t *)av_malloc(rgb_buf_size);
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  // 视频同步
  using clock = std::chrono::steady_clock;
  clock::time_point playback_start_time;

  // 计算总时长（只在视频线程发一次）
  qint64 duration_ms =
      fmt_ctx->duration >= 0 ? fmt_ctx->duration / (AV_TIME_BASE / 1000) : 0;
  emit durationChanged(duration_ms);

  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      playback_start_time = clock::now();
    }
    // seek
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(vctx);
      playback_start_time = clock::now();
      // 丢弃所有未处理包
      av_packet_unref(pkt);
      av_frame_unref(frame);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_videoSeekHandled = true;
        if (m_audioSeekHandled) {
          m_seeking = false;
        }
      }
      continue; // 立即进入下次循环，避免处理旧包
    }
    if (av_read_frame(fmt_ctx, pkt) < 0) {
      // 播放结束，进入 idle 等待区，允许 seek/pause/stop
      m_eof = true; // 新增
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || m_seeking || m_eof == false; });
      if (m_stop)
        break;
      if (m_seeking) {
        m_eof = false; // 新增
        continue; // 继续循环以处理 seek
      }
      // 若只是 pause，继续等待
      continue;
    }
    if (pkt->stream_index != vid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    // seek 状态下丢弃包
    if (m_seeking) {
      av_packet_unref(pkt);
      continue;
    }
    avcodec_send_packet(vctx, pkt);
    while (avcodec_receive_frame(vctx, frame) == 0) {
      // seek 状态下丢弃帧
      if (m_seeking)
        break;

      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;

      // ----------- 音画同步逻辑 begin -----------
      qint64 audioClock = m_audioClockMs.load();
      qint64 diff = ms - audioClock;
      if (audioClock > 0) {
        if (diff > 80) { // 放宽跳帧阈值
          // 视频帧比音频快，分步短等待，提升高帧率流畅度
          int waited = 0;
          const int max_wait = 40; // 增加最大等待时长
          while (diff > 10 && waited < max_wait && !m_stop && !m_pause &&
                 !m_seeking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
            audioClock = m_audioClockMs.load();
            diff = ms - audioClock;
          }
          if (diff > 80) {
            qWarning() << "Drop video frame: video ahead of audio (diff =" << diff << "ms, video ms =" << ms << ", audio ms =" << audioClock << ")";
            continue;
          }
        } else if (diff < -300) { // 放宽丢帧阈值
          qWarning() << "Drop video frame: video lags audio too much (diff =" << diff << "ms, video ms =" << ms << ", audio ms =" << audioClock << ")";
          continue;
        }
        // 否则正常显示
      }
      // ----------- 音画同步逻辑 end -------------

      // 检查并初始化/重建 sws_ctx
      if (!sws_ctx || sws_src_pix_fmt != frame->format) {
        if (sws_ctx)
          sws_freeContext(sws_ctx);
        sws_ctx = sws_getCachedContext(
            nullptr, vwidth, vheight, (AVPixelFormat)frame->format, vwidth,
            vheight, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr,
            nullptr);
        sws_src_pix_fmt = frame->format;
        if (!sws_ctx) {
          qWarning() << "Failed to initialize sws context, format:"
                     << frame->format;
          continue;
        }
      }

      // 检查分辨率变化，必要时重新分配 rgb_buf
      if (frame->width != vwidth || frame->height != vheight) {
        vwidth = frame->width;
        vheight = frame->height;
        rgb_stride = vwidth * 3;
        int new_buf_size =
            av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
        if (rgb_buf)
          av_free(rgb_buf);
        rgb_buf = (uint8_t *)av_malloc(new_buf_size);
        rgb_buf_size = new_buf_size;
      }

      // 转为 RGB24
      uint8_t *dst[1] = {rgb_buf};
      int dst_linesize[1] = {rgb_stride};
      sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst,
                dst_linesize);

      QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);

      if (img.isNull()) {
        qWarning()
            << "QImage isNull after construction, fallback to buffer copy";
        QByteArray tmp((const char *)rgb_buf, vheight * rgb_stride);
        QImage img2((const uchar *)tmp.constData(), vwidth, vheight,
                    QImage::Format_RGB888);
        emit frameReady(img2.copy());
      } else {
        emit frameReady(img.copy());
      }

      emit positionChanged(ms);
    }
    av_packet_unref(pkt);
  }

  if (rgb_buf)
    av_free(rgb_buf);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (sws_ctx)
    sws_freeContext(sws_ctx);
  if (vctx)
    avcodec_free_context(&vctx);
  avformat_close_input(&fmt_ctx);
}

// 拆分后的音频解码循环
void FFMpegDecoder::audioDecodeLoop() {
  AVFormatContext *fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0); // 1MB
  av_dict_set(&opts, "analyzeduration", "1000000", 0); // 1秒
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr, &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }

  int aid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
      aid_idx = i;
  }
  if (aid_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return;
  }

  AVCodecParameters *acodecpar = fmt_ctx->streams[aid_idx]->codecpar;
  const AVCodec *acodec = avcodec_find_decoder(acodecpar->codec_id);
  if (!acodec) {
      qWarning() << "Audio decoder not found for codec id:" << acodecpar->codec_id;
      return;
  }
  AVCodecContext *actx = avcodec_alloc_context3(acodec);
  if (!actx) {
    qWarning() << "Failed to allocate audio decoder context";
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_parameters_to_context(actx, fmt_ctx->streams[aid_idx]->codecpar) <
      0) {
    qWarning() << "Failed to copy audio decoder parameters";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_open2(actx, acodec, nullptr) < 0) {
    qWarning() << "Failed to open audio decoder";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  AVRational atime_base = fmt_ctx->streams[aid_idx]->time_base;
  SwrContext *swr_ctx = nullptr;

  // 初始化 swr
  swr_alloc();
  av_opt_set_chlayout(swr_ctx, "in_chlayout", &actx->ch_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", actx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", actx->sample_fmt,5);

  AVChannelLayout out_ch_layout;
  av_channel_layout_default(&out_ch_layout, OUT_CHANNELS);

  av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout,0);
  av_opt_set_int(swr_ctx,"out_sample_rate",OUT_SAMPLE_RATE,0);
  av_opt_set_sample_fmt(swr_ctx,"out_sample_fmt",OUT_SAMPLE_FMT,0);

  swr_init(swr_ctx);

  if (!swr_ctx || swr_init(swr_ctx) < 0) {
      qWarning() << "Failed to initialize audio resample context";
      swr_free(&swr_ctx);
      avformat_close_input(&fmt_ctx);
      return;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  // 预分配音频输出缓冲区
  uint8_t **out_buf = nullptr;
  int out_buf_samples = 0;

  using clock = std::chrono::steady_clock;
  clock::time_point audio_playback_start_time;
  int64_t first_audio_pts = AV_NOPTS_VALUE;
  bool first_audio_frame = true;

  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      // 重置同步基准
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
    }
    // seek
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(actx);
      // 重置同步基准
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
      // 丢弃所有未处理包
      av_packet_unref(pkt);
      av_frame_unref(frame);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audioSeekHandled = true;
        if (m_videoSeekHandled) {
          m_seeking = false;
        }
      }
      continue; // 立即进入下次循环，避免处理旧包
    }
    if (av_read_frame(fmt_ctx, pkt) < 0) {
      // 播放结束，进入 idle 等待区，允许 seek/pause/stop
      m_eof = true; // 新增
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || m_seeking || m_eof == false; });
      if (m_stop)
        break;
      if (m_seeking) {
        m_eof = false; // 新增
        continue; // 继续循环以处理 seek
      }
      // 若只是 pause，继续等待
      continue;
    }
    if (pkt->stream_index != aid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    // seek 状态下丢弃包
    if (m_seeking) {
      av_packet_unref(pkt);
      continue;
    }
    avcodec_send_packet(actx, pkt);
    while (avcodec_receive_frame(actx, frame) == 0) {
      // seek 状态下丢弃帧
      if (m_seeking)
        break;

      if (frame->nb_samples == 0) {
        continue;
      }
      int64_t pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      int64_t ms = pts * atime_base.num * 1000LL / atime_base.den;
      if (ms < 0) {
        continue;
      }
      // 更新音频时钟
      m_audioClockMs.store(ms);

      // ----------- 修正同步基准 begin -----------
      if (first_audio_frame) {
          audio_playback_start_time = clock::now();
          first_audio_pts = ms;
          first_audio_frame = false;
      } else {
          // 计算理论应播放的时间长度（基于PTS）
          int64_t expected_elapsed = ms - first_audio_pts;
          
          // 计算实际经过的时间
          int64_t actual_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              clock::now() - audio_playback_start_time).count();

          // 计算偏差（毫秒）
          int64_t diff = expected_elapsed - actual_elapsed;

          // 动态调整阈值（更严格的同步控制）
          double sync_threshold = std::max(10.0, std::min(expected_elapsed * 0.01, 30.0));

          if (std::abs(diff) > sync_threshold) {
              if (diff > 0) {
                  // 音频比预期快，需要减速
                  std::this_thread::sleep_for(std::chrono::milliseconds(diff));
              } else {
                  // 音频比预期慢，跳过当前帧（仅当严重滞后时）
                  if (diff < -100) continue;
              }
          }
      }
      // ----------- 修正同步基准 end -------------

      // 重采样
      int out_nb = av_rescale_rnd(
          swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples,
          OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);

      // 仅当 out_nb 变化时重新分配 out_buf
      if (out_nb > out_buf_samples) {
        if (out_buf) {
          av_freep(&out_buf[0]);
          av_freep(&out_buf);
        }
        av_samples_alloc_array_and_samples(&out_buf, nullptr, OUT_CHANNELS,
                                           out_nb, OUT_SAMPLE_FMT, 0);
        out_buf_samples = out_nb;
      }

      int converted =
          swr_convert(swr_ctx, out_buf, out_nb, (const uint8_t **)frame->data,
                      frame->nb_samples);

      int data_size = av_samples_get_buffer_size(nullptr, OUT_CHANNELS,
                                                 converted, OUT_SAMPLE_FMT, 1);
      QByteArray pcm((const char *)out_buf[0], data_size);
      emit audioReady(pcm);

      emit positionChanged(ms);
    }
    av_packet_unref(pkt);
  }

  // 循环结束后释放 out_buf
  if (out_buf) {
    av_freep(&out_buf[0]);
    av_freep(&out_buf);
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (swr_ctx)
    swr_free(&swr_ctx);
  if (actx)
    avcodec_free_context(&actx);
  avformat_close_input(&fmt_ctx);
}