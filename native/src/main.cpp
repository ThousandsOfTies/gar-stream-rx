#include "gar_stream_rx/ili9341.hpp"
#include "gar_stream_rx/ky040.hpp"
#include "gar_stream_rx/receiver_state.hpp"
#include "gar_stream_rx/source_browser.hpp"
#include "gar_stream_rx/source_protocol.hpp"
#include "gar_stream_rx/telemetry.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <glib-unix.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gar::stream::rx {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFps = 15;

std::string environment(std::string_view name, std::string fallback) {
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
}

std::uint32_t environment_u32(std::string_view name, std::uint32_t fallback) {
    const auto value = environment(name, std::to_string(fallback));
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument("invalid integer in " + std::string(name));
    }
    return static_cast<std::uint32_t>(parsed);
}

std::string hostname() {
    const auto configured = environment("HOSTNAME", "gar-stream-rx");
    return configured.empty() ? "gar-stream-rx" : configured;
}

std::string pipeline_description(std::uint16_t stream_port) {
    return
        "videotestsrc pattern=smpte is-live=true "
        "! video/x-raw,width=320,height=240,framerate=15/1 "
        "! videoconvert ! queue max-size-buffers=2 leaky=downstream ! sel.sink_0 "
        "udpsrc name=rx_source port=" + std::to_string(stream_port) +
        " caps=\"application/x-rtp,media=video,encoding-name=JPEG,payload=26\" "
        "! rtpjitterbuffer name=jitterbuffer latency=100 ! rtpjpegdepay ! jpegdec name=rx_decoder "
        "! videoconvert ! videoscale ! video/x-raw,width=320,height=240 "
        "! queue max-size-buffers=2 leaky=downstream ! sel.sink_1 "
        "input-selector name=sel "
        "! videobalance name=bal brightness=0.0 contrast=1.0 "
        "! textoverlay name=osd text=\"\" silent=true shaded-background=true "
        "valignment=top halignment=left font-desc=\"Sans Bold 16\" "
        "color=4294967295 draw-outline=true draw-shadow=false outline-color=4278190080 "
        "shading-value=180 xpad=8 ypad=6 "
        "! videoconvert ! video/x-raw,format=RGB16,width=320,height=240 "
        "! appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true";
}

std::string json_string_or_null(const std::string& value) {
    return value.empty() ? "null" : json_quote(value);
}

const char* mode_name(UiMode mode) {
    switch (mode) {
    case UiMode::view:
        return "view";
    case UiMode::menu:
        return "menu";
    case UiMode::submenu:
        return "submenu";
    }
    return "unknown";
}

const char* menu_name(MenuItem item) {
    switch (item) {
    case MenuItem::source:
        return "SOURCE";
    case MenuItem::brightness:
        return "BRIGHTNESS";
    case MenuItem::contrast:
        return "CONTRAST";
    case MenuItem::exit:
        return "EXIT";
    }
    return "UNKNOWN";
}

void invoke_main(std::function<void()> function) {
    auto* pending = new std::function<void()>(std::move(function));
    g_main_context_invoke(
        nullptr,
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> callback(
                static_cast<std::function<void()>*>(data)
            );
            (*callback)();
            return G_SOURCE_REMOVE;
        },
        pending
    );
}

class VideoMonitor {
  public:
    VideoMonitor(Ili9341& display, std::uint16_t stream_port, std::string metrics_path)
        : display_(display), telemetry_(std::move(metrics_path)) {
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(pipeline_description(stream_port).c_str(), &error);
        if (pipeline_ == nullptr) {
            const std::string message = error == nullptr ? "unknown error" : error->message;
            if (error != nullptr) {
                g_error_free(error);
            }
            throw std::runtime_error("cannot create GStreamer pipeline: " + message);
        }
        selector_ = require_element("sel");
        balance_ = require_element("bal");
        overlay_ = require_element("osd");
        sink_ = require_element("sink");
        rx_decoder_ = require_element("rx_decoder");
        jitterbuffer_ = require_element("jitterbuffer");
        colorbar_pad_ = gst_element_get_static_pad(selector_, "sink_0");
        rx_pad_ = gst_element_get_static_pad(selector_, "sink_1");
        if (colorbar_pad_ == nullptr || rx_pad_ == nullptr) {
            throw std::runtime_error("input-selector pads are unavailable");
        }

        g_signal_connect(sink_, "new-sample", G_CALLBACK(on_new_sample), this);
        GstPad* source_pad = gst_element_get_static_pad(rx_decoder_, "src");
        if (source_pad != nullptr) {
            gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, on_decoded_frame, this, nullptr);
            gst_object_unref(source_pad);
        }
        GstBus* bus = gst_element_get_bus(pipeline_);
        bus_watch_id_ = gst_bus_add_watch(bus, on_bus_message, this);
        gst_object_unref(bus);
        stream_watch_id_ = g_timeout_add(500, on_stream_watch, this);
        apply_state();
    }

    ~VideoMonitor() {
        stop();
        if (bus_watch_id_ != 0) {
            g_source_remove(bus_watch_id_);
        }
        if (stream_watch_id_ != 0) {
            g_source_remove(stream_watch_id_);
        }
        if (colorbar_pad_ != nullptr) {
            gst_object_unref(colorbar_pad_);
        }
        if (rx_pad_ != nullptr) {
            gst_object_unref(rx_pad_);
        }
        for (auto* element : {selector_, balance_, overlay_, sink_, rx_decoder_, jitterbuffer_}) {
            if (element != nullptr) {
                gst_object_unref(element);
            }
        }
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
        }
    }

    VideoMonitor(const VideoMonitor&) = delete;
    VideoMonitor& operator=(const VideoMonitor&) = delete;

    void attach_browser(SourceBrowser& browser) {
        browser_ = &browser;
    }

    void start() {
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error("cannot start GStreamer pipeline");
        }
    }

    void stop() {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
    }

    void update_sources(std::vector<Source> sources) {
        sources_ = sources;
        const auto selected_id = state_.selected_source_id();
        const bool selected_online = !selected_id || std::any_of(
            sources.begin(), sources.end(), [&selected_id](const Source& source) {
                return source.id == *selected_id && source.online;
            }
        );
        state_.update_sources(sources);
        if (!selected_online) {
            received_first_packet_ = false;
        }
        if (!state_.selected_source_id() && auto_select_) {
            const auto found = std::find_if(sources.begin(), sources.end(), [](const Source& source) {
                return source.online;
            });
            if (found != sources.end()) {
                static_cast<void>(state_.select_source(found->id));
                auto_select_ = false;
            }
        }
        apply_state();
        publish_metrics();
    }

    void rotate(int direction) {
        ++encoder_rotate_count_;
        state_.rotate(direction);
        apply_state();
        publish_metrics();
    }

    void press() {
        ++encoder_press_count_;
        state_.press();
        apply_state();
        publish_metrics();
    }

  private:
    GstElement* require_element(const char* name) {
        GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline_), name);
        if (element == nullptr) {
            throw std::runtime_error(std::string("GStreamer element is unavailable: ") + name);
        }
        return element;
    }

    void apply_state() {
        if (const auto selection = state_.take_pending_source_selection()) {
            received_first_packet_ = false;
            last_frame_ns_ = 0;
            if (browser_ != nullptr && !browser_->select_source(selection->source_id)) {
                std::cerr << "[stream_rx] selected source is no longer available\n";
            } else {
                auto_select_ = false;
                std::cout << "[stream_rx] source: " << state_.selected_source_label() << '\n';
            }
        }

        GstPad* active_pad = state_.selected_source_id() && received_first_packet_
            ? rx_pad_
            : colorbar_pad_;
        displaying_stream_ = active_pad == rx_pad_;
        g_object_set(selector_, "active-pad", active_pad, nullptr);
        g_object_set(
            balance_,
            "brightness", state_.brightness(),
            "contrast", state_.contrast(),
            nullptr
        );

        const auto lines = state_.menu_lines();
        std::string text;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (index != 0) {
                text.push_back('\n');
            }
            text += lines[index];
        }
        const gboolean silent = state_.mode() == UiMode::view ? TRUE : FALSE;
        g_object_set(
            overlay_,
            "text", text.c_str(),
            "silent", silent,
            "shaded-background", silent == FALSE ? TRUE : FALSE,
            nullptr
        );
    }

    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data) {
        auto& monitor = *static_cast<VideoMonitor*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) {
            return GST_FLOW_OK;
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        GstFlowReturn result = GST_FLOW_OK;
        if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            try {
                monitor.display_.blit_native_rgb565(map.data, map.size);
                const auto checksum = frame_checksum(map.data, map.size);
                const bool displaying_stream = monitor.displaying_stream_.load();
                invoke_main([&monitor, checksum, displaying_stream] {
                    monitor.record_display_update(checksum, displaying_stream);
                });
            } catch (const std::exception& error) {
                std::cerr << "[stream_rx] display write failed: " << error.what() << '\n';
                result = GST_FLOW_ERROR;
            }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return result;
    }

    static GstPadProbeReturn on_decoded_frame(GstPad*, GstPadProbeInfo*, gpointer user_data) {
        auto& monitor = *static_cast<VideoMonitor*>(user_data);
        const auto captured_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        monitor.last_frame_ns_ = captured_ns;
        invoke_main([&monitor, captured_ns] { monitor.record_received_frame(captured_ns); });
        if (!monitor.received_first_packet_.exchange(true)) {
            std::cout << "[stream_rx] first decoded video frame received\n";
            invoke_main([&monitor] { monitor.apply_state(); });
        }
        return GST_PAD_PROBE_OK;
    }

    static gboolean on_stream_watch(gpointer user_data) {
        auto& monitor = *static_cast<VideoMonitor*>(user_data);
        if (!monitor.received_first_packet_) {
            return G_SOURCE_CONTINUE;
        }
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        const auto last_frame_ns = monitor.last_frame_ns_.load();
        if (last_frame_ns > 0 && now_ns - last_frame_ns > 2'000'000'000LL) {
            monitor.received_first_packet_ = false;
            std::cout << "[stream_rx] video timed out; showing COLORBAR\n";
            monitor.apply_state();
            monitor.publish_metrics();
        }
        return G_SOURCE_CONTINUE;
    }

    static std::string frame_checksum(const std::uint8_t* data, std::size_t size) {
        std::uint64_t checksum = 1469598103934665603ULL;
        for (std::size_t index = 0; index < size; ++index) {
            checksum ^= data[index];
            checksum *= 1099511628211ULL;
        }
        std::ostringstream output;
        output << std::hex << checksum;
        return output.str();
    }

    void record_received_frame(std::int64_t captured_ns) {
        ++received_frame_count_;
        if (first_frame_ns_ == 0) {
            first_frame_ns_ = captured_ns;
        }
        last_processed_frame_ns_ = captured_ns;
        publish_metrics();
    }

    void record_display_update(std::string checksum, bool displaying_stream) {
        display_metrics_.record(std::move(checksum), displaying_stream);
        publish_metrics();
    }

    void publish_metrics() {
        if (!telemetry_.enabled()) {
            return;
        }
        std::size_t online_count = 0;
        for (const auto& source : sources_) {
            if (source.online) {
                ++online_count;
            }
        }
        std::ostringstream output;
        const double frame_rate = measured_fps(received_frame_count_, first_frame_ns_, last_processed_frame_ns_);
        const auto jitter = jitter_stats();
        const auto latency = pipeline_latency_ms();
        output << "{\"schema_version\":1,\"role\":\"rx\",\"health\":{\"ok\":true}"
               << ",\"build\":{\"id\":" << json_string_or_null(environment("GAR_ARTIFACT_BUILD_ID", ""))
               << ",\"hash\":" << json_string_or_null(environment("GAR_ARTIFACT_HASH", "")) << "}"
               << ",\"sources\":{\"count\":" << sources_.size()
               << ",\"online_count\":" << online_count
               << ",\"selected_id\":" << json_quote(state_.selected_source_id().value_or("")) << "}"
               << ",\"frames\":{\"received_count\":" << received_frame_count_
               << ",\"last_frame_monotonic_ns\":" << last_processed_frame_ns_
               << ",\"fps\":" << std::fixed << std::setprecision(3) << frame_rate
               << ",\"configured_fps\":" << kFps
               << ",\"drop_count\":" << jitter.lost + jitter.late
               << ",\"lost_count\":" << jitter.lost << ",\"late_count\":" << jitter.late
               << ",\"latency_ms\":";
        if (latency) {
            output << std::fixed << std::setprecision(3) << *latency;
        } else {
            output << "null";
        }
        output
               << ",\"display_update_count\":" << display_metrics_.update_count()
               << ",\"framebuffer_checksum\":" << json_quote(display_metrics_.framebuffer_checksum())
               << ",\"stream_display_update_count\":" << display_metrics_.stream_update_count()
               << ",\"stream_framebuffer_checksum\":"
               << json_quote(display_metrics_.stream_framebuffer_checksum()) << "}"
               << ",\"menu\":{\"mode\":" << json_quote(mode_name(state_.mode()))
               << ",\"item\":" << json_quote(menu_name(state_.menu_item())) << "}"
               << ",\"encoder\":{\"rotate_count\":" << encoder_rotate_count_
               << ",\"press_count\":" << encoder_press_count_ << "}}";
        telemetry_.write(output.str());
    }

    struct JitterStats {
        std::uint64_t lost{0};
        std::uint64_t late{0};
    };

    [[nodiscard]] JitterStats jitter_stats() const {
        GstStructure* stats = nullptr;
        g_object_get(jitterbuffer_, "stats", &stats, nullptr);
        JitterStats result;
        if (stats != nullptr) {
            static_cast<void>(gst_structure_get_uint64(stats, "num-lost", &result.lost));
            static_cast<void>(gst_structure_get_uint64(stats, "num-late", &result.late));
            gst_structure_free(stats);
        }
        return result;
    }

    [[nodiscard]] std::optional<double> pipeline_latency_ms() const {
        GstQuery* query = gst_query_new_latency();
        gboolean live = FALSE;
        GstClockTime minimum = GST_CLOCK_TIME_NONE;
        GstClockTime maximum = GST_CLOCK_TIME_NONE;
        const gboolean success = gst_element_query(pipeline_, query);
        if (success) {
            gst_query_parse_latency(query, &live, &minimum, &maximum);
        }
        gst_query_unref(query);
        if (!success || minimum == GST_CLOCK_TIME_NONE) {
            return std::nullopt;
        }
        return static_cast<double>(minimum) / static_cast<double>(GST_MSECOND);
    }

    static gboolean on_bus_message(GstBus*, GstMessage* message, gpointer) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::cerr << "[gst error] " << (error == nullptr ? "unknown" : error->message);
            if (debug != nullptr) {
                std::cerr << ": " << debug;
            }
            std::cerr << '\n';
            if (error != nullptr) {
                g_error_free(error);
            }
            g_free(debug);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            std::cerr << "[gst] end of stream\n";
        }
        return G_SOURCE_CONTINUE;
    }

    Ili9341& display_;
    TelemetryWriter telemetry_;
    ReceiverState state_;
    SourceBrowser* browser_{nullptr};
    bool auto_select_{true};
    std::atomic_bool received_first_packet_{false};
    std::atomic_bool displaying_stream_{false};
    GstElement* pipeline_{nullptr};
    GstElement* selector_{nullptr};
    GstElement* balance_{nullptr};
    GstElement* overlay_{nullptr};
    GstElement* sink_{nullptr};
    GstElement* rx_decoder_{nullptr};
    GstElement* jitterbuffer_{nullptr};
    GstPad* colorbar_pad_{nullptr};
    GstPad* rx_pad_{nullptr};
    guint bus_watch_id_{0};
    guint stream_watch_id_{0};
    std::atomic<std::int64_t> last_frame_ns_{0};
    std::int64_t first_frame_ns_{0};
    std::int64_t last_processed_frame_ns_{0};
    std::vector<Source> sources_;
    std::uint64_t received_frame_count_{0};
    FrameDisplayMetrics display_metrics_;
    std::uint64_t encoder_rotate_count_{0};
    std::uint64_t encoder_press_count_{0};
};

gboolean quit_loop(gpointer data) {
    g_main_loop_quit(static_cast<GMainLoop*>(data));
    return G_SOURCE_REMOVE;
}

}  // namespace
}  // namespace gar::stream::rx

int main(int argc, char** argv) {
    using namespace gar::stream::rx;
    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        gst_init(&argc, &argv);

        const auto gpio_chip = environment("GAR_GPIO_CHIP", "/dev/gpiochip0");
        const auto stream_port = static_cast<std::uint16_t>(environment_u32("GAR_STREAM_RX_PORT", 5600));
        Ili9341 display(
            environment("GAR_SPI_DEVICE", "/dev/spidev0.0"),
            environment_u32("GAR_SPI_MAX_HZ", 24000000),
            gpio_chip,
            environment_u32("GAR_LCD_DC_GPIO", 23),
            environment_u32("GAR_LCD_RST_GPIO", 24)
        );
        VideoMonitor monitor(display, stream_port, environment("GAR_STREAM_METRICS_PATH", ""));

        auto peers = parse_discovery_peers(environment("GAR_STREAM_DISCOVERY_PEERS", ""));
        peers.insert(peers.begin(), DiscoveryPeer{"255.255.255.255", kDefaultDiscoveryPort});
        SourceBrowser browser(
            environment("GAR_STREAM_RECEIVER_ID", hostname() + "-rx"),
            static_cast<std::uint16_t>(environment_u32("GAR_STREAM_DISCOVERY_PORT", 5601)),
            stream_port,
            std::move(peers),
            [&monitor](std::vector<Source> sources) {
                invoke_main([&monitor, sources = std::move(sources)]() mutable {
                    monitor.update_sources(std::move(sources));
                });
            }
        );
        monitor.attach_browser(browser);

        Ky040 encoder(
            gpio_chip,
            environment_u32("GAR_ENC_CLK_GPIO", 20),
            environment_u32("GAR_ENC_DT_GPIO", 21),
            environment_u32("GAR_ENC_SW_GPIO", 22),
            [&monitor](int direction) {
                std::cout << "[input] KY-040 rotate direction="
                          << (direction > 0 ? "+1" : "-1") << std::endl;
                invoke_main([&monitor, direction] { monitor.rotate(direction); });
            },
            [&monitor] {
                std::cout << "[input] KY-040 press" << std::endl;
                invoke_main([&monitor] { monitor.press(); });
            }
        );

        GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
        g_unix_signal_add(SIGINT, quit_loop, loop);
        g_unix_signal_add(SIGTERM, quit_loop, loop);
        browser.start();
        encoder.start();
        monitor.start();
        std::cout << "GarStreamRx native receiver running at " << kWidth << 'x' << kHeight
                  << '@' << kFps << "fps\n";
        std::cout << "[input] KY-040 gpio chip=" << gpio_chip
                  << " clk=" << environment_u32("GAR_ENC_CLK_GPIO", 20)
                  << " dt=" << environment_u32("GAR_ENC_DT_GPIO", 21)
                  << " sw=" << environment_u32("GAR_ENC_SW_GPIO", 22) << std::endl;
        g_main_loop_run(loop);
        encoder.stop();
        browser.stop();
        monitor.stop();
        g_main_loop_unref(loop);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gar-stream-rx: " << error.what() << '\n';
        return 1;
    }
}
