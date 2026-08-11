#include "gar_stream_rx/source_browser.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int create_server(std::uint16_t& port) {
    const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        throw std::runtime_error(std::strerror(errno));
    }
    timeval timeout{2, 0};
    setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(descriptor);
        throw std::runtime_error(std::strerror(errno));
    }
    socklen_t size = sizeof(address);
    getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size);
    port = ntohs(address.sin_port);
    return descriptor;
}

std::string receive_packet(int descriptor, sockaddr_in& sender) {
    char buffer[4096]{};
    socklen_t size = sizeof(sender);
    const auto count = recvfrom(
        descriptor, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr*>(&sender), &size
    );
    if (count < 0) {
        throw std::runtime_error(std::strerror(errno));
    }
    return std::string(buffer, static_cast<std::size_t>(count));
}

std::string receive_type(int descriptor, std::string_view type) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        sockaddr_in sender{};
        const auto packet = receive_packet(descriptor, sender);
        if (packet.find("\"type\":\"" + std::string(type) + "\"") != std::string::npos) {
            return packet;
        }
    }
    throw std::runtime_error("expected packet type was not received");
}

}  // namespace

int main() {
    using namespace gar::stream::rx;

    std::uint16_t server_port = 0;
    const int server = create_server(server_port);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Source> latest;
    SourceBrowser browser(
        "rx-test", 0, 5600,
        {DiscoveryPeer{"127.0.0.1", server_port}},
        [&](std::vector<Source> sources) {
            {
                std::lock_guard lock(mutex);
                latest = std::move(sources);
            }
            changed.notify_all();
        }
    );
    browser.start();

    sockaddr_in browser_address{};
    const auto query = receive_packet(server, browser_address);
    assert(query.find("\"type\":\"source_query\"") != std::string::npos);
    const std::string announcement =
        "{\"protocol\":\"gar-stream/1\",\"type\":\"source_announce\","
        "\"source_id\":\"tx-one\",\"source_name\":\"Test TX\","
        "\"transport\":\"rtp/udp\",\"encoding\":\"JPEG\","
        "\"control_port\":" + std::to_string(server_port) + "}";
    assert(sendto(
        server, announcement.data(), announcement.size(), 0,
        reinterpret_cast<sockaddr*>(&browser_address), sizeof(browser_address)
    ) == static_cast<ssize_t>(announcement.size()));

    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [&] { return !latest.empty(); }));
        const Source expected{"tx-one", "Test TX", true};
        assert(latest.front() == expected);
    }
    assert(browser.select_source("tx-one"));
    const auto request = receive_type(server, "stream_request");
    assert(request.find("\"receiver_id\":\"rx-test\"") != std::string::npos);
    assert(request.find("\"stream_port\":5600") != std::string::npos);

    browser.stop();
    close(server);
    return 0;
}
