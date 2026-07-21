# Jetson YUV422 → JPEG 验证程序

本项目用于验证文档中推荐的数据路径：

```text
普通内存 packed YUV422
→ appsrc
→ nvvidconv（VIC）
→ NVMM NV12
→ nvjpegenc
→ appsink
```

详细背景见 [Jetson AGX Orin 网络 YUV422 转 JPEG 实施报告](docs/jetson_orin_nvjpeg_network_yuv422_report.md)。

## 生成测试帧

生成器只使用 Python 标准库。默认输出 1920×1080、BT.709 limited-range 的 YUY2
彩条和灰度渐变，文件中只有一张紧密排列的图像帧。

```bash
python3 tools/generate_yuv422.py \
  --output test_1920x1080_yuy2.yuv \
  --width 1920 \
  --height 1080 \
  --format yuy2
```

正常情况下文件大小为：

```text
width × height × 2
1920 × 1080 × 2 = 4,147,200 bytes
```

`--format uyvy` 可生成字节顺序为 `U0 Y0 V0 Y1` 的测试帧。宽高必须都是偶数，
以满足 packed YUV422 和后续 NV12 转换要求。

## 在 Jetson 上构建

测试程序只链接标准 GStreamer API；`nvvidconv` 和 `nvjpegenc` 在运行时加载。
需要安装 C++ 编译器、CMake、pkg-config，以及 GStreamer core/app/video 开发包。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建过程使用以下 pkg-config 模块：

- `gstreamer-1.0`
- `gstreamer-app-1.0`
- `gstreamer-video-1.0`

程序启动时会检查 `nvvidconv` 和 `nvjpegenc`。本项目不提供软件编码回退，缺少
任一 Jetson 插件都会返回错误，避免把软件测试误认为硬件路径验证。

## 运行硬件管线

```bash
./build/nvjpeg_yuv422_bench \
  --input test_1920x1080_yuy2.yuv \
  --width 1920 \
  --height 1080 \
  --format YUY2 \
  --fps 30 \
  --quality 85 \
  --warmup 30 \
  --frames 300 \
  --output result.jpg
```

输入文件在启动时一次性读入一个模板 `GstBuffer`。预热结束后，程序重复提交共享
同一只读图像内存的 buffer，不在计时区间内读取磁盘或复制整张图像。队列和
`appsink` 均禁用丢帧；程序只有在收到全部输出后才报告成功。

输出统计包括：

- appsrc 到 appsink 的总吞吐 FPS；
- JPEG 平均大小；
- 每帧端到端延迟的平均值、P50、P95 和 P99；
- Jetson `nvjpegenc Enableperf=true` 输出的编码器性能信息。

第一张计时帧会保存到 `--output` 指定的位置，并检查 JPEG SOI/EOI 标记。

可进一步检查输出尺寸和色度采样：

```bash
ffprobe -hide_banner result.jpg
```

预期图像尺寸与输入一致，JPEG 像素格式为 4:2:0。YUY2 和 UYVY 必须使用匹配的
`--format`，否则会出现明显偏色。

## 当前环境限制

仓库当前所在的 x86_64 主机有 GStreamer 运行时和 Python，但没有 GStreamer 开发
包，也没有 Jetson 的 `nvvidconv`/`nvjpegenc`。因此本机可以生成和检查 YUV 文件，
但最终编译及硬件管线验证需要在目标 Jetson 上完成。
