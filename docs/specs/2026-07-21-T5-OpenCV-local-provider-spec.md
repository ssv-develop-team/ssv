# T5 OpenCV local provider spec

**日期：** 2026-07-21
**主线：** T5 工程集成与质量
**跨主线影响：** T2 感知算法与元数据
**状态：** 已完成

## 1. 背景

managed OpenCV provider 使用项目准备的安装目录。开发者在宿主机上自行编译 OpenCV 时，当前只能把安装目录伪装成 system pkg-config 依赖，不能只提供头文件和库目录，也不能复用项目的 4.10.0 ABI 探针。

## 2. 目标

1. 增加 `SSV_OPENCV_SOURCE=local` provider。
2. 继续固定项目需要的 OpenCV 版本契约为 `4.10.0`。
3. 通过 `SSV_OPENCV_INCLUDE_DIR` 和 `SSV_OPENCV_LIB_DIR` 暴露本地编译安装的头文件、库文件目录。
4. local provider 生成项目私有的 `opencv4.pc`，执行和 managed 相同的模块、运行库闭包、编译和加载探针。
5. 不修改用户安装目录，不下载或替换本地 OpenCV。

## 3. 配置契约

| 配置 | 合法值 | 默认值 | 语义 |
| --- | --- | --- | --- |
| `SSV_OPENCV_SOURCE` | `managed`、`local`、`system` | `managed` | OpenCV 来源 |
| `SSV_OPENCV_MODE` | `enabled`、`disabled` | `enabled` | GMC 是否启用 |
| `SSV_OPENCV_ROOT` | managed 安装目录 | `.deps/opencv` | 仅 managed 使用 |
| `SSV_OPENCV_INCLUDE_DIR` | 已存在目录 | 无 | local 的 OpenCV 头文件根目录 |
| `SSV_OPENCV_LIB_DIR` | 已存在目录 | 无 | local 的 OpenCV 动态库目录 |

local 模式要求两个路径都存在，并拒绝显式设置 managed 专用的 `SSV_OPENCV_ROOT`。include 目录可以是 `include/opencv4` 或其父级 `include`；provider 会规范化为包含 `opencv2/core.hpp` 的目录。lib 目录必须直接包含 `libopencv_core.so*` 等项目所需模块。

## 4. Provider 行为

local provider 使用 `.deps/opencv-local` 保存生成的 `opencv4.pc`，其中 `includedir` 和 `libdir` 指向用户提供的绝对路径。它要求 `core`、`imgproc`、`video`、`calib3d`、`features2d`、`flann` 和 `dnn` 七个模块，检查这些模块的 OpenCV 内部 `DT_NEEDED` 闭包和传递运行库，并运行读取 `cv::getVersionString()` 的 C++ 探针。探针必须返回 `4.10.0`，因此 Python `cv2` 或其他版本的 OpenCV 不能替代 C++ SDK。local 安装中的其他未使用模块不参与校验。

provider 的结果仍遵循统一三行协议：版本、真实 pkg-config 目录、非系统运行库目录。构建快照记录运行库路径；build、run、test 和 inspect 继续使用同一份快照。

## 5. 本地编译验证

本阶段在仓库内 `.dep/source/opencv` 下载并编译 OpenCV 4.10.0，安装到该目录下的 `install` 子目录。网络失败时允许使用：

```bash
export HTTPS_PROXY=http://192.168.2.8:7890
```

验证命令为：

```bash
SSV_OPENCV_SOURCE=local \
SSV_OPENCV_INCLUDE_DIR="$PWD/.dep/source/opencv/install/include/opencv4" \
SSV_OPENCV_LIB_DIR="$PWD/.dep/source/opencv/install/lib" \
./ssv build
```

## 6. 非本阶段范围

- local provider 不负责编译 OpenCV；编译仍由开发者或外部构建流程完成。
- 不把 OpenCV 静态库或第三方通用 ABI 复制进仓库。
- 不改变 managed/system provider 的默认行为。
