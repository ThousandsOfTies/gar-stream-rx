#include "gar_stream_rx/source_browser.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace gar::stream::rx {
namespace {

constexpr auto kQueryInterval = std::chrono::seconds(2);
constexpr auto kLeaseSeconds = 7.0;
constexpr auto kSourceOnline = std::chrono::seconds(6);
constexpr auto kSourceRetention = std::chrono::seconds(60);

sockaddr_in resolve_udp_address(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const auto service = std::to_string(port);
    const int status = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (status != 0 || result == nullptr) {
        throw std::runtime_error("cannot resolve UDP host " + host + ": " + gai_strerror(status));
    }
    const auto address = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);
    return address;
}

bool same_sources(const std::vector<Source>& left, const std::vector<Source>& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

SourceBrowser::SourceBrowser(
    std::string receiver_id,
    std::uint16_t discovery_port,
    std::uint16_t stream_port,
    std::vector<DiscoveryPeer> query_targets,
    SourcesChanged on_sources_changed
)
    : receiver_id_(std::move(receiver_id)),
      discovery_port_(discovery_port),
      stream_port_(stream_port),
      query_targets_(std::move(query_targets)),
      on_sources_changed_(std::move(on_sources_changed)) {
    // Fail during construction, rather than terminating the worker thread on
    // its first query, when a runtime-provided receiver identity is invalid.
    static_cast<void>(encode_source_query(receiver_id_));
    socket_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        throw std::runtime_error("cannot create discovery socket: " + std::string(std::strerror(errno)));
    }
    int enabled = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    setsockopt(socket_fd_, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(discovery_port_);
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const auto message = std::string(std::strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        throw std::runtime_error("cannot bind discovery socket: " + message);
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(socket_fd_, reinterpret_cast<sockaddr*>(&address), &address_size) == 0) {
        discovery_port_ = ntohs(address.sin_port);
    }
}

SourceBrowser::~SourceBrowser() {
    stop();
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
}

void SourceBrowser::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&SourceBrowser::run, this);
}

void SourceBrowser::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    static_cast<void>(select_source(std::nullopt));
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SourceBrowser::select_source(std::optional<std::string> source_id) {
    std::optional<SourceRecord> previous;
    std::optional<SourceRecord> selected;
    {
        std::lock_guard lock(mutex_);
        if (selected_source_id_) {
            const auto found = sources_.find(*selected_source_id_);
            if (found != sources_.end()) {
                previous = found->second;
            }
        }
        if (source_id) {
            const auto found = sources_.find(*source_id);
            if (found == sources_.end()) {
                return false;
            }
            selected = found->second;
        }
        selected_source_id_ = std::move(source_id);
    }
    if (previous && (!selected || previous->id != selected->id)) {
        send_stream_message(*previous, true);
    }
    if (selected) {
        send_stream_message(*selected, false);
    }
    return true;
}

std::uint16_t SourceBrowser::discovery_port() const noexcept {
    return discovery_port_;
}

void SourceBrowser::run() {
    auto next_query = std::chrono::steady_clock::time_point{};
    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_query) {
            query_sources();
            renew_selected();
            expire_sources();
            next_query = now + kQueryInterval;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_fd_, &read_set);
        timeval timeout{0, 200000};
        const int ready = select(socket_fd_ + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0 || !FD_ISSET(socket_fd_, &read_set)) {
            continue;
        }
        std::array<char, 4096> buffer{};
        sockaddr_in sender{};
        socklen_t sender_size = sizeof(sender);
        const auto received = recvfrom(
            socket_fd_, buffer.data(), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size
        );
        if (received <= 0) {
            continue;
        }
        std::array<char, INET_ADDRSTRLEN> host{};
        if (inet_ntop(AF_INET, &sender.sin_addr, host.data(), host.size()) == nullptr) {
            continue;
        }
        handle_packet(
            std::string_view(buffer.data(), static_cast<std::size_t>(received)),
            host.data(), ntohs(sender.sin_port)
        );
    }
}

void SourceBrowser::query_sources() {
    const auto payload = encode_source_query(receiver_id_);
    for (const auto& target : query_targets_) {
        try {
            send_to(payload, target.host, target.port);
        } catch (const std::exception& error) {
            std::cerr << "[stream_rx] discovery query failed: " << error.what() << '\n';
        }
    }
}

void SourceBrowser::renew_selected() {
    std::optional<SourceRecord> selected;
    {
        std::lock_guard lock(mutex_);
        if (selected_source_id_) {
            const auto found = sources_.find(*selected_source_id_);
            if (found != sources_.end()) {
                selected = found->second;
            }
        }
    }
    if (selected) {
        send_stream_message(*selected, false);
    }
}

void SourceBrowser::expire_sources() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex_);
        for (auto iterator = sources_.begin(); iterator != sources_.end();) {
            if (now - iterator->second.last_seen > kSourceRetention &&
                (!selected_source_id_ || iterator->first != *selected_source_id_)) {
                iterator = sources_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    notify_if_changed();
}

void SourceBrowser::handle_packet(std::string_view payload, const std::string& host, std::uint16_t port) {
    const auto announcement = decode_source_announcement(payload, port);
    if (!announcement) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        sources_[announcement->source_id] = SourceRecord{
            announcement->source_id,
            announcement->source_name,
            host,
            announcement->control_port,
            std::chrono::steady_clock::now(),
        };
    }
    notify_if_changed();
}

void SourceBrowser::send_to(
    const std::string& payload,
    const std::string& host,
    std::uint16_t port
) const {
    const auto address = resolve_udp_address(host, port);
    if (sendto(
            socket_fd_, payload.data(), payload.size(), 0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address)
        ) < 0 && running_) {
        throw std::runtime_error("cannot send UDP packet to " + host + ": " + std::strerror(errno));
    }
}

void SourceBrowser::send_stream_message(const SourceRecord& source, bool stop) const {
    const auto payload = stop
        ? encode_stream_stop(source.id, receiver_id_, stream_port_, kLeaseSeconds)
        : encode_stream_request(source.id, receiver_id_, stream_port_, kLeaseSeconds);
    try {
        send_to(payload, source.host, source.control_port);
    } catch (const std::exception& error) {
        if (running_) {
            std::cerr << "[stream_rx] stream request failed: " << error.what() << '\n';
        }
    }
}

std::vector<Source> SourceBrowser::snapshot() const {
    const auto now = std::chrono::steady_clock::now();
    std::vector<Source> result;
    std::lock_guard lock(mutex_);
    result.reserve(sources_.size());
    for (const auto& [id, record] : sources_) {
        result.push_back(Source{id, record.name, now - record.last_seen <= kSourceOnline});
    }
    std::sort(result.begin(), result.end(), [](const Source& left, const Source& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        return left.id < right.id;
    });
    return result;
}

void SourceBrowser::notify_if_changed() {
    auto current = snapshot();
    {
        std::lock_guard lock(mutex_);
        if (same_sources(current, last_snapshot_)) {
            return;
        }
        last_snapshot_ = current;
    }
    if (on_sources_changed_) {
        on_sources_changed_(std::move(current));
    }
}

}  // namespace gar::stream::rx
