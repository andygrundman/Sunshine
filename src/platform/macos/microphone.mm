/**
 * @file src/platform/macos/microphone.mm
 * @brief macOS 14.2+ system audio capture
 *
 * References: https://gist.github.com/directmusic/7d653806c24fe5bb8166d12a9f4422de
 */
// local includes
#import "src/config.h"
#import "src/logging.h"
#import "src/platform/common.h"
#import "src/platform/macos/audio_device.h"
#import "src/platform/macos/audio_tap.h"

namespace platf {
  using namespace std::literals;

  struct av_mic_t: public mic_t {
    AudioTap *audioTap {};

    ~av_mic_t() override {
      [audioTap release];
    }

    capture_e sample(std::vector<float> &sample_in, std::chrono::steady_clock::time_point &capture_timestamp_out) override {
      auto sample_size = sample_in.size();
      capture_timestamp_out = std::chrono::steady_clock::now();

      uint32_t length = 0;
      void *byteSampleBuffer = TPCircularBufferTail(&audioTap->audioSampleBuffer, &length);

      while (length < sample_size * sizeof(float)) {
        [audioTap.samplesArrivedSignal wait];
        byteSampleBuffer = TPCircularBufferTail(&audioTap->audioSampleBuffer, &length);
      }

      const float *sampleBuffer = (float *) byteSampleBuffer;
      std::vector<float> vectorBuffer(sampleBuffer, sampleBuffer + sample_size);

      std::copy_n(std::begin(vectorBuffer), sample_size, std::begin(sample_in));

      TPCircularBufferConsume(&audioTap->audioSampleBuffer, sample_size * sizeof(float));

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

      AudioDevice *device = nullptr;
      if (!config::audio.sink.empty()) {
        // config contains a specific audio sink
        audio_sink = config::audio.sink.c_str();
        NSArray<AudioDevice *> *outputDevices = [AudioTap getOutputDevices];

        for (AudioDevice *d in outputDevices) {
          if ([[d uid] isEqualToString:[NSString stringWithUTF8String:audio_sink]]) {
            device = d;
            break;
          }
        }

        if (device == nullptr) {
          BOOST_LOG(error) << "Could not find audio sink device '"sv << audio_sink << "'. Please enter one of the following devices or leave audio sink blank."sv;
          BOOST_LOG(error) << "Available inputs:"sv;

          for (AudioDevice *d in outputDevices) {
            BOOST_LOG(error) << "\t"sv << [[d uid] UTF8String] << " ("sv << [[d name] UTF8String] << ")"sv;
          }

          return nullptr;
        }
      }

      if (device == nullptr) {
        device = [AudioDevice defaultOutputDevice];
      }

      mic->audioTap = [[AudioTap alloc] initWithDeviceUID:[device uid] sampleRate:sample_rate frameSize:frame_size channels:channels];
      if (!mic->audioTap) {
        BOOST_LOG(error) << "Failed to initialize audio tap for "sv << audio_sink << ", uid: "sv << audioSinkUID;
        return nullptr;
      }

      return mic;
    }

    bool is_sink_available(const std::string &sink) override {
      NSString *sink_ns = [NSString stringWithUTF8String:sink.c_str()];
      for (AudioDevice *device in [AudioDevice getAllDevices]) {
        if ([[device uid] isEqualToString:sink_ns]) {
          return [device isOutput];
        }
      }

      return false;
    }

    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Fill host sink name with the device_id of the current default audio device.
      AudioDevice *device = [AudioDevice defaultOutputDevice];
      if (!device) {
        return std::nullopt;
      }
      sink.host = std::string([[device uid] UTF8String]);

      // virtual 5.1/7.1 is always available
      // XXX naming
      sink.null = std::make_optional(sink_t::null_t {
        "virtual-stereo"s     + sink.host,
        "virtual-surround51"s + sink.host,
        "virtual-surround71"s + sink.host,
      });

      return sink;
    }
  };

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
