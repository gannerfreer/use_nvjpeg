#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string input_path;
  std::string output_path = "result.jpg";
  std::string pixel_format = "YUY2";
  int width = 0;
  int height = 0;
  int fps = 30;
  int quality = 85;
  int warmup_frames = 30;
  int measured_frames = 300;
};

struct BenchmarkState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<Clock::time_point> pushed_at;
  std::vector<double> measured_latencies_ms;
  std::size_t warmup_frames = 0;
  std::size_t expected_frames = 0;
  std::size_t output_frames = 0;
  std::uint64_t measured_jpeg_bytes = 0;
  Clock::time_point measured_start;
  Clock::time_point measured_end;
  bool have_measured_start = false;
  bool have_measured_end = false;
  GstBuffer *first_measured_jpeg = nullptr;
};

void print_usage(const char *program) {
  std::cout
      << "Usage: " << program
      << " --input FILE --width N --height N [options]\n"
      << "\nOptions:\n"
      << "  --format YUY2|UYVY  Packed YUV422 byte order (default: YUY2)\n"
      << "  --fps N              Input timestamps per second (default: 30)\n"
      << "  --quality N          JPEG quality, 0..100 (default: 85)\n"
      << "  --warmup N           Frames excluded from statistics (default: "
         "30)\n"
      << "  --frames N           Frames included in statistics (default: 300)\n"
      << "  --output FILE        Save the first measured JPEG (default: "
         "result.jpg)\n"
      << "  --help               Show this help\n";
}

std::optional<int> parse_integer(const std::string &text) {
  try {
    std::size_t consumed = 0;
    const long value = std::stol(text, &consumed, 10);
    if (consumed != text.size() || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    return static_cast<int>(value);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool parse_options(int argc, char **argv, Options &options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      print_usage(argv[0]);
      std::exit(EXIT_SUCCESS);
    }
    if (index + 1 >= argc) {
      std::cerr << "error: missing value for " << argument << '\n';
      return false;
    }
    const std::string value = argv[++index];
    if (argument == "--input") {
      options.input_path = value;
    } else if (argument == "--output") {
      options.output_path = value;
    } else if (argument == "--format") {
      options.pixel_format = value;
      std::transform(options.pixel_format.begin(), options.pixel_format.end(),
                     options.pixel_format.begin(), [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                     });
    } else {
      const auto parsed = parse_integer(value);
      if (!parsed) {
        std::cerr << "error: invalid integer for " << argument << ": " << value
                  << '\n';
        return false;
      }
      if (argument == "--width") {
        options.width = *parsed;
      } else if (argument == "--height") {
        options.height = *parsed;
      } else if (argument == "--fps") {
        options.fps = *parsed;
      } else if (argument == "--quality") {
        options.quality = *parsed;
      } else if (argument == "--warmup") {
        options.warmup_frames = *parsed;
      } else if (argument == "--frames") {
        options.measured_frames = *parsed;
      } else {
        std::cerr << "error: unknown option " << argument << '\n';
        return false;
      }
    }
  }

  if (options.input_path.empty()) {
    std::cerr << "error: --input is required\n";
    return false;
  }
  if (options.width <= 0 || options.height <= 0 || options.width % 2 != 0 ||
      options.height % 2 != 0) {
    std::cerr << "error: width and height must be positive even numbers\n";
    return false;
  }
  if (options.pixel_format != "YUY2" && options.pixel_format != "UYVY") {
    std::cerr << "error: --format must be YUY2 or UYVY\n";
    return false;
  }
  if (options.fps <= 0 || options.quality < 0 || options.quality > 100 ||
      options.warmup_frames < 0 || options.measured_frames <= 0) {
    std::cerr
        << "error: fps and frames must be positive, warmup non-negative, and "
           "quality in 0..100\n";
    return false;
  }
  if (options.warmup_frames >
      std::numeric_limits<int>::max() - options.measured_frames) {
    std::cerr << "error: total frame count is too large\n";
    return false;
  }
  return true;
}

bool factory_exists(const char *name) {
  GstElementFactory *factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

GstBuffer *load_frame(const Options &options) {
  const auto width = static_cast<std::size_t>(options.width);
  const auto height = static_cast<std::size_t>(options.height);
  if (width > std::numeric_limits<std::size_t>::max() / height / 2U) {
    std::cerr << "error: frame dimensions overflow size_t\n";
    return nullptr;
  }
  const std::size_t expected_size = width * height * 2U;

  std::ifstream input(options.input_path, std::ios::binary | std::ios::ate);
  if (!input) {
    std::cerr << "error: cannot open input file: " << options.input_path
              << '\n';
    return nullptr;
  }
  const std::streamoff file_size = input.tellg();
  if (file_size < 0 || static_cast<std::uint64_t>(file_size) != expected_size) {
    std::cerr << "error: input must contain exactly one tightly packed frame "
                 "(expected "
              << expected_size << " bytes, got " << file_size << ")\n";
    return nullptr;
  }
  input.seekg(0, std::ios::beg);

  if (expected_size >
      static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    std::cerr << "error: input frame is too large for std::ifstream\n";
    return nullptr;
  }
  auto *data = static_cast<guint8 *>(g_try_malloc(expected_size));
  if (data == nullptr) {
    std::cerr << "error: failed to allocate input frame memory\n";
    return nullptr;
  }
  input.read(reinterpret_cast<char *>(data),
             static_cast<std::streamsize>(expected_size));
  const bool read_ok = input.good() || input.eof();
  const auto bytes_read = input.gcount();
  if (!read_ok || bytes_read != static_cast<std::streamsize>(expected_size)) {
    std::cerr << "error: failed to read complete input frame\n";
    g_free(data);
    return nullptr;
  }

  GstBuffer *buffer =
      gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, data, expected_size,
                                  0, expected_size, data, g_free);
  if (buffer == nullptr) {
    std::cerr << "error: failed to wrap input frame in GstBuffer\n";
    g_free(data);
    return nullptr;
  }
  return buffer;
}

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  auto &state = *static_cast<BenchmarkState *>(user_data);
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (sample == nullptr) {
    return GST_FLOW_ERROR;
  }
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  const auto now = Clock::now();
  const std::size_t jpeg_size = gst_buffer_get_size(buffer);

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    const std::size_t frame_index = state.output_frames++;
    if (frame_index >= state.warmup_frames &&
        frame_index < state.expected_frames) {
      if (frame_index < state.pushed_at.size()) {
        state.measured_latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(
                now - state.pushed_at[frame_index])
                .count());
      }
      state.measured_jpeg_bytes += jpeg_size;
      if (state.first_measured_jpeg == nullptr) {
        state.first_measured_jpeg = gst_buffer_ref(buffer);
      }
      if (frame_index + 1 == state.expected_frames) {
        state.measured_end = now;
        state.have_measured_end = true;
      }
    }
  }
  state.condition.notify_all();
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

bool save_and_validate_jpeg(GstBuffer *buffer, const std::string &path) {
  if (buffer == nullptr) {
    std::cerr << "error: no measured JPEG was received\n";
    return false;
  }
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    std::cerr << "error: failed to map output JPEG\n";
    return false;
  }
  const bool markers_ok =
      map.size >= 4 && map.data[0] == 0xff && map.data[1] == 0xd8 &&
      map.data[map.size - 2] == 0xff && map.data[map.size - 1] == 0xd9;
  std::ofstream output(path, std::ios::binary);
  if (output) {
    output.write(reinterpret_cast<const char *>(map.data),
                 static_cast<std::streamsize>(map.size));
  }
  const bool write_ok = output.good();
  gst_buffer_unmap(buffer, &map);
  if (!write_ok) {
    std::cerr << "error: failed to write JPEG: " << path << '\n';
    return false;
  }
  if (!markers_ok) {
    std::cerr << "error: encoded output does not have JPEG SOI/EOI markers\n";
    return false;
  }
  return true;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, values.size() - 1);
  const double weight = position - static_cast<double>(lower);
  return values[lower] * (1.0 - weight) + values[upper] * weight;
}

void print_statistics(const Options &options, const BenchmarkState &state) {
  const double elapsed_seconds =
      std::chrono::duration<double>(state.measured_end - state.measured_start)
          .count();
  const double throughput =
      static_cast<double>(options.measured_frames) / elapsed_seconds;
  const double average_latency =
      std::accumulate(state.measured_latencies_ms.begin(),
                      state.measured_latencies_ms.end(), 0.0) /
      static_cast<double>(state.measured_latencies_ms.size());
  const double average_jpeg = static_cast<double>(state.measured_jpeg_bytes) /
                              static_cast<double>(options.measured_frames);

  std::cout << std::fixed << std::setprecision(3) << "\nBenchmark result\n"
            << "  input:          " << options.width << 'x' << options.height
            << ' ' << options.pixel_format << '\n'
            << "  warmup frames:  " << options.warmup_frames << '\n'
            << "  measured frames:" << options.measured_frames << '\n'
            << "  elapsed:        " << elapsed_seconds << " s\n"
            << "  throughput:     " << throughput << " fps\n"
            << "  JPEG avg size:  " << average_jpeg << " bytes\n"
            << "  latency avg:    " << average_latency << " ms\n"
            << "  latency p50:    "
            << percentile(state.measured_latencies_ms, 0.50) << " ms\n"
            << "  latency p95:    "
            << percentile(state.measured_latencies_ms, 0.95) << " ms\n"
            << "  latency p99:    "
            << percentile(state.measured_latencies_ms, 0.99) << " ms\n"
            << "  saved JPEG:     " << options.output_path << '\n';
}

bool push_frame(GstAppSrc *appsrc, GstBuffer *template_buffer,
                std::size_t index, const Options &options,
                BenchmarkState &state) {
  GstBuffer *frame = gst_buffer_copy(template_buffer);
  if (frame == nullptr) {
    std::cerr << "error: failed to copy GstBuffer metadata\n";
    return false;
  }
  GST_BUFFER_PTS(frame) = gst_util_uint64_scale(index, GST_SECOND, options.fps);
  GST_BUFFER_DTS(frame) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(frame) =
      gst_util_uint64_scale(1, GST_SECOND, options.fps);
  GST_BUFFER_OFFSET(frame) = index;

  const auto pushed_at = Clock::now();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pushed_at[index] = pushed_at;
    if (index == state.warmup_frames) {
      state.measured_start = pushed_at;
      state.have_measured_start = true;
    }
  }
  const GstFlowReturn flow = gst_app_src_push_buffer(appsrc, frame);
  if (flow != GST_FLOW_OK) {
    std::cerr << "error: appsrc push failed at frame " << index << ": "
              << gst_flow_get_name(flow) << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  gst_init(nullptr, nullptr);
  for (const char *plugin : {"nvvidconv", "nvjpegenc"}) {
    if (!factory_exists(plugin)) {
      std::cerr << "error: required Jetson GStreamer element is missing: "
                << plugin << '\n';
      return EXIT_FAILURE;
    }
  }

  GstBuffer *template_buffer = load_frame(options);
  if (template_buffer == nullptr) {
    return EXIT_FAILURE;
  }

  GstElement *pipeline = gst_pipeline_new("yuv422-nvjpeg-benchmark");
  GstElement *source = gst_element_factory_make("appsrc", "source");
  GstElement *queue = gst_element_factory_make("queue", "input-queue");
  GstElement *converter = gst_element_factory_make("nvvidconv", "converter");
  GstElement *caps_filter = gst_element_factory_make("capsfilter", "nvmm-caps");
  GstElement *encoder = gst_element_factory_make("nvjpegenc", "encoder");
  GstElement *sink = gst_element_factory_make("appsink", "sink");
  if (pipeline == nullptr || source == nullptr || queue == nullptr ||
      converter == nullptr || caps_filter == nullptr || encoder == nullptr ||
      sink == nullptr) {
    std::cerr << "error: failed to create GStreamer pipeline elements\n";
    for (GstElement *element :
         {source, queue, converter, caps_filter, encoder, sink}) {
      if (element != nullptr) {
        gst_object_unref(element);
      }
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
    }
    gst_buffer_unref(template_buffer);
    return EXIT_FAILURE;
  }

  GstCaps *input_caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, options.pixel_format.c_str(),
      "width", G_TYPE_INT, options.width, "height", G_TYPE_INT, options.height,
      "framerate", GST_TYPE_FRACTION, options.fps, 1, "colorimetry",
      G_TYPE_STRING, "bt709", nullptr);
  GstCaps *nvmm_caps =
      gst_caps_from_string("video/x-raw(memory:NVMM),format=NV12");
  const auto frame_size =
      static_cast<guint64>(gst_buffer_get_size(template_buffer));
  const auto maximum_bytes = std::numeric_limits<guint64>::max();
  const guint64 appsrc_queue_bytes =
      frame_size <= maximum_bytes / 4U ? frame_size * 4U : maximum_bytes;
  gst_app_src_set_caps(GST_APP_SRC(source), input_caps);
  g_object_set(source, "is-live", TRUE, "block", TRUE, "format",
               GST_FORMAT_TIME, "max-bytes", appsrc_queue_bytes, nullptr);
  g_object_set(queue, "max-size-buffers", static_cast<guint>(4),
               "max-size-bytes", static_cast<guint>(0), "max-size-time",
               static_cast<guint64>(0), nullptr);
  g_object_set(converter, "compute-hw", 2, nullptr);
  g_object_set(caps_filter, "caps", nvmm_caps, nullptr);
  g_object_set(encoder, "quality", options.quality, nullptr);
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "Enableperf") !=
      nullptr) {
    g_object_set(encoder, "Enableperf", TRUE, nullptr);
  } else {
    std::cerr << "warning: nvjpegenc has no Enableperf property; continuing\n";
  }
  g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, "max-buffers",
               static_cast<guint>(4), "drop", FALSE, nullptr);
  gst_caps_unref(input_caps);
  gst_caps_unref(nvmm_caps);

  BenchmarkState state;
  state.warmup_frames = static_cast<std::size_t>(options.warmup_frames);
  state.expected_frames = static_cast<std::size_t>(options.warmup_frames) +
                          static_cast<std::size_t>(options.measured_frames);
  state.pushed_at.resize(state.expected_frames);
  g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), &state);

  gst_bin_add_many(GST_BIN(pipeline), source, queue, converter, caps_filter,
                   encoder, sink, nullptr);
  if (!gst_element_link_many(source, queue, converter, caps_filter, encoder,
                             sink, nullptr)) {
    std::cerr << "error: failed to link GStreamer pipeline\n";
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_buffer_unref(template_buffer);
    return EXIT_FAILURE;
  }

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "error: failed to start GStreamer pipeline\n";
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_buffer_unref(template_buffer);
    return EXIT_FAILURE;
  }

  bool push_ok = true;
  for (int index = 0; index < options.warmup_frames && push_ok; ++index) {
    push_ok = push_frame(GST_APP_SRC(source), template_buffer,
                         static_cast<std::size_t>(index), options, state);
  }
  if (push_ok && options.warmup_frames > 0) {
    std::unique_lock<std::mutex> lock(state.mutex);
    const bool warmed_up =
        state.condition.wait_for(lock, std::chrono::seconds(30), [&state] {
          return state.output_frames >= state.warmup_frames;
        });
    if (!warmed_up) {
      std::cerr << "error: timed out waiting for warmup frames\n";
      push_ok = false;
    }
  }
  for (std::size_t index = state.warmup_frames;
       index < state.expected_frames && push_ok; ++index) {
    push_ok =
        push_frame(GST_APP_SRC(source), template_buffer, index, options, state);
  }
  gst_buffer_unref(template_buffer);

  const GstFlowReturn eos_flow = gst_app_src_end_of_stream(GST_APP_SRC(source));
  if (push_ok && eos_flow != GST_FLOW_OK) {
    std::cerr << "error: failed to send EOS to appsrc: "
              << gst_flow_get_name(eos_flow) << '\n';
    push_ok = false;
  }

  GstBus *bus = gst_element_get_bus(pipeline);
  const int nominal_seconds =
      (options.warmup_frames + options.measured_frames) / options.fps;
  const int timeout_seconds = std::max(30, nominal_seconds * 3 + 10);
  GstMessage *message = gst_bus_timed_pop_filtered(
      bus, static_cast<GstClockTime>(timeout_seconds) * GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  bool pipeline_ok = push_ok;
  if (message == nullptr) {
    std::cerr << "error: pipeline timed out after " << timeout_seconds
              << " seconds\n";
    pipeline_ok = false;
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::cerr << "error: GStreamer: "
              << (error != nullptr ? error->message : "unknown") << '\n';
    if (debug != nullptr) {
      std::cerr << "debug: " << debug << '\n';
    }
    g_clear_error(&error);
    g_free(debug);
    pipeline_ok = false;
  }
  if (message != nullptr) {
    gst_message_unref(message);
  }
  gst_object_unref(bus);

  GstBuffer *first_jpeg = nullptr;
  std::size_t output_frames = 0;
  bool timing_complete = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    output_frames = state.output_frames;
    timing_complete = state.have_measured_start && state.have_measured_end &&
                      state.measured_latencies_ms.size() ==
                          static_cast<std::size_t>(options.measured_frames);
    if (state.first_measured_jpeg != nullptr) {
      first_jpeg = gst_buffer_ref(state.first_measured_jpeg);
    }
  }
  if (output_frames != state.expected_frames) {
    std::cerr << "error: expected " << state.expected_frames
              << " output frames, got " << output_frames << '\n';
    pipeline_ok = false;
  }
  if (!timing_complete) {
    std::cerr << "error: incomplete benchmark timing data\n";
    pipeline_ok = false;
  }
  if (!save_and_validate_jpeg(first_jpeg, options.output_path)) {
    pipeline_ok = false;
  }
  if (first_jpeg != nullptr) {
    gst_buffer_unref(first_jpeg);
  }
  if (pipeline_ok) {
    print_statistics(options, state);
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  if (state.first_measured_jpeg != nullptr) {
    gst_buffer_unref(state.first_measured_jpeg);
  }
  return pipeline_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
