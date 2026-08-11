#include "gar_stream_rx/linux_gpio.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace gar::stream::rx {
namespace {

int open_chip(const std::string& chip_path) {
    const int fd = open(chip_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("cannot open " + chip_path + ": " + std::strerror(errno));
    }
    return fd;
}

int request_handle(
    const std::string& chip_path,
    std::uint32_t line,
    std::uint64_t flags,
    bool initial_value
) {
    const int chip_fd = open_chip(chip_path);
    gpio_v2_line_request request{};
    request.offsets[0] = line;
    request.config.flags = flags;
    request.num_lines = 1;
    std::strncpy(request.consumer, "gar-stream-rx", sizeof(request.consumer) - 1);
    if ((flags & GPIO_V2_LINE_FLAG_OUTPUT) != 0U) {
        request.config.num_attrs = 1;
        request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        request.config.attrs[0].attr.values = initial_value ? 1U : 0U;
        request.config.attrs[0].mask = 1U;
    }
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const auto message = std::string(std::strerror(errno));
        close(chip_fd);
        throw std::runtime_error("cannot request GPIO line " + std::to_string(line) + ": " + message);
    }
    close(chip_fd);
    return request.fd;
}

}  // namespace

GpioOutput::GpioOutput(const std::string& chip_path, std::uint32_t line, bool initial_value)
    : fd_(request_handle(chip_path, line, GPIO_V2_LINE_FLAG_OUTPUT, initial_value)) {}

GpioOutput::~GpioOutput() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

void GpioOutput::set(bool value) {
    gpio_v2_line_values data{};
    data.bits = value ? 1U : 0U;
    data.mask = 1U;
    if (ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &data) < 0) {
        throw std::runtime_error("cannot set GPIO value: " + std::string(std::strerror(errno)));
    }
}

GpioInput::GpioInput(const std::string& chip_path, std::uint32_t line)
    : fd_(request_handle(chip_path, line, GPIO_V2_LINE_FLAG_INPUT, false)) {}

GpioInput::~GpioInput() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool GpioInput::get() const {
    gpio_v2_line_values data{};
    data.mask = 1U;
    if (ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &data) < 0) {
        throw std::runtime_error("cannot read GPIO value: " + std::string(std::strerror(errno)));
    }
    return (data.bits & 1U) != 0U;
}

GpioBothEdges::GpioBothEdges(const std::string& chip_path, std::uint32_t line) {
    const int chip_fd = open_chip(chip_path);
    gpio_v2_line_request request{};
    request.offsets[0] = line;
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                           GPIO_V2_LINE_FLAG_EDGE_RISING |
                           GPIO_V2_LINE_FLAG_EDGE_FALLING;
    request.num_lines = 1;
    std::strncpy(request.consumer, "gar-stream-rx", sizeof(request.consumer) - 1);
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const auto message = std::string(std::strerror(errno));
        close(chip_fd);
        throw std::runtime_error("cannot request GPIO edges " + std::to_string(line) + ": " + message);
    }
    close(chip_fd);
    fd_ = request.fd;
}

GpioBothEdges::~GpioBothEdges() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

int GpioBothEdges::fd() const noexcept {
    return fd_;
}

bool GpioBothEdges::get() const {
    gpio_v2_line_values data{};
    data.mask = 1U;
    if (ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &data) < 0) {
        throw std::runtime_error("cannot read GPIO value: " + std::string(std::strerror(errno)));
    }
    return (data.bits & 1U) != 0U;
}

std::optional<bool> GpioBothEdges::consume() {
    gpio_v2_line_event event{};
    const auto count = read(fd_, &event, sizeof(event));
    if (count != static_cast<ssize_t>(sizeof(event))) {
        return std::nullopt;
    }
    if (event.id == GPIO_V2_LINE_EVENT_RISING_EDGE) {
        return true;
    }
    if (event.id == GPIO_V2_LINE_EVENT_FALLING_EDGE) {
        return false;
    }
    return std::nullopt;
}

GpioFallingEdge::GpioFallingEdge(const std::string& chip_path, std::uint32_t line) {
    const int chip_fd = open_chip(chip_path);
    gpio_v2_line_request request{};
    request.offsets[0] = line;
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    request.num_lines = 1;
    std::strncpy(request.consumer, "gar-stream-rx", sizeof(request.consumer) - 1);
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const auto message = std::string(std::strerror(errno));
        close(chip_fd);
        throw std::runtime_error("cannot request GPIO edge " + std::to_string(line) + ": " + message);
    }
    close(chip_fd);
    fd_ = request.fd;
}

GpioFallingEdge::~GpioFallingEdge() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

int GpioFallingEdge::fd() const noexcept {
    return fd_;
}

bool GpioFallingEdge::consume() {
    gpio_v2_line_event event{};
    const auto count = read(fd_, &event, sizeof(event));
    return count == static_cast<ssize_t>(sizeof(event)) &&
           event.id == GPIO_V2_LINE_EVENT_FALLING_EDGE;
}

}  // namespace gar::stream::rx
