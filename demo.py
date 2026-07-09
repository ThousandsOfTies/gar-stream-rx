"""Turn the KY-040 -> background hue + big counter change on the ILI9341.
Press the KY-040 -> counter resets to 0 and the screen flashes white.

Fill in CONFIG below with the GPIO numbers reported by `luckfox-config show`
after you've routed SPI0 + free GPIOs with `luckfox-config` (see README.md).
"""
import colorsys
import threading
import time

from ili9341 import ILI9341
from ky040 import KY040

CONFIG = {
    "spi_bus": 0,
    "spi_device": 0,
    "spi_max_hz": 10_000_000,  # matches the Lyra's default spidev spi-max-frequency
    "dc_gpio": None,           # <- fill in from `luckfox-config show`
    "rst_gpio": None,          # <- fill in from `luckfox-config show`
    "enc_clk_gpio": None,      # <- fill in from `luckfox-config show`
    "enc_dt_gpio": None,       # <- fill in from `luckfox-config show`
    "enc_sw_gpio": None,       # <- fill in from `luckfox-config show`
}

# 5x7 bitmap font, digits + minus sign only (all this demo needs).
FONT = {
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": [".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "#....", "####.", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", "..#.."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
}


def _glyph_buffer(glyph, scale, fg, bg):
    buf = bytearray()
    for row in glyph:
        # each source pixel becomes `scale` pixels wide
        row_pixels = b"".join((fg if ch == "#" else bg) * scale for ch in row)
        # repeating this one scaled row `scale` times = `scale` identical output rows
        buf += row_pixels * scale
    return bytes(buf)


def draw_text(display, x, y, text, scale, fg, bg):
    glyph_w, glyph_h = 5 * scale, 7 * scale
    cursor_x = x
    for ch in text:
        glyph = FONT.get(ch, FONT[" "])
        buf = _glyph_buffer(glyph, scale, fg, bg)
        display.blit(cursor_x, y, glyph_w, glyph_h, buf)
        cursor_x += glyph_w + scale  # 1-glyph-scale of spacing


def hue_to_rgb565(display, hue_degrees):
    r, g, b = colorsys.hsv_to_rgb((hue_degrees % 360) / 360.0, 0.85, 1.0)
    return display.rgb565(int(r * 255), int(g * 255), int(b * 255))


def luminance(hue_degrees):
    r, g, b = colorsys.hsv_to_rgb((hue_degrees % 360) / 360.0, 0.85, 1.0)
    return 0.299 * r + 0.587 * g + 0.114 * b


def main():
    missing = [k for k in ("dc_gpio", "rst_gpio", "enc_clk_gpio", "enc_dt_gpio", "enc_sw_gpio")
               if CONFIG[k] is None]
    if missing:
        raise SystemExit(
            "Fill in CONFIG%s in demo.py first - run `luckfox-config show` on the "
            "board to find these GPIO numbers (see README.md)." % missing
        )

    display = ILI9341(
        CONFIG["spi_bus"], CONFIG["spi_device"],
        CONFIG["dc_gpio"], CONFIG["rst_gpio"],
        spi_max_hz=CONFIG["spi_max_hz"], rotation=1, bgr=True,
    )

    lock = threading.Lock()
    state = {"counter": 0}

    def redraw():
        hue = (state["counter"] * 15) % 360
        bg = hue_to_rgb565(display, hue)
        fg = display.rgb565(0, 0, 0) if luminance(hue) > 0.6 else display.rgb565(255, 255, 255)
        with lock:
            display.fill_screen(bg)
            text = str(state["counter"])
            scale = 8
            text_w = len(text) * (5 * scale + scale) - scale
            x = max(0, (display.width - text_w) // 2)
            y = max(0, (display.height - 7 * scale) // 2)
            draw_text(display, x, y, text, scale, fg, bg)

    def on_rotate(direction, counter):
        state["counter"] = counter
        redraw()

    def on_press():
        with lock:
            display.fill_screen(display.rgb565(255, 255, 255))
        time.sleep(0.1)
        state["counter"] = 0
        redraw()

    redraw()

    encoder = KY040(
        CONFIG["enc_clk_gpio"], CONFIG["enc_dt_gpio"], CONFIG["enc_sw_gpio"],
        on_rotate=on_rotate, on_press=on_press,
    )
    encoder.start()

    print("Running. Turn the KY-040 knob, press it to reset. Ctrl+C to quit.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        encoder.stop()
        display.close()


if __name__ == "__main__":
    main()
