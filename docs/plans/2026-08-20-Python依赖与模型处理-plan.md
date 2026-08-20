# Python 依赖与模型处理迁移计划

## 步骤 1：冻结 Python 服务接口

新增 `scripts/ssv_cli/services/dependencies.py`、`models.py` 以及对应测试。
定义不可变配置、profile/provider 纯函数、快照键集合和模型服务边界。先覆盖
不需要下载或 GPU 的校验路径。

## 步骤 2：迁移依赖 provider

在 `dependencies.py` 中实现：

1. `.env` 依赖键解析、显式配置互斥校验和 profile 自动检测。
2. `pkg-config` 版本/路径查询、ELF/Provider/运行时闭包探针和 C++ probe。
3. ONNX Runtime 的 managed/local/system 三种来源，OpenCV 的
   managed/local/system 三种来源，TensorRT 的 managed/system 与
   `auto|enabled|disabled` 行为。
4. 下载缓存、归档解压、候选目录原子替换和依赖内容签名。

每完成一个 provider，先运行纯函数和 fake artifact 测试；网络下载只在具有
明确资源的环境中验证。

## 步骤 3：改造 build 与运行时

修改 `commands/build.py` 和 `services/runtime_env.py`：Python 取得
`DependencySnapshot` 后直接执行 `meson setup`、`meson compile`，处理依赖签名
变化时的 `--clearcache`/`--wipe`，检查五个插件产物，最后发布快照。删除生产
路径对 `scripts/build.sh` 和 `scripts/deps*.sh` 的调用，再将构建集成测试改为
fake `meson` 可观测参数。

## 步骤 4：接入模型服务

修改 `commands/model.py`、`cli.py` 和 `pyproject.toml`：

- `model export` 直接调用已安装 `ultralytics`；
- `model prepare` 直接调用 `prepare_wrapper.prepare_wrapper`；
- 增加 `model verify` 和 `model manifest` 的 Python 服务入口；
- 使用 `model`、`model-export` optional extras 声明 `numpy`、`onnx`、
  `onnxruntime` 和 `ultralytics`；
- 缺少 extra 时输出安装提示而不是在运行时执行 `pip`。

## 步骤 5：测试、清理和文档收口

新增 Python dependency/model service tests，迁移 `ssv_deps_test.sh` 的公开
契约测试，修改 `commands/test.py`、CI、README、`.env.example` 和模型验证
说明。确认 `scripts/build.sh`、`scripts/deps.sh`、`scripts/deps/` 不再被生产
代码引用后删除旧后端及过时的 Shell 测试；保留纯测试 harness 的必要性并说明
其边界。`./ssv test` 在运行 wrapper/manifest 契约测试前显式检查 `model` extra，
缺少依赖时给出安装命令，不让 Python 导入错误泄漏到用户终端。

## 验证命令

```text
python -m compileall -q ssv scripts
python -m unittest discover -s scripts/ssv_cli/tests -p 'test_*.py'
./ssv --help
./ssv test
git diff --check
```

依赖下载、TensorRT、OpenCV 源码构建和 RTSP smoke 按主机能力单独记录，不以
缺少 GPU、网络或测试模型的结果冒充全量通过。

## 变更边界

- 本阶段不提交、不推送、不创建 PR；每个步骤完成后保持未暂存供人工审阅。
- 不加入 `docs/ssv-rules-rag-architecture.html`。
- 不修改 C++/Agent 业务协议；Python 只接管构建、依赖和模型生命周期。
