---
name: verify
description: Verify Sunshine changes end-to-end by streaming to the local Moonlight client (loopback, RX 9070 XT box)
---

# Verify via loopback stream to local Moonlight

Build: `cmake --build build -j` (already configured with -DSUNSHINE_ENABLE_PYROWAVE=ON;
full recipe in the pyrowave memory). Run `./build/sunshine` in background under the live
KDE Wayland session; config `~/.config/sunshine/sunshine.conf` (capture=kwin,
encoder=vulkan, pyrowave_mode=2); log `~/.config/sunshine/sunshine.log`.

Drive the paired local client headlessly:

```bash
timeout 25 /home/azr/src/moonlight-qt/app/moonlight stream localhost Test \
  --video-codec PyroWave --yuv444 --performance-overlay --1080 --bitrate 150000 > /tmp/ml.log 2>&1
```

- **Stream the app named "Test", not "Desktop"** — Desktop launch fails on this host
  ("Property tree is empty" → client sees Malformed XML).
- Evidence: host log `PyroWave encoder ready (GPU): WxH 4:x:x`; client log
  `Video stream is ... (format 0xNN)` (0x10=PyroWave 420, 0x20=PyroWave 444).
- Codec advertisement: `curl -s http://localhost:47989/serverinfo` → `ServerCodecModeSupport`.
- Screenshot mid-stream: `spectacle -b -n -o out.png`.
- PyroWave shader edit? Regenerate the committed header:
  `glslc --target-env=vulkan1.3 -fshader-stage=comp -mfmt=c src_assets/linux/assets/shaders/vulkan/pyrowave_rgb2yuv.comp -o src/platform/linux/pyrowave_rgb2yuv.spv.h`
