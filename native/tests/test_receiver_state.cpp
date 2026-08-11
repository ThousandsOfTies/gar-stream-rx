#include "gar_stream_rx/receiver_state.hpp"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

using gar::stream::rx::MenuItem;
using gar::stream::rx::ReceiverState;
using gar::stream::rx::Source;
using gar::stream::rx::UiMode;

int main() {
    ReceiverState state;
    assert(state.mode() == UiMode::view);
    assert(state.selected_source_label() == "COLORBAR");

    state.update_sources({
        Source{"tx-two", "Workshop TX", false},
        Source{"tx-one", "Kitchen TX", true},
    });
    state.press();
    assert(state.mode() == UiMode::menu);
    assert(state.menu_item() == MenuItem::source);
    state.press();
    assert(state.mode() == UiMode::submenu);
    state.rotate(1);
    state.press();
    assert(state.mode() == UiMode::menu);
    assert(state.selected_source_id() == std::optional<std::string>{"tx-one"});
    const auto selection = state.take_pending_source_selection();
    assert(selection.has_value());
    assert(selection->source_id == std::optional<std::string>{"tx-one"});

    state.rotate(1);
    assert(state.menu_item() == MenuItem::brightness);
    state.press();
    state.rotate(1);
    assert(std::abs(state.brightness() - 0.05) < 0.0001);
    state.press();

    state.rotate(1);
    assert(state.menu_item() == MenuItem::contrast);
    state.press();
    state.rotate(-1);
    assert(std::abs(state.contrast() - 0.95) < 0.0001);
    state.press();

    const auto lines = state.menu_lines();
    assert(lines.size() == 5);
    assert(lines.front() == "RX MENU");
    assert(lines[3].find("CONTRAST") != std::string::npos);

    state.rotate(1);
    assert(state.menu_item() == MenuItem::exit);
    state.press();
    assert(state.mode() == UiMode::view);
    assert(state.menu_lines().empty());
}
