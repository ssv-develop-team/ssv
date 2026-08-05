# managed OpenCV 源码构建规范

## 目标

将 `SSV_OPENCV_SOURCE=managed` 的实现从 Ubuntu 预编译 Debian 包切换为
OpenCV `4.10.0` 源码构建。构建入口仍然是 `./ssv build`，不要求用户手工
执行额外的 OpenCV 命令。

## 需求

1. managed provider 从 OpenCV 官方源码归档下载 `4.10.0`，解压到
   `.deps/opencv/source/opencv-4.10.0`。
2. 使用 CMake 的 Release 配置编译项目实际需要的 CPU OpenCV 模块，安装到
   `.deps/opencv/install`。
3. 安装布局必须包含 `include/opencv4`、`lib`，并由 provider 生成
   `lib/pkgconfig/opencv4.pc`。`pkg-config` 的输出、运行库目录和版本探针
   必须继续通过现有 OpenCV provider 接口返回。
4. 构建必须复用仓库已有下载缓存、代理环境、原子目录替换和错误处理机制；
   不执行 `apt install`、`dpkg -i`，也不把源码或构建产物提交到仓库。
5. 已存在且校验通过的 `install` 目录直接复用；源码归档和源码目录可以复用。
   安装目录无效时允许清理 `build` 与 `install` 后重建，但不得删除 managed
   工作区中的其他文件。
6. 旧版本留下的 `.deps/opencv/managed` 目录不删除，但不会被新 provider
   复用；首次使用新 provider 时会在 `install` 目录生成源码构建结果。
7. `SSV_OPENCV_SOURCE=local` 与 `SSV_OPENCV_SOURCE=system` 的输入、验证和
   输出接口保持不变。

## 构建约束

- OpenCV managed 构建使用宿主 C++ 编译器、CMake 和本地 BLAS/LAPACK；这些
  工具和宿主开发库由系统负责安装，脚本只检查并给出错误信息。
- 构建关闭 CUDA、GStreamer、GUI 和视频解码后端；OpenCV 只服务于
  BoT-SORT 的 sparse optical-flow GMC，不接管视频解码、显示或推理。
- 宿主只有 Ninja 而没有 make 时，provider 显式使用 Ninja CMake generator；
  并行度由 `SSV_OPENCV_BUILD_JOBS` 或宿主 CPU 数量决定。
- 失效安装在隔离的候选 `build/install` 目录中重建，完整探针通过后再原子
  替换稳定目录；未知非空目录不得被覆盖。
- 生成的 `opencv4.pc` 只暴露项目需要的 OpenCV 模块和宿主数学库，不把
  构建目录写入公开链接参数。

## 非本阶段范围

- 不修改系统 OpenCV 安装，不提供 `apt`/发行版包管理器自动安装。
- 不把 OpenCV 源码、源码归档、CMake 构建目录或安装目录纳入 Git。
- 不改变 ONNX Runtime、TensorRT、推理模型和运行时 Provider 的选择逻辑。
