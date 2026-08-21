# GTK 显示后端选择规范

## 背景

`./ssv run --display` 在同时存在 `DISPLAY` 和 `WAYLAND_DISPLAY` 的桌面环境中，当前由 GTK
自己决定 `auto` 后端。部分 WSLg、远程桌面或嵌套 Weston 会话没有提供可靠的 Wayland 窗口
呈现路径，但 GTK 仍会优先连接 Wayland，导致窗口 surface 已创建却没有出现在用户正在使用的
窗口路径中。相同环境强制使用 X11 时，窗口可以正常映射。

## 需求

1. `display.gl_backend` 显式为 `x11` 或 `wayland` 时，保持严格选择，不由自动规则改写。
2. `auto` 根据当前会话和可用显示环境选择稳定的 GTK 后端：
   - 明确的 `XDG_SESSION_TYPE=wayland` 优先 Wayland；
   - 没有明确 Wayland 会话且 `DISPLAY` 可用时优先 X11；
   - 只有 Wayland 可用时选择 Wayland；
   - 没有可用显示时保留 GTK 的初始化失败语义。
3. 用户显式设置非空 `GDK_BACKEND` 时，自动模式不得覆盖该环境变量的选择。
4. 显示窗口调用 `show()` 后必须向窗口管理器请求呈现，而不只完成 widget 映射。

## 约束

- 后端选择发生在 `gtk_init_check()` 前；GTK 初始化后不得切换后端。
- GTK widget 的创建、呈现和销毁继续由同一个 GTK 主线程执行。
- 不改变 GStreamer sink、推理、overlay 数据和窗口资源所有权。
- 显式后端失败仍由现有 `SsvDisplayWindowError` 传播，不增加静默回退。

## 非本阶段范围

- 不重构 GTK 窗口生命周期。
- 不实现 Wayland 输出枚举、窗口跨显示器定位或桌面环境探测。
- 不修改 `gtksink`/`gtkglsink` 的管线选择规则。

## 验收标准

- 自动选择规则对 Wayland-only、X11-only、明确 Wayland 会话和混合但未声明会话均有测试。
- `show()` 的显示集成测试能观察到 SSV 顶层窗口。
- 原始命令在当前混合环境下能创建带有 `Site Safety Vision - local-test` 标题的窗口。
- 显示测试和 Meson 测试通过。
