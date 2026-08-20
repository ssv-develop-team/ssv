# Python CLI 与 Redis 清理实施计划

## 步骤 1：建立 Python CLI 基础层

新增 `scripts/__init__.py`、`scripts/ssv_cli/__init__.py`、`cli.py`、`context.py`、`config.py`、`output.py` 和 `process.py`。

完成项目根目录发现、`.env` 非覆盖加载、配置路径搜索、命令解析、统一错误码和子进程环境继承。

## 步骤 2：迁移命令编排

新增 `commands/` 下的 build、clean、run、test、agent、model、inspect 和 redis 命令，以及 `services/` 下的 Docker Compose、运行时快照和 Redis 服务；停止 Redis 由 `redis stop` 负责。

简单 shell 编排、依赖准备和构建编排均由 Python 接管。run/inspect 使用 Python 加载成功的 `ssv-deps.env`，保持 `GST_PLUGIN_PATH` 与 `LD_LIBRARY_PATH` 契约。

## 步骤 3：实现 Redis 管理

使用成熟的 `redis-py` 客户端和 `RedisConnection` 适配层实现 Redis 管理。覆盖 `PING`、
stream/group 状态、pending 分页 `XPENDING`、批量 `XACK` 和显式确认的 `XTRIM`。通过
协议接口注入测试 doubles，不连接真实 Redis也能完成 `RedisAdmin` 单元测试；客户端
连接、超时、认证、RESP 编解码和异常由 `redis-py` 负责。

## 步骤 4：迁移测试与文档引用

将根 `tests/` 下四个测试文件移动到 `scripts/ssv_cli/tests/`，修正根目录计算和模型工具路径；删除旧目录。更新 CI、README 和相关命令引用。

## 步骤 5：验证与交付

依次运行 Python CLI 测试、迁移后的契约测试、Ruff/编译检查、Agent pytest、Meson 测试和 CLI smoke。审查未跟踪的 `docs/ssv-rules-rag-architecture.html` 不进入变更集；本阶段不自动提交、推送或创建 PR。

## 文件清单

### 新增

- `scripts/__init__.py`
- `scripts/ssv_cli/__init__.py`
- `scripts/ssv_cli/cli.py`
- `scripts/ssv_cli/context.py`
- `scripts/ssv_cli/config.py`
- `scripts/ssv_cli/output.py`
- `scripts/ssv_cli/process.py`
- `scripts/ssv_cli/commands/*.py`
- `scripts/ssv_cli/services/*.py`
- `scripts/ssv_cli/tests/*.py`

### 修改

- `ssv`
- `README.md`
- `.github/workflows/ci.yml`

### 删除或迁移

- `tests/` 下四个文件迁移到 `scripts/ssv_cli/tests/`
- Python 已接管的简单 shell 编排脚本
