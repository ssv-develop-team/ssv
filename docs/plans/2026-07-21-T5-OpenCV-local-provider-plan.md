# T5 OpenCV local provider plan

**日期：** 2026-07-21
**主线：** T5 工程集成与质量
**设计依据：** `docs/specs/2026-07-21-T5-OpenCV-local-provider-spec.md`
**状态：** 已完成

## 1. 实施步骤

1. 在依赖配置测试中冻结 `local` 来源、路径要求和冲突规则。
2. 抽取 OpenCV 的路径验证、pkg-config 生成和 ABI 探针，使 managed 与 local 共用同一套行为。
3. 新增 local provider，生成 `.deps/opencv-local/opencv4.pc`，并将 include/lib 路径纳入依赖签名。
4. 更新 `.env.example`、README、CI 相关检查和 `.gitignore`。
5. 在 `.dep/source/opencv` 编译 OpenCV 4.10.0 并安装到 `install`。
6. 用 local provider 完成 `./ssv build`、`meson test -C build` 和脚本测试。
7. 运行代码审查，确认 managed、system、disabled 路径未回归。

## 2. 验收标准

- `SSV_OPENCV_SOURCE=local` 在缺少任一路径时快速失败，并给出明确错误。
- local 路径包含有效的 OpenCV 4.10.0 C++ SDK 时，provider 探针和 Meson 构建成功。
- local 版本不是 4.10.0、只有 Python `cv2` 或缺少模块时，构建不会误通过。
- 生成的 `opencv4.pc` 不写入用户安装目录，运行快照包含本地库目录。
- 默认 managed、system 及 OpenCV disabled 的既有测试继续通过。
