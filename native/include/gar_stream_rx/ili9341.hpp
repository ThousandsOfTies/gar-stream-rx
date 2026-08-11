#pragma once

#include "gar_stream_rx/linux_gpio.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gar::stream::rx {

class Ili9341 {
  public:
    static constexpr int kWidth = 320;
    static constexpr int kHeight = 240;

    Ili9341(
        std::string spi_path,
        std::uint32_t spi_speed_hz,
        std::string gpio_chip,
        std::uint32_t dc_line,
        std::uint32_t reset_line
    );
    ~Ili9341();

    Ili9341(const Ili9341&) = delete;
    Ili9341& operator=(const Ili9341&) = delete;

    void blit_native_rgb565(const std::uint8_t* pixels, std::size_t size);

  private:
    void hard_reset();
    void initialize();
    void command(std::uint8_t value, const std::vector<std::uint8_t>& data = {});
    void set_window(int x0, int y0, int x1, int y1);
    void write_all(const std::uint8_t* data, std::size_t size);

    int spi_fd_{-1};
    GpioOutput dc_;
    GpioOutput reset_;
    std::mutex mutex_;
    std::vector<std::uint8_t> transfer_buffer_;
};

}  // namespace gar::stream::rx
