# Jetson AGX Orin 网络 YUV422 转 JPEG 实施报告

## 1. 报告目的

本文记录目标 Jetson AGX Orin 的 JPEG 编解码能力，并给出将网络相机接收到的 YUV422 图像高效编码为 JPEG 的推荐方案。

目标数据路径为：

```text
网络相机 YUV422
    ↓
普通内存中的完整图像帧
    ↓ appsrc
VIC：YUV422 → NV12，并搬运至 NVMM
    ↓
nvjpegenc：NV12 → JPEG 4:2:0
    ↓
appsink：取得 JPEG 数据
```

## 2. 已确认的设备与软件环境

通过 SSH 对目标设备进行了只读检查，结果如下：

| 项目 | 检查结果 |
|---|---|
| 设备 | NVIDIA Jetson AGX Orin Developer Kit |
| 模组 | Jetson AGX Orin 64GB |
| 模组编号 | P3701-0005 |
| JetPack | 6.0 |
| Jetson Linux / L4T | 36.3.0 |
| CUDA SDK | 12.6.11 |
| 当前功耗模式 | MAXN |

需要注意，设备上的 `nvidia-l4t-jetson-multimedia-api` 头文件/示例包为 36.4，而运行时 Multimedia 库为 36.3。本文涉及的格式能力在两个版本中一致，但正式构建和部署时建议将头文件、示例与运行库统一到相同版本。

## 3. 名称与组件说明

### 3.1 Jetson NvJPEG 与 CUDA nvJPEG

Jetson 环境中容易混淆两套名称相近的接口：

1. Jetson Multimedia API 的 `NvJPEGEncoder`、`NvJPEGDecoder` 和 GStreamer `nvjpegenc`、`nvjpegdec`。
2. CUDA Toolkit 的 GPU 加速 `nvJPEG` 库及 `nvjpegEncodeYUV()` 等接口。

本文推荐使用第一种，即 Jetson Multimedia API/GStreamer 的固定功能 JPEG 编码路径。它适合 `NV12/I420 → JPEG 4:2:0`，不会占用 CUDA 核心。

当前设备已经安装 Jetson Multimedia 的 `libnvjpeg.so`、`nvjpegenc` 和 `nvjpegdec`，但没有发现 CUDA nvJPEG 的 `nvjpeg.h` 开发头文件。因此，链接时不能仅凭库名判断使用的是哪一套接口。

### 3.2 `nvvidconv`

`nvvidconv` 是 NVIDIA 提供的 GStreamer 图像转换插件。在 Jetson 上默认使用 VIC（Video Image Compositor）固定功能硬件，可以完成：

- YUV 色彩采样与像素格式转换；
- 缩放、裁剪和旋转；
- pitch-linear 与 block-linear 布局转换；
- 普通系统内存与 NVMM 硬件缓冲区之间的传递。

目标设备上的 `compute-hw` 选项为：

| 值 | 执行单元 |
|---|---|
| `0` | 默认；Jetson 上使用 VIC |
| `1` | CUDA GPU |
| `2` | 强制 VIC |

本方案显式设置 `compute-hw=2`，使色彩转换由 VIC 完成，不与 CUDA/TensorRT 推理争用 GPU。

CPU 仍负责网络协议栈、GStreamer 调度、buffer 管理及提交硬件任务，但不进行逐像素的 YUV422→NV12 计算。

### 3.3 `appsrc`

`appsrc` 是 GStreamer 官方标准插件，不是自定义或虚拟的程序名称。它用于把应用程序已有的内存数据送入 GStreamer 管线。

```text
appsrc name=net_src
│      └── 实例名称，可由应用自行命名
└───────── GStreamer 元素类型，名称固定
```

`appsrc` 本身不接收网络数据。网络收包、协议解析、分片重组和丢包处理由应用程序完成；应用把一张完整图像包装为一个 `GstBuffer` 后推送给 `appsrc`。

## 4. JPEG 编解码格式能力

### 4.1 解码

Jetson `NvJPEGDecoder` 支持以下 JPEG 色度采样：

- JPEG YUV 4:2:0；
- JPEG YUV 4:2:2；
- JPEG YUV 4:4:4。

设备上的 `nvjpegdec` 声明的 NVMM 输出包括：

- `I420`、`NV12`；
- `YUY2`、`Y42B`；
- `Y444`；
- `GRAY8`；
- `RGB`、`RGBA`。

实测 4:2:2 JPEG 可以成功解码，原生协商输出为 NVMM `Y42B`，即 planar YUV422。

### 4.2 编码

Jetson `NvJPEGEncoder` 的直接输入能力为：

| 接口/内存类型 | 支持输入 |
|---|---|
| `encodeFromFd()` / NVMM | YUV420、NV12 |
| `encodeFromBuffer()` | YUV420 |
| GStreamer `nvjpegenc` / NVMM | I420、NV12 |
| GStreamer `nvjpegenc` / 普通内存 | I420、YV12、GRAY8 |

`YUY2/UYVY/Y42B/NV16` 不能直接送入 Jetson `nvjpegenc` 编码。实测直接连接 `YUY2 → nvjpegenc` 会因 caps 不兼容而失败。

因此，原始输入为 YUV422 时必须先转换为 NV12 或 I420。本项目接受输出 JPEG 4:2:0，所以推荐转换为 NV12。

## 5. 推荐的高效数据路径

```text
┌───────────────────────────────┐
│ 网络协议栈                      │
│ TCP / UDP / 自定义传输协议      │
└───────────────┬───────────────┘
                │ recv/recvmsg/recvmmsg
                ▼
┌───────────────────────────────┐
│ 普通内存 GstBuffer             │
│ 一张完整 YUY2 或 UYVY 图像      │
└───────────────┬───────────────┘
                │ appsrc
                ▼
┌───────────────────────────────┐
│ nvvidconv compute-hw=2        │
│ VIC：YUV422 → NV12            │
│ 普通内存 → NVMM                │
└───────────────┬───────────────┘
                ▼
┌───────────────────────────────┐
│ NVMM NV12                     │
└───────────────┬───────────────┘
                │ nvjpegenc
                ▼
┌───────────────────────────────┐
│ JPEG 4:2:0 GstBuffer          │
└───────────────┬───────────────┘
                │ appsink
                ▼
       存盘、再次发网或业务处理
```

该路径的主要优点：

- 不经过 RGB/BGR 中间格式；
- YUV422→NV12 由 VIC 处理；
- JPEG 编码使用 Jetson JPEG 编码组件；
- 不占用 CUDA 核心；
- 可以把普通内存到 NVMM 的传递和格式转换放在同一个 `nvvidconv` 阶段；
- 如果直接把网络数据接收到 `GstBuffer`，应用层无需额外复制一遍整帧。

## 6. GStreamer 管线

下面的管线适用于应用程序通过 `appsrc` 推入 packed YUV422 图像：

```text
appsrc name=net_src is-live=true block=true format=time
! queue max-size-buffers=3 leaky=downstream
! nvvidconv compute-hw=2
! video/x-raw(memory:NVMM),format=NV12
! nvjpegenc quality=85
! appsink name=jpeg_sink sync=false max-buffers=3 drop=true
```

`appsrc` 需要应用程序主动推送 buffer，因此不能只在命令行启动上述管线后等待数据。通常通过 `gst_parse_launch()` 或逐个创建 GStreamer 元素构建管线。

### 6.1 C/C++ 中创建管线

以下示例以 1920×1080、30 FPS、YUY2 输入为例：

```cpp
GError *error = nullptr;

const char *pipeline_desc =
    "appsrc name=net_src is-live=true block=true format=time "
    "! queue max-size-buffers=3 leaky=downstream "
    "! nvvidconv compute-hw=2 "
    "! video/x-raw(memory:NVMM),format=NV12 "
    "! nvjpegenc quality=85 "
    "! appsink name=jpeg_sink sync=false max-buffers=3 drop=true";

GstElement *pipeline = gst_parse_launch(pipeline_desc, &error);
GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "net_src");
GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg_sink");

GstCaps *caps = gst_caps_new_simple(
    "video/x-raw",
    "format", G_TYPE_STRING, "YUY2",
    "width", G_TYPE_INT, 1920,
    "height", G_TYPE_INT, 1080,
    "framerate", GST_TYPE_FRACTION, 30, 1,
    nullptr);

g_object_set(appsrc, "caps", caps, nullptr);
gst_caps_unref(caps);

gst_element_set_state(pipeline, GST_STATE_PLAYING);
```

如果网络相机使用 UYVY 字节顺序，应把 caps 中的 `YUY2` 改为 `UYVY`。

## 7. 网络数据如何进入 `GstBuffer`

### 7.1 推荐方式：直接接收到 `GstBuffer`

packed YUV422 在没有行填充时，每帧大小为：

```text
frame_size = width × height × 2
```

示例：

```cpp
const size_t frame_size = width * height * 2;

GstBuffer *buffer = gst_buffer_new_allocate(
    nullptr, frame_size, nullptr);

GstMapInfo map;
if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    return false;
}

// 对 TCP 必须循环读取，直到取得完整的一帧。
bool received = recv_full(socket_fd, map.data, frame_size);

gst_buffer_unmap(buffer, &map);

if (!received) {
    gst_buffer_unref(buffer);
    return false;
}

GST_BUFFER_PTS(buffer) = pts;
GST_BUFFER_DURATION(buffer) =
    gst_util_uint64_scale_int(1, GST_SECOND, fps);

GstFlowReturn flow;
g_signal_emit_by_name(appsrc, "push-buffer", buffer, &flow);
gst_buffer_unref(buffer);

return flow == GST_FLOW_OK;
```

该方式让 socket 接收数据直接写入 GStreamer buffer，避免如下低效路径：

```text
socket → 临时数组 → memcpy → GstBuffer
```

示例代码为了清晰逐帧分配 buffer。正式程序应使用预分配 buffer pool，循环复用 3～6 个 buffer，避免频繁申请和释放内存。

### 7.2 包装已有接收内存

如果网络模块已经维护自己的 buffer pool，可用 `gst_buffer_new_wrapped_full()` 包装已有内存：

```cpp
GstBuffer *buffer = gst_buffer_new_wrapped_full(
    GST_MEMORY_FLAG_READONLY,
    data,
    capacity,
    0,
    frame_size,
    buffer_context,
    release_callback);
```

此方式可以避免再复制整帧，但必须遵守 buffer 生命周期：

- 在 `release_callback` 被调用前，接收线程不能覆盖或释放 `data`；
- 推荐使用固定数量的接收 buffer 形成循环池；
- 下游释放 buffer 后，`release_callback` 再把内存归还接收池。

### 7.3 TCP 与 UDP 的处理差异

TCP 是字节流，一次 `recv()` 不保证返回完整帧。应用必须根据协议头中的长度字段或已知固定帧大小循环读取，并处理断流与重新同步。

UDP 应在推送 `appsrc` 前完成：

- 分片重组；
- 帧号和分片序号检查；
- 丢包检测；
- 超时丢弃不完整帧；
- 必要的乱序重排。

一个 `GstBuffer` 应对应一张完整图像，不应将任意网络分片直接当作图像帧推送。

## 8. 输出 JPEG 的获取

`appsink` 输出的每个 `GstBuffer` 是一张完整 JPEG。应用可以通过 `gst_app_sink_pull_sample()` 获取：

```cpp
GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
if (sample != nullptr) {
    GstBuffer *jpeg_buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (gst_buffer_map(jpeg_buffer, &map, GST_MAP_READ)) {
        // map.data 指向 JPEG 数据，map.size 是 JPEG 字节数。
        send_or_store_jpeg(map.data, map.size);
        gst_buffer_unmap(jpeg_buffer, &map);
    }

    gst_sample_unref(sample);
}
```

在 `gst_sample_unref()` 后，不应继续使用 `map.data`。如果发送接口不能在回调期间消费完数据，需要自行持有 buffer/sample 或复制 JPEG 数据。

## 9. 像素格式和步长要求

### 9.1 常见 packed YUV422 字节顺序

| GStreamer 格式 | 两像素字节顺序 |
|---|---|
| `YUY2` | `Y0 U0 Y1 V0` |
| `UYVY` | `U0 Y0 V0 Y1` |
| `YVYU` | `Y0 V0 Y1 U0` |

caps 必须与网络相机实际发送的字节顺序一致，否则图像会出现明显偏色。

### 9.2 Stride

如果网络相机每行数据紧密排列：

```text
stride = width × 2
```

如果协议中每行带有对齐填充，不能简单使用 `width × height × 2`。可选择：

1. 接收时去除每行 padding，形成紧密排列的 buffer；
2. 为 `GstBuffer` 添加正确的 `GstVideoMeta`，描述 plane offset 和 stride，并验证当前 `nvvidconv` 版本能够正确识别该元数据。

为简化实现和降低风险，推荐网络协议发送紧密排列的 YUV422 帧。

YUV422 的宽度必须为偶数；转换到 NV12 时，宽度和高度最好都保持偶数。

## 10. 性能与延迟建议

1. 直接 `recv()` 到映射后的 `GstBuffer`，避免完整帧二次复制。
2. 使用 3～6 个预分配接收 buffer 形成循环池。
3. 使用 `nvvidconv compute-hw=2`，明确选择 VIC。
4. 输出 caps 明确指定 `video/x-raw(memory:NVMM),format=NV12`。
5. 不要在 VIC 转换与 JPEG 编码之间把 buffer map 到 CPU。
6. 不要经过 RGB/BGR 中间格式。
7. 不要逐帧创建和销毁 GStreamer pipeline 或编码器。
8. 对实时预览/推理类业务，可使用 `leaky=downstream` 丢弃积压的旧帧，避免延迟不断增长。
9. 对不允许丢帧的采集任务，移除 `leaky=downstream`，并根据峰值编码时间扩大队列和接收池。
10. 给输入帧设置正确的 PTS 和 duration，便于限速、同步和性能统计。

这条路径不会消除所有 CPU 开销。仍然存在网络协议栈、用户态收包、buffer 管理和 JPEG 数据输出等工作，但 YUV422→NV12 的像素计算由 VIC 完成。

## 11. 根据网络相机传输格式选择入口

### 11.1 自定义协议传输原始 YUV422

使用本文推荐路径：

```text
自定义网络接收代码 → appsrc → nvvidconv → NV12 NVMM → nvjpegenc
```

### 11.2 RTP Raw Video（例如 RFC 4175）

优先使用 GStreamer 的标准网络组件：

```text
udpsrc → rtpvrawdepay → nvvidconv → NV12 NVMM → nvjpegenc
```

此时通常不需要应用自行实现 `appsrc` 和 RTP 分片重组。

### 11.3 相机传输 MJPEG

如果相机已经输出所需质量和尺寸的 JPEG/MJPEG，最高效方案是直接 depay/拆帧并使用原始 JPEG，不要先解码再重新编码。

### 11.4 相机传输 H.264/H.265

如果最终业务确实需要逐帧 JPEG，推荐：

```text
rtspsrc/udpsrc
→ RTP depay/parser
→ nvv4l2decoder
→ NVMM NV12
→ nvjpegenc
→ appsink
```

这样视频解码和 JPEG 编码都走 Jetson 硬件路径，避免在 CPU 内存中产生中间 YUV 图像。

## 12. 推荐的验证项目

正式集成后应记录以下指标：

- 输入分辨率、帧率和 YUV422 字节顺序；
- 网络接收丢包率与不完整帧数量；
- `appsrc` 推送速率；
- VIC 转换耗时；
- JPEG 编码耗时与输出大小；
- 端到端延迟；
- CPU、GPU、VIC 和内存带宽使用情况；
- 队列积压及主动丢帧数量。

可在目标机使用以下工具辅助观察：

```bash
tegrastats
```

同时可给 `nvjpegenc` 设置 `Enableperf=true`，查看编码性能信息：

```text
nvjpegenc quality=85 Enableperf=true
```

## 13. 最终结论

对于网络接收的原始 YUV422 图像，推荐实现为：

```text
网络直接接收到 GstBuffer
→ appsrc
→ nvvidconv compute-hw=2
→ NVMM NV12
→ nvjpegenc
→ appsink
```

可以从普通内存搬运到 NVMM。最佳实践是让网络接收函数直接写入预分配的 `GstBuffer`，从而避免应用层完整帧的额外 `memcpy`。普通内存到 NVMM 的传递无法完全省略，但可以与 VIC 的 YUV422→NV12 转换合并在同一个 `nvvidconv` 阶段完成。

## 14. 参考资料

- [NVIDIA Jetson Linux 36.3：NvJPEGEncoder Class Reference](https://docs.nvidia.com/jetson/archives/r36.3/ApiReference/classNvJPEGEncoder.html)
- [NVIDIA Jetson Linux 36.3：NvJPEGDecoder Class Reference](https://docs.nvidia.com/jetson/archives/r36.3/ApiReference/classNvJPEGDecoder.html)
- [NVIDIA Jetson Linux 36.3：JPEG Encode Sample](https://docs.nvidia.com/jetson/archives/r36.3/ApiReference/l4t_mm_05_jpeg_encode.html)
- [NVIDIA Jetson Linux 36.3：Video Convert Sample](https://docs.nvidia.com/jetson/archives/r36.3/ApiReference/l4t_mm_07_video_convert.html)
- [NVIDIA Jetson Linux 36.3：GStreamer Test Plan and Validation](https://docs.nvidia.com/jetson/archives/r36.3/DeveloperGuide/SD/TestPlanValidation.html)
- [CUDA 12.6 nvJPEG Documentation](https://docs.nvidia.com/cuda/archive/12.6.0/nvjpeg/index.html)
