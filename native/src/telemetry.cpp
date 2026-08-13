#include "gar_stream_rx/telemetry.hpp"

#include <filesystem>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace gar::stream::rx {

std::string json_quote(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output << '\\' << static_cast<char>(character);
        } else if (character < 0x20U || character > 0x7eU) {
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(character) << std::dec << std::setfill(' ');
        } else {
            output << static_cast<char>(character);
        }
    }
    return output.str() + '"';
}

double measured_fps(
    std::uint64_t frame_count,
    std::int64_t first_frame_ns,
    std::int64_t last_frame_ns
) noexcept {
    if (frame_count < 2 || last_frame_ns <= first_frame_ns) {
        return 0.0;
    }
    const auto interval_count = static_cast<double>(frame_count - 1);
    const auto elapsed_seconds = static_cast<double>(last_frame_ns - first_frame_ns) / 1'000'000'000.0;
    return interval_count / elapsed_seconds;
}

void FrameDisplayMetrics::record(std::string checksum, bool displaying_stream) {
    ++update_count_;
    framebuffer_checksum_ = checksum;
    if (displaying_stream) {
        ++stream_update_count_;
        stream_framebuffer_checksum_ = std::move(checksum);
    }
}

std::uint64_t FrameDisplayMetrics::update_count() const noexcept {
    return update_count_;
}

const std::string& FrameDisplayMetrics::framebuffer_checksum() const noexcept {
    return framebuffer_checksum_;
}

std::uint64_t FrameDisplayMetrics::stream_update_count() const noexcept {
    return stream_update_count_;
}

const std::string& FrameDisplayMetrics::stream_framebuffer_checksum() const noexcept {
    return stream_framebuffer_checksum_;
}

TelemetryWriter::TelemetryWriter(std::string path) : path_(std::move(path)) {}

bool TelemetryWriter::enabled() const noexcept {
    return !path_.empty();
}

void TelemetryWriter::write(const std::string& json) const noexcept {
    if (!enabled()) {
        return;
    }
    try {
        const std::filesystem::path path(path_);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return;
        }
        const auto temporary = path.parent_path() /
            ("." + path.filename().string() + "." + std::to_string(getpid()) + ".new");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                return;
            }
            output << json << '\n';
            output.close();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return;
            }
        }
        static_cast<void>(chmod(temporary.c_str(), 0644));
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
        }
    } catch (...) {
        // Metrics must not interrupt live media delivery.
    }
}

}  // namespace gar::stream::rx
