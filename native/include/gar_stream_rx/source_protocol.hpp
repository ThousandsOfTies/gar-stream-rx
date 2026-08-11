#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gar::stream::rx {

inline constexpr std::string_view kProtocol = "gar-stream/1";
inline constexpr std::uint16_t kDefaultDiscoveryPort = 5601;
inline constexpr std::uint16_t kDefaultStreamPort = 5600;

struct DiscoveryPeer {
    std::string host;
    std::uint16_t port{kDefaultDiscoveryPort};

    bool operator==(const DiscoveryPeer& other) const {
        return host == other.host && port == other.port;
    }
};

struct SourceAnnouncement {
    std::string source_id;
    std::string source_name;
    std::uint16_t control_port{kDefaultDiscoveryPort};
};

[[nodiscard]] std::vector<DiscoveryPeer> parse_discovery_peers(
    std::string_view value,
    std::uint16_t default_port = kDefaultDiscoveryPort
);

[[nodiscard]] std::optional<SourceAnnouncement> decode_source_announcement(
    std::string_view payload,
    std::uint16_t sender_port
);

[[nodiscard]] std::string encode_source_query(std::string_view receiver_id);

[[nodiscard]] std::string encode_stream_request(
    std::string_view source_id,
    std::string_view receiver_id,
    std::uint16_t stream_port,
    double lease_seconds
);

[[nodiscard]] std::string encode_stream_stop(
    std::string_view source_id,
    std::string_view receiver_id,
    std::uint16_t stream_port,
    double lease_seconds
);

}  // namespace gar::stream::rx
