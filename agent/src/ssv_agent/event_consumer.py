from __future__ import annotations

import json
import signal
import time

import structlog
from redis import Redis

from ssv_agent.config import SsvConfig

logger = structlog.get_logger()


class EventConsumer:
    """Minimal Redis Streams consumer that reads detection events and logs them.

    M2 scope: consume and print.  Full state-machine orchestration arrives in M6.
    """

    def __init__(self, config: SsvConfig) -> None:
        self._stream = config.redis.stream_key
        self._group = config.redis.consumer_group
        self._running = False
        self._redis = Redis(
            host=config.redis.host,
            port=config.redis.port,
            db=config.redis.db,
            decode_responses=True,
        )

    def _ensure_group(self) -> None:
        """Create the consumer group if it does not exist."""
        try:
            self._redis.xgroup_create(self._stream, self._group, id="0", mkstream=True)
            logger.info("created consumer group", group=self._group, stream=self._stream)
        except Exception:
            # Group already exists — that's fine.
            pass

    def start(self) -> None:
        """Blocking consumer loop.  Returns on SIGINT/SIGTERM."""
        self._running = True
        self._ensure_group()
        consumer_name = "ssv-agent-0"

        logger.info(
            "event consumer started",
            stream=self._stream,
            group=self._group,
            consumer=consumer_name,
        )

        while self._running:
            try:
                entries = self._redis.xreadgroup(
                    self._group,
                    consumer_name,
                    {self._stream: ">"},
                    count=10,
                    block=1000,  # 1 s
                )
            except Exception as exc:
                logger.warning("redis read error", error=str(exc))
                time.sleep(2)
                continue

            for _stream_name, messages in entries:
                for msg_id, fields in messages:
                    self._handle_event(msg_id, fields)

    def stop(self) -> None:
        self._running = False

    def _handle_event(self, msg_id: str, fields: dict[str, str]) -> None:
        raw = fields.get("event", "{}")
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            logger.warning("malformed event", msg_id=msg_id, raw=raw)
            return

        detections = event.get("detections", [])
        det_summary = ", ".join(
            f"{d['class']}({d['confidence']:.2f}"
            + (f", track={d['track_id']}" if d.get("track_id", -1) >= 0 else "")
            + ")"
            for d in detections
        )

        logger.info(
            "detection event",
            msg_id=msg_id,
            source=event.get("source", "?"),
            frame_id=event.get("frame_id"),
            detections=det_summary,
            count=len(detections),
        )

        # ACK after processing
        self._redis.xack(self._stream, self._group, msg_id)


def run_consumer(config: SsvConfig) -> None:
    """Run the event consumer with graceful shutdown."""
    consumer = EventConsumer(config)

    def _shutdown(sig: int, _frame: object) -> None:
        logger.info("received signal, stopping consumer", signal=sig)
        consumer.stop()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    consumer.start()
