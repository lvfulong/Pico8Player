## `pico_editor` 使用说明

`pico_editor` 是一个最小的 PICO-8 `.p8` 地图查看器（目前偏“查看”，不做修改写回）。它会从 `.p8` 文本文件中读取并解析 `__gfx__` / `__map__`（可选 `__gff__`），然后用工程现有的渲染代码把 tilemap 画出来。

### 功能

- **导入 `.p8`**：从磁盘读取 `.p8`（纯文本）并加载资源段
- **显示地图**：按 **16×16** 个 tile 的视口绘制（每 tile 8×8 像素）
- **滚动查看**：方向键平移地图视口

### 构建（Windows）

前置依赖：

- **Visual Studio 2022**（MSVC 工具链）
- **CMake**
- **Git**（用于子模块）
- **Python 3**（用于生成 `src/generated/static_game_data.h`）

方式 A：用脚本（推荐，Git Bash / MSYS2 Bash）

```bash
./build_windows_editor.sh
```

方式 B：手动执行（PowerShell / CMD 也可）

```bash
cmake -S . -B pc_pico_editor -DBACKEND=PC
cmake --build pc_pico_editor --target pico_editor --config Release -j 8
```

构建产物（Release）默认在：

- `pc_pico_editor/Release/pico_editor.exe`

### 运行

#### 1) 不带参数（默认文件）

默认会尝试加载：

- `carts/map.p8`

#### 2) 指定 `.p8` 路径

```bash
pc_pico_editor/Release/pico_editor.exe carts/celeste.p8
```

### 操作说明

- **方向键**：滚动地图视口
- **ESC / Q**：退出
- **F1**：打开/关闭“关卡下拉菜单”
  - 打开后用 **↑/↓** 选择关卡
  - 按 **Enter** 切换并重新加载显示

### `.p8` 支持范围

当前实现解析这些段：

- **`__gfx__`**：精灵表（128×128 像素，按 hex 字符读取调色板索引）
- **`__map__`**：地图（按 2 个 hex 字符 = 1 字节 tile id，读取 64 行 × 128 列）
- **`__gff__`**（可选）：精灵 flags（256 字节）

显示逻辑与运行时保持一致：

- **tile id = 0** 会被当作“空”，不绘制

### 已知限制 / TODO

- **不支持 `.p8.png`**（只支持文本 `.p8`）
- **不解析扩展地图（`__gfx__` 下半区映射到 map 的那部分）**：目前仅显示 `__map__` 段里的 64×128
- **不支持编辑/写回**：当前是查看器（后续可扩展为真正的编辑器：点选 tile、画笔、保存等）
- **不支持 PICO-8 的调色板/透明映射指令影响**：目前按默认调色板直接渲染

### 代码位置

- CMake target：`CMakeLists.txt`（`BACKEND=PC` 分支里的 `add_executable(pico_editor ...)`）
- 实现：`src/pico_editor.cpp`

