# Intel managed ONNX Runtime 源码构建规范

## 1. 背景

当前 `--profile intel` 只支持 system/local 来源的 ONNX Runtime，要求宿主预先提供带
OpenVINO Execution Provider 的 `onnxruntime.pc`。官方 ONNX Runtime Release 不发布 Linux
C++ OpenVINO 制品，Intel 自己的 OpenVINO EP 发布也只提供 Windows zip 与 Python wheel，
因此“下载即用”的 managed 路径缺失。

本规范为 Intel profile 增加 managed 源码构建 provider：由脚本下载 ONNX Runtime
`1.25.1` 源码与 OpenVINO `2025.4.1` Linux runtime 归档，在本机构建带
OpenVINOExecutionProvider 的共享库，再复用现有探针、pkg-config 与依赖签名流程接入
Meson。

## 2. 目标

1. `./ssv build --profile intel` 在具备基础工具链（CMake、C++ 编译器、Python 3、
   ninja 或 make）的 Linux x86_64 主机上自动完成 ORT 源码构建，不再要求宿主预装
   OpenVINO 或 system `onnxruntime.pc`。
2. managed Intel 产物与 CPU/NVIDIA 一样经过严格校验：版本精确 `1.25.1`，Provider
   集合为 `OpenVINOExecutionProvider,CPUExecutionProvider`，Provider 动态库与运行时
   闭包完整，构建结果写入成功依赖快照。
3. CPU/NVIDIA 的 managed 预编译路径、AMD 的 system/local 路径保持不变。

## 3. 配置与默认值

| 配置 | 值 |
| --- | --- |
| `SSV_DEPS_PROFILE=intel` | 默认 `SSV_ONNXRUNTIME_SOURCE=managed` |
| `SSV_ONNXRUNTIME_ROOT` | 默认 `.deps/onnxruntime-openvino` |
| ONNX Runtime 版本 | 固定 `1.25.1`（源码 tag `v1.25.1`） |
| OpenVINO 版本 | 固定 `2025.4.1`（Linux x86_64 runtime 归档） |
| `SSV_ONNXRUNTIME_BUILD_JOBS` | 可选；默认等于逻辑 CPU 数，必须为正整数 |

`SSV_ONNXRUNTIME_SOURCE=local|system` 仍可覆盖 managed 默认值，local/system 的版本与
Provider 校验规则不变。

## 4. 工作区与缓存布局

```text
.deps/onnxruntime-openvino/
├── source/onnxruntime-1.25.1/    # ORT 源码
├── build/                        # ORT CMake/Ninja 构建目录（保留复用）
├── openvino/                     # OpenVINO runtime 解包目录（保留复用）
└── install/                      # 组装后的 ORT 安装根：include/lib/VERSION_NUMBER

.deps/downloads/
├── onnxruntime/v1.25.1/onnxruntime-v1.25.1.tar.gz
└── openvino/2025.4.1/openvino_toolkit_ubuntu22_2025.4.1.20426.82bbf0292c5_x86_64.tgz
```

`install/` 是原子替换边界；`source/`、`build/`、`openvino/` 只允许空目录或可识别的
既有内容，否则拒绝覆盖。下载缓存复用 `ssv_deps_cached_download`，坏缓存下载一次后重试。

## 5. 构建与组装

1. 校验并复用已有 `install/`；若已通过 `ssv_onnxruntime_validate_layout`，直接返回。
2. 下载并解包 OpenVINO runtime，确认 `runtime/cmake/OpenVINOConfig.cmake` 与
   `runtime/lib/intel64/libopenvino.so` 存在。
3. 下载并解包 ORT 源码，确认 `tools/ci_build/build.py` 存在。
4. 执行：

   ```text
   python3 tools/ci_build/build.py \
     --config Release \
     --build_dir <build> \
     --use_openvino GPU \
     --build_shared_lib \
     --skip_tests \
     --parallel <jobs> \
     --cmake_extra_defines OpenVINO_DIR=<openvino>/runtime/cmake
   ```

5. 从 `build/Release` 组装 `install/`：复制 `include/onnxruntime`、
   `libonnxruntime.so*`、`libonnxruntime_providers_shared.so`、
   `libonnxruntime_providers_openvino.so`，写入 `VERSION_NUMBER=1.25.1`。
6. 对 `install/` 调用既有 `ssv_onnxruntime_validate_layout`，生成 `onnxruntime.pc`
   并完成编译/加载探针、Provider 集合校验与运行时闭包检查。
7. 全部通过后原子替换 `build/` 与 `install/`。

## 6. 约束

- 只支持 Linux x86_64；OpenVINO runtime 归档本身是 Ubuntu 22 构建，脚本不安装其
  `install_dependencies` 脚本中的系统包。
- 脚本不安装 Intel GPU 驱动/OpenCL runtime；GPU 实际推理由宿主系统提供
  （例如 Arch 的 `intel-compute-runtime`）。
- 不修改 AMD profile 的默认来源；AMD managed 仍明确拒绝。
- 不把 GStreamer、yaml-cpp、hiredis、nlohmann-json 纳入 managed 范围。

## 7. 验收

1. `./ssv test` 离线测试通过，包含 Intel 默认 managed 配置断言。
2. `./ssv build --profile intel` 在本机完整走通：下载、解包、编译、探针、Meson 编译。
3. 构建摘要显示 `profile=intel source=managed version=1.25.1
   providers=CPUExecutionProvider,OpenVINOExecutionProvider`。
4. `build/ssv-deps.env` 记录 `SSV_DEPS_PROFILE=intel`、managed PCDIR 与
   RUNTIME_DIRS。
5. 再次执行 `./ssv build --profile intel` 复用已有 `install/`，不重复编译。

## 8. 验证结果（2026-08-07）

- 本机完整走通，构建摘要与快照符合第 7 节；二次构建 2 秒完成并复用 install。
- `tests/ssv_deps_test.sh` 97/97 通过，`./ssv test` 全部通过。
- 未安装 `intel-compute-runtime` 时 OpenVINO GPU 回退 CPU，属预期行为。
