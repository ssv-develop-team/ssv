# Python CLI 与 Redis 清理

## 1. 背景

项目入口 `./ssv` 当前由 Bash 根据命令转发到多个脚本。命令解析、项目路径、环境变量、进程退出语义和 Redis 运维操作分散在不同脚本中，难以复用和测试。本阶段将入口及命令编排迁移到 Python，并把 CLI 测试收拢到 CLI 包内。

## 2. 目标

1. `./ssv` 使用 Python 启动，并通过明确的子命令组织构建、Redis 和模型生命周期。
2. 将路径发现、`.env` 加载、依赖快照运行环境、子进程交接、Docker Compose 服务控制和测试编排集中到 `scripts/ssv_cli`。
3. 增加 Redis 状态查看和积压清理命令，清理范围由 YAML 配置或显式参数确定。
4. 将根目录 `tests/` 迁移到 `scripts/ssv_cli/tests/`，不保留独立根测试目录。
5. 保留现有复杂依赖 provider 的行为和验证契约。

## 3. CLI 契约

当前命令契约：

```text
./ssv build [--profile auto|cpu|nvidia|intel|amd]
./ssv clean
./ssv redis start
./ssv redis status
./ssv redis clean [--stream] [--dry-run] [--yes]
./ssv redis stop
./ssv test
./ssv run [runner options]
./ssv model export
./ssv model prepare [model options]
./ssv model verify [model options]
./ssv model manifest [model options]
./ssv agent
./ssv inspect
```

Redis 连接参数默认来自运行配置：`redis.host`、`redis.port`、`redis.db`、`redis.stream_key` 和 `redis.consumer_group`。命令行可用 `--host`、`--port`、`--db`、`--stream-key` 和 `--group` 覆盖连接/目标；`--stream` 保留给清理命令的历史数据裁剪开关。

### 3.1 清理语义

- 默认清理目标是指定 stream/group 的 pending entries，通过 `XACK` 从消费组 PEL 移除；stream 历史数据保留。
- `--dry-run` 只统计 pending 数量，不执行 `XACK` 或裁剪。
- `--stream` 在 pending 清理后执行 `XTRIM <stream> MAXLEN = 0`，删除 stream 中的历史 entries；必须同时提供 `--yes`。
- `--stream` 不执行 `FLUSHDB`，不删除其他 key，不销毁消费组。
- Redis 不可用、消费组不存在或命令失败时返回非零状态，并保留已完成操作的统计信息。

## 4. 目录与模块边界

```text
ssv
scripts/
├── ssv_cli/
│   ├── cli.py
│   ├── context.py
│   ├── config.py
│   ├── output.py
│   ├── process.py
│   ├── commands/
│   ├── services/
│   └── tests/
└── model/
```

依赖、构建和模型生命周期由 `scripts/ssv_cli/services/` 负责；Meson、编译器和其他系统工具仍由 Python 以参数数组调用。`scripts/model/` 只保留可复用的模型契约工具，由 `ModelService` 直接导入。

## 5. 非目标

- 不改变 C++ runner、GStreamer 插件、Agent 事件协议或 YAML schema。
- 不自动删除 Redis stream、consumer group 以外的数据。
- 不把 Redis 清理绑定到 Agent 进程生命周期。
- 不在本阶段重写 800 行级别的依赖 provider shell 实现。

## 6. 选型修订（2026-08-20）

Redis 连接由自定义 RESP 客户端调整为 `redis-py`。保留 `RedisConnectionProtocol`
作为 `RedisAdmin` 的测试注入边界，生产实现通过 `redis.Redis` 负责连接池、超时、
认证、RESP 编解码和异常处理；这样不改变清理命令的 Redis 语义，也不让 CLI 自行维护
socket 和协议细节。`redis` 作为 CLI 的运行时依赖声明在 `pyproject.toml`，但连接模块
仍延迟导入，以便 `./ssv --help` 在未安装运行依赖时能够显示帮助并由实际 Redis 命令
返回明确的安装提示。

## 7. 验收标准

- `./ssv --help`、现有命令和错误退出码可用。
- Python 单元测试覆盖命令解析、路径/快照解析、Redis 清理的 dry-run 与 mutation 分支。
- 根目录不存在 `tests/`，原有依赖/模型/CLI 测试均能从新位置运行。
- `./ssv test`、Agent 测试、相关 C++ 测试和 `git diff --check` 通过，或记录明确环境限制。
