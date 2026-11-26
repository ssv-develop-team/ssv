# MinIO 独立账号与配置收敛实施计划

> 执行按任务设置人工审阅点；开发过程采用 superpowers:test-driven-development，完成报告采用 superpowers:verification-before-completion。用户已授权在当前工作区实施，每步先保留未暂存变更；进入下一步前按根 AGENTS.md 检查并暂存已验证的上一步，不提交。

**目标：** 恢复 MinIO 独立应用账号，收敛配置来源并保持现有产物读写行为。

**架构：** 根 `.env` 保存两组凭据，Docker 环境文件只加载端口。Compose 的一次性容器持有 root 凭据并初始化应用账号，Agent 只使用应用凭据访问对象。

**技术：** Python、Pydantic、python-dotenv、MinIO Python SDK、Docker Compose、现有 MinIO 镜像构建与 CI。

**规格：** [MinIO 独立账号与配置收敛](../specs/2026-10-22-MinIO独立账号与配置收敛-spec.md)。

## 全局约束

- 不修改原有 2026-10-21 spec 和 plan，不改真实 `.env` 或本地运行 YAML。
- root 和应用凭据都放在根 `.env`；`docker/.env` 仅放端口。
- 产物账本、对象位置和 volume 保持已有事实，不迁移或删除历史对象。
- 新初始化内联 Compose，不新增宿主机初始化脚本或策略文件。
- 每步测试先失败再实现；只在明确进入下一步后暂存前一步，不提交。

## 步骤 1：统一配置与环境来源

涉及文件：

- 修改 `scripts/ssv_cli/context.py`、`scripts/ssv_cli/services/compose.py`、`scripts/ssv_cli/commands/agent.py`。
- 修改 `scripts/ssv_cli/tests/test_cli.py`、`scripts/ssv_cli/tests/test_compose.py`。
- 修改 `agent/src/ssv_agent/config.py`、`agent/src/ssv_agent/cli.py`。
- 修改 `agent/tests/test_config.py`、`agent/tests/test_artifact_worker.py`。
- 修改 `.env.example`、`docker/.env.example`、`config/ssv.example.yaml`、`README.md`、`docs/Agent配置.md`、`docs/运行与配置.md`。

接口：`ProjectContext.discover` 保持返回类型；仅合并 Docker 端口键；`_compose_argv` 使用根环境文件。新增 `_load_environment()` 负责 Agent 三类命令共有的环境加载，过滤 root 凭据。`MinioStorageConfig` 保留现有字段，但 bucket 冲突时报错，默认 endpoint 从 API 端口派生，缺省值执行校验；`SsvConfig.agent` 使用工厂确保环境变化在加载时生效。

- [x] 先写环境来源和凭据过滤测试：根 `.env` 凭据可用、Docker 非端口键不导入、进程值优先、直接 Agent 环境与统一 CLI 一致、子进程没有 root 凭据。
- [x] 先写配置测试：自定义环境 bucket 生效、旧 YAML 冲突拒绝、非法环境 bucket/端口拒绝、显式远端 endpoint 不受本地端口影响。
- [x] 运行上述测试，确认失败原因对应当前缺失行为。
- [x] 实现环境与配置调整，省略示例中的重复默认项；现有自定义超时和凭据变量名继续接受。
- [x] 同步当前使用文档的文件位置、迁移提示与配置优先级；账号初始化仍描述当前基线行为，第二步完成后再调整。
- [x] 运行 `python3 -m unittest scripts.ssv_cli.tests.test_cli scripts.ssv_cli.tests.test_compose` 与 `cd agent && .venv/bin/python -m pytest tests/test_config.py tests/test_artifact_worker.py -q`；执行相关 Ruff 和空白检查，报告未暂存 diff。

第一步验证：CLI 两个测试文件共 57 项通过；Agent 配置、上传 worker、服务、runtime、Web 入口配置与路由构造共 115 项通过。CLI 测试使用临时 `UV_CACHE_DIR` 和 `UV_NO_SYNC=1`，避免宿主缓存访问及同步依赖；临时目录运行后删除。相关 Ruff 检查及 `git diff --check` 通过，九个相关 Python 文件未新增格式差异，保留基线的既有格式问题。变更均未暂存。

HTTP 回归限制：`test_web_api` 在 Starlette TestClient 的 AnyIO portal 启动处挂起；无项目代码的最小 FastAPI/TestClient 同样挂起，已用超时停止测试。未把 HTTP 请求级回归记为通过，也未修改依赖或业务代码绕过。

## 步骤 2：恢复一次性初始化与独立应用权限

涉及文件：

- 修改 `docker/minio/Dockerfile`、`docker/compose.yaml`、`.github/workflows/ci.yml`。
- 修改 `scripts/ssv_cli/services/compose.py`、`scripts/ssv_cli/tests/test_compose.py`。
- 修改 `agent/src/ssv_agent/minio_store.py`、`agent/tests/test_artifact_storage.py`。
- 修改 `.env.example`、`README.md`、`docs/Agent配置.md`、`docs/运行与配置.md`。
- 新增 `scripts/ssv_cli/tests/test_minio_init.py`，执行 Compose 中的真实初始化命令，使用受控的外部 mc 替身记录动作和注入失败。

接口：`start_redis` 同时确认服务健康和 `minio-init` 成功；root 与应用访问键必须不同。`MinioObjectStore.ensure_uploaded` 不创建 bucket，缺失 bucket 映射为受控存储故障。初始化容器使用 `SSV_MINIO_BUCKET`，新镜像必须含固定版本 `mc`；旧本地镜像不自动复用。

- [x] 收到进入本步指示后，检查 diff/status，尝试暂存已验证第一步及必要新文档；`.git` 为只读文件系统，创建 `index.lock` 失败，cached diff 为空，已报告并保留未暂存状态，没有绕过。
- [ ] 核验实际可用的固定 `mc` 发布版本，使用只读公开元数据或可复现构建验证；若联网审批失败，保留其他工作并报告阻塞，不能把未经构建验证的镜像称为已可用。
- [x] 先写测试，覆盖初始化成功/失败/超时、应用账号与 root 不同、重复执行、已有对象保留、任一 mc 命令失败非零退出、bucket 缺失时 Agent 不调用 `make_bucket`。
- [x] 在现有 Dockerfile 增加构建和复制 mc 的配置，采用区别旧内容的 `RELEASE.2025-04-22T22-12-26Z-mc1` tag，同步 CI、Compose/CLI 默认值；固定候选版本为 `RELEASE.2025-04-16T18-13-26Z`，实际联网核验和镜像构建仍未完成。
- [x] 在 Compose 内增加 `minio-init`，等待 `minio` 健康，内联创建私有 bucket、应用用户、bucket 限定策略及策略绑定；临时策略文件仅在容器内生成。策略允许 `s3:GetBucketLocation`、`s3:ListBucket`、`s3:GetObject`、`s3:PutObject`，不允许删除或管理。
- [x] 在现有 CLI 等待流程中确认初始化退出码，缺失/失败/超时均报告受控错误；正常停止继续不传 `-v`。移除 Agent 自动建 bucket 分支。每次启动在服务健康后强制重新创建初始化容器，兼容 Compose 的数组、单对象和逐行 JSON 状态输出。
- [x] 更新操作文档与新账号示例；说明现有凭据迁移、直接 Compose 初始化检查，以及现有自定义 bucket 必须通过环境变量配置。
- [x] 运行 CLI 测试、Agent 存储与 worker 测试、Compose 渲染、相关静态检查。
- [ ] 构建镜像并验证初始化两次均成功，应用账号可写可读但无法删除/管理，匿名不可读，重启后对象保留；使用独立测试项目及数据卷，不操作用户运行中的服务。

第二步本地验证：CLI 三个测试文件共 70 项通过；Agent 配置、存储、上传 worker、服务、runtime、Web 配置与路由构造共 132 项通过。初始化测试实际运行 Compose 中的 shell，外部 mc 替身模拟管理服务并注入每项命令的失败；这些结果不能替代真实 mc 和 MinIO 的接口、权限与持久性联调。Compose 配置渲染、相关 YAML 解析、12 个相关 Python 文件的 Ruff 检查以及 `git diff --check` 通过，未新增格式差异。HTTP 请求级测试仍受前述 TestClient 挂起限制，未重复运行。

实施选择：镜像增加 `jq`，CI 安装相同工具，避免依赖文本匹配推断账号权限；已有同名项目策略必须与生成策略一致，账号不能附加其他策略或组，遇到不一致失败并由管理员处理。没有新增宿主初始化脚本或策略文件，没有修改旧 spec/plan、真实凭据、已有数据卷或无关本地 YAML。

最终审查修正：已有策略通过 `mc admin policy info --policy-file` 导出后比较，避免将 `PolicyInfo` 包装字段与裸策略文档比较而误判重复初始化失败；测试替身同步模拟策略文件导出及包装输出。

## 当前环境记录

2026-10-22，读取公开 mc 版本元数据的联网操作未执行：自动审批接口返回 403（模型权限错误）。第一步不依赖联网；第二步的镜像构建和真实权限验证需要恢复此条件。未把该失败视为用户拒绝，也不绕过自动审批。

第二步重试公开 Go 模块元数据查询时仍收到相同自动审批接口 403，操作未执行。沙箱内无法连接本机 7897 代理，且无 `/var/run/docker.sock` 访问权限；因此固定 mc 版本可用性、镜像构建、真实权限及持久性验证仍未完成。代码和文档变更全部保持未暂存、未提交。
