# Agent 只读 Web API 实施计划

> 决策修订（2026-10-17）：WVP 通道映射不属于共享 SSV 配置或 Agent Web API；原步骤 4、5 已撤销，保留在文末作为历史方案记录。

## 步骤 1：配置与独立 Web 命令

- 修改 `agent/src/ssv_agent/config.py`，新增 `AgentWebConfig`，配置 `host` 和 `port`，并由 `AgentConfig.web` 持有；默认监听 `127.0.0.1:8765`。
- 修改 `config/ssv.example.yaml`，记录服务端 Web 监听配置。
- 修改 `agent/src/ssv_agent/cli.py`，在不改变现有 Agent 默认运行方式的前提下增加 `ssv-agent web` 子命令。
- 新增 `agent/src/ssv_agent/web_server.py`，只启动 Uvicorn 和 FastAPI 应用，不创建 `AgentService`、Redis consumer 或后台 worker。
- 测试 Web 配置验证、命令参数解析、Uvicorn 配置和独立启动。

## 步骤 2：只读 SQLite 查询与 HTTP DTO

- 新增 `agent/src/ssv_agent/event_store/readonly.py`，实现基于 `mode=ro` 和 `query_only` 的只读连接、统计、筛选分页、详情查询与图片证据校验；直接使用 `agent.event_db_path` 和 EventLedger 的 evidence roots 约定，不构造可迁移 schema 的 `EventLedger`。
- 新增 `agent/src/ssv_agent/web_api.py`，定义 Pydantic 响应 DTO、四个 GET 路由、统一参数和运行时错误体。
- 修改 `agent/pyproject.toml` 与 `agent/uv.lock`，增加 FastAPI、Uvicorn 和用于解码验证图片的 Pillow 运行依赖。
- 统计、分页、详情、review revision、稳定 evidence ID、图片字节和安全错误通过临时 SQLite 库与证据文件验证。

## 步骤 3：回归验证与交付

- 新增 `agent/tests/test_web_api.py`，覆盖接口 DTO、状态筛选、大小写查询、分页、统计与稳定排序、revision 和证据精确关联、图片路径与文件校验、统一错误、只读连接和不触发 schema migration。
- 新增 `agent/tests/test_web_server.py`，覆盖 `ssv-agent web` 的独立启动及配置监听地址、端口。
- 运行 Agent 相关 pytest、mypy 和 Ruff；检查 `git status` 与未暂存 diff，确认本次修改未暂存且原有改动仍保留。

## 已撤销方案记录：通道事件扩展

以下为先前讨论过但不再实施的方案，仅保留决策历史，不代表当前 API 或配置契约：

- 曾计划增加 `sources[].wvp_channel_id`、写入和迁移 `events.channel_id`，并在 Web API 中按该字段筛选与返回。
- 该方案扩大了普通 SSV 用户的配置与账本职责；当前选择是移除映射，保持事件 `source` 为 SSV source 标识。未来 WVP 关联由独立适配层或 BFF 负责。
- Agent 配置、账本及 API 当前不维护映射，也不读写或筛选 `channel_id`。历史数据库若已有该列，不执行破坏性删列迁移。
