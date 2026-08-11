#include "gar_stream_rx/ili9341.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace gar::stream::rx {
namespace {

constexpr std::uint8_t kColumnAddress = 0x2a;
constexpr std::uint8_t kPageAddress = 0x2b;
constexpr std::uint8_t kMemoryWrite = 0x2c;
constexpr std::uint8_t kMemoryAccess = 0x36;
constexpr std::size_t kSpiChunk = 4096;

struct InitCommand {
    std::uint8_t command;
    std::vector<std::uint8_t> data;
    int delay_ms;
};

const std::vector<InitCommand> kInitialization{
    {0x01, {}, 150},
    {0x28, {}, 10},
    {0xcf, {0x00, 0x83, 0x30}, 0},
    {0xed, {0x64, 0x03, 0x12, 0x81}, 0},
    {0xe8, {0x85, 0x01, 0x79}, 0},
    {0xcb, {0x39, 0x2c, 0x00, 0x34, 0x02}, 0},
    {0xf7, {0x20}, 0},
    {0xea, {0x00, 0x00}, 0},
    {0xc0, {0x26}, 0},
    {0xc1, {0x11}, 0},
    {0xc5, {0x35, 0x3e}, 0},
    {0xc7, {0xbe}, 0},
    {0x3a, {0x55}, 0},
    {0xb1, {0x00, 0x1b}, 0},
    {0xf2, {0x08}, 0},
    {0x26, {0x01}, 0},
    {0xe0, {0x1f, 0x1a, 0x18, 0x0a, 0x0f, 0x06, 0x45, 0x87, 0x32, 0x0a, 0x07, 0x02, 0x07, 0x05, 0x00}, 0},
    {0xe1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3a, 0x78, 0x4d, 0x05, 0x18, 0x0d, 0x38, 0x3a, 0x1f}, 0},
    {0x11, {}, 150},
    {0x29, {}, 100},
};

}  // namespace

Ili9341::Ili9341(
    std::string spi_path,
    std::uint32_t spi_speed_hz,
    std::string gpio_chip,
    std::uint32_t dc_line,
    std::uint32_t reset_line
)
    : dc_(gpio_chip, dc_line, true),
      reset_(gpio_chip, reset_line, true),
      transfer_buffer_(kWidth * kHeight * 2) {
    spi_fd_ = open(spi_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (spi_fd_ < 0) {
        throw std::runtime_error("cannot open " + spi_path + ": " + std::strerror(errno));
    }
    std::uint8_t mode = 0;
    std::uint8_t bits = 8;
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed_hz) < 0) {
        const auto message = std::string(std::strerror(errno));
        close(spi_fd_);
        spi_fd_ = -1;
        throw std::runtime_error("cannot configure SPI: " + message);
    }
    hard_reset();
    initialize();
}

Ili9341::~Ili9341() {
    if (spi_fd_ >= 0) {
        close(spi_fd_);
    }
}

void Ili9341::hard_reset() {
    reset_.set(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reset_.set(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reset_.set(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

void Ili9341::initialize() {
    for (const auto& item : kInitialization) {
        command(item.command, item.data);
        if (item.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(item.delay_ms));
        }
    }
    command(kMemoryAccess, {0x28});
}

void Ili9341::command(std::uint8_t value, const std::vector<std::uint8_t>& data) {
    dc_.set(false);
    write_all(&value, 1);
    if (!data.empty()) {
        dc_.set(true);
        write_all(data.data(), data.size());
    }
}

void Ili9341::set_window(int x0, int y0, int x1, int y1) {
    command(kColumnAddress, {
        static_cast<std::uint8_t>(x0 >> 8), static_cast<std::uint8_t>(x0),
        static_cast<std::uint8_t>(x1 >> 8), static_cast<std::uint8_t>(x1),
    });
    command(kPageAddress, {
        static_cast<std::uint8_t>(y0 >> 8), static_cast<std::uint8_t>(y0),
        static_cast<std::uint8_t>(y1 >> 8), static_cast<std::uint8_t>(y1),
    });
    command(kMemoryWrite);
}

void Ili9341::write_all(const std::uint8_t* data, std::size_t size) {
    for (std::size_t offset = 0; offset < size;) {
        const auto count = std::min(kSpiChunk, size - offset);
        const auto written = write(spi_fd_, data + offset, count);
        if (written < 0) {
            throw std::runtime_error("SPI write failed: " + std::string(std::strerror(errno)));
        }
        if (written == 0) {
            throw std::runtime_error("SPI write returned zero bytes");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void Ili9341::blit_native_rgb565(const std::uint8_t* pixels, std::size_t size) {
    if (size != transfer_buffer_.size()) {
        throw std::invalid_argument("RGB565 frame must be exactly 320x240 pixels");
    }
    std::lock_guard lock(mutex_);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (std::size_t index = 0; index < size; index += 2) {
        transfer_buffer_[index] = pixels[index + 1];
        transfer_buffer_[index + 1] = pixels[index];
    }
#else
    std::copy(pixels, pixels + size, transfer_buffer_.begin());
#endif
    set_window(0, 0, kWidth - 1, kHeight - 1);
    dc_.set(true);
    write_all(transfer_buffer_.data(), transfer_buffer_.size());
}

}  // namespace gar::stream::rx
