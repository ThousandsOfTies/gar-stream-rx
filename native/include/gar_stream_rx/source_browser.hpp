#pragma once

#include "gar_stream_rx/receiver_state.hpp"
#include "gar_stream_rx/source_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gar::stream::rx {

class SourceBrowser {
  public:
    using SourcesChanged = std::function<void(std::vector<Source>)>;

    SourceBrowser(
        std::string receiver_id,
        std::uint16_t discovery_port,
        std::uint16_t stream_port,
        std::vector<DiscoveryPeer> query_targets,
        SourcesChanged on_sources_changed
    );
    ~SourceBrowser();

    SourceBrowser(const SourceBrowser&) = delete;
    SourceBrowser& operator=(const SourceBrowser&) = delete;

    void start();
    void stop();
    bool select_source(std::optional<std::string> source_id);
    [[nodiscard]] std::uint16_t discovery_port() const noexcept;

  private:
    struct SourceRecord {
        std::string id;
        std::string name;
        std::string host;
        std::uint16_t control_port;
        std::chrono::steady_clock::time_point last_seen;
    };

    void run();
    void query_sources();
    void renew_selected();
    void expire_sources();
    void handle_packet(std::string_view payload, const std::string& host, std::uint16_t port);
    void send_to(const std::string& payload, const std::string& host, std::uint16_t port) const;
    void send_stream_message(const SourceRecord& source, bool stop) const;
    void notify_if_changed();
    [[nodiscard]] std::vector<Source> snapshot() const;

    std::string receiver_id_;
    std::uint16_t discovery_port_;
    std::uint16_t stream_port_;
    std::vector<DiscoveryPeer> query_targets_;
    SourcesChanged on_sources_changed_;
    int socket_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::string, SourceRecord> sources_;
    std::optional<std::string> selected_source_id_;
    std::vector<Source> last_snapshot_;
};

}  // namespace gar::stream::rx
