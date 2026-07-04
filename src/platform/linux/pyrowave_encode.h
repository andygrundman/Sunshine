/**
 * @file src/platform/linux/pyrowave_encode.h
 * @brief PyroWave (Vulkan wavelet, intra-only) host encoder for Sunshine.
 *
 * PyroWave does not ride the avcodec/nvenc encoder_t abstraction: it owns its own Vulkan 1.3
 * device and emits an already-packetized byte bitstream that Sunshine ships as packet_raw_generic.
 * This first pass is the CPU-input path (Stage A): captured RGB frames are converted to YUV420P on
 * the CPU and handed to pyrowave_encoder_encode_cpu_synchronous. A later pass replaces the input
 * with a zero-copy dmabuf->VkImage->NV12 GPU path (Stage B) reusing vulkan_encode.cpp.
 *
 * The whole translation unit is compiled only when SUNSHINE_ENABLE_PYROWAVE is defined.
 */
#pragma once

#ifdef SUNSHINE_ENABLE_PYROWAVE

  #include <cstdint>
  #include <memory>
  #include <vector>

namespace platf {
  struct img_t;
}

namespace platf::pyrowave {

  /**
   * @brief Probe for a PyroWave-capable Vulkan device.
   *
   * Creates a throwaway Vulkan 1.3 instance/device and checks the compute features PyroWave's
   * encoder requires (subgroup size control, shaderInt16, storageBuffer8BitAccess). Cheap enough
   * to call from the encoder-probe path; result gates whether SCM_PYROWAVE is advertised.
   *
   * @return True when at least one physical device can run the PyroWave encoder.
   */
  bool validate();

  /**
   * @brief One PyroWave encode session bound to a stream's width/height.
   *
   * Owns a dedicated Vulkan device (with its create-infos kept alive for the device lifetime, as
   * pyrowave_create_device requires) plus a pyrowave_encoder. Not thread-safe: the caller must
   * serialize encode() calls, matching the pyrowave encoder contract.
   */
  class encoder_t {
  public:
    ~encoder_t();

    /**
     * @brief Create a session for the given frame dimensions.
     *
     * @param width Encoded luma width (made even for 4:2:0).
     * @param height Encoded luma height (made even for 4:2:0).
     * @param bitrate_kbps Negotiated target bitrate in kbps, mapped to a per-frame byte budget.
     * @param frame_rate Frames per second, used with the bitrate to size the per-frame budget.
     * @return Session on success, nullptr on failure.
     */
    static std::unique_ptr<encoder_t> create(int width, int height, int bitrate_kbps, int frame_rate);

    /**
     * @brief Encode one captured frame into a PyroWave bitstream.
     *
     * Converts the captured image to YUV420P, runs the synchronous CPU encode, and packetizes the
     * result into a single contiguous byte buffer (packet boundaries are internal to PyroWave and
     * carried in the stream). Every frame is intra (IDR).
     *
     * @param img Captured image from the display backend.
     * @param out Receives the encoded, packetized bitstream bytes.
     * @return 0 on success, negative on failure.
     */
    int encode(const platf::img_t &img, std::vector<uint8_t> &out);

  private:
    encoder_t() = default;

    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf::pyrowave

#endif  // SUNSHINE_ENABLE_PYROWAVE
