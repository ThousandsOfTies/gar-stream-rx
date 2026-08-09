from __future__ import annotations

import json
import socket
import threading
import unittest

from source_browser import PROTOCOL, SourceBrowser, parse_discovery_peers


class SourceBrowserTest(unittest.TestCase):
    def test_discovers_and_requests_selected_source(self) -> None:
        changed = threading.Event()
        snapshots = []

        def on_sources_changed(sources):
            snapshots.append(sources)
            changed.set()

        fake_tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fake_tx.bind(("127.0.0.1", 0))
        fake_tx.settimeout(2)
        control_port = fake_tx.getsockname()[1]
        browser = SourceBrowser(
            "rx-one",
            discovery_port=0,
            stream_port=6200,
            query_targets=(),
            query_interval=60,
            on_sources_changed=on_sources_changed,
        )
        browser.start()
        try:
            announcement = {
                "protocol": PROTOCOL,
                "type": "source_announce",
                "source_id": "tx-one",
                "source_name": "Kitchen TX",
                "control_port": control_port,
                "transport": "rtp/udp",
                "encoding": "JPEG",
                "payload": 26,
            }
            fake_tx.sendto(
                json.dumps(announcement).encode(),
                ("127.0.0.1", browser.discovery_port),
            )
            self.assertTrue(changed.wait(2))
            self.assertEqual("tx-one", snapshots[-1][0].source_id)
            self.assertEqual("Kitchen TX", snapshots[-1][0].name)
            self.assertTrue(snapshots[-1][0].online)

            self.assertTrue(browser.select_source("tx-one"))
            payload, _address = fake_tx.recvfrom(4096)
            request = json.loads(payload)
            self.assertEqual("stream_request", request["type"])
            self.assertEqual("tx-one", request["source_id"])
            self.assertEqual("rx-one", request["receiver_id"])
            self.assertEqual(6200, request["stream_port"])
        finally:
            browser.stop()
            fake_tx.close()

    def test_keeps_multiple_channels_and_moves_request_when_selected(self) -> None:
        changed = threading.Event()
        fake_tx_one = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fake_tx_two = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fake_tx_one.bind(("127.0.0.1", 0))
        fake_tx_two.bind(("127.0.0.1", 0))
        fake_tx_one.settimeout(2)
        fake_tx_two.settimeout(2)
        browser = SourceBrowser(
            "rx-one",
            discovery_port=0,
            stream_port=6200,
            query_targets=(),
            query_interval=60,
            on_sources_changed=lambda _sources: changed.set(),
        )
        browser.start()
        try:
            for source_id, source_name, control_port in (
                ("tx-one", "Kitchen TX", fake_tx_one.getsockname()[1]),
                ("tx-two", "Workshop TX", fake_tx_two.getsockname()[1]),
            ):
                announcement = {
                    "protocol": PROTOCOL,
                    "type": "source_announce",
                    "source_id": source_id,
                    "source_name": source_name,
                    "control_port": control_port,
                    "transport": "rtp/udp",
                    "encoding": "JPEG",
                    "payload": 26,
                }
                changed.clear()
                fake_tx_one.sendto(
                    json.dumps(announcement).encode(),
                    ("127.0.0.1", browser.discovery_port),
                )
                self.assertTrue(changed.wait(2))

            self.assertEqual(
                ("tx-one", "tx-two"),
                tuple(source.source_id for source in browser.sources),
            )

            self.assertTrue(browser.select_source("tx-one"))
            request_one = json.loads(fake_tx_one.recv(4096))
            self.assertEqual("stream_request", request_one["type"])

            self.assertTrue(browser.select_source("tx-two"))
            stop_one = json.loads(fake_tx_one.recv(4096))
            request_two = json.loads(fake_tx_two.recv(4096))
            self.assertEqual("stream_stop", stop_one["type"])
            self.assertEqual("stream_request", request_two["type"])
            self.assertEqual("tx-two", browser.selected_source_id)
        finally:
            browser.stop()
            fake_tx_one.close()
            fake_tx_two.close()

    def test_parse_discovery_peers(self) -> None:
        self.assertEqual(
            (("10.0.0.10", 5601), ("tx.example", 6001)),
            parse_discovery_peers("10.0.0.10, tx.example:6001"),
        )
        with self.assertRaises(ValueError):
            parse_discovery_peers("tx.example:70000")


if __name__ == "__main__":
    unittest.main()
