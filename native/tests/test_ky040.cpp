#include "gar_stream_rx/ky040.hpp"

#include <cassert>
#include <optional>

using gar::stream::rx::QuadratureDecoder;

namespace {

void expect_none(const std::optional<int>& value) {
    assert(!value.has_value());
}

}  // namespace

int main() {
    {
        QuadratureDecoder decoder(true, true);
        expect_none(decoder.update(false, true));
        expect_none(decoder.update(false, false));
        expect_none(decoder.update(true, false));
        assert(decoder.update(true, true) == std::optional<int>{1});
    }
    {
        QuadratureDecoder decoder(true, true);
        expect_none(decoder.update(true, false));
        expect_none(decoder.update(false, false));
        expect_none(decoder.update(false, true));
        assert(decoder.update(true, true) == std::optional<int>{-1});
    }
    {
        QuadratureDecoder decoder(true, true);
        // CLK contact bounces away from and back to the detent before the
        // real clockwise sequence. The bounce must not emit a UI step.
        expect_none(decoder.update(false, true));
        expect_none(decoder.update(true, true));
        expect_none(decoder.update(false, true));
        expect_none(decoder.update(false, false));
        expect_none(decoder.update(true, false));
        assert(decoder.update(true, true) == std::optional<int>{1});
    }
    {
        QuadratureDecoder decoder(true, true);
        // Skipping directly across both phases is invalid and must not be
        // interpreted as rotation.
        expect_none(decoder.update(false, false));
        expect_none(decoder.update(true, true));
    }
    return 0;
}
