# GTK 显示后端选择实施计划

## 步骤 1：锁定自动选择行为

修改 `runner/display/ssv_display_window.hpp`、`runner/display/ssv_display_window.cpp` 和
`runner/display/tests/test_ssv_display_window.cpp`。

- 新增可测试的自动后端解析函数，输入会话类型和两个显示环境是否存在。
- 在 `SsvDisplayWindow::initialize` 中应用解析结果，并保留显式 `GDK_BACKEND` 的优先级。
- 为混合环境、明确 Wayland 会话、单一显示后端补充单元测试。

## 步骤 2：锁定窗口呈现行为

修改 `runner/display/ssv_display_window.cpp` 和
`runner/display/tests/test_ssv_display_window.cpp`。

- `DisplayState::show` 在显示 widget 后调用 `gtk_window_present`。
- 显示集成测试在 GTK 主循环中检查带有 source ID 的顶层窗口。

## 步骤 3：更新使用说明

修改 `README.md`，说明 `auto` 的选择条件、显式 `GDK_BACKEND` 覆盖方式和当前环境的验证命令。

## 验证

- `meson test -C build --suite display`
- `meson test -C build`
- `./ssv run --display --overlay` 配合 X11 窗口树检查
- `GDK_BACKEND=wayland ./ssv run --display --overlay` 保持可启动并保留 Wayland 行为
