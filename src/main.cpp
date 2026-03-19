// Terminal video player for macOS — C++20, FFmpeg, CMake + Conan
// Full pixel playback: Kitty / WezTerm / Warp / iTerm2 image protocol support
// FTXUI: floating centered window (320px width, aspect-ratio height)

#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <atomic>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"  // third_party (PNG for iTerm2)

namespace {

// ——— Display mode: block art vs full pixel image ———

enum class DisplayMode { Block, KittyImage, Iterm2Image };

enum class BlockColorMode { Grayscale, Truecolor };

DisplayMode detect_display_mode() {
  const char* term = std::getenv("TERM");
  const char* term_program = std::getenv("TERM_PROGRAM");
  if (std::getenv("KITTY_WINDOW_ID") != nullptr)
    return DisplayMode::KittyImage;
  if (term && std::strstr(term, "kitty"))
    return DisplayMode::KittyImage;
  if (std::getenv("WEZTERM_EXECUTABLE") != nullptr || (term && std::strstr(term, "wezterm")))
    return DisplayMode::KittyImage;
  if (term_program && (std::strstr(term_program, "Warp") != nullptr))  // Warp: Kitty graphics protocol
    return DisplayMode::KittyImage;
  if (term_program && std::strstr(term_program, "iTerm") != nullptr)
    return DisplayMode::Iterm2Image;
  return DisplayMode::Block;
}

// ——— Terminal control ———

struct TermSize {
  int cols{80};
  int rows{24};
  int xpixel{0};  // pixel width (0 = not supported)
  int ypixel{0};  // pixel height
};

TermSize get_term_size() {
  TermSize s;
#ifdef __APPLE__
  struct winsize w {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
    s.cols = static_cast<int>(w.ws_col);
    s.rows = static_cast<int>(w.ws_row);
    s.xpixel = static_cast<int>(w.ws_xpixel);
    s.ypixel = static_cast<int>(w.ws_ypixel);
  }
#else
  if (const char* cols = std::getenv("COLUMNS"); cols && std::atoi(cols) > 0)
    s.cols = std::atoi(cols);
  if (const char* rows = std::getenv("LINES"); rows && std::atoi(rows) > 0)
    s.rows = std::atoi(rows);
#endif
  return s;
}

void set_raw_terminal(bool raw) {
  static struct termios saved;
  if (raw) {
    if (tcgetattr(STDIN_FILENO, &saved) != 0)
      return;
    struct termios t = saved;
    t.c_lflag &= ~static_cast<unsigned>(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
  } else {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  }
}

void clear_screen() {
  std::cout << "\033[2J\033[H" << std::flush;
}

void hide_cursor() {
  std::cout << "\033[?25l" << std::flush;
}

void show_cursor() {
  std::cout << "\033[?25h" << std::flush;
}

// ——— Base64 (for image protocol) ———

static const char B64_TABLE[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    unsigned a = data[i];
    unsigned b = (i + 1 < len) ? data[i + 1] : 0u;
    unsigned c = (i + 2 < len) ? data[i + 2] : 0u;
    out += B64_TABLE[a >> 2];
    out += B64_TABLE[((a & 3) << 4) | (b >> 4)];
    out += (i + 1 < len) ? B64_TABLE[((b & 15) << 2) | (c >> 6)] : '=';
    out += (i + 2 < len) ? B64_TABLE[c & 63] : '=';
  }
  return out;
}

// Kitty graphics protocol: 24-bit RGB, chunks <=4096, multiple of 4 except last
void render_kitty_image(const std::vector<uint8_t>& rgb, int w, int h,
                        int cell_cols = 0, int cell_rows = 0) {
  constexpr int MAX_CHUNK = 4096;
  std::string b64 = base64_encode(rgb.data(), rgb.size());
  size_t pos = 0;
  bool first = true;
  while (pos < b64.size()) {
    size_t chunk_len = std::min(b64.size() - pos, static_cast<size_t>(MAX_CHUNK));
    if (chunk_len < b64.size() - pos && (chunk_len % 4) != 0)
      chunk_len -= chunk_len % 4;
    bool more = (pos + chunk_len) < b64.size();
    if (first) {
      std::cout << "\033_Ga=T,f=24,s=" << w << ",v=" << h;
      if (cell_cols > 0 && cell_rows > 0)
        std::cout << ",c=" << cell_cols << ",r=" << cell_rows;
      std::cout << ",m=" << (more ? 1 : 0) << ";";
      first = false;
    } else {
      std::cout << "\033_Gm=" << (more ? 1 : 0) << ";";
    }
    std::cout.write(b64.data() + pos, static_cast<std::streamsize>(chunk_len));
    std::cout << "\033\\" << std::flush;
    pos += chunk_len;
  }
}

// iTerm2 Inline Images: OSC 1337 ; File=inline=1 : base64(PNG) BEL
void render_iterm2_image(const std::vector<uint8_t>& rgb, int w, int h) {
  int png_len = 0;
  unsigned char* png = stbi_write_png_to_mem(
      rgb.data(), 0, w, h, 3, &png_len);
  if (!png || png_len <= 0) return;
  std::string b64 = base64_encode(png, static_cast<size_t>(png_len));
  STBIW_FREE(png);
  std::cout << "\033]1337;File=inline=1;width=" << w << "px;height=" << h << "px:" << b64 << "\007" << std::flush;
}

// Block characters by luminance (top = brighter)
inline constexpr std::string_view BLOCKS = " \u2591\u2592\u2593\u2588";  // space, light, medium, dark, full

// ——— FFmpeg decoder ———

struct Decoder {
  AVFormatContext* fmt_ctx{nullptr};
  AVCodecContext* codec_ctx{nullptr};
  const AVCodec* codec{nullptr};
  int stream_index{-1};
  AVPacket* pkt{nullptr};
  AVFrame* frame{nullptr};
  SwsContext* sws_ctx{nullptr};
  std::array<uint8_t, 4> buf_rgb{};  // placeholder for dest
  int width{0};
  int height{0};
  double time_base{0.0};

  ~Decoder() {
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
  }

  void open(const std::string& path) {
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
      throw std::runtime_error("avformat_open_input failed: " + path);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("avformat_find_stream_info failed");
    }

    stream_index = av_find_best_stream(
        fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("no video stream");
    }

    AVStream* stream = fmt_ctx->streams[stream_index];
    time_base = av_q2d(stream->time_base);

    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("decoder not found");
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("avcodec_alloc_context3 failed");
    }
    if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0) {
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("avcodec_parameters_to_context failed");
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("avcodec_open2 failed");
    }

    width = codec_ctx->width;
    height = codec_ctx->height;
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame)
      throw std::runtime_error("av_packet_alloc/av_frame_alloc failed");
  }

  void init_swscale(int out_width, int out_height) {
    sws_ctx = sws_getContext(
        width, height, codec_ctx->pix_fmt,
        out_width, out_height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx)
      throw std::runtime_error("sws_getContext failed");
  }

  bool read_frame(std::vector<uint8_t>& rgb, int out_width, int out_height) {
    if (!sws_ctx)
      init_swscale(out_width, out_height);

    for (;;) {
      int ret = av_read_frame(fmt_ctx, pkt);
      if (ret < 0) {
        if (ret == AVERROR_EOF)
          return false;
        continue;
      }
      if (pkt->stream_index != stream_index) {
        av_packet_unref(pkt);
        continue;
      }

      ret = avcodec_send_packet(codec_ctx, pkt);
      av_packet_unref(pkt);
      if (ret < 0)
        continue;

      ret = avcodec_receive_frame(codec_ctx, frame);
      if (ret == AVERROR(EAGAIN))
        continue;
      if (ret == AVERROR_EOF)
        return false;
      if (ret < 0)
        continue;

      rgb.resize(static_cast<size_t>(out_width) * out_height * 3);
      uint8_t* dst[1] = {rgb.data()};
      int dst_stride[1] = {out_width * 3};
      sws_scale(sws_ctx,
                frame->data, frame->linesize, 0, height,
                dst, dst_stride);
      return true;
    }
  }

  double next_pts_seconds() const {
    if (!frame->pts)
      return 0.0;
    return static_cast<double>(frame->pts) * time_base;
  }
};

#ifdef __APPLE__
// ——— macOS audio (separate demuxer + AudioQueue; stereo S16 @ 44.1kHz) ———

class MacAudioPlayer {
 public:
  MacAudioPlayer() = default;
  ~MacAudioPlayer() { shutdown(); }
  MacAudioPlayer(const MacAudioPlayer&) = delete;
  MacAudioPlayer& operator=(const MacAudioPlayer&) = delete;

  bool open(const std::string& path);
  void start();
  void shutdown();
  void pause();
  void resume();

  bool is_open() const { return fmt_ctx_ != nullptr; }
  /// Seconds of PCM played since AudioQueueStart (device timeline).
  double playback_sec() const;
  /// First decoded audio frame PTS in seconds (media timeline); 0 if unknown.
  double first_pts_media_sec() const {
    return first_audio_pts_media_sec_.load(std::memory_order_relaxed);
  }
  bool has_first_audio_pts() const {
    return first_pts_set_.load(std::memory_order_relaxed);
  }

 private:
  static void aq_callback(void* userdata, AudioQueueRef q, AudioQueueBufferRef buffer);
  void fill_aq_buffer(AudioQueueRef q, AudioQueueBufferRef buffer);
  void decode_thread_main();
  bool decode_one();
  void push_swr_from_frame();
  void flush_decoder();
  void flush_swr();

  AVFormatContext* fmt_ctx_{nullptr};
  AVCodecContext* codec_ctx_{nullptr};
  const AVCodec* codec_{nullptr};
  int stream_index_{-1};
  AVPacket* pkt_{nullptr};
  AVFrame* frame_{nullptr};
  SwrContext* swr_{nullptr};

  AudioQueueRef queue_{nullptr};
  std::vector<AudioQueueBufferRef> aq_buffers_;

  std::vector<uint8_t> pcm_;
  size_t read_off_{0};
  std::mutex pcm_mu_;

  std::thread decode_thread_;
  std::atomic<bool> stop_decode_{false};
  std::atomic<bool> decode_eof_{false};
  std::atomic<bool> first_pts_set_{false};
  std::atomic<double> first_audio_pts_media_sec_{0.0};

  static constexpr int kOutHz = 44100;
  static constexpr UInt32 kAqBufferBytes = 16384;
};

bool MacAudioPlayer::open(const std::string& path) {
  if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr) < 0)
    return false;
  if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }
  stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (stream_index_ < 0) {
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }
  AVStream* st = fmt_ctx_->streams[stream_index_];
  codec_ = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec_) {
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }
  codec_ctx_ = avcodec_alloc_context3(codec_);
  if (!codec_ctx_ || avcodec_parameters_to_context(codec_ctx_, st->codecpar) < 0) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }
  if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }

  AVChannelLayout out_ch{};
  av_channel_layout_default(&out_ch, 2);
  int swr_err = swr_alloc_set_opts2(&swr_, &out_ch, AV_SAMPLE_FMT_S16, kOutHz,
                                    &codec_ctx_->ch_layout, codec_ctx_->sample_fmt,
                                    codec_ctx_->sample_rate, 0, nullptr);
  if (swr_err < 0 || !swr_ || swr_init(swr_) < 0) {
    swr_free(&swr_);
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    return false;
  }

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  if (!pkt_ || !frame_) {
    shutdown();
    return false;
  }
  return true;
}

double MacAudioPlayer::playback_sec() const {
  if (!queue_)
    return 0.0;
  AudioTimeStamp ts{};
  OSStatus err = AudioQueueGetCurrentTime(queue_, nullptr, &ts, nullptr);
  if (err != noErr)
    return 0.0;
  if ((ts.mFlags & kAudioTimeStampSampleTimeValid) == 0)
    return 0.0;
  return static_cast<double>(ts.mSampleTime) / static_cast<double>(kOutHz);
}

void MacAudioPlayer::push_swr_from_frame() {
  const int64_t delay = swr_get_delay(swr_, frame_->sample_rate);
  const int64_t max_out =
      av_rescale_rnd(delay + frame_->nb_samples, kOutHz, frame_->sample_rate, AV_ROUND_UP);
  if (max_out <= 0)
    return;
  uint8_t* out = nullptr;
  int out_linesize = 0;
  if (av_samples_alloc(&out, &out_linesize, 2, static_cast<int>(max_out), AV_SAMPLE_FMT_S16, 0) < 0)
    return;
  const uint8_t** in = const_cast<const uint8_t**>(frame_->extended_data);
  int got = swr_convert(swr_, &out, static_cast<int>(max_out), in, frame_->nb_samples);
  if (got > 0) {
    int bytes = av_samples_get_buffer_size(nullptr, 2, got, AV_SAMPLE_FMT_S16, 1);
    if (bytes > 0) {
      std::lock_guard<std::mutex> lk(pcm_mu_);
      pcm_.insert(pcm_.end(), out, out + bytes);
    }
  }
  av_freep(&out);
}

void MacAudioPlayer::flush_decoder() {
  avcodec_send_packet(codec_ctx_, nullptr);
  for (;;) {
    int ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR_EOF || ret < 0)
      break;
    push_swr_from_frame();
  }
}

void MacAudioPlayer::flush_swr() {
  if (!swr_)
    return;
  constexpr int kChunk = 8192;
  for (;;) {
    uint8_t* out = nullptr;
    int out_linesize = 0;
    if (av_samples_alloc(&out, &out_linesize, 2, kChunk, AV_SAMPLE_FMT_S16, 0) < 0)
      break;
    int got = swr_convert(swr_, &out, kChunk, nullptr, 0);
    if (got <= 0) {
      av_freep(&out);
      break;
    }
    int bytes = av_samples_get_buffer_size(nullptr, 2, got, AV_SAMPLE_FMT_S16, 1);
    if (bytes > 0) {
      std::lock_guard<std::mutex> lk(pcm_mu_);
      pcm_.insert(pcm_.end(), out, out + bytes);
    }
    av_freep(&out);
  }
}

bool MacAudioPlayer::decode_one() {
  for (;;) {
    int ret = av_read_frame(fmt_ctx_, pkt_);
    if (ret == AVERROR_EOF) {
      flush_decoder();
      flush_swr();
      return false;
    }
    if (ret < 0) {
      av_packet_unref(pkt_);
      continue;
    }
    if (pkt_->stream_index != stream_index_) {
      av_packet_unref(pkt_);
      continue;
    }
    ret = avcodec_send_packet(codec_ctx_, pkt_);
    av_packet_unref(pkt_);
    if (ret < 0)
      continue;
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR(EAGAIN))
      continue;
    if (ret < 0)
      return false;
    if (!first_pts_set_.load(std::memory_order_relaxed)) {
      AVStream* st = fmt_ctx_->streams[stream_index_];
      double tb = av_q2d(st->time_base);
      int64_t fts = frame_->best_effort_timestamp;
      if (fts == AV_NOPTS_VALUE)
        fts = frame_->pts;
      if (fts != AV_NOPTS_VALUE) {
        first_audio_pts_media_sec_.store(static_cast<double>(fts) * tb,
                                         std::memory_order_relaxed);
        first_pts_set_.store(true, std::memory_order_relaxed);
      }
    }
    push_swr_from_frame();
    return true;
  }
}

void MacAudioPlayer::decode_thread_main() {
  while (!stop_decode_) {
    bool too_much = false;
    {
      std::lock_guard<std::mutex> lk(pcm_mu_);
      too_much = (pcm_.size() - read_off_ > 512000);
    }
    if (too_much) {
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
      continue;
    }
    if (!decode_one()) {
      decode_eof_ = true;
      break;
    }
  }
}

void MacAudioPlayer::fill_aq_buffer(AudioQueueRef q, AudioQueueBufferRef buffer) {
  UInt32 cap = buffer->mAudioDataBytesCapacity;
  auto* dst = static_cast<UInt8*>(buffer->mAudioData);
  UInt32 done = 0;
  {
    std::lock_guard<std::mutex> lk(pcm_mu_);
    while (done < cap && read_off_ < pcm_.size()) {
      size_t chunk = std::min(static_cast<size_t>(cap - done), pcm_.size() - read_off_);
      std::memcpy(dst + done, pcm_.data() + read_off_, chunk);
      read_off_ += chunk;
      done += static_cast<UInt32>(chunk);
    }
    if (read_off_ > 262144 && read_off_ > pcm_.size() / 2) {
      pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(read_off_));
      read_off_ = 0;
    }
  }
  if (done < cap)
    std::memset(dst + done, 0, cap - done);
  buffer->mAudioDataByteSize = cap;
  AudioQueueEnqueueBuffer(q, buffer, 0, nullptr);
}

void MacAudioPlayer::aq_callback(void* userdata, AudioQueueRef q, AudioQueueBufferRef buffer) {
  auto* self = static_cast<MacAudioPlayer*>(userdata);
  self->fill_aq_buffer(q, buffer);
}

void MacAudioPlayer::start() {
  if (!fmt_ctx_ || queue_)
    return;

  AudioStreamBasicDescription desc{};
  desc.mSampleRate = kOutHz;
  desc.mFormatID = kAudioFormatLinearPCM;
  desc.mFormatFlags =
      kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  desc.mBitsPerChannel = 16;
  desc.mChannelsPerFrame = 2;
  desc.mBytesPerFrame = 4;
  desc.mFramesPerPacket = 1;
  desc.mBytesPerPacket = 4;

  OSStatus st = AudioQueueNewOutput(&desc, aq_callback, this, nullptr, nullptr, 0, &queue_);
  if (st != noErr || !queue_)
    return;

  AudioQueueSetParameter(queue_, kAudioQueueParam_Volume, 1.0f);

  stop_decode_ = false;
  decode_eof_ = false;
  decode_thread_ = std::thread(&MacAudioPlayer::decode_thread_main, this);

  // Wait briefly for PCM so priming buffers can carry real samples (not 0-byte enqueue).
  for (int w = 0; w < 100; ++w) {
    {
      std::lock_guard<std::mutex> lk(pcm_mu_);
      if (pcm_.size() - read_off_ >= static_cast<size_t>(kAqBufferBytes))
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  for (int i = 0; i < 3; ++i) {
    AudioQueueBufferRef buf = nullptr;
    if (AudioQueueAllocateBuffer(queue_, kAqBufferBytes, &buf) != noErr || !buf)
      continue;
    aq_buffers_.push_back(buf);
    fill_aq_buffer(queue_, buf);
  }

  AudioQueueStart(queue_, nullptr);
}

void MacAudioPlayer::pause() {
  if (queue_)
    AudioQueuePause(queue_);
}

void MacAudioPlayer::resume() {
  if (queue_)
    AudioQueueStart(queue_, nullptr);
}

void MacAudioPlayer::shutdown() {
  stop_decode_ = true;
  if (decode_thread_.joinable())
    decode_thread_.join();

  if (queue_) {
    AudioQueueStop(queue_, true);
    for (AudioQueueBufferRef b : aq_buffers_)
      AudioQueueFreeBuffer(queue_, b);
    aq_buffers_.clear();
    AudioQueueDispose(queue_, true);
    queue_ = nullptr;
  }

  swr_free(&swr_);
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  avcodec_free_context(&codec_ctx_);
  avformat_close_input(&fmt_ctx_);

  {
    std::lock_guard<std::mutex> lk(pcm_mu_);
    pcm_.clear();
    read_off_ = 0;
  }
  stream_index_ = -1;
  codec_ = nullptr;
  decode_eof_ = false;
  first_pts_set_.store(false);
  first_audio_pts_media_sec_.store(0.0);
}

#else

class MacAudioPlayer {
 public:
  bool open(const std::string&) { return false; }
  void start() {}
  void shutdown() {}
  void pause() {}
  void resume() {}
  bool is_open() const { return false; }
  double playback_sec() const { return 0.0; }
  double first_pts_media_sec() const { return 0.0; }
  bool has_first_audio_pts() const { return false; }
};

#endif  // __APPLE__

// ——— Render frame to terminal (block chars + grayscale or 24-bit true color) ———

void render_to_terminal(const std::vector<uint8_t>& rgb,
                        int in_width, int in_height,
                        int term_cols, int term_rows,
                        BlockColorMode color_mode,
                        bool cursor_at_home = true,
                        int start_col = 1) {
  // Each terminal cell = block of pixels. 2 chars width per block for aspect.
  const int block_w = (in_width + term_cols - 1) / term_cols;
  const int block_h = (in_height + term_rows - 1) / term_rows;
  if (block_w <= 0 || block_h <= 0)
    return;

  if (cursor_at_home)
    std::cout << "\033[H";  // cursor home (skip when drawing in floating window)

  for (int ty = 0; ty < term_rows; ++ty) {
    int y = ty * block_h;
    if (y >= in_height)
      break;
    for (int tx = 0; tx < term_cols; ++tx) {
      int x = tx * block_w;
      if (x >= in_width)
        break;

      // Average block (simple box)
      int r = 0, g = 0, b = 0;
      int n = 0;
      for (int dy = 0; dy < block_h && (y + dy) < in_height; ++dy) {
        for (int dx = 0; dx < block_w && (x + dx) < in_width; ++dx) {
          size_t idx = static_cast<size_t>((y + dy) * in_width + (x + dx)) * 3;
          r += rgb[idx];
          g += rgb[idx + 1];
          b += rgb[idx + 2];
          ++n;
        }
      }
      if (n > 0) {
        r /= n;
        g /= n;
        b /= n;
      }
      // Luminance -> block density; color from mode
      int lum = (r * 77 + g * 150 + b * 29) >> 8;
      size_t block_idx = static_cast<size_t>(lum * (BLOCKS.size() + 1)) >> 8;
      if (block_idx >= BLOCKS.size())
        block_idx = BLOCKS.size() - 1;
      if (color_mode == BlockColorMode::Grayscale) {
        int ansi = 232 + (lum * 24) / 256;
        if (ansi > 255)
          ansi = 255;
        std::cout << "\033[38;5;" << ansi << "m" << BLOCKS[block_idx];
      } else {
        std::cout << "\033[38;2;" << r << ";" << g << ";" << b << "m" << BLOCKS[block_idx];
      }
    }
    if (ty + 1 < term_rows) {
      std::cout << "\033[E";
      if (start_col > 1)
        std::cout << "\033[" << start_col << "G";
    }
  }
  std::cout << "\033[0m" << std::flush;
}

// ——— FTXUI: floating window (centered box) ———

constexpr int FLOAT_WINDOW_PIXEL_MODE_WIDTH = 320;  // Kitty / iTerm2 (image protocol)
constexpr int FLOAT_WINDOW_BLOCK_MODE_WIDTH = 640;  // block mode: 2× decode for finer blocks

void render_floating_window_frame(int box_inner_cols, int box_inner_rows,
                                   int content_rows) {
  using namespace ftxui;
  auto inner = emptyElement()
               | size(WIDTH, EQUAL, box_inner_cols)
               | size(HEIGHT, EQUAL, box_inner_rows);
  auto box = borderDouble(inner);
  auto doc = center(box);
  // Use content_rows height so box stays above status line (last row)
  auto screen = Screen::Create(Dimension::Full(), Dimension::Fixed(content_rows));
  Render(screen, doc);
  screen.Print();
}

}  // namespace

static void print_usage(const char* prog) {
  std::cerr << "Usage: " << (prog ? prog : "tmplay")
            << " [--mute] [-c|--colormode grayscale|truecolor] <video_file>\n"
            << "  Default: play audio (macOS); use --mute to disable.\n"
            << "  Default colormode: truecolor (block mode only).\n";
}

static bool arg_iequals(const char* a, const char* b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
    char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
    if (ca != cb)
      return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

int main(int argc, char* argv[]) {
  BlockColorMode block_color_mode = BlockColorMode::Truecolor;
  bool mute = false;
  std::string path;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!a)
      continue;
    if (std::strcmp(a, "--mute") == 0) {
      mute = true;
      continue;
    }
    if (std::strcmp(a, "-c") == 0 || std::strcmp(a, "--colormode") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << a << " requires grayscale or truecolor\n";
        print_usage(argv[0]);
        return 1;
      }
      const char* v = argv[++i];
      if (arg_iequals(v, "grayscale") || arg_iequals(v, "greyscale") ||
          arg_iequals(v, "gray")) {
        block_color_mode = BlockColorMode::Grayscale;
      } else if (arg_iequals(v, "truecolor") || arg_iequals(v, "rgb") ||
                 arg_iequals(v, "24bit")) {
        block_color_mode = BlockColorMode::Truecolor;
      } else {
        std::cerr << "Error: unknown colormode '" << v
                  << "' (use grayscale or truecolor)\n";
        print_usage(argv[0]);
        return 1;
      }
    } else if (a[0] == '-') {
      std::cerr << "Error: unknown option " << a << '\n';
      print_usage(argv[0]);
      return 1;
    } else {
      if (!path.empty()) {
        std::cerr << "Error: multiple video paths\n";
        print_usage(argv[0]);
        return 1;
      }
      path = a;
    }
  }

  if (path.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  av_log_set_level(AV_LOG_QUIET);

  Decoder dec;
  try {
    dec.open(path);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }

  MacAudioPlayer audio;
  const bool audio_on = (!mute && audio.open(path));
  if (audio_on)
    audio.start();

  TermSize term = get_term_size();
  DisplayMode mode = detect_display_mode();

  // Floating window: block mode decodes at 2× width for sharper characters; pixel mode stays 320
  const bool pixel_image_mode =
      (mode == DisplayMode::KittyImage || mode == DisplayMode::Iterm2Image);
  const int out_width =
      pixel_image_mode ? FLOAT_WINDOW_PIXEL_MODE_WIDTH : FLOAT_WINDOW_BLOCK_MODE_WIDTH;
  int out_height = (out_width * dec.height) / dec.width;
  out_height = (out_height / 2) * 2;  // even for scaler
  if (mode == DisplayMode::Iterm2Image && out_height > 676)
    out_height = 676;

  // Box size in terminal cells: use actual cell pixel size when available so border fits image
  auto box_cells_from_term = [out_width, out_height](const TermSize& t) -> std::pair<int, int> {
    if (t.xpixel > 0 && t.ypixel > 0 && t.cols > 0 && t.rows > 0) {
      int cw = t.xpixel / t.cols;
      int ch = t.ypixel / t.rows;
      if (cw > 0 && ch > 0) {
        int cols = (out_width + cw - 1) / cw;
        int rows = (out_height + ch - 1) / ch;
        return {std::max(1, cols), std::max(1, rows)};
      }
    }
    return {std::max(1, (out_width + 7) / 8),
            std::max(1, (out_height + 15) / 16)};  // fallback ~8×16 px/cell
  };
  int box_inner_cols = box_cells_from_term(term).first;
  int box_inner_rows = box_cells_from_term(term).second;

  set_raw_terminal(true);
  if (mode != DisplayMode::Block)
    std::cerr << "Floating " << out_width << "x" << out_height << " px — "
              << (mode == DisplayMode::KittyImage ? "Kitty/WezTerm/Warp" : "iTerm2") << "\n";
  clear_screen();
  hide_cursor();

  std::vector<uint8_t> rgb;
  double video_time = 0.0;
  bool paused = false;
  bool quit = false;

  auto refresh_layout = [&]() {
    TermSize t = get_term_size();
    int content_rows = t.rows > 1 ? t.rows - 1 : t.rows;
    int video_cursor_row = (content_rows - (box_inner_rows + 2)) / 2 + 2;
    int video_cursor_col = (t.cols - (box_inner_cols + 2)) / 2 + 2;
    int status_row = t.rows;
    clear_screen();
    render_floating_window_frame(box_inner_cols, box_inner_rows, content_rows);
    return std::make_tuple(content_rows, video_cursor_row, video_cursor_col, status_row);
  };

  auto draw_frame_and_status = [&](int video_cursor_row, int video_cursor_col,
                                  int status_row) {
    std::cout << "\033[" << video_cursor_row << ";" << video_cursor_col << "H";
    if (mode == DisplayMode::KittyImage) {
      render_kitty_image(rgb, out_width, out_height, box_inner_cols, box_inner_rows);
    } else if (mode == DisplayMode::Iterm2Image) {
      render_iterm2_image(rgb, out_width, out_height);
    } else {
      render_to_terminal(rgb, out_width, out_height, box_inner_cols, box_inner_rows,
                         block_color_mode, false, video_cursor_col);
    }
    std::cout << "\033[" << status_row << ";1H\033[K";
    int min = static_cast<int>(video_time / 60);
    int sec = static_cast<int>(std::fmod(video_time, 60.0));
    std::cout << " time: " << min << ":" << std::setfill('0') << std::setw(2) << sec
              << " [space] pause [q] quit " << std::flush;
  };

  int content_rows = term.rows > 1 ? term.rows - 1 : term.rows;
  int video_cursor_row = (content_rows - (box_inner_rows + 2)) / 2 + 2;
  int video_cursor_col = (term.cols - (box_inner_cols + 2)) / 2 + 2;
  int status_row = term.rows;

  render_floating_window_frame(box_inner_cols, box_inner_rows, content_rows);

  bool prev_paused = false;
  while (!quit) {
    TermSize current_term = get_term_size();
    if (current_term.rows != term.rows || current_term.cols != term.cols) {
      term = current_term;
      auto [bc, br] = box_cells_from_term(term);
      box_inner_cols = bc;
      box_inner_rows = br;
      std::tie(content_rows, video_cursor_row, video_cursor_col, status_row) = refresh_layout();
      if (!rgb.empty())
        draw_frame_and_status(video_cursor_row, video_cursor_col, status_row);
      continue;
    }

    // Non-blocking key read
    char key = 0;
    if (read(STDIN_FILENO, &key, 1) > 0) {
      if (key == 'q' || key == 'Q' || key == 3)  // 3 = Ctrl+C
        quit = true;
      else if (key == ' ')
        paused = !paused;
    }

    if (quit)
      break;
    if (audio_on && paused != prev_paused) {
      if (paused)
        audio.pause();
      else
        audio.resume();
      prev_paused = paused;
    }
    if (paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (!dec.read_frame(rgb, out_width, out_height)) {
      break;
    }

    const double vpts = dec.next_pts_seconds();

    // Pace video to audio (audio master); without audio, use consecutive PTS deltas.
    constexpr double kSyncLeadSec = 0.045;
    if (audio_on) {
      double a0 = audio.first_pts_media_sec();
      if (!audio.has_first_audio_pts())
        a0 = 0.0;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (std::chrono::steady_clock::now() < deadline && !quit) {
        const double played = audio.playback_sec();
        if (a0 + played >= vpts - kSyncLeadSec)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } else {
      static std::optional<double> prev_vpts_pacing;
      if (prev_vpts_pacing && vpts > *prev_vpts_pacing) {
        const double delta = vpts - *prev_vpts_pacing;
        if (delta > 0.0 && delta < 2.0)
          std::this_thread::sleep_for(std::chrono::duration<double>(delta));
      }
      prev_vpts_pacing = vpts;
    }

    video_time = vpts;

    draw_frame_and_status(video_cursor_row, video_cursor_col, status_row);
  }

  audio.shutdown();

  show_cursor();
  set_raw_terminal(false);
  clear_screen();

  return 0;
}
