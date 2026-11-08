#include "gar_stream_rx/gpio_config.hpp"
#include <cassert>

int main() {
    using gar::stream::rx::input_edge_config;
    constexpr auto flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                           GPIO_V2_LINE_FLAG_EDGE_FALLING;
    const auto disabled = input_edge_config(flags, 0);
    assert(disabled.flags == flags && disabled.num_attrs == 0);
    const auto enabled = input_edge_config(flags, 1000);
    assert(enabled.flags == flags && enabled.num_attrs == 1);
    assert(enabled.attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_DEBOUNCE);
    assert(enabled.attrs[0].attr.debounce_period_us == 1000);
    assert(enabled.attrs[0].mask == 1);
    bool rejected = false;
    try { (void)input_edge_config(flags, 1000001); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    return 0;
}
