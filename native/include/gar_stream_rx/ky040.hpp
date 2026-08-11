#pragma once

#include "gar_stream_rx/linux_gpio.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace gar::stream::rx {

class QuadratureDecoder {
  public:
    QuadratureDecoder(bool clock, bool data);

    std::optional<int> update(bool clock, bool data);

  private:
    std::uint8_t state_{0};
    int accumulator_{0};
};

class Ky040 {
  public:
    Ky040(
        const std::string& gpio_chip,
        std::uint32_t clock_line,
        std::uint32_t data_line,
        std::uint32_t switch_line,
        std::function<void(int)> on_rotate,
        std::function<void()> on_press
    );
    ~Ky040();

    Ky040(const Ky040&) = delete;
    Ky040& operator=(const Ky040&) = delete;

    void start();
    void stop();

  private:
    void run();

    GpioBothEdges clock_;
    GpioBothEdges data_;
    GpioFallingEdge switch_;
    std::function<void(int)> on_rotate_;
    std::function<void()> on_press_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace gar::stream::rx
