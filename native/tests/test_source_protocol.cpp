#include "gar_stream_rx/source_protocol.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

using gar::stream::rx::DiscoveryPeer;
using gar::stream::rx::decode_source_announcement;
using gar::stream::rx::encode_source_query;
using gar::stream::rx::encode_stream_request;
using gar::stream::rx::parse_discovery_peers;

int main() {
    const auto peers = parse_discovery_peers("10.0.0.10, tx.example:6001");
    assert(peers.size() == 2);
    assert((peers[0] == DiscoveryPeer{"10.0.0.10", 5601}));
    assert((peers[1] == DiscoveryPeer{"tx.example", 6001}));

    bool rejected = false;
    try {
        static_cast<void>(parse_discovery_peers("tx.example:70000"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    const std::string announcement =
        R"({"protocol":"gar-stream/1","type":"source_announce","source_id":"tx-one",)"
        R"("source_name":"Kitchen TX","control_port":6201,"transport":"rtp/udp",)"
        R"("encoding":"JPEG","payload":26})";
    const auto decoded = decode_source_announcement(announcement, 5601);
    assert(decoded.has_value());
    assert(decoded->source_id == "tx-one");
    assert(decoded->source_name == "Kitchen TX");
    assert(decoded->control_port == 6201);
    assert(!decode_source_announcement("{}", 5601).has_value());

    const auto query = encode_source_query("rx-one");
    assert(query.find("\"type\":\"source_query\"") != std::string::npos);
    const auto request = encode_stream_request("tx-one", "rx-one", 5600, 7.0);
    assert(request.find("\"type\":\"stream_request\"") != std::string::npos);
    assert(request.find("\"stream_port\":5600") != std::string::npos);
}
