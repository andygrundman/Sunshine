/**
 * @file src/platform/macos/microphone.mm
 * @brief macOS 14.2+ system audio capture
 *
 * References: https://gist.github.com/directmusic/7d653806c24fe5bb8166d12a9f4422de
 */
// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/audio_tap.h"

namespace platf {
  using namespace std::literals;

  struct av_mic_t: public mic_t {
    AudioTap *audioTap {};

    ~av_mic_t() override {
      [audioTap release];
    }

    capture_e sample(std::vector<float> &sample_in, std::chrono::steady_clock::time_point &capture_timestamp_out) override {
      // auto sample_size = sample_in.size();
      // capture_timestamp_out = std::chrono::steady_clock::now();

      // uint32_t length = 0;
      // void *byteSampleBuffer = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &length);

      // while (length < sample_size * sizeof(float)) {
      //   [av_audio_capture.samplesArrivedSignal wait];
      //   byteSampleBuffer = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &length);
      // }

      // const float *sampleBuffer = (float *) byteSampleBuffer;
      // std::vector<float> vectorBuffer(sampleBuffer, sampleBuffer + sample_size);

      // std::copy_n(std::begin(vectorBuffer), sample_size, std::begin(sample_in));

      // TPCircularBufferConsume(&av_audio_capture->audioSampleBuffer, sample_size * sizeof(float));

      return capture_e::ok;
    }
  };

  struct macos_audio_control_t: public audio_control_t {
    NSString *audioSinkUID {};

  public:
    int set_sink(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::set_sink() unimplemented: "sv << sink;
      return 0;
    }

    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
      auto mic = std::make_unique<av_mic_t>();
      const char *audio_sink = "";

      if (!config::audio.sink.empty()) {
        audio_sink = config::audio.sink.c_str();
      }

      if ((audioSinkUID = [AudioTap findAudioSinkUID:[NSString stringWithUTF8String:audio_sink]]) == nil) {
        BOOST_LOG(error) << "Could not find audio sink device '"sv << audio_sink << "'. Please enter one of the following devices or leave audio sink blank."sv;
        BOOST_LOG(error) << "Available inputs:"sv;

        for (NSString *name in [AudioTap getOutputDevices]) {
          BOOST_LOG(error) << "\t"sv << [name UTF8String];
        }

        return nullptr;
      }

      mic->audioTap = [[AudioTap alloc] initWithDeviceUID:audioSinkUID sampleRate:sample_rate frameSize:frame_size channels:channels];
      if (!mic->audioTap) {
        BOOST_LOG(error) << "Failed to initialize audio tap for "sv << audio_sink << ", uid: "sv << audioSinkUID;
        return nullptr;
      }

      return mic;
    }

    bool is_sink_available(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
      return true;
    }

    std::optional<sink_t> sink_info() override {
      sink_t sink;
      BOOST_LOG(warning) << "audio_control_t::sink_info() unimplemented: "sv;

      return sink;
    }
  };

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
