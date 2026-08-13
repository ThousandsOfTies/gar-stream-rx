#include "gar_stream_rx/telemetry.hpp"

#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

bool parses_json_string(std::string_view value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        const unsigned char character = value[index];
        if (character < 0x20U) {
            return false;
        }
        if (character != '\\') {
            continue;
        }
        if (++index + 1 >= value.size()) {
            return false;
        }
        const char escaped = value[index];
        if (escaped == 'u') {
            if (index + 4 >= value.size()) {
                return false;
            }
            for (std::size_t digit = 1; digit <= 4; ++digit) {
                if (std::isxdigit(static_cast<unsigned char>(value[index + digit])) == 0) {
                    return false;
                }
            }
            index += 4;
        } else if (std::string_view("\\\"/bfnrt").find(escaped) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool parses_json_object_with_value(std::string_view value) {
    constexpr std::string_view prefix = R"({"value":)";
    return value.size() > prefix.size() + 1 && value.substr(0, prefix.size()) == prefix && value.back() == '}' &&
        parses_json_string(value.substr(prefix.size(), value.size() - prefix.size() - 1));
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("gar-stream-rx-telemetry-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto output = root / "metrics.json";
    gar::stream::rx::TelemetryWriter writer(output.string());
    assert(writer.enabled());
    writer.write(R"({"schema_version":1,"role":"rx"})");
    std::ifstream input(output);
    std::string line;
    std::getline(input, line);
    assert(line == R"({"schema_version":1,"role":"rx"})");
    const std::string quoted = gar::stream::rx::json_quote(
        std::string("quote=\" slash=\\ newline=\n control=\x01 nonascii=") + "\xC3\xA9\xff"
    );
    assert(quoted == "\"quote=\\\" slash=\\\\ newline=\\u000a control=\\u0001 nonascii=\\u00c3\\u00a9\\u00ff\"");
    const std::string json = "{\"value\":" + quoted + "}";
    // The exact expected JSON validates each escape and preserves UTF-8 rather
    // than accidentally producing a malformed short \\u sequence. The small
    // parser below also validates the complete JSON string token's grammar.
    assert(json == "{\"value\":\"quote=\\\" slash=\\\\ newline=\\u000a control=\\u0001 nonascii=\\u00c3\\u00a9\\u00ff\"}");
    assert(parses_json_object_with_value(json));

    assert(gar::stream::rx::measured_fps(1, 1, 1) == 0.0);
    assert(gar::stream::rx::measured_fps(2, 1, 1'000'000'001) == 1.0);

    gar::stream::rx::FrameDisplayMetrics display_metrics;
    display_metrics.record("colorbar", false);
    assert(display_metrics.update_count() == 1);
    assert(display_metrics.framebuffer_checksum() == "colorbar");
    assert(display_metrics.stream_update_count() == 0);
    assert(display_metrics.stream_framebuffer_checksum().empty());
    display_metrics.record("rtp-frame", true);
    assert(display_metrics.update_count() == 2);
    assert(display_metrics.stream_update_count() == 1);
    assert(display_metrics.stream_framebuffer_checksum() == "rtp-frame");
    assert(!std::filesystem::exists(root / ".metrics.json."));
    std::filesystem::remove_all(root);
}
