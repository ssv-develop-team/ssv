# Python 依赖与模型处理迁移

## 1. 背景

上一阶段已经把 `./ssv` 的入口、参数解析和命令编排迁移到 Python，但
`./ssv build` 仍通过 `scripts/build.sh` 加载 `scripts/deps*.sh`，模型命令仍
通过 `uv run` 或运行时 `pip install` 启动工具。这样入口虽然是 Python，依赖
和模型的实际生命周期仍由 Shell 或临时环境控制，无法被同一套 Python 测试和
错误处理验证。

## 2. 目标

1. 由 `scripts/ssv_cli/services/dependencies.py` 负责 runtime profile、`.env`
   配置解析、GPU vendor 检测、依赖来源选择、下载缓存、ABI/ELF/pkg-config
   探针、运行时路径和 `ssv-deps.env` 快照。
2. 由 `scripts/ssv_cli/services/models.py` 负责模型导出、wrapper 生成、模型
   验证和 TensorRT manifest 工具调用；工具通过 Python import 和函数接口调用，
   不执行 `uv run --script`，不在运行过程中安装 Python 包。
3. `commands/build.py` 直接编排 Python dependency service、Meson 配置/编译、
   插件产物检查和快照发布；`scripts/build.sh`、`scripts/deps.sh` 及 provider
   Shell 文件不再是生产后端。
4. 保持现有 profile 矩阵、依赖 ABI 契约、缓存目录、快照变量、Meson 选项、
   模型 wrapper metadata 和工具退出码语义。

## 3. 依赖接口契约

### 3.1 Profile

`auto|cpu|nvidia|intel|amd` 是唯一允许的 profile。`auto` 从
`/sys/class/drm/card*/device/vendor` 检测厂商，按 NVIDIA、Intel、AMD、CPU
的优先级解析。解析后的 profile 才能进入依赖配置。

ONNX Runtime provider 顺序保持如下：

| profile | providers |
| --- | --- |
| `cpu` | `CPUExecutionProvider` |
| `nvidia` | `TensorrtExecutionProvider,CUDAExecutionProvider,CPUExecutionProvider` |
| `intel` | `OpenVINOExecutionProvider,CPUExecutionProvider` |
| `amd` | `MIGraphXExecutionProvider,CPUExecutionProvider` |

### 3.2 配置来源

Python 先读取进程环境，再读取项目根 `.env` 中未设置的键。依赖配置键与原
契约保持一致：`SSV_ONNXRUNTIME_SOURCE`、`SSV_ONNXRUNTIME_ROOT`、
`SSV_OPENCV_SOURCE`、`SSV_OPENCV_MODE`、`SSV_OPENCV_ROOT`、
`SSV_OPENCV_INCLUDE_DIR`、`SSV_OPENCV_LIB_DIR`、`SSV_TENSORRT_SOURCE`、
`SSV_TENSORRT_MODE`、`SSV_TENSORRT_ROOT`、`SSV_TENSORRT_ARCHIVE`、
`SSV_TENSORRT_URL`、`CUDA_HOME` 和 `SSV_EXTRA_PKG_CONFIG_PATH`。

managed ONNX Runtime 的版本继续由 profile 派生，禁止用
`SSV_ONNXRUNTIME_VERSION` 覆盖。managed OpenCV、TensorRT 和 Intel OpenVINO
源码构建继续使用仓库固定版本与下载缓存；版本常量迁移到 Python 模块。

### 3.3 Snapshot

`DependencySnapshot` 以固定键集合生成 `build/ssv-deps.env`，供
`runtime_env.py` 和 C++ runner 使用。写入采用临时文件加 `os.replace`，读取
拒绝未知键、重复键、Shell 控制字符和缺失键。依赖签名继续包含 profile、来源、
版本、pkg-config/runtime 路径、Provider 列表以及链接库内容身份。

## 4. 模型接口契约

`ModelService` 提供以下操作：

- `export_default()`：导入已安装的 `ultralytics`，导出 `yolov8n.onnx`，
  已存在的目标保持幂等；缺少 optional dependency 时返回带安装 extra 的明确
  `CliError`。
- `prepare(arguments)`：导入 `scripts.model.prepare_wrapper`，构造其
  `argparse.Namespace` 并调用 `prepare_wrapper()`；保留 wrapper 的原子发布、
  metadata、CPU smoke、`--force` 和退出码契约。
- `verify(arguments)` 与 `write_manifest(arguments)`：分别调用已有模型验证和
  TensorRT manifest Python 模块，复用其校验逻辑，不复制 ONNX 解析实现。

模型依赖通过 `pyproject.toml` 的 optional extras 声明。生产 CLI 不负责解析
依赖管理器，也不执行 `pip install`；环境准备由项目安装命令完成。

## 5. 错误处理与外部工具

配置错误、缺失依赖、下载失败、探针失败和模型 optional dependency 缺失统一
转换为 `CliError`，保留构建失败的非零状态。`./ssv test` 在启动模型契约测试前
检查 `numpy`、`onnx` 和 `onnxruntime`，缺少时提示安装 `.[model]`，不输出原始
`ModuleNotFoundError` traceback。外部工具只通过参数数组调用，
禁止拼接 Shell 命令字符串。下载使用 Python HTTP 客户端并写入临时文件后
原子替换；归档解压限制在候选目录内。

Meson、CMake、`pkg-config`、`readelf`、`ldd`、`dpkg-deb`、`tar`、`unzip` 和
编译器仍属于系统工具，不纳入 Python 重实现范围；Python 负责参数、环境、
输出和生命周期。

## 6. 非目标

- 不改变 C++ runner、GStreamer 插件、Agent 事件协议或 YAML schema。
- 不把 C/C++ 编译器、Meson、GStreamer 和 TensorRT 本体实现为 Python。
- 不在 CLI 运行期间自动安装 Python 包。
- 不改变 Redis 清理语义和 `docs/ssv-rules-rag-architecture.html` 的未提交状态。

## 7. 验收标准

- `commands/build.py`、`commands/model.py` 的生产路径不再引用
  `scripts/build.sh`、`scripts/deps.sh`、`uv run` 或 `pip install`。
- Python 测试覆盖 profile 解析、配置互斥校验、Provider 校验、快照读写、
  system/local 依赖探针，以及模型服务的直接调用和缺包错误。
- `./ssv build` 在依赖齐全主机上能够配置、编译并发布快照；`./ssv run` 能
  读取同一快照。
- `./ssv model prepare`、`model manifest` 和 `model verify` 的输出契约保持稳定。
- 运行 Python 测试、C++ Meson 测试、Agent 测试、静态检查和 `git diff --check`，
  对缺少硬件、网络或模型资源的验证记录明确限制。
