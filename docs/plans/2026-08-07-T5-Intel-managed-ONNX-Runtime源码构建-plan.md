# Intel managed ONNX Runtime 源码构建计划

基于 `docs/specs/2026-08-07-T5-Intel-managed-ONNX-Runtime源码构建-spec.md`，为 Intel
profile 增加 managed 源码构建 provider，并在本机完整验证。

## 步骤 1：编写 spec/plan

- 新增上文两份文档，明确配置、工作区、构建命令与验收。

## 步骤 2：扩展 onnxruntime-managed.sh

涉及文件
- `scripts/deps/onnxruntime-managed.sh`：修改

涉及标识符
- `ssv_onnxruntime_archive_info`：保持 CPU/NVIDIA 行为不变
- `ssv_onnxruntime_validate_layout`：允许 `profile=intel` 的 managed 布局
- 新增 `ssv_onnxruntime_intel_*` 系列：OpenVINO 归档常量、源码/构建/安装目录检查、
  build.py 调用、install 组装、managed prepare 分支
- 常量：`SSV_ONNXRUNTIME_INTEL_OPENVINO_VERSION`、OpenVINO 归档文件名与 URL、
  ORT 源码 URL

接口与所有权影响
- `ssv_onnxruntime_managed_prepare` 按 profile 分派：intel 走源码构建，cpu/nvidia 走
  既有二进制归档路径
- install 目录为 provider 所有并原子替换；source/build/openvino 目录可复用但不可被
  未知内容覆盖

## 步骤 3：修改 deps.sh 默认配置

涉及文件
- `scripts/deps.sh`：修改

涉及标识符
- `ssv_deps_resolve_config`：Intel profile 默认 `SSV_ONNXRUNTIME_SOURCE=managed`，
  `SSV_ONNXRUNTIME_ROOT=.deps/onnxruntime-openvino`；managed 校验允许 intel

## 步骤 4：更新测试

涉及文件
- `tests/ssv_deps_test.sh`：修改

涉及标识符
- `intel_defaults` 断言：`system|1.25.1` -> `managed|1.25.1`
- 原“Intel profile rejects a managed ONNX Runtime package”改为 managed 配置成功
- 新增 managed intel 工作区/目录可复用性相关断言（离线，不触发真实编译）

## 步骤 5：更新文档与示例配置

涉及文件
- `README.md`：修改
- `.env.example`：修改

## 步骤 6：离线测试

- 运行 `./ssv test`，确认依赖脚本测试与既有测试全部通过。

## 步骤 7：本机完整验证

- 运行 `./ssv build --profile intel`，观察下载、编译、探针与 Meson 结果。
- 检查 `build/ssv-deps.env` 与构建摘要。
- 重复执行一次确认缓存复用。

## 非本阶段范围

- AMD managed 源码构建。
- OpenVINO 自身源码构建。
- Intel GPU 驱动/OpenCL runtime 的自动安装。
