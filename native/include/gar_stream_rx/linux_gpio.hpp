#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace gar::stream::rx {

class GpioOutput {
  public:
    GpioOutput(const std::string& chip_path, std::uint32_t line, bool initial_value);
    ~GpioOutput();

    GpioOutput(const GpioOutput&) = delete;
    GpioOutput& operator=(const GpioOutput&) = delete;

    void set(bool value);

  private:
    int fd_{-1};
};

class GpioInput {
  public:
    GpioInput(const std::string& chip_path, std::uint32_t line);
    ~GpioInput();

    GpioInput(const GpioInput&) = delete;
    GpioInput& operator=(const GpioInput&) = delete;

    [[nodiscard]] bool get() const;

  private:
    int fd_{-1};
};

class GpioBothEdges {
  public:
    GpioBothEdges(const std::string& chip_path, std::uint32_t line);
    ~GpioBothEdges();

    GpioBothEdges(const GpioBothEdges&) = delete;
    GpioBothEdges& operator=(const GpioBothEdges&) = delete;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool get() const;
    std::optional<bool> consume();

  private:
    int fd_{-1};
};

class GpioFallingEdge {
  public:
    GpioFallingEdge(const std::string& chip_path, std::uint32_t line);
    ~GpioFallingEdge();

    GpioFallingEdge(const GpioFallingEdge&) = delete;
    GpioFallingEdge& operator=(const GpioFallingEdge&) = delete;

    [[nodiscard]] int fd() const noexcept;
    bool consume();

  private:
    int fd_{-1};
};

}  // namespace gar::stream::rx
