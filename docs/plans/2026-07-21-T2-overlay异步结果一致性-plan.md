# T2 overlay 异步结果一致性实施计划

## 目标

基于 `docs/specs/2026-07-21-T2-overlay异步结果一致性-spec.md`，修复显示分支读取
到 raw detection 中间状态的问题，并让项目默认分析 pipeline 保持推理和跟踪顺序。
改动保持 `ssvinfer` 通用异步能力、provider 配置和下游消息契约不变。

## 文件变更

| 文件 | 动作 | 责任 |
| --- | --- | --- |
| `docs/specs/2026-07-21-T2-overlay异步结果一致性-spec.md` | 新增 | 冻结 overlay 快照与 pipeline 时序契约。 |
| `docs/plans/2026-07-21-T2-overlay异步结果一致性-plan.md` | 新增 | 记录实施步骤、验证和回滚边界。 |
| `gst/tests/test_ssv_meta.cpp` | 修改 | 增加 raw 写入不覆盖 tracked overlay 的回归测试。 |
| `tests/ssv_cli_test.sh` | 修改 | 断言项目 pipeline 使用 `async=false`。 |
| `gst/ssv-common/ssv_meta.cpp` | 修改 | 只在 `set_tracked()` 发布 overlay 快照。 |
| `gst/ssv-common/include/ssv_meta.hpp` | 修改 | 更新 store 方法注释，说明 raw/tracked 的发布边界。 |
| `scripts/pipeline.sh` | 修改 | 分析分支显式传入 `async=false`。 |
| `docs/yolo推理链路说明.md` | 修改 | 区分插件默认异步能力和项目默认同步分析链路。 |

## 实施步骤

### 1. 建立红灯反馈环

- [x] 在 `test_ssv_meta.cpp` 构造 tracked frame 180，调用 `take()` 模拟发布消费，
      再写入 raw frame 181；断言 `peek_latest()` 仍为 frame 180 的 tracked 快照。
- [x] 断言后续 `set_tracked(frame 181)` 才切换快照，并覆盖空 tracked 结果清除旧框。
- [x] 将 CLI 断言从 `async=true` 改为 `async=false`，保留 worker/属性存在性检查。
- [x] 先运行针对性测试，记录当前实现的失败输出。

### 2. 修复元数据发布时序

- [x] 从 `SsvDetectionStore::set()` 移除对 `overlay_current_` 的写入；保留规范化、
      `current_` 写入和状态转换。
- [x] 保持 `set_tracked()` 对非空和空帧都执行同一份 overlay 快照复制。
- [x] 更新头文件注释，明确 `peek_latest()` 返回最近一次完整 tracked 结果。
- [x] 运行元数据回归测试，确认红灯转绿。

### 3. 固定项目默认分析模式

- [x] 将 `scripts/pipeline.sh` 的 `infer_props` 改为 `async=false`。
- [x] 不修改插件属性默认值，以保留直接调用方的既有选择。
- [x] 运行 CLI 脚本测试和 Shell 语法检查。

### 4. 全量验证

- [x] `./ssv build`（在本机 Arch 上通过 `LD_PRELOAD=/usr/lib/libcblas.so.3` 补充宿主 BLAS 符号）
- [x] `meson test -C build --print-errorlogs`
- [x] `bash tests/ssv_cli_test.sh`
- [x] `bash tests/ssv_deps_test.sh`
- [x] `cd agent && uv run --extra dev pytest`
- [x] `git diff --check`
- [x] 检查仓库中不存在临时 `[DEBUG-...]` 日志或测试文件。

## 兼容性与回滚

- 插件 `async` 属性、`SsvDetectionStore` 方法签名、Redis schema 和 provider 选择
  均保持兼容。
- 如需回滚，只需回退本分支新增提交；不需要删除依赖缓存或修改用户配置。
- 真实 RTSP、GPU/TensorRT 运行验证不在本机执行；依赖其环境的命令只记录缺失原因，
  不改变本地默认 CPU 测试路径。

## 完成定义

回归测试、构建和静态检查通过，且文档与实际默认 pipeline 一致；提交信息应说明
根因是 raw detection 提前发布 overlay 快照，以及项目默认异步推理造成的顺序脱钩。
