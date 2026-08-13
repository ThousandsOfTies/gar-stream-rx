"""Discover GarStream TX sources and request the selected source's stream."""

from __future__ import annotations

import json
import socket
import threading
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass

PROTOCOL = "gar-stream/1"
DEFAULT_DISCOVERY_PORT = 5601
DEFAULT_STREAM_PORT = 5600
DEFAULT_QUERY_INTERVAL = 2.0
DEFAULT_LEASE_SECONDS = 7.0
SOURCE_ONLINE_SECONDS = 6.0
SOURCE_RETENTION_SECONDS = 60.0
MAX_PACKET_SIZE = 4096


@dataclass(frozen=True)
class DiscoveredSource:
    """A TX channel retained by the RX for source selection."""

    source_id: str
    name: str
    host: str
    control_port: int
    online: bool


@dataclass
class _SourceRecord:
    source_id: str
    name: str
    host: str
    control_port: int
    last_seen: float


def _safe_identifier(value: object, *, maximum: int = 96) -> str | None:
    if not isinstance(value, str):
        return None
    value = value.strip()
    if not value or len(value) > maximum:
        return None
    if any(ord(character) < 32 for character in value):
        return None
    return value


def _decode_packet(payload: bytes) -> dict | None:
    try:
        message = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(message, dict) or message.get("protocol") != PROTOCOL:
        return None
    return message


def parse_discovery_peers(value: str, default_port: int = DEFAULT_DISCOVERY_PORT) -> tuple[tuple[str, int], ...]:
    """Parse comma-separated HOST or HOST:PORT discovery query destinations."""

    peers: list[tuple[str, int]] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        host, separator, port_text = item.rpartition(":")
        if separator and host and port_text.isdigit():
            port = int(port_text)
        else:
            host = item
            port = default_port
        if not host or not 1 <= port <= 65535:
            raise ValueError(f"invalid discovery peer: {item}")
        peers.append((host, port))
    return tuple(peers)


class SourceBrowser:
    """Maintain the RX channel list and renew the selected source lease."""

    def __init__(
        self,
        receiver_id: str,
        *,
        discovery_port: int = DEFAULT_DISCOVERY_PORT,
        stream_port: int = DEFAULT_STREAM_PORT,
        query_targets: Iterable[tuple[str, int]] | None = None,
        query_interval: float = DEFAULT_QUERY_INTERVAL,
        lease_seconds: float = DEFAULT_LEASE_SECONDS,
        on_sources_changed: Callable[[tuple[DiscoveredSource, ...]], None] | None = None,
    ) -> None:
        checked_id = _safe_identifier(receiver_id)
        if checked_id is None:
            raise ValueError("receiver_id must be a printable non-empty string")
        if not 0 <= discovery_port <= 65535:
            raise ValueError("discovery_port must be in the range 0..65535")
        if not 1 <= stream_port <= 65535:
            raise ValueError("stream_port must be in the range 1..65535")

        self.receiver_id = checked_id
        self.stream_port = stream_port
        self.query_interval = max(0.1, float(query_interval))
        self.lease_seconds = min(30.0, max(1.0, float(lease_seconds)))
        self.on_sources_changed = on_sources_changed
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._sources: dict[str, _SourceRecord] = {}
        self._selected_source_id: str | None = None
        self._last_snapshot: tuple[DiscoveredSource, ...] = ()

        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._socket.bind(("", discovery_port))
        self._socket.settimeout(0.2)
        self.discovery_port = int(self._socket.getsockname()[1])
        self.query_targets = tuple(
            query_targets
            if query_targets is not None
            else (("255.255.255.255", self.discovery_port),)
        )
        self._thread = threading.Thread(
            target=self._run,
            name=f"gar-stream-browser-{self.receiver_id}",
            daemon=True,
        )

    @property
    def selected_source_id(self) -> str | None:
        with self._lock:
            return self._selected_source_id

    @property
    def sources(self) -> tuple[DiscoveredSource, ...]:
        now = time.monotonic()
        with self._lock:
            return tuple(
                sorted(
                    (
                        DiscoveredSource(
                            source_id=record.source_id,
                            name=record.name,
                            host=record.host,
                            control_port=record.control_port,
                            online=now - record.last_seen <= SOURCE_ONLINE_SECONDS,
                        )
                        for record in self._sources.values()
                    ),
                    key=lambda source: (source.name.casefold(), source.source_id),
                )
            )

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self.select_source(None)
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._socket.close()

    def select_source(self, source_id: str | None) -> bool:
        with self._lock:
            previous_id = self._selected_source_id
            previous = self._sources.get(previous_id) if previous_id else None
            selected = self._sources.get(source_id) if source_id else None
            if source_id is not None and selected is None:
                return False
            self._selected_source_id = source_id
        if previous is not None and previous_id != source_id:
            self._send_stream_message(previous, "stream_stop")
        if selected is not None:
            self._send_stream_message(selected, "stream_request")
        return True

    def _query(self) -> None:
        payload = json.dumps(
            {
                "protocol": PROTOCOL,
                "type": "source_query",
                "receiver_id": self.receiver_id,
            },
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        for target in self.query_targets:
            try:
                self._socket.sendto(payload, target)
            except OSError:
                if not self._stop.is_set():
                    continue

    def _send_stream_message(self, source: _SourceRecord, message_type: str) -> None:
        payload = json.dumps(
            {
                "protocol": PROTOCOL,
                "type": message_type,
                "source_id": source.source_id,
                "receiver_id": self.receiver_id,
                "stream_port": self.stream_port,
                "lease_seconds": self.lease_seconds,
            },
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        try:
            self._socket.sendto(payload, (source.host, source.control_port))
        except OSError:
            pass

    def _handle_announcement(self, message: dict, address: tuple[str, int]) -> None:
        if message.get("type") != "source_announce":
            return
        source_id = _safe_identifier(message.get("source_id"))
        source_name = _safe_identifier(message.get("source_name"))
        if source_id is None or source_name is None:
            return
        if message.get("transport") != "rtp/udp" or message.get("encoding") != "JPEG":
            return
        try:
            control_port = int(message.get("control_port", address[1]))
        except (TypeError, ValueError):
            return
        if not 1 <= control_port <= 65535:
            return

        with self._lock:
            self._sources[source_id] = _SourceRecord(
                source_id=source_id,
                name=source_name,
                host=address[0],
                control_port=control_port,
                last_seen=time.monotonic(),
            )
        self._notify_if_changed()

    def _expire_sources(self) -> None:
        now = time.monotonic()
        with self._lock:
            expired = [
                source_id
                for source_id, source in self._sources.items()
                if now - source.last_seen > SOURCE_RETENTION_SECONDS
                and source_id != self._selected_source_id
            ]
            for source_id in expired:
                self._sources.pop(source_id, None)
        self._notify_if_changed()

    def _renew_selected(self) -> None:
        with self._lock:
            source = self._sources.get(self._selected_source_id)
        if source is not None:
            self._send_stream_message(source, "stream_request")

    def _notify_if_changed(self) -> None:
        snapshot = self.sources
        if snapshot == self._last_snapshot:
            return
        self._last_snapshot = snapshot
        if self.on_sources_changed is not None:
            self.on_sources_changed(snapshot)

    def _run(self) -> None:
        next_query = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_query:
                self._query()
                self._renew_selected()
                self._expire_sources()
                next_query = now + self.query_interval
            try:
                payload, address = self._socket.recvfrom(MAX_PACKET_SIZE)
            except TimeoutError:
                continue
            except OSError:
                break
            message = _decode_packet(payload)
            if message is not None:
                self._handle_announcement(message, address)
