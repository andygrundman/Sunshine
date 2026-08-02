/**
 * @file src/platform/windows/pyrowave_encode.cpp
 * @brief PyroWave (Vulkan wavelet) host encoder — Windows D3D11 capture bridge.
 *
 * Frame path:
 *   img_d3d_t (D3D11 shared capture texture, keyed mutex)
 *     -> open on a private D3D11 "bridge" device (cached per image)
 *     -> CopyResource into a shareable intermediate texture (NT-handle shared, no keyed mutex)
 *     -> D3D11 fence signal + CPU wait
 *     -> intermediate imported once into PyroWave's Vulkan device (D3D11_TEXTURE NT handle)
 *     -> RGB->YUV compute shader into R8/R16 plane images (4:2:0 or 4:4:4; FP16 scRGB captures
 *        are sRGB/PQ-encoded in the shader)
 *     -> pyrowave GPU encode + packetize
 *
 * The extra GPU copy exists because the capture texture uses a keyed mutex, which Granite (the
 * Vulkan backend PyroWave uses) cannot acquire at submit time (no VK_KHR_win32_keyed_mutex).
 * The keyed mutex is instead acquired on the D3D11 side around the copy, and the fence CPU wait
 * orders the copy against the Vulkan reads.
 */
#ifdef SUNSHINE_ENABLE_PYROWAVE

  #include <algorithm>
  #include <array>
  #include <cstring>
  #include <map>
  #include <vector>

  #include <vulkan/vulkan.h>

  #include "pyrowave.h"

  #include "display.h"
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "pyrowave_encode.h"

namespace platf::pyrowave {

  namespace {

    // RGB->YUV420P BT.601 compute shader (SPIR-V), shared with the Linux backend. Generated from
    // src_assets/linux/assets/shaders/vulkan/pyrowave_rgb2yuv.comp via:
    //   glslc --target-env=vulkan1.3 -O pyrowave_rgb2yuv.comp -mfmt=c -o pyrowave_rgb2yuv.spv.h
    const uint32_t rgb2yuv_spv[] =
  #include "src/platform/linux/pyrowave_rgb2yuv.spv.h"
      ;

    using device5_t = util::safe_ptr<ID3D11Device5, dxgi::Release<ID3D11Device5>>;
    using device_ctx4_t = util::safe_ptr<ID3D11DeviceContext4, dxgi::Release<ID3D11DeviceContext4>>;
    using fence_t = util::safe_ptr<ID3D11Fence, dxgi::Release<ID3D11Fence>>;
    using adapter_base_t = util::safe_ptr<IDXGIAdapter, dxgi::Release<IDXGIAdapter>>;

    VkFormat dxgi_to_vk_format(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
          return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
          return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          // scRGB capture (linear, Rec.709 primaries): the convert shader encodes it to sRGB
          // (SDR) or BT.2020 PQ (HDR) via the pc.scrgb branch.
          return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
          return VK_FORMAT_UNDEFINED;
      }
    }

    // Features PyroWave's encoder requires, per pyrowave.h. Filled by query_device_features().
    struct required_features_t {
      bool shader_int16 = false;
      bool storage_buffer_8bit = false;
      bool subgroup_size_control = false;

      bool ok() const {
        return shader_int16 && storage_buffer_8bit && subgroup_size_control;
      }
    };

    required_features_t query_device_features(VkPhysicalDevice phys) {
      required_features_t out;

      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(phys, &props);
      if (props.apiVersion < VK_API_VERSION_1_3) {
        return out;  // not ok
      }

      VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      f12.pNext = &f13;
      VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &f12;
      vkGetPhysicalDeviceFeatures2(phys, &f2);

      out.shader_int16 = f2.features.shaderInt16 == VK_TRUE;
      out.storage_buffer_8bit = f12.storageBuffer8BitAccess == VK_TRUE;
      out.subgroup_size_control = f13.subgroupSizeControl == VK_TRUE;
      return out;
    }

    // Find a queue family for PyroWave. Prefer a universal graphics+compute queue, which is what
    // PyroWave's reference adoption uses (QUEUE_INDEX_GRAPHICS) — Granite maps its graphics/compute/
    // transfer queues onto it and its internal cross-queue sync works. A compute-only family caused
    // a self-deadlock in Granite's submission path.
    bool find_compute_queue_family(VkPhysicalDevice phys, uint32_t &family_out) {
      uint32_t count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
      for (uint32_t i = 0; i < count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
          family_out = i;
          return true;
        }
      }
      for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
          family_out = i;
          return true;
        }
      }
      return false;
    }

    uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags props) {
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(pd, &mp);
      for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
          return i;
        }
      }
      return UINT32_MAX;
    }

    // A single R8/R16_UNORM plane image on PyroWave's device: written by the RGB->YUV compute pass
    // and read by the GPU encode.
    struct plane_image_t {
      VkDevice dev = VK_NULL_HANDLE;
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
      int w = 0, h = 0;
      VkFormat format = VK_FORMAT_R8_UNORM;

      bool init(VkDevice device, VkPhysicalDevice pd, int width, int height, VkFormat fmt) {
        dev = device;
        w = width;
        h = height;
        format = fmt;

        VkImageCreateInfo ci {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {(uint32_t) w, (uint32_t) h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &image) != VK_SUCCESS) {
          return false;
        }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, image, &mr);
        VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) {
          return false;
        }
        vkBindImageMemory(dev, image, mem, 0);

        VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(dev, &vi, nullptr, &view) == VK_SUCCESS;
      }

      pyrowave_image_view image_view() const {
        pyrowave_image_view v {};
        v.image = image;
        v.width = (uint32_t) w;
        v.height = (uint32_t) h;
        v.image_format = format;
        v.view_format = format;
        v.mip_level = 0;
        v.layer = 0;
        v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        v.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
        v.layout = VK_IMAGE_LAYOUT_GENERAL;
        return v;
      }

      void destroy() {
        if (view) {
          vkDestroyImageView(dev, view, nullptr);
        }
        if (image) {
          vkDestroyImage(dev, image, nullptr);
        }
        if (mem) {
          vkFreeMemory(dev, mem, nullptr);
        }
      }
    };

  }  // namespace

  bool validate() {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
      return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(inst, &count, devs.data());

    bool ok = false;
    for (auto phys : devs) {
      uint32_t fam;
      if (query_device_features(phys).ok() && find_compute_queue_family(phys, fam)) {
        ok = true;
        break;
      }
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
  }

  // PyroWave wavelet layout constants, mirrored from pyrowave_common.hpp.
  constexpr int DECOMPOSITION_LEVELS = 5;
  constexpr int NUM_COMPONENTS = 3;
  constexpr int BANDS_PER_LEVEL = 4;
  constexpr int WAVELET_ALIGNMENT = 1 << DECOMPOSITION_LEVELS;
  constexpr int MIN_IMAGE_SIZE = 4 << DECOMPOSITION_LEVELS;

  /**
   * @brief Map wavelet decomposition levels onto PyroWave's global 32x32 block numbering.
   *
   * PyroWave numbers blocks coarsest level first (`WaveletBuffers::init_block_meta()`, iterating
   * level 4 down to 0, then component, then band) and `Encoder::packetize()` emits them in that
   * same ascending index order. So a block index alone says which level a block belongs to, and
   * the bitstream is laid out most-important-first: level 4 (the 1/32-scale LL and its detail
   * bands) leads, level 0 (the finest detail) trails.
   *
   * @param width Encoded luma width.
   * @param height Encoded luma height.
   * @param yuv444 True for 4:4:4, false for 4:2:0 (which omits chroma at level 0).
   * @return Per-level exclusive block-index bound: entry `L` is the first block index belonging
   *         to a level finer than `L`, i.e. levels 4..L occupy indices `[0, ret[L])`.
   */
  std::array<uint32_t, DECOMPOSITION_LEVELS> compute_level_block_ends(int width, int height, bool yuv444) {
    auto align_up = [](int v) {
      v = ((v + WAVELET_ALIGNMENT - 1) / WAVELET_ALIGNMENT) * WAVELET_ALIGNMENT;
      return std::max(v, MIN_IMAGE_SIZE);
    };

    // The wavelet pyramid starts at half resolution, then halves per level.
    const int aligned_width = align_up(width);
    const int aligned_height = align_up(height);

    std::array<uint32_t, DECOMPOSITION_LEVELS> ends {};
    uint32_t blocks = 0;

    for (int level = DECOMPOSITION_LEVELS - 1; level >= 0; --level) {
      const int level_width = (aligned_width / 2) >> level;
      const int level_height = (aligned_height / 2) >> level;
      const int blocks_x_32 = (level_width + 31) / 32;
      const int blocks_y_32 = (((level_height + 7) / 8) + 3) / 4;

      for (int component = 0; component < NUM_COMPONENTS; ++component) {
        // The coarsest level carries the LL band (band 0); finer levels only carry detail bands.
        // 4:2:0 drops chroma entirely at the finest level.
        if (level == 0 && component != 0 && !yuv444) {
          continue;
        }
        const int first_band = (level == DECOMPOSITION_LEVELS - 1) ? 0 : 1;
        blocks += (uint32_t) ((BANDS_PER_LEVEL - first_band) * blocks_x_32 * blocks_y_32);
      }

      ends[level] = blocks;
    }

    return ends;
  }

  /**
   * @brief Finest wavelet level included in the FEC-protected head.
   *
   * The head of a PyroWave frame — the sequence header plus levels 4 and 3 — is where loss is
   * catastrophic (a single lost LL4 block costs ~10 dB); loss anywhere past it degrades to blur
   * that partial delivery can show anyway. So the RTP layer protects levels 4 down to this one
   * with Reed-Solomon and leaves the rest bare. Protecting level 4 alone would be cheaper still
   * but leaves no floor: one early loss in a bare L3 truncates the frame to L4-only (< 35 dB).
   * See docs/pyrowave-partial-du-design.md in the client tree for the measurements.
   */
  constexpr int FEC_PROTECTED_THROUGH_LEVEL = 3;

  struct encoder_t::impl_t {
    pyrowave_device pdev = nullptr;
    pyrowave_encoder enc = nullptr;
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    bool ten_bit = false;
    bool hdr = false;
    size_t max_bitstream = 0;

    // Exclusive block-index bound per wavelet level; see compute_level_block_ends().
    std::array<uint32_t, DECOMPOSITION_LEVELS> level_block_ends {};

    // Packetization buffers, reused across frames (see the sizing comment in encode()).
    std::vector<uint8_t> scratch;
    std::vector<pyrowave_packet> packets;

    // GPU resources are created lazily on the first frame (the capture adapter is only known once
    // a captured image is seen). Once init fails, the session stays failed instead of retrying and
    // spamming the log every frame.
    enum class state_e {
      pending,
      ready,
      failed
    };

    state_e state = state_e::pending;

    // D3D11 bridge device on the capture adapter.
    dxgi::device_t d3d_dev;
    dxgi::device_ctx_t d3d_ctx;
    device5_t d3d_dev5;
    device_ctx4_t d3d_ctx4;
    fence_t d3d_fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    // Shareable intermediate texture (copy destination) and its Vulkan import. Recreated if the
    // capture format changes (e.g. the initial dummy frame is BGRA8 but real capture is FP16).
    dxgi::texture2d_t inter_tex;
    int cap_w = 0, cap_h = 0;
    DXGI_FORMAT cap_fmt = DXGI_FORMAT_UNKNOWN;
    pyrowave_image imp_img = nullptr;
    VkImageView imp_view = VK_NULL_HANDLE;
    bool first_convert = true;

    // Capture textures opened on the bridge device, cached per image id (the capture image pool is
    // small and reused round-robin; a texture changes when an image goes from dummy to real).
    struct src_tex_t {
      dxgi::texture2d_t::const_pointer capture_texture_p = nullptr;  // identity only, never dereferenced
      dxgi::texture2d_t tex;
      dxgi::keyed_mutex_t mutex;
    };

    std::map<uint32_t, src_tex_t> src_cache;

    // GPU encode resources on PyroWave's own VkDevice.
    VkDevice vk_dev = VK_NULL_HANDLE;
    VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
    VkQueue vk_queue = VK_NULL_HANDLE;
    uint32_t vk_family = 0;
    VkCommandPool vk_pool = VK_NULL_HANDLE;
    VkCommandBuffer vk_cmd = VK_NULL_HANDLE;
    VkFence vk_fence = VK_NULL_HANDLE;
    plane_image_t plane_y, plane_cb, plane_cr;

    // RGB->YUV compute-convert pipeline.
    VkShaderModule conv_shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout conv_dsl = VK_NULL_HANDLE;
    VkPipelineLayout conv_pl = VK_NULL_HANDLE;
    VkPipeline conv_pipe = VK_NULL_HANDLE;
    VkSampler conv_sampler = VK_NULL_HANDLE;
    VkDescriptorPool conv_pool = VK_NULL_HANDLE;
    VkDescriptorSet conv_set = VK_NULL_HANDLE;

    struct convert_pc_t {
      int32_t dst[2];
      int32_t scaled[2];
      int32_t offset[2];
      int32_t flip;
      int32_t chroma444;
      int32_t hdr;
      int32_t scrgb;  // 1 => FP16 scRGB capture: shader encodes linear -> sRGB (SDR) / PQ (HDR)
    };

    ~impl_t() {
      if (enc) {
        pyrowave_encoder_destroy(enc);
      }
      // Destroy our GPU resources before the pyrowave device (they live on its VkDevice).
      if (vk_dev) {
        vkDeviceWaitIdle(vk_dev);
        destroy_import();
        plane_y.destroy();
        plane_cb.destroy();
        plane_cr.destroy();
        if (conv_pipe) {
          vkDestroyPipeline(vk_dev, conv_pipe, nullptr);
        }
        if (conv_pl) {
          vkDestroyPipelineLayout(vk_dev, conv_pl, nullptr);
        }
        if (conv_dsl) {
          vkDestroyDescriptorSetLayout(vk_dev, conv_dsl, nullptr);
        }
        if (conv_pool) {
          vkDestroyDescriptorPool(vk_dev, conv_pool, nullptr);
        }
        if (conv_sampler) {
          vkDestroySampler(vk_dev, conv_sampler, nullptr);
        }
        if (conv_shader) {
          vkDestroyShaderModule(vk_dev, conv_shader, nullptr);
        }
        if (vk_fence) {
          vkDestroyFence(vk_dev, vk_fence, nullptr);
        }
        if (vk_pool) {
          vkDestroyCommandPool(vk_dev, vk_pool, nullptr);
        }
      }
      if (pdev) {
        pyrowave_device_destroy(pdev);
      }
      if (fence_event) {
        CloseHandle(fence_event);
      }
    }

    void destroy_import() {
      if (imp_view) {
        vkDestroyImageView(vk_dev, imp_view, nullptr);
        imp_view = VK_NULL_HANDLE;
      }
      if (imp_img) {
        pyrowave_image_destroy(imp_img);
        imp_img = nullptr;
      }
    }

    // One-time init from the first captured frame: bridge D3D11 device on the capture adapter,
    // matching PyroWave Vulkan device (by adapter LUID), encoder, and convert pipeline.
    bool init_gpu(const dxgi::img_d3d_t &img) {
      // The capture texture belongs to the display's D3D11 device; use its adapter.
      // (COM methods are never const-qualified; GetDevice does not mutate the texture.)
      dxgi::device_t cap_dev;
      const_cast<ID3D11Texture2D *>(img.capture_texture.get())->GetDevice(&cap_dev);
      dxgi::dxgi_t dxgi_dev;
      auto status = cap_dev->QueryInterface(__uuidof(IDXGIDevice), (void **) &dxgi_dev);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to query IDXGIDevice [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }
      adapter_base_t adapter;
      status = dxgi_dev->GetAdapter(&adapter);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to get capture adapter [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }
      DXGI_ADAPTER_DESC adapter_desc;
      adapter->GetDesc(&adapter_desc);

      D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      status = D3D11CreateDevice(
        adapter.get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        dxgi::D3D11_CREATE_DEVICE_FLAGS,
        feature_levels,
        2,
        D3D11_SDK_VERSION,
        &d3d_dev,
        nullptr,
        &d3d_ctx
      );
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create bridge D3D11 device [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }

      // D3D11.4 fence for CPU-visible completion of the capture->intermediate copy.
      if (FAILED(d3d_dev->QueryInterface(__uuidof(ID3D11Device5), (void **) &d3d_dev5)) ||
          FAILED(d3d_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **) &d3d_ctx4))) {
        BOOST_LOG(error) << "PyroWave: D3D11.4 (fences) is not supported on this system";
        return false;
      }
      status = d3d_dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **) &d3d_fence);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create D3D11 fence [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }
      fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!fence_event) {
        return false;
      }

      // Create PyroWave's Vulkan device on the same GPU as the capture adapter.
      pyrowave_luid luid {};
      static_assert(sizeof(adapter_desc.AdapterLuid) == sizeof(luid.luid), "LUID size mismatch");
      memcpy(luid.luid, &adapter_desc.AdapterLuid, sizeof(luid.luid));
      if (pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &luid, &pdev) != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: no Vulkan device matching the capture adapter LUID";
        return false;
      }

      pyrowave_encoder_create_info eci {};
      eci.device = pdev;
      eci.width = width;
      eci.height = height;
      eci.chroma = yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      if (pyrowave_encoder_create(&eci, &enc) != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: pyrowave_encoder_create failed";
        return false;
      }

      pyrowave_device_get_vk_device_handles(pdev, nullptr, &vk_phys, &vk_dev);
      if (!vk_dev || !vk_phys) {
        return false;
      }
      if (!find_compute_queue_family(vk_phys, vk_family)) {
        return false;
      }
      vkGetDeviceQueue(vk_dev, vk_family, 0, &vk_queue);
      if (!vk_queue) {
        return false;
      }

      VkCommandPoolCreateInfo pci {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pci.queueFamilyIndex = vk_family;
      pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      if (vkCreateCommandPool(vk_dev, &pci, nullptr, &vk_pool) != VK_SUCCESS) {
        return false;
      }
      VkCommandBufferAllocateInfo cai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = vk_pool;
      cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cai.commandBufferCount = 1;
      if (vkAllocateCommandBuffers(vk_dev, &cai, &vk_cmd) != VK_SUCCESS) {
        return false;
      }
      VkFenceCreateInfo fci {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (vkCreateFence(vk_dev, &fci, nullptr, &vk_fence) != VK_SUCCESS) {
        return false;
      }

      int chroma_w = yuv444 ? width : width / 2;
      int chroma_h = yuv444 ? height : height / 2;
      // PyroWave is depth-agnostic (normalized-float wavelet); the plane container depth just
      // has to match what the client decodes into.
      VkFormat plane_fmt = ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
      if (!plane_y.init(vk_dev, vk_phys, width, height, plane_fmt) ||
          !plane_cb.init(vk_dev, vk_phys, chroma_w, chroma_h, plane_fmt) ||
          !plane_cr.init(vk_dev, vk_phys, chroma_w, chroma_h, plane_fmt)) {
        return false;
      }

      return init_convert();
    }

    // Build the RGB->YUV compute pipeline and bind the (persistent) plane storage images.
    bool init_convert() {
      VkSamplerCreateInfo sci {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
      sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      if (vkCreateSampler(vk_dev, &sci, nullptr, &conv_sampler) != VK_SUCCESS) {
        return false;
      }

      VkDescriptorSetLayoutBinding b[4] = {};
      b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      for (int i = 1; i < 4; i++) {
        b[i] = {(uint32_t) i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      }
      VkDescriptorSetLayoutCreateInfo dli {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      dli.bindingCount = 4;
      dli.pBindings = b;
      if (vkCreateDescriptorSetLayout(vk_dev, &dli, nullptr, &conv_dsl) != VK_SUCCESS) {
        return false;
      }

      VkPushConstantRange pcr {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(convert_pc_t)};
      VkPipelineLayoutCreateInfo pli {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pli.setLayoutCount = 1;
      pli.pSetLayouts = &conv_dsl;
      pli.pushConstantRangeCount = 1;
      pli.pPushConstantRanges = &pcr;
      if (vkCreatePipelineLayout(vk_dev, &pli, nullptr, &conv_pl) != VK_SUCCESS) {
        return false;
      }

      VkShaderModuleCreateInfo smi {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      smi.codeSize = sizeof(rgb2yuv_spv);
      smi.pCode = rgb2yuv_spv;
      if (vkCreateShaderModule(vk_dev, &smi, nullptr, &conv_shader) != VK_SUCCESS) {
        return false;
      }

      VkComputePipelineCreateInfo cpi {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module = conv_shader;
      cpi.stage.pName = "main";
      cpi.layout = conv_pl;
      if (vkCreateComputePipelines(vk_dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &conv_pipe) != VK_SUCCESS) {
        return false;
      }

      VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
      VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dpi.maxSets = 1;
      dpi.poolSizeCount = 2;
      dpi.pPoolSizes = ps;
      if (vkCreateDescriptorPool(vk_dev, &dpi, nullptr, &conv_pool) != VK_SUCCESS) {
        return false;
      }
      VkDescriptorSetAllocateInfo dsa {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dsa.descriptorPool = conv_pool;
      dsa.descriptorSetCount = 1;
      dsa.pSetLayouts = &conv_dsl;
      if (vkAllocateDescriptorSets(vk_dev, &dsa, &conv_set) != VK_SUCCESS) {
        return false;
      }

      // Bind the persistent plane storage images once (binding 0 / RGB is bound when the
      // intermediate texture is imported).
      VkDescriptorImageInfo si1 {VK_NULL_HANDLE, plane_y.view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo si2 {VK_NULL_HANDLE, plane_cb.view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo si3 {VK_NULL_HANDLE, plane_cr.view, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w[3] = {};
      w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[0].dstSet = conv_set;
      w[0].dstBinding = 1;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[0].pImageInfo = &si1;
      w[1] = w[0];
      w[1].dstBinding = 2;
      w[1].pImageInfo = &si2;
      w[2] = w[0];
      w[2].dstBinding = 3;
      w[2].pImageInfo = &si3;
      vkUpdateDescriptorSets(vk_dev, 3, w, 0, nullptr);
      return true;
    }

    // (Re)create the shareable intermediate texture and its Vulkan import to match the captured
    // image. Runs once up front and again if the capture format changes (dummy BGRA8 -> real HDR).
    bool ensure_intermediate(const dxgi::img_d3d_t &img) {
      if (inter_tex && img.format == cap_fmt && img.width == cap_w && img.height == cap_h) {
        return true;
      }

      VkFormat vk_format = dxgi_to_vk_format(img.format);
      if (vk_format == VK_FORMAT_UNDEFINED) {
        BOOST_LOG(error) << "PyroWave: unsupported capture format " << img.format;
        return false;
      }

      // All prior Vulkan work is fence-waited, so the old import can be destroyed safely.
      destroy_import();
      inter_tex.reset();

      D3D11_TEXTURE2D_DESC t {};
      t.Width = img.width;
      t.Height = img.height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = img.format;
      t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      // NT-handle sharing without a keyed mutex (SHARED | SHARED_NTHANDLE), matching PyroWave's
      // own D3D11 interop test. The NT handle path is the one PyroWave's NVIDIA workarounds
      // cover; legacy KMT sharing crashed in the driver with FP16 (HDR) capture formats.
      t.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      auto status = d3d_dev->CreateTexture2D(&t, nullptr, &inter_tex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create intermediate texture [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }

      dxgi::resource1_t resource;
      status = inter_tex->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
      if (FAILED(status)) {
        return false;
      }
      HANDLE nt_handle = nullptr;
      status = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &nt_handle);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create intermediate share handle [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }

      VkImageCreateInfo ici {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ici.imageType = VK_IMAGE_TYPE_2D;
      ici.format = vk_format;
      ici.extent = {(uint32_t) img.width, (uint32_t) img.height, 1};
      ici.mipLevels = 1;
      ici.arrayLayers = 1;
      ici.samples = VK_SAMPLE_COUNT_1_BIT;
      ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      pyrowave_image_create_info pici {};
      pici.device = pdev;
      pici.external_handle = (pyrowave_os_handle) nt_handle;
      pici.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
      pici.image_create_info = &ici;
      // PyroWave takes ownership of the NT handle and closes it on import. Do not close it here
      // even on failure: the import path may already have consumed it.
      if (pyrowave_image_create(&pici, &imp_img) != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: failed to import intermediate texture into Vulkan (format " << img.format << ')';
        return false;
      }

      VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = pyrowave_image_get_handle(imp_img);
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = vk_format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(vk_dev, &vi, nullptr, &imp_view) != VK_SUCCESS) {
        destroy_import();
        return false;
      }

      // Bind the RGB sampler input once; it stays valid until the intermediate is recreated.
      VkDescriptorImageInfo rgb {conv_sampler, imp_view, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = conv_set;
      w.dstBinding = 0;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &rgb;
      vkUpdateDescriptorSets(vk_dev, 1, &w, 0, nullptr);

      cap_w = img.width;
      cap_h = img.height;
      cap_fmt = img.format;
      first_convert = true;
      BOOST_LOG(info) << "PyroWave: intermediate texture ready " << cap_w << 'x' << cap_h << " format " << cap_fmt;
      return true;
    }

    // Copy the shared capture texture into the intermediate on the bridge device, holding the
    // keyed mutex around the copy, then CPU-wait so the Vulkan reads below are ordered after it.
    bool copy_capture(const dxgi::img_d3d_t &img) {
      auto &src = src_cache[img.id];
      if (!src.tex || src.capture_texture_p != img.capture_texture.get()) {
        // Not opened yet, or the image switched textures (dummy -> real capture).
        src = {};
        dxgi::device1_t dev1;
        auto status = d3d_dev->QueryInterface(__uuidof(ID3D11Device1), (void **) &dev1);
        if (FAILED(status)) {
          return false;
        }
        status = dev1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &src.tex);
        if (FAILED(status)) {
          BOOST_LOG(error) << "PyroWave: failed to open shared capture texture [0x" << util::hex(status).to_string_view() << ']';
          return false;
        }
        status = src.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &src.mutex);
        if (FAILED(status)) {
          src = {};
          return false;
        }
        src.capture_texture_p = img.capture_texture.get();
      }

      auto status = src.mutex->AcquireSync(0, INFINITE);
      if (status != S_OK) {
        BOOST_LOG(error) << "PyroWave: failed to acquire capture texture mutex [0x" << util::hex(status).to_string_view() << ']';
        return false;
      }
      d3d_ctx->CopyResource(inter_tex.get(), src.tex.get());
      src.mutex->ReleaseSync(0);

      ++fence_value;
      d3d_ctx4->Signal(d3d_fence.get(), fence_value);
      d3d_ctx->Flush();
      if (FAILED(d3d_fence->SetEventOnCompletion(fence_value, fence_event))) {
        return false;
      }
      WaitForSingleObject(fence_event, INFINITE);
      return true;
    }

    // Convert RGB->YUV into the plane images (GPU), leaving them GENERAL.
    bool convert() {
      // Aspect-preserving fit + even-aligned letterbox.
      float scalar = std::min((float) width / cap_w, (float) height / cap_h);
      int scaled_w = ((int) (cap_w * scalar)) & ~1;
      int scaled_h = ((int) (cap_h * scalar)) & ~1;
      convert_pc_t pc {};
      pc.dst[0] = width;
      pc.dst[1] = height;
      pc.scaled[0] = scaled_w > 0 ? scaled_w : 2;
      pc.scaled[1] = scaled_h > 0 ? scaled_h : 2;
      pc.offset[0] = ((width - pc.scaled[0]) / 2) & ~1;
      pc.offset[1] = ((height - pc.scaled[1]) / 2) & ~1;
      pc.flip = 0;  // D3D11 capture textures are top-down
      pc.chroma444 = yuv444 ? 1 : 0;
      pc.hdr = hdr ? 1 : 0;
      // FP16 captures are scRGB (linear, Rec.709 primaries); the shader encodes them to sRGB
      // (SDR) or BT.2020 PQ (HDR). 8/10-bit captures arrive already display-referred.
      pc.scrgb = (cap_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;

      vkResetCommandBuffer(vk_cmd, 0);
      VkCommandBufferBeginInfo beg {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(vk_cmd, &beg);

      auto img_barrier = [&](VkImage im, VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da,
                             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier mb {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        mb.oldLayout = o;
        mb.newLayout = n;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = im;
        mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        mb.srcAccessMask = sa;
        mb.dstAccessMask = da;
        vkCmdPipelineBarrier(vk_cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &mb);
      };

      // Imported intermediate: external images are assumed GENERAL (see pyrowave.h). The D3D11
      // copy into it finished before this submit (fence CPU wait), so only visibility is needed.
      img_barrier(pyrowave_image_get_handle(imp_img),
                  first_convert ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      first_convert = false;
      img_barrier(plane_y.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      img_barrier(plane_cb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      img_barrier(plane_cr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

      vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, conv_pipe);
      vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, conv_pl, 0, 1, &conv_set, 0, nullptr);
      vkCmdPushConstants(vk_cmd, conv_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
      vkCmdDispatch(vk_cmd, (width + 7) / 8, (height + 7) / 8, 1);

      // Make the plane writes visible; PyroWave's encode reads them (in GENERAL) after our fence.
      img_barrier(plane_y.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      img_barrier(plane_cb.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      img_barrier(plane_cr.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      vkEndCommandBuffer(vk_cmd);

      VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &vk_cmd;
      vkResetFences(vk_dev, 1, &vk_fence);
      auto vr = vkQueueSubmit(vk_queue, 1, &si, vk_fence);
      if (vr != VK_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: convert submit failed (" << vr << ')';
        return false;
      }
      vr = vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, UINT64_MAX);
      if (vr != VK_SUCCESS) {
        // Likely VK_ERROR_DEVICE_LOST; do not hand a dead device to the PyroWave encoder.
        BOOST_LOG(error) << "PyroWave: convert fence wait failed (" << vr << ')';
        return false;
      }
      return true;
    }
  };

  std::unique_ptr<encoder_t> encoder_t::create(int width, int height, int bitrate_kbps, int frame_rate, bool yuv444, bool ten_bit, bool hdr) {
    // 4:2:0 requires even dimensions (harmless for 4:4:4; keeps the letterbox math shared).
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
      return nullptr;
    }

    auto self = std::unique_ptr<encoder_t>(new encoder_t());
    self->impl = std::make_unique<impl_t>();
    auto &impl = *self->impl;

    impl.width = width;
    impl.height = height;
    impl.yuv444 = yuv444;
    impl.ten_bit = ten_bit;
    impl.hdr = hdr && ten_bit;  // HDR color math only makes sense in 10-bit containers
    impl.level_block_ends = compute_level_block_ends(width, height, yuv444);

    // Per-frame byte budget from bitrate. Intra-only: bitrate / fps bytes per frame.
    if (frame_rate <= 0) {
      frame_rate = 60;
    }
    int64_t bits_per_frame = (int64_t) bitrate_kbps * 1000 / frame_rate;
    impl.max_bitstream = (size_t) (bits_per_frame / 8);
    if (impl.max_bitstream < 4096) {
      impl.max_bitstream = 4096;
    }

    // GPU resources (bridge D3D11 device, PyroWave Vulkan device, encoder) are created on the
    // first encode() call, when the capture adapter is known from the captured image.
    BOOST_LOG(info) << "PyroWave encoder session: " << width << "x" << height
                    << (yuv444 ? " 4:4:4" : " 4:2:0")
                    << (ten_bit ? (impl.hdr ? " 10-bit HDR" : " 10-bit SDR") : " 8-bit")
                    << " budget " << impl.max_bitstream << " bytes/frame";
    return self;
  }

  int encoder_t::encode(const platf::img_t &img, std::vector<uint8_t> &out, size_t &head_bytes) {
    auto &impl = *this->impl;

    // Never leave a previous frame's boundary behind on an early failure return.
    head_bytes = 0;

    if (impl.state == impl_t::state_e::failed) {
      return -1;
    }

    // PyroWave requires a D3D11 (VRAM) capture, i.e. a hardware-encoder display backend. RAM
    // (software-capture) images are not supported.
    auto d3d_img = dynamic_cast<const dxgi::img_d3d_t *>(&img);
    if (!d3d_img || !d3d_img->capture_texture) {
      BOOST_LOG(error) << "PyroWave requires a D3D11 (VRAM) capture";
      return -1;
    }

    if (impl.state == impl_t::state_e::pending) {
      if (!impl.init_gpu(*d3d_img)) {
        BOOST_LOG(error) << "PyroWave: GPU encode resource init failed";
        impl.state = impl_t::state_e::failed;
        return -1;
      }
      impl.state = impl_t::state_e::ready;
      BOOST_LOG(info) << "PyroWave encoder ready (GPU): " << impl.width << 'x' << impl.height
                      << (impl.yuv444 ? " 4:4:4" : " 4:2:0")
                      << (impl.ten_bit ? (impl.hdr ? " 10-bit HDR" : " 10-bit SDR") : " 8-bit");
    }

    if (!impl.ensure_intermediate(*d3d_img)) {
      impl.state = impl_t::state_e::failed;
      return -1;
    }
    if (!impl.copy_capture(*d3d_img)) {
      BOOST_LOG(error) << "PyroWave: capture texture copy failed";
      return -1;
    }
    if (!impl.convert()) {
      // Vulkan-level failure (submit or fence): the device is almost certainly lost, so stop the
      // session instead of feeding a dead device to the PyroWave encoder every frame.
      BOOST_LOG(error) << "PyroWave: RGB->YUV convert failed";
      impl.state = impl_t::state_e::failed;
      return -1;
    }

    pyrowave_gpu_buffers gb {};
    gb.planes[0] = impl.plane_y.image_view();
    gb.planes[1] = impl.plane_cb.image_view();
    gb.planes[2] = impl.plane_cr.image_view();

    pyrowave_rate_control rc {};
    rc.maximum_bitstream_size = impl.max_bitstream;

    if (pyrowave_encoder_encode_gpu_synchronous(impl.enc, nullptr, nullptr, &gb, &rc) != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: encode_gpu_synchronous failed";
      return -1;
    }

    // PyroWave splits a frame into several independently-decodable packets, each of which must be
    // pushed separately on the decode side. Sunshine's RTP layer, however, ships one opaque payload
    // per frame and reassembles it as a single buffer. So we frame the PyroWave packets ourselves:
    //
    //   [u32 packet_count] { [u32 size] [size bytes] } * packet_count
    //
    // The Moonlight PyroWave decoder parses this framing and re-pushes each PyroWave packet.
    const size_t packet_boundary = 1024;
    size_t num_packets = 0;
    if (pyrowave_encoder_compute_num_packets(impl.enc, packet_boundary, &num_packets) != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: compute_num_packets failed";
      return -1;
    }

    // Size the bitstream buffer for the worst case, NOT num_packets * packet_boundary:
    // pyrowave's packetize() only closes a packet after the block that overflows it, so a packet
    // can exceed packet_boundary by one block (up to 4097 words; payload_words is a 12-bit
    // field), and packetize() does not bounds-check the output buffer in release builds.
    // High-entropy frames (e.g. full-range 10-bit HDR) actually hit this. Buffers are reused
    // across frames (grow-only).
    const size_t max_block_bytes = 64 * 1024;  // hard cap is ~16.4 KiB/block; 4x margin
    size_t scratch_bound = 4096 + num_packets * (packet_boundary + max_block_bytes);
    if (impl.scratch.size() < scratch_bound) {
      impl.scratch.resize(scratch_bound);
    }
    if (impl.packets.size() < num_packets) {
      impl.packets.resize(num_packets);
    }
    size_t out_packets = 0;
    if (pyrowave_encoder_packetize(impl.enc, impl.packets.data(), packet_boundary,
                                   &out_packets, impl.scratch.data(), impl.scratch.size()) != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: packetize failed";
      return -1;
    }

    // XXX write header fields, this starts at impl.scratch.data()
    /*
    struct BitstreamSequenceHeader
    {
      uint32_t width_minus_1 : 14;
      uint32_t height_minus_1 : 14;
      uint32_t sequence : 3;
      uint32_t extended : 1;
      uint32_t total_blocks : 24;
      uint32_t code : 2;
      uint32_t chroma_resolution : 1;
      uint32_t color_primaries : 1;
      uint32_t transfer_function : 1;
      uint32_t ycbcr_transform : 1;
      uint32_t ycbcr_range : 1;
      uint32_t chroma_siting : 1;
    };
    */

    auto put_u32 = [&out](uint32_t v) {
      out.push_back((uint8_t) (v & 0xff));
      out.push_back((uint8_t) ((v >> 8) & 0xff));
      out.push_back((uint8_t) ((v >> 16) & 0xff));
      out.push_back((uint8_t) ((v >> 24) & 0xff));
    };

    // Block index of the first block in a packet. Each block starts with an 8-byte
    // BitstreamHeader whose trailing u32 is { quant_code : 8, block_index : 24 }.
    auto first_block_index = [&impl](size_t i) {
      const uint8_t *src = impl.scratch.data() + impl.packets[i].offset;
      uint32_t w = (uint32_t) src[4] | ((uint32_t) src[5] << 8) | ((uint32_t) src[6] << 16) | ((uint32_t) src[7] << 24);
      return w >> 8;
    };
    const uint32_t head_block_end = impl.level_block_ends[FEC_PROTECTED_THROUGH_LEVEL];

    out.clear();
    put_u32((uint32_t) out_packets);
    for (size_t i = 0; i < out_packets; i++) {
      // The protected head closes at the first packet that *starts* past the last protected
      // level — a packet holds several blocks and may straddle the level boundary, in which
      // case it stays whole in the head at the cost of a few blocks of overshoot.
      //
      // Packet 0 is skipped because it leads with the BitstreamSequenceHeader rather than a
      // BitstreamHeader; it is always part of the head anyway.
      if (head_bytes == 0 && i > 0 && first_block_index(i) >= head_block_end) {
        head_bytes = out.size();
      }

      put_u32((uint32_t) impl.packets[i].size);
      const uint8_t *src = impl.scratch.data() + impl.packets[i].offset;
      out.insert(out.end(), src, src + impl.packets[i].size);
    }
    return 0;
  }

  encoder_t::~encoder_t() = default;

}  // namespace platf::pyrowave

#endif  // SUNSHINE_ENABLE_PYROWAVE
