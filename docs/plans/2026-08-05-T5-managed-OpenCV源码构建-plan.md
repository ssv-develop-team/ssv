# managed OpenCV 源码构建计划

## 步骤 1：替换 managed provider

- 修改 `scripts/deps/opencv-managed.sh`。
- 同步更新 `.env.example`、`tests/ssv_cli_test.sh` 和
  `tests/ssv_deps_test.sh` 中的来源、目录和构建流程断言。
- 保留现有 OpenCV 布局探针、运行库闭包检查、版本探针和 `opencv4.pc` 生成
  接口。
- 将 managed 准备流程改为源码归档缓存、CMake 配置、并行构建、安装和验证；
  复用 `scripts/deps/common.sh` 的下载、解压、候选目录和原子替换工具。
- 为 CMake、C++ 编译器和构建工具提供明确的缺失依赖错误。
- 保留旧 `.deps/opencv/managed` 目录但不再读取，避免破坏用户已有缓存。

## 步骤 2：调整依赖测试

- 修改 `tests/ssv_deps_test.sh` 中 managed OpenCV 的工作区断言。
- 增加源码目录、构建目录、安装目录复用和无效安装重建的测试替身。
- 保持 local/system OpenCV、运行库闭包、版本不匹配和快照测试覆盖。

## 步骤 3：同步使用文档

- 修改 `README.md` 中 managed OpenCV 的来源、前置工具和产物说明。
- 更新本规范涉及的目录和验证命令，明确不会执行系统包安装。

## 步骤 4：验证

- 运行 `bash -n` 检查依赖脚本和测试脚本。
- 运行 `bash tests/ssv_deps_test.sh`。
- 在可用网络和编译工具环境执行一次 managed OpenCV provider 验证，并运行
  `./ssv build --profile cpu` 或等价的已有构建验证。
- 检查 `git diff`、`git status`，确保没有把 `.deps`、日志或临时文件加入交付。
