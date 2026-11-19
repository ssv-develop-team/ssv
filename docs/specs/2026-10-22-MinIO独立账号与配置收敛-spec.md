# MinIO 独立账号与配置收敛规格

## 1. 背景与目标

基线提交 `176cbe5` 已实现产物异步上传、完整性校验、暂存清理和远端读取。目前 MinIO root 凭据与 Agent 凭据相同，bucket 由 Agent 首次上传时创建；配置同时存在 YAML 和环境变量入口。本规格记录 2026-10-22 确认的新调整，原有 spec、plan 保留历史内容。

本次恢复独立应用账号，并明确各配置来源；产物登记、上传租约、历史文件读取与清理规则继续沿用已实现行为。

## 2. 配置来源

| 来源 | 职责 |
| --- | --- |
| 根目录 `.env` | `MINIO_ROOT_USER`、`MINIO_ROOT_PASSWORD`、`SSV_MINIO_ACCESS_KEY`、`SSV_MINIO_SECRET_KEY`；可选 `SSV_MINIO_BUCKET`、`SSV_MINIO_IMAGE` |
| `docker/.env` | 仅 `REDIS_PORT`、`QDRANT_PORT`、`MINIO_API_PORT`、`MINIO_CONSOLE_PORT` |
| 运行 YAML 的 `agent.storage` | backend、prefix、上传 worker 策略；外部 MinIO 的显式 endpoint，以及可选的高级超时和凭据变量名配置 |
| `agent/.env` | 保留既有模型配置职责 |

统一 CLI 的优先级为进程环境、根 `.env`、`docker/.env` 的端口。`docker/.env` 中的非端口键不加入项目环境。Compose 使用根 `.env` 和已解析的进程环境；直接调用 Compose 时显式按根 `.env`、`docker/.env` 顺序传两个环境文件。

直接 Agent、Web、产物重试命令使用同一个环境加载过程：进程环境优先，然后根 `.env`、Docker 端口和 `agent/.env`。Agent 入口不加载文件中的 root 凭据，并移除继承的 `MINIO_ROOT_USER`、`MINIO_ROOT_PASSWORD`；统一 CLI 启动 Agent 时也不传这两个键。凭据文件仍属于本机运维用户的文件权限管理范围。

bucket 的唯一部署来源为 `SSV_MINIO_BUCKET`，缺省 `ssv-artifacts`；已有产物仍按账本中的 bucket 读取。旧 YAML 的 `bucket` 字段仅在与环境值或缺省值相同的情况下兼容；冲突必须报告迁移错误，不能静默选择另一处配置。环境派生的 bucket 同样执行合法性校验。

未显式指定 endpoint 时，默认使用 `http://127.0.0.1:<MINIO_API_PORT>`，端口缺省 `9000`。外部 MinIO 可显式指定 HTTP(S) endpoint。非法默认端口与非法显式 endpoint 在配置加载时失败。

连接超时和读取超时保留已有 `5` 秒、`15` 秒默认值及可选配置接口；标准示例省略这些默认项及凭据变量名。上传 worker 配置继续复用现有 `PollingWorkerConfig`，不新增一套调度字段。

## 3. 初始化与权限

在现有 `docker/minio/Dockerfile` 中加入固定版本的 `mc`，MinIO 服务和一次性 `minio-init` 使用同一个包含管理工具的新镜像 tag。新 tag 区分不含 `mc` 的旧镜像；不自动复用旧本地镜像。实施时先核验固定 `mc` 发布版本并验证构建成功，再把该版本用于发布，不以浮动 tag 代替核验。

`minio-init` 等待 MinIO 健康，使用 root 凭据完成私有 bucket、独立应用账号和 bucket 权限策略的幂等初始化。脚本与策略内容内联于 Compose，策略临时文件只存在于初始化容器内。重复初始化可成功，错误必须非零退出，禁止输出凭据。

应用账号使用 `SSV_MINIO_ACCESS_KEY`、`SSV_MINIO_SECRET_KEY`，访问键必须与 root 用户不同。权限仅限目标 bucket：bucket 位置查询和列举、对象读取与上传；不授予删除对象、管理账号或修改 bucket 策略的权限。目标 bucket 关闭匿名访问；其他 bucket 和非项目账号不改动。已有同名应用账号若具有范围外权限，初始化明确失败，不能仅附加新策略后声称权限已收敛。已有旧 root 数据及 volume 不迁移、不删除。

`./ssv redis start` 继续启动整组依赖，除服务健康检查外还必须确认初始化成功；失败给出受控错误，不能宣布整体就绪。正常 `stop/down` 不删除 volume。直接 Compose 操作的文档必须包含初始化结果检查。

Agent 的 `MinioObjectStore.ensure_uploaded` 只读取、写入和校验已存在 bucket 中的对象。移除创建 bucket 和管理账号的职责；bucket 缺失作为受控存储故障进入现有有限重试，不再调用管理接口。

## 4. 迁移与验证

第一步完成环境与配置来源调整；此时运行服务仍遵循基线的账号初始化行为，现有同值凭据可以继续工作。第二步完成镜像、独立账号初始化及 Agent 职责调整后，按新文档配置不同的 root 和应用账号并执行启动命令。每步保持未暂存供人工审阅。

配置验证覆盖根凭据、端口白名单、进程覆盖、直接 Agent/Web 环境加载、root 凭据过滤、bucket 冲突与默认值校验、端口推导及显式远端 endpoint。初始化测试覆盖真实命令流程的错误退出、幂等执行、应用账号仅有指定 bucket 权限，以及 CLI 不误报就绪。具备 Docker 和网络条件时验证镜像构建、真实上传/读取、禁止删除/管理/匿名访问及重启持久性；无法执行时明确记录环境原因。

## 5. 非本次范围

- 产物历史迁移、视频上传、远端对象删除或重建。
- 新 Web 页面、独立 Web 只读账号、凭据自动轮换。
- 修改既有产物上传、业务复核与 SQLite 任务语义。
- 改写 2026-10-21 的原 spec 和 plan、提交或推送 Git 变更。
