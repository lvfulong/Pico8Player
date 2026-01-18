#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
// 复用现有引擎的渲染/调色板/SDL 后端
#include "lua/fix32.h"
#include "lua/lua.h"
#include <stdbool.h>
#include "engine.c"
#if defined(SDL_BACKEND)
#include "sdl_backend.c"
#elif defined(PICO_BACKEND)
#include "pico_backend.c"
#elif defined(RAWDRAW_BACKEND)
#include "rawdraw_backend.c"
#elif defined(TEST_BACKEND)
#include "test_backend.c"
#elif defined(ESP_BACKEND)
// #include "esp/backend.c"
#elif defined(__3DS__)
#include "3ds_backend.cpp"
#endif

namespace fs = std::filesystem;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static std::vector<std::string> list_p8_files(const std::string& dir) {
    std::vector<std::string> out;
    try {
        if (!fs::exists(dir)) return out;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const auto p = entry.path();
            if (p.extension() == ".p8") {
                // 统一用 /，并尽量保存相对路径（方便从工程根目录运行）
                std::string s = p.generic_string();
                // 若是绝对路径，也能直接打开；相对路径更友好
                out.push_back(s);
            }
        }
    } catch (...) {
        // ignore
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void draw_map_view(int map_x, int map_y, int screen_x, int screen_y, int cell_w, int cell_h) {
    map_x = MAX(0, MIN(map_x, 127));
    map_y = MAX(0, MIN(map_y, 63));
    cell_w = MAX(1, MIN(cell_w, 16));
    cell_h = MAX(1, MIN(cell_h, 16));

    const uint8_t sprite_count = 16;
    for (int y = map_y; y < map_y + cell_h; y++) {
        if (y < 0 || y >= 64) continue;
        const int ty = screen_y + (y - map_y) * 8;
        for (int x = map_x; x < map_x + cell_w; x++) {
            if (x < 0 || x >= 128) continue;
            const uint8_t sprite = map_data[x + y * 128];
            // 与运行时一致：0 视为“空”
            if (sprite == 0) continue;

            const int tx = screen_x + (x - map_x) * 8;
            (void)sprite_count;
            render(&spritesheet, sprite, (uint16_t)tx, (uint16_t)ty, -1, false, false);
        }
    }
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void draw_map_view_scaled(int map_x, int map_y, int screen_x, int screen_y, int tiles_w, int tiles_h, z8::fix32 scale) {
    map_x = MAX(0, MIN(map_x, 127));
    map_y = MAX(0, MIN(map_y, 63));
    tiles_w = MAX(1, MIN(tiles_w, 128));
    tiles_h = MAX(1, MIN(tiles_h, 64));

    // tile 像素尺寸（8*scale），我们只用 2/4/8/16/32 这类整数，避免亚像素抖动
    const int tile_px = z8::fix32::ceil(8 * scale);

    for (int y = map_y; y < map_y + tiles_h; y++) {
        if (y < 0 || y >= 64) continue;
        const int ty = screen_y + (y - map_y) * tile_px;
        for (int x = map_x; x < map_x + tiles_w; x++) {
            if (x < 0 || x >= 128) continue;
            const uint8_t sprite = map_data[x + y * 128];
            if (sprite == 0) continue;
            const int tx = screen_x + (x - map_x) * tile_px;
            render_many(&spritesheet, sprite, (int16_t)tx, (int16_t)ty, -1, false, false, scale, scale);
        }
    }
}

static bool load_p8_file(const std::string& path) {
    std::ifstream in(path, std::ios::in);
    if (!in.good()) {
        std::printf("无法打开文件：%s\n", path.c_str());
        return false;
    }

    // 默认清空
    std::memset(spritesheet.sprite_data, 0, sizeof(spritesheet.sprite_data));
    std::memset(spritesheet.flags, 0, sizeof(spritesheet.flags));
    std::memset(map_data, 0, sizeof(map_data));

    enum class Section { None, Gfx, Map, Gff };
    Section sec = Section::None;
    int gfx_y = 0;
    int map_y = 0;
    int gff_i = 0; // 0..255

    std::string line;
    while (std::getline(in, line)) {
        // 去掉 Windows CR
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("__", 0) == 0) {
            if (line == "__gfx__") {
                sec = Section::Gfx;
                gfx_y = 0;
            } else if (line == "__map__") {
                sec = Section::Map;
                map_y = 0;
            } else if (line == "__gff__") {
                sec = Section::Gff;
                gff_i = 0;
            } else {
                sec = Section::None;
            }
            continue;
        }

        if (sec == Section::Gfx) {
            if (gfx_y >= 128) continue;
            // 每行最多 128 个十六进制字符（每个字符=1像素调色板索引）
            int x = 0;
            for (size_t i = 0; i < line.size() && x < 128; i++) {
                const int n = hex_nibble(line[i]);
                if (n < 0) continue;
                spritesheet.sprite_data[gfx_y * 128 + x] = (uint8_t)n;
                x++;
            }
            // 若行不足 128，剩余保持 0
            gfx_y++;
        } else if (sec == Section::Map) {
            if (map_y >= 64) continue;
            // 每行 128 个字节，表现为 256 个 hex 字符（2 chars per tile id）
            int x = 0;
            int half = -1;
            for (size_t i = 0; i < line.size() && x < 128; i++) {
                const int n = hex_nibble(line[i]);
                if (n < 0) continue;
                if (half < 0) {
                    half = n;
                } else {
                    const uint8_t byte = (uint8_t)((half << 4) | n);
                    map_data[map_y * 128 + x] = byte;
                    x++;
                    half = -1;
                }
            }
            map_y++;
        } else if (sec == Section::Gff) {
            // flags：256 字节（每个精灵 1 字节），通常为 2 行 * 128 字节
            int half = -1;
            for (size_t i = 0; i < line.size() && gff_i < 256; i++) {
                const int n = hex_nibble(line[i]);
                if (n < 0) continue;
                if (half < 0) {
                    half = n;
                } else {
                    spritesheet.flags[gff_i++] = (uint8_t)((half << 4) | n);
                    half = -1;
                }
            }
        }
    }

    // 兼容 PICO-8 “扩展地图”（map 的下半部分与 gfx 共享内存）
    // 复用项目里已有的 mapParser 逻辑（在 parser.c 中），把 gfx 的后半区解出 map 行 32..63。
    //
    // 说明：
    // - spritesheet.sprite_data 是 128*128 的“展开后”像素（每像素 0..15，占 1 字节）
    // - 每个 map tile id 是 1 字节（0..255），用两个 nibble 表示，分别存放在两个连续像素里
    // - 因此 1 行 map（128 byte）对应 256 个像素；在展开格式下就是 256 字节
    for (int i = 32; i < 64; i++) {
        // i*256 指向“256 像素/字节”的起始位置
        mapParser((const uint8_t*)spritesheet.sprite_data + (i * 256), i, map_data);
    }

    std::printf("已加载：%s\n", path.c_str());
    return true;
}

static std::string filename_only(const std::string& p) {
    try {
        return fs::path(p).filename().string();
    } catch (...) {
        return p;
    }
}

static void draw_dropdown(const std::vector<std::string>& items, int current_idx, int hover_idx, int scroll_start) {
    // 画面坐标（HUD_HEIGHT 不在 frontbuffer 内，这里都画在 0..127 的内容区）
    const int x0 = 1;
    const int y0 = 16;
    const int w = 126;
    const int row_h = 6;      // _print 默认行高约 6
    const int visible = 14;   // 最多显示行数
    const int count = (int)items.size();
    const int rows = MIN(visible, count - scroll_start);
    const int h = rows * row_h + 2;

    gfx_rectfill(x0, y0, x0 + w, y0 + h, 1); // 深蓝底
    gfx_rect(x0, y0, x0 + w, y0 + h, 7);     // 白边

    for (int i = 0; i < rows; i++) {
        const int idx = scroll_start + i;
        const int ty = y0 + 2 + i * row_h;
        const bool is_hover = (idx == hover_idx);
        const bool is_current = (idx == current_idx);

        if (is_hover) {
            gfx_rectfill(x0 + 1, ty - 1, x0 + w - 1, ty + row_h - 2, 12); // 蓝色高亮
        }

        std::string label = filename_only(items[idx]);
        if (is_current) label = "* " + label;
        _print(label.c_str(), (uint8_t)label.size(), x0 + 3, ty, is_hover ? 0 : 7);
    }
}

int main(int argc, char** argv) {
    const char* p8_path = (argc >= 2) ? argv[1] : nullptr;

    if (!init_video()) {
        std::printf("初始化视频失败\n");
        return 1;
    }

    // 复用现有资源（字体/HUD），否则 _print 之类无法显示
    engine_init();

    // 扫描 carts 下所有 .p8，当作“关卡列表”
    std::vector<std::string> levels = list_p8_files("carts");
    if (levels.empty()) {
        // 兜底：仍可通过命令行路径打开
        std::printf("未找到 carts/*.p8（将只支持命令行传入 .p8 路径）\n");
    }

    int current_level = 0;
    if (p8_path != nullptr) {
        // 若用户传入了路径：尝试选中它（找不到就当作临时路径直接加载）
        std::string arg = fs::path(p8_path).generic_string();
        auto it = std::find(levels.begin(), levels.end(), arg);
        if (it != levels.end()) {
            current_level = (int)std::distance(levels.begin(), it);
        } else {
            levels.insert(levels.begin(), arg);
            current_level = 0;
        }
    }

    if (!levels.empty()) {
        p8_path = levels[current_level].c_str();
    } else {
        // 完全无列表时的默认值
        p8_path = (p8_path != nullptr) ? p8_path : "carts/map.p8";
    }

    if (!load_p8_file(p8_path)) {
        video_close();
        return 1;
    }

    int map_x = 0;
    int map_y = 0;
    uint32_t last_move = now();

    // “下拉菜单”状态（键盘驱动）
    bool dropdown_open = false;
    int dropdown_hover = current_level;
    int dropdown_scroll = 0;
    bool prev_f1 = false;
    bool prev_enter = false;

    // 缩放：W 拉远（更小 tile -> 显示更多地图），S 拉近（更大 tile）
    // 使用固定等级，确保 tile 像素大小为整数：2/4/8/16/32
    static const z8::fix32 zoom_levels[] = {
        z8::fix32(1) / 4, // 2px
        z8::fix32(1) / 2, // 4px
        z8::fix32(1),     // 8px
        z8::fix32(2),     // 16px
        z8::fix32(4),     // 32px
    };
    const int zoom_levels_count = (int)(sizeof(zoom_levels) / sizeof(zoom_levels[0]));
    int zoom_idx = 2; // 默认 1.0
    bool prev_w = false;
    bool prev_s = false;

    while (true) {
        if (handle_input()) break;

        // 通过 SDL 键盘状态实现额外按键（不改 backend 的 handle_input）
        SDL_PumpEvents();
        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        const bool f1 = ks[SDL_SCANCODE_F1] != 0;
        const bool enter = ks[SDL_SCANCODE_RETURN] != 0;
        const bool key_w = ks[SDL_SCANCODE_W] != 0;
        const bool key_s = ks[SDL_SCANCODE_S] != 0;

        if (f1 && !prev_f1) {
            dropdown_open = !dropdown_open;
            dropdown_hover = current_level;
            dropdown_scroll = MAX(0, dropdown_hover - 6);
        }
        prev_f1 = f1;

        if (dropdown_open) {
            if (buttons_frame[BTN_IDX_UP]) dropdown_hover--;
            if (buttons_frame[BTN_IDX_DOWN]) dropdown_hover++;
            if (!levels.empty()) {
                dropdown_hover = MAX(0, MIN(dropdown_hover, (int)levels.size() - 1));
            } else {
                dropdown_hover = 0;
            }

            // 维持滚动窗口
            const int visible = 14;
            dropdown_scroll = MAX(0, MIN(dropdown_scroll, MAX(0, (int)levels.size() - visible)));
            if (dropdown_hover < dropdown_scroll) dropdown_scroll = dropdown_hover;
            if (dropdown_hover >= dropdown_scroll + visible) dropdown_scroll = dropdown_hover - visible + 1;

            if (enter && !prev_enter) {
                if (!levels.empty()) {
                    current_level = dropdown_hover;
                    p8_path = levels[current_level].c_str();
                    (void)load_p8_file(p8_path);
                    map_x = 0;
                    map_y = 0;
                }
                dropdown_open = false;
            }
            prev_enter = enter;
        } else {
            prev_enter = enter;
        }

        // 缩放只在菜单关闭时生效，避免和菜单导航冲突
        if (!dropdown_open) {
            if (key_w && !prev_w) {
                // 拉远：更小 scale
                zoom_idx = clampi(zoom_idx - 1, 0, zoom_levels_count - 1);
            }
            if (key_s && !prev_s) {
                // 拉近：更大 scale
                zoom_idx = clampi(zoom_idx + 1, 0, zoom_levels_count - 1);
            }
        }
        prev_w = key_w;
        prev_s = key_s;

        const uint32_t t = now();
        if (!dropdown_open) {
            const bool edge = buttons_frame[BTN_IDX_LEFT] || buttons_frame[BTN_IDX_RIGHT] || buttons_frame[BTN_IDX_UP] ||
                              buttons_frame[BTN_IDX_DOWN];
            const bool held = buttons[BTN_IDX_LEFT] || buttons[BTN_IDX_RIGHT] || buttons[BTN_IDX_UP] || buttons[BTN_IDX_DOWN];
            const uint32_t repeat_ms = 90;
            if (edge || (held && (t - last_move) > repeat_ms)) {
                if (buttons[BTN_IDX_LEFT]) map_x -= 1;
                if (buttons[BTN_IDX_RIGHT]) map_x += 1;
                if (buttons[BTN_IDX_UP]) map_y -= 1;
                if (buttons[BTN_IDX_DOWN]) map_y += 1;
                map_x = MAX(0, MIN(map_x, 128 - 16));
                map_y = MAX(0, MIN(map_y, 64 - 16));
                last_move = t;
            }
        }

        gfx_cls(0);
        const z8::fix32 zoom = zoom_levels[zoom_idx];
        const int tile_px = z8::fix32::ceil(8 * zoom);
        const int tiles_w = MAX(1, MIN(128, SCREEN_WIDTH / tile_px));
        const int tiles_h = MAX(1, MIN(64, SCREEN_HEIGHT / tile_px));

        // 视口边界根据缩放动态限制（避免拉远后 map_x 过大导致全黑）
        map_x = clampi(map_x, 0, MAX(0, 128 - tiles_w));
        map_y = clampi(map_y, 0, MAX(0, 64 - tiles_h));

        draw_map_view_scaled(map_x, map_y, 0, 0, tiles_w, tiles_h, zoom);

        char hud[128];
        std::snprintf(hud, sizeof(hud), "pico_editor  map(%d,%d)  zoom:%dpx  file:%s", map_x, map_y, tile_px, p8_path);
        _print(hud, (uint8_t)std::strlen(hud), 1, 1, 7);
        // 注意：内置字体/print 目前只稳定支持 ASCII，所以 UI 文案用 ASCII，避免乱码
        _print("F1 levels | ESC/Q quit | Arrows pan | W out | S in", (uint8_t)std::strlen("F1 levels | ESC/Q quit | Arrows pan | W out | S in"), 1, 8, 6);

        // 顶部“下拉框”当前选择显示
        if (!levels.empty()) {
            std::string cur = "Level: " + filename_only(levels[current_level]);
            _print(cur.c_str(), (uint8_t)cur.size(), 1, 14, 10);
        }
        if (dropdown_open) {
            draw_dropdown(levels, current_level, dropdown_hover, dropdown_scroll);
            _print("UP/DN select  Enter load  F1 close", (uint8_t)std::strlen("UP/DN select  Enter load  F1 close"), 2, 112, 7);
        }

        gfx_flip();
        delay(16);
    }

    video_close();
    return 0;
}

