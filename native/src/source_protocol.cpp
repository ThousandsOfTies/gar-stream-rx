#include "gar_stream_rx/source_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gar::stream::rx {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

bool safe_identifier(std::string_view value) {
    return !value.empty() && value.size() <= 96 && std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7eU;
    });
}

std::optional<std::string> json_string(std::string_view payload, std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    auto position = payload.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = payload.find(':', position + token.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = payload.find('"', position + 1);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    ++position;
    std::string value;
    bool escaped = false;
    for (; position < payload.size(); ++position) {
        const char character = payload[position];
        if (escaped) {
            switch (character) {
            case '"':
            case '\\':
            case '/':
                value.push_back(character);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return std::nullopt;
}

std::optional<int> json_integer(std::string_view payload, std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    auto position = payload.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = payload.find(':', position + token.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    ++position;
    while (position < payload.size() && payload[position] == ' ') {
        ++position;
    }
    int value = 0;
    const auto result = std::from_chars(payload.data() + position, payload.data() + payload.size(), value);
    return result.ec == std::errc{} ? std::optional<int>{value} : std::nullopt;
}

std::string json_escape(std::string_view value) {
    if (!safe_identifier(value)) {
        throw std::invalid_argument("identifier must be printable ASCII, non-empty, and at most 96 bytes");
    }
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    return output;
}

std::string encode_stream_message(
    std::string_view type,
    std::string_view source_id,
    std::string_view receiver_id,
    std::uint16_t stream_port,
    double lease_seconds
) {
    if (!std::isfinite(lease_seconds)) {
        throw std::invalid_argument("lease_seconds must be finite");
    }
    lease_seconds = std::clamp(lease_seconds, 1.0, 30.0);
    std::ostringstream output;
    output << "{\"protocol\":\"" << kProtocol << "\",\"type\":\"" << type
           << "\",\"source_id\":\"" << json_escape(source_id)
           << "\",\"receiver_id\":\"" << json_escape(receiver_id)
           << "\",\"stream_port\":" << stream_port << ",\"lease_seconds\":"
           << std::fixed << std::setprecision(1) << lease_seconds << '}';
    return output.str();
}

}  // namespace

std::vector<DiscoveryPeer> parse_discovery_peers(std::string_view value, std::uint16_t default_port) {
    std::vector<DiscoveryPeer> peers;
    while (!value.empty()) {
        const auto separator = value.find(',');
        const auto item = trim(value.substr(0, separator));
        value = separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
        if (item.empty()) {
            continue;
        }

        std::string host = item;
        auto port = default_port;
        const auto colon = item.rfind(':');
        if (colon != std::string::npos && colon != 0 && colon + 1 < item.size()) {
            int parsed_port = 0;
            const auto port_text = std::string_view(item).substr(colon + 1);
            const auto result = std::from_chars(
                port_text.data(), port_text.data() + port_text.size(), parsed_port
            );
            if (result.ec == std::errc{} && result.ptr == port_text.data() + port_text.size()) {
                if (parsed_port < 1 || parsed_port > 65535) {
                    throw std::invalid_argument("invalid discovery peer port: " + item);
                }
                host = item.substr(0, colon);
                port = static_cast<std::uint16_t>(parsed_port);
            }
        }
        if (host.empty()) {
            throw std::invalid_argument("invalid discovery peer: " + item);
        }
        peers.push_back(DiscoveryPeer{std::move(host), port});
    }
    return peers;
}

std::optional<SourceAnnouncement> decode_source_announcement(
    std::string_view payload,
    std::uint16_t sender_port
) {
    if (json_string(payload, "protocol") != kProtocol ||
        json_string(payload, "type") != "source_announce" ||
        json_string(payload, "transport") != "rtp/udp" ||
        json_string(payload, "encoding") != "JPEG") {
        return std::nullopt;
    }
    const auto source_id = json_string(payload, "source_id");
    const auto source_name = json_string(payload, "source_name");
    if (!source_id || !source_name || !safe_identifier(*source_id) || !safe_identifier(*source_name)) {
        return std::nullopt;
    }
    auto control_port = static_cast<int>(sender_port);
    if (const auto declared_port = json_integer(payload, "control_port")) {
        control_port = *declared_port;
    }
    if (control_port < 1 || control_port > 65535) {
        return std::nullopt;
    }
    return SourceAnnouncement{*source_id, *source_name, static_cast<std::uint16_t>(control_port)};
}

std::string encode_source_query(std::string_view receiver_id) {
    return "{\"protocol\":\"" + std::string(kProtocol) +
           "\",\"type\":\"source_query\",\"receiver_id\":\"" + json_escape(receiver_id) + "\"}";
}

std::string encode_stream_request(
    std::string_view source_id,
    std::string_view receiver_id,
    std::uint16_t stream_port,
    double lease_seconds
) {
    return encode_stream_message("stream_request", source_id, receiver_id, stream_port, lease_seconds);
}

std::string encode_stream_stop(
    std::string_view source_id,
    std::string_view receiver_id,
    std::uint16_t stream_port,
    double lease_seconds
) {
    return encode_stream_message("stream_stop", source_id, receiver_id, stream_port, lease_seconds);
}

}  // namespace gar::stream::rx
