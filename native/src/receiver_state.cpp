#include "gar_stream_rx/receiver_state.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

namespace gar::stream::rx {
namespace {

constexpr std::array<MenuItem, 4> kMenuItems{
    MenuItem::source,
    MenuItem::brightness,
    MenuItem::contrast,
    MenuItem::exit,
};

std::string menu_item_name(MenuItem item) {
    switch (item) {
    case MenuItem::source:
        return "SOURCE";
    case MenuItem::brightness:
        return "BRIGHTNESS";
    case MenuItem::contrast:
        return "CONTRAST";
    case MenuItem::exit:
        return "EXIT";
    }
    return "";
}

std::string fixed_value(double value, bool show_sign) {
    std::ostringstream output;
    if (show_sign) {
        output << std::showpos;
    }
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}

std::size_t wrapped_index(std::size_t current, int direction, std::size_t count) {
    if (count == 0) {
        return 0;
    }
    const auto step = direction >= 0 ? std::size_t{1} : count - 1;
    return (current + step) % count;
}

}  // namespace

void ReceiverState::update_sources(std::vector<Source> sources) {
    std::sort(sources.begin(), sources.end(), [](const Source& left, const Source& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        return left.id < right.id;
    });
    sources_ = std::move(sources);
    const auto choices = source_choices();
    if (!choices.empty()) {
        source_index_ %= choices.size();
    }
}

bool ReceiverState::select_source(std::optional<std::string> source_id, bool emit_selection) {
    if (source_id) {
        const auto found = std::find_if(sources_.begin(), sources_.end(), [&source_id](const Source& source) {
            return source.id == *source_id;
        });
        if (found == sources_.end()) {
            return false;
        }
    }
    selected_source_id_ = std::move(source_id);
    if (emit_selection) {
        pending_source_selection_ = SourceSelection{selected_source_id_};
    }
    return true;
}

void ReceiverState::rotate(int direction) {
    if (direction == 0 || mode_ == UiMode::view) {
        return;
    }
    if (mode_ == UiMode::menu) {
        menu_index_ = wrapped_index(menu_index_, direction, kMenuItems.size());
        return;
    }

    switch (menu_item()) {
    case MenuItem::source: {
        const auto choices = source_choices();
        source_index_ = wrapped_index(source_index_, direction, choices.size());
        break;
    }
    case MenuItem::brightness:
        brightness_ = std::clamp(brightness_ + (direction >= 0 ? 0.05 : -0.05), -1.0, 1.0);
        break;
    case MenuItem::contrast:
        contrast_ = std::clamp(contrast_ + (direction >= 0 ? 0.05 : -0.05), 0.0, 2.0);
        break;
    case MenuItem::exit:
        break;
    }
}

void ReceiverState::press() {
    if (mode_ == UiMode::view) {
        mode_ = UiMode::menu;
        menu_index_ = 0;
        return;
    }
    if (mode_ == UiMode::menu) {
        if (menu_item() == MenuItem::exit) {
            mode_ = UiMode::view;
            return;
        }
        if (menu_item() == MenuItem::source) {
            const auto choices = source_choices();
            source_index_ = 0;
            if (selected_source_id_) {
                const auto found = std::find_if(
                    choices.begin(), choices.end(), [this](const Source& source) {
                        return source.id == *selected_source_id_;
                    }
                );
                if (found != choices.end()) {
                    source_index_ = static_cast<std::size_t>(std::distance(choices.begin(), found));
                }
            }
        }
        mode_ = UiMode::submenu;
        return;
    }

    if (menu_item() == MenuItem::source) {
        const auto choices = source_choices();
        if (source_index_ < choices.size()) {
            const auto& source = choices[source_index_];
            if (source.id.empty()) {
                static_cast<void>(select_source(std::nullopt));
            } else {
                static_cast<void>(select_source(source.id));
            }
        }
    }
    mode_ = UiMode::menu;
}

UiMode ReceiverState::mode() const noexcept {
    return mode_;
}

MenuItem ReceiverState::menu_item() const noexcept {
    return kMenuItems[menu_index_];
}

double ReceiverState::brightness() const noexcept {
    return brightness_;
}

double ReceiverState::contrast() const noexcept {
    return contrast_;
}

const std::optional<std::string>& ReceiverState::selected_source_id() const noexcept {
    return selected_source_id_;
}

std::string ReceiverState::source_label(const Source& source) const {
    return source.name + (source.online ? "" : " [OFF]");
}

std::string ReceiverState::selected_source_label() const {
    if (!selected_source_id_) {
        return "COLORBAR";
    }
    const auto found = std::find_if(sources_.begin(), sources_.end(), [this](const Source& source) {
        return source.id == *selected_source_id_;
    });
    return found == sources_.end() ? *selected_source_id_ : source_label(*found);
}

std::vector<Source> ReceiverState::source_choices() const {
    std::vector<Source> choices;
    choices.reserve(sources_.size() + 1);
    choices.push_back(Source{"", "COLORBAR", true});
    choices.insert(choices.end(), sources_.begin(), sources_.end());
    return choices;
}

std::vector<std::string> ReceiverState::menu_lines() const {
    if (mode_ == UiMode::view) {
        return {};
    }

    if (mode_ == UiMode::submenu) {
        const auto item = menu_item();
        std::vector<std::string> lines{"RX > " + menu_item_name(item)};
        if (item == MenuItem::source) {
            const auto choices = source_choices();
            for (std::size_t index = 0; index < choices.size(); ++index) {
                lines.push_back(
                    std::string(index == source_index_ ? "> " : "  ") + source_label(choices[index])
                );
            }
        } else if (item == MenuItem::brightness) {
            lines.push_back("> " + fixed_value(brightness_, true));
        } else if (item == MenuItem::contrast) {
            lines.push_back("> " + fixed_value(contrast_, false));
        }
        return lines;
    }

    std::vector<std::string> lines{"RX SETTINGS"};
    for (std::size_t index = 0; index < kMenuItems.size(); ++index) {
        const auto item = kMenuItems[index];
        std::string value;
        switch (item) {
        case MenuItem::source:
            value = selected_source_label();
            break;
        case MenuItem::brightness:
            value = fixed_value(brightness_, true);
            break;
        case MenuItem::contrast:
            value = fixed_value(contrast_, false);
            break;
        case MenuItem::exit:
            break;
        }
        std::string line = index == menu_index_ ? "> " : "  ";
        line += menu_item_name(item);
        if (!value.empty()) {
            line.append(11 - std::min<std::size_t>(11, menu_item_name(item).size()), ' ');
            line += value;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<SourceSelection> ReceiverState::take_pending_source_selection() {
    auto selection = std::move(pending_source_selection_);
    pending_source_selection_.reset();
    return selection;
}

}  // namespace gar::stream::rx
