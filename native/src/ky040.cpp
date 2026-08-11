#include "gar_stream_rx/ky040.hpp"

#include <poll.h>

#include <array>
#include <utility>

namespace gar::stream::rx {
namespace {

constexpr std::array<int, 16> kTransitions{
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

std::uint8_t phase_state(bool clock, bool data) {
    return static_cast<std::uint8_t>((clock ? 2U : 0U) | (data ? 1U : 0U));
}

}  // namespace

QuadratureDecoder::QuadratureDecoder(bool clock, bool data)
    : state_(phase_state(clock, data)) {}

std::optional<int> QuadratureDecoder::update(bool clock, bool data) {
    const auto next = phase_state(clock, data);
    if (next == state_) {
        return std::nullopt;
    }

    const auto transition = kTransitions[(state_ << 2U) | next];
    state_ = next;
    if (transition == 0) {
        accumulator_ = 0;
        return std::nullopt;
    }
    accumulator_ += transition;

    // A KY-040 detent rests at CLK=HIGH, DT=HIGH. Emit only after all four
    // valid Gray-code transitions return to that position. Contact bounce
    // reverses the preceding transition and therefore cancels out.
    if (state_ != 3U) {
        return std::nullopt;
    }
    const int direction = accumulator_ >= 4 ? 1 : accumulator_ <= -4 ? -1 : 0;
    accumulator_ = 0;
    return direction == 0 ? std::nullopt : std::optional<int>{direction};
}

Ky040::Ky040(
    const std::string& gpio_chip,
    std::uint32_t clock_line,
    std::uint32_t data_line,
    std::uint32_t switch_line,
    std::function<void(int)> on_rotate,
    std::function<void()> on_press
)
    : clock_(gpio_chip, clock_line),
      data_(gpio_chip, data_line),
      switch_(gpio_chip, switch_line),
      on_rotate_(std::move(on_rotate)),
      on_press_(std::move(on_press)) {}

Ky040::~Ky040() {
    stop();
}

void Ky040::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&Ky040::run, this);
}

void Ky040::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Ky040::run() {
    std::array<pollfd, 3> descriptors{{
        pollfd{clock_.fd(), POLLIN, 0},
        pollfd{data_.fd(), POLLIN, 0},
        pollfd{switch_.fd(), POLLIN, 0},
    }};
    bool clock_level = clock_.get();
    bool data_level = data_.get();
    QuadratureDecoder decoder(clock_level, data_level);
    auto last_press = std::chrono::steady_clock::time_point{};
    while (running_) {
        const int ready = poll(descriptors.data(), descriptors.size(), 200);
        if (ready <= 0) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if ((descriptors[0].revents & POLLIN) != 0) {
            if (const auto level = clock_.consume()) {
                clock_level = *level;
                if (const auto direction = decoder.update(clock_level, data_level); direction && on_rotate_) {
                    on_rotate_(*direction);
                }
            }
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            if (const auto level = data_.consume()) {
                data_level = *level;
                if (const auto direction = decoder.update(clock_level, data_level); direction && on_rotate_) {
                    on_rotate_(*direction);
                }
            }
        }
        if ((descriptors[2].revents & POLLIN) != 0 && switch_.consume() &&
            now - last_press >= std::chrono::milliseconds(30)) {
            last_press = now;
            if (on_press_) {
                on_press_();
            }
        }
    }
}

}  // namespace gar::stream::rx
