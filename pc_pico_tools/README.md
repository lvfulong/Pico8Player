## pc_pico_tools

这里是 **PC 端命令行工具**集合，目前包含：

- **`p8_export`**：输入 `.p8` 路径，导出 `__lua__` 段为 `.lua`，并把 `__gfx__` 导出为 `PNG` 精灵表。

---

## p8_export 使用说明

### 1) 构建（Windows）

前置依赖：

- **Visual Studio 2022**（MSVC 工具链）
- **CMake**
- **Python 3**（工程会生成 `src/generated/static_game_data.h`，需要 Python）

在工程根目录执行（PowerShell / CMD 都可以，建议分两条命令执行）：

```bash
cmake -S . -B pc_pico_tools_build -DBACKEND=PC
cmake --build pc_pico_tools_build --target p8_export --config Release -j 8
```

构建产物一般在：

- `pc_pico_tools_build/pc_pico_tools/Release/p8_export.exe`

### 2) 运行

#### 基本用法

```bash
pc_pico_tools_build\pc_pico_tools\Release\p8_export.exe carts\map.p8
```

默认输出到 **输入文件所在目录**：

- `carts/map.lua`
- `carts/map_sprites.png`

#### 指定输出目录

```bash
pc_pico_tools_build\pc_pico_tools\Release\p8_export.exe carts\map.p8 --out-dir out
```

输出：

- `out/map.lua`
- `out/map_sprites.png`

### 3) 输出内容说明

- **`<cart>.lua`**：从 `.p8` 的 `__lua__` 段原样导出（按行拼接，保留换行）
- **`<cart>_sprites.png`**：
  - 尺寸：**128×128**
  - 像素：来自 `.p8` 的 `__gfx__` 段（hex nibble -> 调色板索引）
  - 调色板：使用 PICO-8 默认 16 色（RGB888）
- **`<cart>_map.csv`** / **`<cart>_map.json`** / **`<cart>_map.bin`**：
  - 尺寸：**128×64（tile id: 0..255）**
  - `csv/json`：可读格式
  - `bin`：原始 8192 bytes（64×128）
  - 规则：前 32 行来自 `__map__`；后 32 行按 PICO-8 共享内存规则从 `__gfx__` 解码（扩展地图）
- **`<cart>_gff.json`** / **`<cart>_gff.bin`**：
  - `gff` 为 **256 bytes**（每个 sprite 一个 flag byte）
  - `json`：256 个整数
  - `bin`：原始 256 bytes

### 4) 已知限制

- 只支持 **文本格式 `.p8`**，不支持 `.p8.png`
- 目前导出 `__lua__` / `__gfx__` / `__map__` / `__gff__`；暂不导出 `__sfx__` / `__music__` 等
- `PNG` 写入使用内置的最小实现（`stb_image_write.h` 的裁剪版），只实现 `stbi_write_png`

