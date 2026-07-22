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

生成器只使用 Python 标准库。默认输出 1440×1080、BT.709 limited-range 的 YUY2
彩条和灰度渐变，文件中只有一张紧密排列的图像帧。

```bash
python3 tools/generate_yuv422.py \
  --output test_1440x1080_yuy2.yuv \
  --width 1440 \
  --height 1080 \
  --format yuy2
```

正常情况下文件大小为：

```text
width × height × 2
1440 × 1080 × 2 = 3,110,400 bytes
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
  --input test_1440x1080_yuy2.yuv \
  --width 1440 \
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
- 整个测试进程的 CPU 占用和累计 CPU 时间，其中 100% 表示占满一个核心；
- packed YUV422 原始帧大小、JPEG 平均大小、原始/JPEG 压缩比和体积减少百分比；
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

## Jetson 实测结果

2026-07-22 在 Jetson AGX Orin（aarch64、GStreamer 1.20）上完成编译和硬件管线
验证，测试目录为 `/home/gray/data/use_nvjpeg_test`。

主测试直接读取 1440×1080 packed YUY2 文件，没有 PNG 或 RGB 中间文件。原始帧大小
严格按 `1440 × 1080 × 2 = 3,110,400 bytes` 计算。

| Quality | 原始 YUV422 | 平均 JPEG | 压缩比 | 30 FPS 码流 |
|---:|---:|---:|---:|---:|
| 85 | 3,110,400 bytes | 46,109 bytes | 67.458:1 | 11.066 Mbit/s |
| 70 | 3,110,400 bytes | 37,424 bytes | 83.112:1 | 8.982 Mbit/s |
| 50 | 3,110,400 bytes | 34,523 bytes | 90.096:1 | 8.286 Mbit/s |
| 30 | 3,110,400 bytes | 32,218 bytes | 96.542:1 | 7.732 Mbit/s |
| 10 | 3,110,400 bytes | 28,053 bytes | 110.876:1 | 6.733 Mbit/s |
| 1/0 | 3,110,400 bytes | 26,533 bytes | 117.228:1 | 6.368 Mbit/s |

Quality 85 的 300 帧正式测试得到 166.834 FPS、28.346% 进程 CPU 和 64.801 ms
平均端到端延迟，`nvjpegenc Enableperf` 的稳定单帧编码耗时约 2.7 ms。生成的 JPEG
为 1440×1080 baseline JPEG，本机复核为 `yuvj420p`，采样因子
`2x2,1x1,1x1`，即 JPEG 4:2:0。

若“4 mb”指 4 Mbit/s，在 30 FPS 下要求每张 JPEG 平均不超过约 16,667 bytes。
当前 `nvjpegenc` 即使 quality 设为 0/1，测试图仍为 26,533 bytes，因此保持
1440×1080、30 FPS 时仅调 quality 无法达到 4 Mbit/s。quality 1 配合约 19 FPS
时为约 4.03 Mbit/s；若必须保持 30 FPS，需要继续降低分辨率或改用视频编码器。

进程 CPU 由计时区间内的用户态与内核态 CPU 时间除以墙钟时间得到，覆盖进程内所有
GStreamer 线程，但不包含 VIC/JPEG 固定功能硬件的工作量。配套 `tegrastats` 采样中
`GR3D_FREQ` 为 0～1%，说明流程没有明显占用 CUDA GPU。以上数据用于验证链路可行性，
并非经过隔离负载、锁频和多轮统计的正式性能基准。生成的彩条/渐变虽然是原生 YUV422，
但内容比真实相机画面简单；实际 JPEG 大小仍应使用相机原始 YUV422 帧复测。

## 海康 MVS 相机 YUV422 输入码率

海康 MVS 中 `YUV422Packed` 和 `YUV 422 (YUYV) Packed` 的像素大小为
16 bit/pixel，即每像素 2 bytes。对于当前 1440×1080 配置，一张无行填充的原始帧为：

```text
1440 × 1080 × 2 = 3,110,400 bytes ≈ 3.11 MB
```

纯图像有效载荷与帧率的关系为：

```text
bytes_per_second = width × height × 2 × fps
bits_per_second  = bytes_per_second × 8
```

| 帧率 | 原始数据量 | 纯图像码率 |
|---:|---:|---:|
| 10 FPS | 31.104 MB/s | 248.832 Mbit/s |
| 15 FPS | 46.656 MB/s | 373.248 Mbit/s |
| 20 FPS | 62.208 MB/s | 497.664 Mbit/s |
| 25 FPS | 77.760 MB/s | 622.080 Mbit/s |
| 30 FPS | 93.312 MB/s | 746.496 Mbit/s |
| 40 FPS | 124.416 MB/s | 995.328 Mbit/s |
| 65.8 FPS | 204.664 MB/s | 1.637 Gbit/s |

因此，1440×1080、30 FPS、packed YUV422 的相机原始有效载荷约为
746.5 Mbit/s。GigE/GVSP、以太网包头、帧间隔及重传会产生额外开销，工程上可估算
网卡实际占用约 0.76～0.80 Gbit/s，并建议启用 Jumbo Frame。40 FPS 的纯图像载荷
已经接近千兆网理论上限，无法再为协议开销留出足够空间。

海康 1440×1080 GigE 彩色相机产品可能标注 65.8 FPS，但官方说明彩色相机最大帧率
通常以 Bayer8 像素格式为条件。YUV422 的每像素数据量是 Bayer8 的两倍，因此在
1 GigE 上不能以全分辨率持续传输 65.8 FPS YUV422。

与本项目 quality 85 的测试图结果对比：

```text
相机 YUV422 输入：746.496 Mbit/s（1440×1080，30 FPS）
JPEG 输出估算：   11.066 Mbit/s（quality 85，测试图）
原始/JPEG 压缩比：67.458:1
```

这里的 JPEG 结果来自彩条/渐变测试帧。真实相机画面的纹理、噪声和运动内容更多，
实际 JPEG 大小和输出码率通常更高，应采集真实 YUV422 帧后重新测量。

参考资料：

- [海康 GigE 面阵相机用户手册：像素格式和像素大小](https://www.hikrobotics.com/en2/source/vision/document/2023/12/19/UD34944B_GigE%20Area%20Scan%20Camera%20User%20Manual_V4.0.0_20231214.pdf)
- [海康 CU 系列 1440×1080 相机产品页](https://www.hikrobotics.com/en/machinevision/visionproduct/?id=168&typeId=78)
- [海康 GigE 相机最大帧率设置说明](https://www.hikrobotics.com/en2/source/vision/video/2022/12/19/Standard%20Camera_How%20to%20reach%20the%20Max.%20frame%20rates%20of%20GigE%20camera_22.12.17.pdf)

## 开发机限制

仓库当前所在的 x86_64 主机有 GStreamer 运行时和 Python，但没有 GStreamer 开发
包，也没有 Jetson 的 `nvvidconv`/`nvjpegenc`。因此本机只能生成和检查 YUV 文件；
C++ 编译及硬件管线已经在上述目标 Jetson 上验证通过。
