"""Video monitor: GStreamer does the heavy lifting (decode/colorbar/scale/
brightness-contrast/OSD text), this script only does two things:

  1. Builds the pipeline and pulls finished RGB565 frames from an appsink,
     pushing each one straight to the ILI9341 over SPI.
  2. Turns KY-040 rotate/press events into GStreamer property changes
     (active input pad, videobalance brightness/contrast, textoverlay text).

Requires PyGObject + the GStreamer 1.0 typelib on the board (system packages
from Buildroot, not pip - see README.md "Buildroot packages needed").
"""
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib  # noqa: E402

from ili9341 import ILI9341  # noqa: E402
from ky040 import KY040  # noqa: E402

CONFIG = {
    "spi_bus": 0,
    "spi_device": 0,
    "spi_max_hz": 24_000_000,  # bump via `luckfox-config` SPI speed - see README bandwidth note
    "dc_gpio": None,           # <- fill in from `luckfox-config show`
    "rst_gpio": None,          # <- fill in from `luckfox-config show`
    "enc_clk_gpio": None,      # <- fill in from `luckfox-config show`
    "enc_dt_gpio": None,       # <- fill in from `luckfox-config show`
    "enc_sw_gpio": None,       # <- fill in from `luckfox-config show`
}

WIDTH, HEIGHT, FPS = 320, 240, 15

# TX = Raspberry Pi 5 + USB UVC camera (OV3660). That camera is a cheap UVC
# webcam - at 2048x1536 it can only be MJPEG (raw YUY2 at that size would blow
# past USB2.0 bandwidth), and MJPEG is also the right call for *this* RX: it's
# far cheaper to software-decode per-frame JPEG than H.264 on a Cortex-A7 with
# no hardware video decoder, and MJPEG's independent frames tolerate UDP packet
# loss much better than an H.264 GOP would. See README "Codec choice" section.
RX_PIPELINE_FRAGMENT = (
    "udpsrc port=5600 "
    'caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26" '
    "! rtpjitterbuffer latency=100 "
    "! rtpjpegdepay ! jpegdec "
    "! videoconvert ! videoscale "
    f"! video/x-raw,width={WIDTH},height={HEIGHT} "
    "! queue max-size-buffers=2 leaky=downstream "
    "! sel.sink_1"
)

COLORBAR_PIPELINE_FRAGMENT = (
    f"videotestsrc pattern=smpte is-live=true "
    f"! video/x-raw,width={WIDTH},height={HEIGHT},framerate={FPS}/1 "
    "! videoconvert "
    "! queue max-size-buffers=2 leaky=downstream "
    "! sel.sink_0"
)

SINK_CHAIN = (
    "input-selector name=sel "
    "! videobalance name=bal brightness=0.0 contrast=1.0 "
    '! textoverlay name=osd text="" silent=true valignment=top halignment=left '
    'font-desc="Sans 14" '
    "! videoconvert "
    f"! video/x-raw,format=RGB16,width={WIDTH},height={HEIGHT} "
    "! appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true"
)

MENU_ITEMS = ["INPUT", "BRIGHTNESS", "CONTRAST", "EXIT"]


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


class VideoMonitor:
    def __init__(self, display):
        self.display = display
        self.state = {
            "mode": "VIEW",       # VIEW | MENU | ADJUST
            "source": "COLORBAR", # COLORBAR | RX
            "menu_index": 0,
            "brightness": 0.0,    # videobalance range: -1.0 .. 1.0
            "contrast": 1.0,      # videobalance range: 0.0 .. 2.0
        }

        pipeline_str = " ".join([COLORBAR_PIPELINE_FRAGMENT, RX_PIPELINE_FRAGMENT, SINK_CHAIN])
        self.pipeline = Gst.parse_launch(pipeline_str)

        self.sel = self.pipeline.get_by_name("sel")
        self.bal = self.pipeline.get_by_name("bal")
        self.osd = self.pipeline.get_by_name("osd")
        self.sink = self.pipeline.get_by_name("sink")

        self.pad_colorbar = self.sel.get_static_pad("sink_0")
        self.pad_rx = self.sel.get_static_pad("sink_1")

        self.sink.connect("new-sample", self._on_new_sample)

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_bus_message)

    # -- GStreamer callbacks ------------------------------------------------
    def _on_new_sample(self, sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buf = sample.get_buffer()
        ok, mapinfo = buf.map(Gst.MapFlags.READ)
        if ok:
            try:
                self.display.blit(0, 0, WIDTH, HEIGHT, bytes(mapinfo.data))
            finally:
                buf.unmap(mapinfo)
        return Gst.FlowReturn.OK

    def _on_bus_message(self, bus, message):
        if message.type == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            # Log and keep running - e.g. a temporarily missing RX stream
            # shouldn't take down the whole monitor, colorbar stays usable.
            print(f"[gst error] {err}: {debug}", file=sys.stderr)
        elif message.type == Gst.MessageType.EOS:
            print("[gst] end of stream", file=sys.stderr)

    # -- KY-040 callbacks -----------------------------------------------
    def apply_source(self):
        pad = self.pad_colorbar if self.state["source"] == "COLORBAR" else self.pad_rx
        self.sel.set_property("active-pad", pad)

    def toggle_source(self):
        self.state["source"] = "RX" if self.state["source"] == "COLORBAR" else "COLORBAR"
        self.apply_source()

    def update_osd_text(self):
        lines = []
        for i, item in enumerate(MENU_ITEMS):
            cursor = ">" if i == self.state["menu_index"] else " "
            if item == "INPUT":
                value = self.state["source"]
            elif item == "BRIGHTNESS":
                value = f"{self.state['brightness']:+.2f}"
            elif item == "CONTRAST":
                value = f"{self.state['contrast']:.2f}"
            else:
                value = ""
            adj = " [adjusting]" if (self.state["mode"] == "ADJUST" and i == self.state["menu_index"]) else ""
            lines.append(f"{cursor} {item} {value}{adj}".rstrip())
        self.osd.set_property("text", "\n".join(lines))

    def on_rotate(self, direction, _counter):
        mode = self.state["mode"]
        if mode == "VIEW":
            self.toggle_source()
        elif mode == "MENU":
            self.state["menu_index"] = (self.state["menu_index"] + direction) % len(MENU_ITEMS)
            self.update_osd_text()
        elif mode == "ADJUST":
            item = MENU_ITEMS[self.state["menu_index"]]
            if item == "INPUT":
                self.toggle_source()
            elif item == "BRIGHTNESS":
                self.state["brightness"] = clamp(self.state["brightness"] + direction * 0.05, -1.0, 1.0)
                self.bal.set_property("brightness", self.state["brightness"])
            elif item == "CONTRAST":
                self.state["contrast"] = clamp(self.state["contrast"] + direction * 0.05, 0.0, 2.0)
                self.bal.set_property("contrast", self.state["contrast"])
            self.update_osd_text()

    def on_press(self):
        mode = self.state["mode"]
        if mode == "VIEW":
            self.state["mode"] = "MENU"
            self.state["menu_index"] = 0
            self.osd.set_property("silent", False)
            self.update_osd_text()
        elif mode == "MENU":
            item = MENU_ITEMS[self.state["menu_index"]]
            if item == "EXIT":
                self.state["mode"] = "VIEW"
                self.osd.set_property("silent", True)
            else:
                self.state["mode"] = "ADJUST"
                self.update_osd_text()
        elif mode == "ADJUST":
            self.state["mode"] = "MENU"
            self.update_osd_text()

    def start(self):
        self.apply_source()
        self.pipeline.set_state(Gst.State.PLAYING)

    def stop(self):
        self.pipeline.set_state(Gst.State.NULL)


def main():
    missing = [k for k in ("dc_gpio", "rst_gpio", "enc_clk_gpio", "enc_dt_gpio", "enc_sw_gpio")
               if CONFIG[k] is None]
    if missing:
        raise SystemExit(
            "Fill in CONFIG%s in video_monitor.py first - run `luckfox-config show` "
            "on the board to find these GPIO numbers (see README.md)." % missing
        )

    Gst.init(None)

    display = ILI9341(
        CONFIG["spi_bus"], CONFIG["spi_device"],
        CONFIG["dc_gpio"], CONFIG["rst_gpio"],
        spi_max_hz=CONFIG["spi_max_hz"], rotation=1, bgr=True,
    )

    monitor = VideoMonitor(display)

    encoder = KY040(
        CONFIG["enc_clk_gpio"], CONFIG["enc_dt_gpio"], CONFIG["enc_sw_gpio"],
        on_rotate=monitor.on_rotate, on_press=monitor.on_press,
    )
    encoder.start()
    monitor.start()

    loop = GLib.MainLoop()
    print("Running. Rotate KY-040 to switch INPUT (colorbar/RX), press to open the menu. Ctrl+C to quit.")
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        encoder.stop()
        monitor.stop()
        display.close()


if __name__ == "__main__":
    main()
