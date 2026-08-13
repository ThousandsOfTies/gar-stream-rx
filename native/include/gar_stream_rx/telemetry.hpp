#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gar::stream::rx {

[[nodiscard]] std::string json_quote(std::string_view value);

[[nodiscard]] double measured_fps(
    std::uint64_t frame_count,
    std::int64_t first_frame_ns,
    std::int64_t last_frame_ns
) noexcept;

class FrameDisplayMetrics {
  public:
    void record(std::string checksum, bool displaying_stream);

    [[nodiscard]] std::uint64_t update_count() const noexcept;
    [[nodiscard]] const std::string& framebuffer_checksum() const noexcept;
    [[nodiscard]] std::uint64_t stream_update_count() const noexcept;
    [[nodiscard]] const std::string& stream_framebuffer_checksum() const noexcept;

  private:
    std::uint64_t update_count_{0};
    std::string framebuffer_checksum_;
    std::uint64_t stream_update_count_{0};
    std::string stream_framebuffer_checksum_;
};

class TelemetryWriter {
  public:
    explicit TelemetryWriter(std::string path);

    [[nodiscard]] bool enabled() const noexcept;
    void write(const std::string& json) const noexcept;

  private:
    std::string path_;
};

}  // namespace gar::stream::rx
