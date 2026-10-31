#pragma once

#include <linux/gpio.h>
#include <cstdint>
#include <stdexcept>

namespace gar::stream::rx {
inline gpio_v2_line_config input_edge_config(std::uint64_t flags, std::uint32_t debounce_us) {
    if (debounce_us > 1000000U) {
        throw std::invalid_argument("GPIO debounce must be 0..1000000 microseconds");
    }
    gpio_v2_line_config config{};
    config.flags = flags;
    if (debounce_us != 0U) {
        config.num_attrs = 1;
        config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
        config.attrs[0].attr.debounce_period_us = debounce_us;
        config.attrs[0].mask = 1;
    }
    return config;
}
}  // namespace gar::stream::rx
