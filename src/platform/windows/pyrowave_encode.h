/**
 * @file src/platform/windows/pyrowave_encode.h
 * @brief PyroWave (Vulkan wavelet, intra-only) host encoder for Sunshine on Windows.
 *
 * PyroWave does not ride the avcodec/nvenc encoder_t abstraction: it owns its own Vulkan 1.3
 * device and emits an already-packetized byte bitstream that Sunshine ships as packet_raw_generic.
 * On Windows the captured frame is a D3D11 shared texture (img_d3d_t): the encoder opens it on a
 * private D3D11 device, copies it into a shareable intermediate texture, imports that texture into
 * PyroWave's Vulkan device, converts RGB->YUV420 with a compute shader, and GPU-encodes.
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
   * Owns a private D3D11 bridge device plus a dedicated PyroWave Vulkan device and encoder.
   * GPU resources are created lazily on the first encoded frame, because the capture adapter is
   * only known once a captured image is seen. Not thread-safe: the caller must serialize encode()
   * calls, matching the pyrowave encoder contract.
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
     * @param yuv444 True for 4:4:4 chroma (full-resolution Cb/Cr), false for 4:2:0.
     * @param ten_bit True when a 10-bit profile was negotiated: planes use R16_UNORM containers
     *                (PyroWave is depth-agnostic; the container just must match the client's).
     * @param hdr True to encode HDR content: BT.2020 NCL full-range PQ (requires ten_bit and an
     *            HDR display; else SDR BT.601 math is used). On Windows the HDR capture is scRGB
     *            FP16 and the convert shader performs the linear->PQ encode.
     * @return Session on success, nullptr on failure.
     */
    static std::unique_ptr<encoder_t> create(int width, int height, int bitrate_kbps, int frame_rate, bool yuv444, bool ten_bit, bool hdr);

    /**
     * @brief Encode one captured frame into a PyroWave bitstream.
     *
     * Requires a D3D11 (VRAM) captured image. Copies the shared capture texture to the Vulkan
     * device, converts to YUV420P on the GPU, runs the synchronous GPU encode, and packetizes the
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
