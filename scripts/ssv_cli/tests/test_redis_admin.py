from __future__ import annotations

import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from scripts.ssv_cli.config import RedisSettings
from scripts.ssv_cli.services.redis_admin import (
    RedisAdmin,
    RedisCleanupError,
    RedisCommandError,
    RedisConnection,
    RedisError,
)


class FakeRedis:
    def __init__(self, responses: list[object]) -> None:
        self.responses = list(responses)
        self.commands: list[tuple[object, ...]] = []

    def execute(self, *args: object) -> object:
        self.commands.append(args)
        if not self.responses:
            raise AssertionError(f"unexpected command: {args}")
        response = self.responses.pop(0)
        if isinstance(response, BaseException):
            raise response
        return response

    def close(self) -> None:
        return None


def fake_redis_module(client: MagicMock) -> SimpleNamespace:
    class RedisPyError(Exception):
        pass

    class ResponseError(RedisPyError):
        pass

    return SimpleNamespace(
        Redis=MagicMock(return_value=client),
        exceptions=SimpleNamespace(
            ResponseError=ResponseError,
            RedisError=RedisPyError,
        ),
    )


class RedisAdminTest(unittest.TestCase):
    def test_status_reads_stream_length_and_pending_summary(self) -> None:
        redis = FakeRedis([42, [3, "1-0", "1-0", 1]])
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        status = admin.status()
        self.assertEqual(status.stream_length, 42)
        self.assertEqual(status.pending, 3)
        self.assertEqual(
            redis.commands,
            [("XLEN", "ssv:events"), ("XPENDING", "ssv:events", "ssv-agent")],
        )

    def test_status_accepts_redis_py_pending_summary(self) -> None:
        redis = FakeRedis([42, {"pending": 3, "min": "1-0", "max": "1-0", "consumers": []}])
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        self.assertEqual(admin.status().pending, 3)

    def test_clean_accepts_redis_py_pending_range_records(self) -> None:
        redis = FakeRedis(
            [
                {"pending": 1, "min": "1-0", "max": "1-0", "consumers": []},
                [
                    {
                        "message_id": "1-0",
                        "consumer": "worker-a",
                        "time_since_delivered": 100,
                        "times_delivered": 1,
                    }
                ],
                1,
            ]
        )
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        result = admin.clean()
        self.assertEqual(result.acknowledged, 1)
        self.assertEqual(
            redis.commands,
            [
                ("XPENDING", "ssv:events", "ssv-agent"),
                ("XPENDING", "ssv:events", "ssv-agent", "-", "+", 500),
                ("XACK", "ssv:events", "ssv-agent", "1-0"),
            ],
        )

    def test_dry_run_does_not_ack_or_trim(self) -> None:
        redis = FakeRedis([[2, "1-0", "1-0", 1]])
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        result = admin.clean(include_stream=True, dry_run=True)
        self.assertEqual(result.pending_before, 2)
        self.assertEqual(result.acknowledged, 0)
        self.assertEqual(result.trimmed, 0)
        self.assertEqual(redis.commands, [("XPENDING", "ssv:events", "ssv-agent")])

    def test_clean_acks_pending_in_batches_then_trims(self) -> None:
        redis = FakeRedis(
            [
                [3, "1-0", "1-0", 1],
                [("1-0", "worker-a", 100, 1), ("1-1", "worker-a", 100, 1)],
                2,
                [("1-2", "worker-b", 100, 1)],
                1,
                3,
            ]
        )
        admin = RedisAdmin(
            redis,
            stream_key="ssv:events",
            consumer_group="ssv-agent",
            batch_size=2,
        )
        result = admin.clean(include_stream=True)
        self.assertEqual(result.pending_before, 3)
        self.assertEqual(result.acknowledged, 3)
        self.assertEqual(result.trimmed, 3)
        self.assertEqual(
            redis.commands,
            [
                ("XPENDING", "ssv:events", "ssv-agent"),
                ("XPENDING", "ssv:events", "ssv-agent", "-", "+", 2),
                ("XACK", "ssv:events", "ssv-agent", "1-0", "1-1"),
                ("XPENDING", "ssv:events", "ssv-agent", "(1-1", "+", 2),
                ("XACK", "ssv:events", "ssv-agent", "1-2"),
                ("XTRIM", "ssv:events", "MAXLEN", "=", 0),
            ],
        )

    def test_empty_pending_skips_range_and_ack(self) -> None:
        redis = FakeRedis([[0, "", "", 0]])
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        result = admin.clean()
        self.assertEqual(result.acknowledged, 0)
        self.assertEqual(redis.commands, [("XPENDING", "ssv:events", "ssv-agent")])

    def test_cleanup_error_keeps_completed_ack_statistics(self) -> None:
        redis = FakeRedis(
            [
                [2, "1-0", "1-1", 1],
                [("1-0", "worker-a", 100, 1), ("1-1", "worker-a", 100, 1)],
                2,
                RedisCommandError("XTRIM failed"),
            ]
        )
        admin = RedisAdmin(redis, stream_key="ssv:events", consumer_group="ssv-agent")
        with self.assertRaises(RedisCleanupError) as raised:
            admin.clean(include_stream=True)
        self.assertEqual(raised.exception.result.pending_before, 2)
        self.assertEqual(raised.exception.result.acknowledged, 2)
        self.assertEqual(raised.exception.result.trimmed, 0)


class RedisConnectionTest(unittest.TestCase):
    def test_connection_delegates_to_redis_py_and_closes_client(self) -> None:
        client = MagicMock()
        client.execute_command.side_effect = ["PONG", 4]
        redis_module = fake_redis_module(client)
        settings = RedisSettings(db=2, password="secret")

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), RedisConnection(settings, timeout=7) as connection:
            self.assertEqual(connection.execute("PING"), "PONG")
            self.assertEqual(connection.execute("XLEN", "ssv:events"), 4)

        redis_module.Redis.assert_called_once_with(
            host="localhost",
            port=6379,
            db=2,
            password="secret",
            socket_timeout=7,
            socket_connect_timeout=7,
            decode_responses=True,
        )
        self.assertEqual(
            client.execute_command.call_args_list[0].args,
            ("PING",),
        )
        self.assertEqual(
            client.execute_command.call_args_list[1].args,
            ("XLEN", "ssv:events"),
        )
        self.assertEqual(client.execute_command.call_args_list[0].kwargs, {})
        self.assertEqual(client.execute_command.call_args_list[1].kwargs, {})
        client.close.assert_called_once_with()

    def test_xpending_range_enables_redis_py_detail_parser(self) -> None:
        client = MagicMock()
        client.execute_command.return_value = []
        redis_module = fake_redis_module(client)

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), RedisConnection(RedisSettings()) as connection:
            self.assertEqual(
                connection.execute("XPENDING", "ssv:events", "ssv-agent", "-", "+", 10),
                [],
            )

        client.execute_command.assert_called_once_with(
            "XPENDING",
            "ssv:events",
            "ssv-agent",
            "-",
            "+",
            10,
            parse_detail=True,
        )

    def test_xpending_summary_keeps_redis_py_summary_parser(self) -> None:
        client = MagicMock()
        client.execute_command.return_value = {"pending": 0}
        redis_module = fake_redis_module(client)

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), RedisConnection(RedisSettings()) as connection:
            self.assertEqual(connection.execute("XPENDING", "ssv:events", "ssv-agent"), {"pending": 0})

        client.execute_command.assert_called_once_with("XPENDING", "ssv:events", "ssv-agent")

    def test_redis_command_error_is_preserved_without_closing_client(self) -> None:
        client = MagicMock()
        redis_module = fake_redis_module(client)
        client.execute_command.side_effect = redis_module.exceptions.ResponseError("NOGROUP")

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), self.assertRaises(RedisCommandError) as raised:
            RedisConnection(RedisSettings()).execute("XPENDING", "ssv:events", "ssv-agent")

        self.assertEqual(str(raised.exception), "NOGROUP")
        client.close.assert_not_called()

    def test_redis_transport_error_closes_client_and_is_normalized(self) -> None:
        client = MagicMock()
        redis_module = fake_redis_module(client)
        client.execute_command.side_effect = redis_module.exceptions.RedisError("timeout")

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), self.assertRaises(RedisError) as raised:
            RedisConnection(RedisSettings()).execute("PING")

        self.assertIn("PING", str(raised.exception))
        self.assertIn("timeout", str(raised.exception))
        client.close.assert_called_once_with()

    def test_os_error_closes_client_and_is_normalized(self) -> None:
        client = MagicMock()
        redis_module = fake_redis_module(client)
        client.execute_command.side_effect = OSError("socket closed")

        with patch(
            "scripts.ssv_cli.services.redis_admin._load_redis_module",
            return_value=redis_module,
        ), self.assertRaises(RedisError) as raised:
            RedisConnection(RedisSettings()).execute("PING")

        self.assertIn("PING", str(raised.exception))
        self.assertIn("socket closed", str(raised.exception))
        client.close.assert_called_once_with()

    def test_missing_redis_py_is_reported_as_cli_redis_error(self) -> None:
        missing = ModuleNotFoundError("No module named 'redis'")
        with patch(
            "scripts.ssv_cli.services.redis_admin.importlib.import_module",
            side_effect=missing,
        ), self.assertRaises(RedisError) as raised:
            RedisConnection(RedisSettings()).execute("PING")
        self.assertIn("redis-py", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
