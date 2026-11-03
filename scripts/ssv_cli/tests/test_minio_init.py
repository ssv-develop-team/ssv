from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


_FAKE_MC = r"""
import json
import os
import sys
from pathlib import Path

path = Path(os.environ["MC_TEST_STATE"])
state = json.loads(path.read_text())
args = [arg for arg in sys.argv[1:] if arg != "--json"]
operation = " ".join(args[:3]) if args[0] == "admin" else " ".join(args[:2])
state["calls"].append(operation)
path.write_text(json.dumps(state))
if operation == os.environ.get("MC_TEST_FAIL"):
    # 管理工具可能输出原始参数；初始化流程必须拦截，避免泄露凭据。
    print("injected: " + " ".join(args), file=sys.stderr)
    sys.exit(1)
if args[:2] == ["alias", "set"]:
    bypass_lists = [
        {
            item.strip().lower()
            for item in os.environ.get(name, "").split(",")
            if item.strip()
        }
        for name in ("NO_PROXY", "no_proxy")
    ]
    if any("minio" not in bypass for bypass in bypass_lists):
        print("internal MinIO request would use the configured proxy", file=sys.stderr)
        sys.exit(1)
elif args[:3] == ["admin", "user", "list"]:
    for key in state["users"]:
        print(json.dumps({"status": "success", "accessKey": key,
                          "secretKey": "", "policyName": state["users"][key]["policyName"],
                          "userStatus": "enabled"}))
elif args[:3] == ["admin", "user", "info"]:
    print(json.dumps({"status": "success", "accessKey": args[4],
                      "userStatus": "enabled", **state["users"][args[4]]}))
elif args[:3] == ["admin", "policy", "list"]:
    for name in state["policies"]:
        print(json.dumps({"status": "success", "policy": name}))
elif args[:3] == ["admin", "policy", "info"]:
    policy = state["policies"][args[4]]
    policy_file = args[args.index("--policy-file") + 1]
    Path(policy_file).write_text(json.dumps(policy))
    print(json.dumps({"PolicyName": args[4], "Policy": policy}))
elif args[0] == "mb":
    state["buckets"].setdefault(args[-1], {"anonymous": True, "objects": {}})
elif args[:2] == ["anonymous", "set"]:
    assert args[2] == "none"
    state["buckets"][args[3]]["anonymous"] = False
elif args[:3] == ["admin", "policy", "create"]:
    state["policies"][args[4]] = json.loads(Path(args[5]).read_text())
elif args[:3] == ["admin", "user", "add"]:
    state["users"].setdefault(args[4], {"policyName": "", "memberOf": []})
    state["users"][args[4]]["secret"] = args[5]
elif args[:3] == ["admin", "policy", "attach"]:
    assert args[5] == "--user"
    state["users"][args[6]]["policyName"] = args[4]
else:
    raise AssertionError("unexpected mc operation: " + operation)
path.write_text(json.dumps(state))
"""


@unittest.skipUnless(shutil.which("jq"), "MinIO 初始化流程测试需要 jq")
class MinioInitTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="ssv-minio-init-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        executable = self.root / "mc"
        executable.write_text(f"#!{sys.executable}\n" + _FAKE_MC)
        executable.chmod(0o755)
        self.state_path = self.root / "state.json"
        self.save({"users": {}, "policies": {}, "buckets": {}, "calls": []})
        self.environment = {
            **os.environ,
            "PATH": f"{self.root}:{os.environ['PATH']}",
            "TMPDIR": str(self.root),
            "MC_TEST_STATE": str(self.state_path),
            "MINIO_ROOT_USER": "test-root",
            "MINIO_ROOT_PASSWORD": "private-root-secret",
            "SSV_MINIO_ACCESS_KEY": "test-application",
            "SSV_MINIO_SECRET_KEY": "private-application-secret",
            "SSV_MINIO_BUCKET": "site-artifacts",
        }

    def save(self, state: dict) -> None:
        self.state_path.write_text(json.dumps(state))

    def state(self) -> dict:
        return json.loads(self.state_path.read_text())

    def run_init(self, **environment: str) -> subprocess.CompletedProcess[str]:
        compose_path = Path(__file__).resolve().parents[3] / "docker" / "compose.yaml"
        service = yaml.safe_load(compose_path.read_text())["services"].get("minio-init")
        self.assertIsNotNone(service, "Compose 必须提供 minio-init 一次性初始化服务")
        command = service["command"]
        script = command[0] if isinstance(command, list) else command
        service_environment = service.get("environment", {})
        proxy_environment = {
            key: service_environment.get(key, "")
            for key in ("NO_PROXY", "no_proxy")
        }
        return subprocess.run(
            ["/bin/sh", "-ec", script.replace("$$", "$")],
            env={
                **self.environment,
                "HTTP_PROXY": "http://proxy.invalid:7897",
                "HTTPS_PROXY": "http://proxy.invalid:7897",
                **proxy_environment,
                **environment,
            },
            text=True,
            capture_output=True,
            timeout=10,
        )

    def assert_private_output(self, result: subprocess.CompletedProcess[str]) -> None:
        output = result.stdout + result.stderr
        self.assertNotIn("private-root-secret", output)
        self.assertNotIn("private-application-secret", output)
        self.assertEqual(list(self.root.glob("tmp.*")), [], "容器临时配置必须清理")

    def test_initialization_creates_private_bucket_and_only_scoped_application_policy(self) -> None:
        result = self.run_init()
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.state()
        self.assertFalse(state["buckets"]["local/site-artifacts"]["anonymous"])
        user = state["users"]["test-application"]
        self.assertEqual(user["secret"], "private-application-secret")
        self.assertEqual(user["memberOf"], [])
        policy = state["policies"][user["policyName"]]
        self.assertEqual(policy["Version"], "2012-10-17")
        statements = {
            tuple(sorted(item["Action"])): item["Resource"] for item in policy["Statement"]
        }
        self.assertEqual(
            statements,
            {
                ("s3:GetBucketLocation", "s3:ListBucket"): ["arn:aws:s3:::site-artifacts"],
                ("s3:GetObject", "s3:PutObject"): ["arn:aws:s3:::site-artifacts/*"],
            },
        )
        self.assertTrue(all(item["Effect"] == "Allow" for item in policy["Statement"]))
        self.assert_private_output(result)

    def test_internal_minio_connection_bypasses_configured_proxy(self) -> None:
        result = self.run_init()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_repeat_initialization_preserves_objects_and_other_accounts(self) -> None:
        self.assertEqual(self.run_init().returncode, 0)
        state = self.state()
        state["buckets"]["local/site-artifacts"]["objects"]["existing.json"] = "evidence"
        state["users"]["unrelated"] = {"policyName": "consoleAdmin", "memberOf": []}
        state["buckets"]["local/other-bucket"] = {"anonymous": True, "objects": {"old": "data"}}
        self.save(state)
        result = self.run_init()
        self.assertEqual(result.returncode, 0, result.stderr)
        after = self.state()
        self.assertEqual(after["buckets"], state["buckets"])
        self.assertEqual(after["users"]["unrelated"], state["users"]["unrelated"])
        self.assert_private_output(result)

    def test_existing_extra_policy_or_group_permissions_fail_without_mutations(self) -> None:
        for field, value in (("policyName", "consoleAdmin"), ("memberOf", ["administrators"])):
            with self.subTest(field=field):
                self.save(
                    {
                        "users": {
                            "test-application": {"policyName": "", "memberOf": [], field: value}
                        },
                        "policies": {},
                        "buckets": {},
                        "calls": [],
                    }
                )
                before = self.state()
                result = self.run_init()
                self.assertNotEqual(result.returncode, 0)
                after = self.state()
                self.assertEqual(after["users"], before["users"])
                self.assertEqual(after["buckets"], {})
                self.assert_private_output(result)

    def test_existing_managed_policy_with_extra_permissions_is_rejected(self) -> None:
        self.assertEqual(self.run_init().returncode, 0)
        state = self.state()
        name = state["users"]["test-application"]["policyName"]
        state["policies"][name]["Statement"][1]["Action"].append("s3:DeleteObject")
        self.save(state)
        result = self.run_init()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.state()["policies"], state["policies"])
        self.assert_private_output(result)

    def test_existing_user_without_groups_accepts_null_membership(self) -> None:
        self.assertEqual(self.run_init().returncode, 0)
        state = self.state()
        state["users"]["test-application"]["memberOf"] = None
        self.save(state)
        result = self.run_init()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_each_mc_failure_aborts_and_hides_command_arguments(self) -> None:
        self.assertEqual(self.run_init().returncode, 0)
        existing = self.state()
        operations = set(existing["calls"])
        # 第二次初始化才会读取已存在用户及策略。
        self.assertEqual(self.run_init().returncode, 0)
        operations.update(self.state()["calls"])
        for operation in sorted(operations):
            with self.subTest(operation=operation):
                initial = {"users": {}, "policies": {}, "buckets": {}, "calls": []}
                self.save(existing if operation.endswith("info") else initial)
                result = self.run_init(MC_TEST_FAIL=operation)
                self.assertNotEqual(result.returncode, 0, operation)
                self.assert_private_output(result)

    def test_failed_mc_write_reports_its_fixed_stage_without_arguments(self) -> None:
        result = self.run_init(MC_TEST_FAIL="admin user add")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MinIO 初始化阶段失败：创建应用账号", result.stderr)
        self.assertNotIn("test-application", result.stderr)
        self.assert_private_output(result)

    def test_existing_policy_read_failure_reports_its_fixed_stage(self) -> None:
        self.assertEqual(self.run_init().returncode, 0)
        result = self.run_init(MC_TEST_FAIL="admin policy info")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MinIO 初始化阶段失败：读取项目策略", result.stderr)
        self.assert_private_output(result)

    def test_invalid_or_shared_credentials_fail_before_mc(self) -> None:
        for values in (
            {"MINIO_ROOT_USER": "ab"},
            {"MINIO_ROOT_PASSWORD": "short"},
            {"SSV_MINIO_ACCESS_KEY": ""},
            {"SSV_MINIO_SECRET_KEY": "short"},
            {"SSV_MINIO_ACCESS_KEY": "test-root"},
        ):
            with self.subTest(values=list(values)):
                result = self.run_init(**values)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.state()["calls"], [])
                self.assert_private_output(result)
