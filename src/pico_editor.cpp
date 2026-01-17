#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
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

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
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

    std::printf("已加载：%s\n", path.c_str());
    return true;
}

int main(int argc, char** argv) {
    const char* p8_path = (argc >= 2) ? argv[1] : "carts/map.p8";

    if (!init_video()) {
        std::printf("初始化视频失败\n");
        return 1;
    }

    // 复用现有资源（字体/HUD），否则 _print 之类无法显示
    engine_init();

    if (!load_p8_file(p8_path)) {
        video_close();
        return 1;
    }

    int map_x = 0;
    int map_y = 0;
    uint32_t last_move = now();

    while (true) {
        if (handle_input()) break;

        const uint32_t t = now();
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

        gfx_cls(0);
        draw_map_view(map_x, map_y, 0, 0, 16, 16);

        char hud[128];
        std::snprintf(hud, sizeof(hud), "pico_editor  map(%d,%d)  file:%s", map_x, map_y, p8_path);
        _print(hud, (uint8_t)std::strlen(hud), 1, 1, 7);
        _print("ESC/Q 退出 | 方向键滚动", (uint8_t)std::strlen("ESC/Q 退出 | 方向键滚动"), 1, 8, 6);

        gfx_flip();
        delay(16);
    }

    video_close();
    return 0;
}

