#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gar::stream::rx {

struct Source {
    std::string id;
    std::string name;
    bool online{true};

    bool operator==(const Source& other) const {
        return id == other.id && name == other.name && online == other.online;
    }
};

enum class UiMode {
    view,
    menu,
    submenu,
};

enum class MenuItem {
    source,
    brightness,
    contrast,
    exit,
};

struct SourceSelection {
    // std::nullopt represents the local COLORBAR source.
    std::optional<std::string> source_id;
};

class ReceiverState {
  public:
    void update_sources(std::vector<Source> sources);
    bool select_source(std::optional<std::string> source_id, bool emit_selection = true);
    void rotate(int direction);
    void press();

    [[nodiscard]] UiMode mode() const noexcept;
    [[nodiscard]] MenuItem menu_item() const noexcept;
    [[nodiscard]] double brightness() const noexcept;
    [[nodiscard]] double contrast() const noexcept;
    [[nodiscard]] const std::optional<std::string>& selected_source_id() const noexcept;
    [[nodiscard]] std::string selected_source_label() const;
    [[nodiscard]] std::vector<std::string> menu_lines() const;
    [[nodiscard]] std::optional<SourceSelection> take_pending_source_selection();

  private:
    [[nodiscard]] std::vector<Source> source_choices() const;
    [[nodiscard]] std::string source_label(const Source& source) const;

    UiMode mode_{UiMode::view};
    std::size_t menu_index_{0};
    std::size_t source_index_{0};
    double brightness_{0.0};
    double contrast_{1.0};
    std::optional<std::string> selected_source_id_;
    std::vector<Source> sources_;
    std::optional<SourceSelection> pending_source_selection_;
};

}  // namespace gar::stream::rx
