// Pico8 Editor - Dear ImGui + SDL2 + OpenGL3
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Engine includes (unity build) — MUST come before SDL/ImGui headers to avoid
// Windows macro collision: winuser.h defines DrawStateA which conflicts with
// our DrawState struct in data.h
#include "lua/fix32.h"
#include "lua/lua.h"
#include <stdbool.h>
#include "engine.c"

// Undefine Windows macros that conflict with our identifiers
#ifdef DrawState
#undef DrawState
#endif

// Dear ImGui
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <SDL.h>
#include <SDL_opengl.h>

// We do NOT include sdl_backend.c — we implement backend functions here for ImGui/OpenGL

namespace fs = std::filesystem;

// ─── Backend globals ───
static SDL_Window*   gWindow   = nullptr;
static SDL_GLContext  gGLContext = nullptr;
static GLuint         gGameTexture = 0;
static uint8_t        rgba_pixels[128 * 128 * 4];
static GLuint         gMapTexture = 0;
static uint8_t*       map_rgba = nullptr;

// ─── Backend interface implementation (required by engine.c) ───

bool init_platform() { return true; }

bool init_video() {
    memset(frontbuffer, 0, sizeof(frontbuffer));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return false;
    }

    // OpenGL 3.3 core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    gWindow = SDL_CreateWindow("Pico Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        W_SCREEN_WIDTH, W_SCREEN_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!gWindow) {
        printf("Window creation failed: %s\n", SDL_GetError());
        return false;
    }

    gGLContext = SDL_GL_CreateContext(gWindow);
    if (!gGLContext) {
        printf("GL context creation failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(gWindow, gGLContext);
    SDL_GL_SetSwapInterval(1); // vsync

    // Create game texture (128x128 RGBA)
    glGenTextures(1, &gGameTexture);
    glBindTexture(GL_TEXTURE_2D, gGameTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    memset(rgba_pixels, 0, sizeof(rgba_pixels));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels);

    return true;
}

void video_close() {
    if (gMapTexture) { glDeleteTextures(1, &gMapTexture); gMapTexture = 0; }
    if (map_rgba) { delete[] map_rgba; map_rgba = nullptr; }
    if (gGameTexture) { glDeleteTextures(1, &gGameTexture); gGameTexture = 0; }
    if (gGLContext) { SDL_GL_DeleteContext(gGLContext); gGLContext = nullptr; }
    if (gWindow) { SDL_DestroyWindow(gWindow); gWindow = nullptr; }
    SDL_Quit();
}

// gfx_flip: convert frontbuffer to RGBA and upload to GL texture
void gfx_flip() {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            palidx_t idx = get_pixel(x, y);
            color_t c = palette[idx];
            int off = (y * 128 + x) * 4;
            rgba_pixels[off + 0] = (uint8_t)((c >> 11) << 3);
            rgba_pixels[off + 1] = (uint8_t)(((c >> 5) & 0x3f) << 2);
            rgba_pixels[off + 2] = (uint8_t)((c & 0x1f) << 3);
            rgba_pixels[off + 3] = 255;
        }
    }
    glBindTexture(GL_TEXTURE_2D, gGameTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels);
}

void draw_hud() {}  // HUD is now part of ImGui UI

void delay(uint16_t ms) { SDL_Delay(ms); }

// handle_input is called from the main loop; we process SDL events there
bool handle_input() { return false; }

uint32_t now() { return SDL_GetTicks(); }

void MyAudioCallback(void* userdata, Uint8* stream, int len) {
    uint16_t samples = SAMPLES_PER_DURATION * SAMPLES_PER_BUFFER;
    memset(stream, 0, len);
    for (uint8_t i = 0; i < 4; i++)
        fill_buffer((uint16_t*)stream, &channels[i], samples);
}

bool init_audio() {
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 1;
    want.samples = SAMPLES_PER_DURATION * SAMPLES_PER_BUFFER;
    want.callback = MyAudioCallback;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev < 2) {
        printf("Failed to open audio device\n");
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

uint8_t current_hour() {
    time_t t = time(nullptr);
    return localtime(&t)->tm_hour;
}
uint8_t current_minute() {
    time_t t = time(nullptr);
    return localtime(&t)->tm_min;
}
uint8_t wifi_strength() { return 0; }
uint8_t battery_left()  { return 3; }

// ─── Editor helpers (from original pico_editor) ───

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
                out.push_back(p.generic_string());
            }
        }
    } catch (...) {}
    std::sort(out.begin(), out.end());
    return out;
}

static std::string filename_only(const std::string& p) {
    try { return fs::path(p).filename().string(); }
    catch (...) { return p; }
}

static void draw_map_view_scaled(int map_x, int map_y, int screen_x, int screen_y,
                                  int tiles_w, int tiles_h, z8::fix32 scale) {
    tiles_w = MAX(1, MIN(tiles_w, 128));
    tiles_h = MAX(1, MIN(tiles_h, 64));
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
        printf("Cannot open: %s\n", path.c_str());
        return false;
    }

    memset(spritesheet.sprite_data, 0, sizeof(spritesheet.sprite_data));
    memset(spritesheet.flags, 0, sizeof(spritesheet.flags));
    memset(map_data, 0, sizeof(map_data));

    enum class Section { None, Gfx, Map, Gff };
    Section sec = Section::None;
    int gfx_y = 0, map_y = 0, gff_i = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("__", 0) == 0) {
            if      (line == "__gfx__") { sec = Section::Gfx; gfx_y = 0; }
            else if (line == "__map__") { sec = Section::Map; map_y = 0; }
            else if (line == "__gff__") { sec = Section::Gff; gff_i = 0; }
            else                        { sec = Section::None; }
            continue;
        }
        if (sec == Section::Gfx) {
            if (gfx_y >= 128) continue;
            int x = 0;
            for (size_t i = 0; i < line.size() && x < 128; i++) {
                int n = hex_nibble(line[i]);
                if (n < 0) continue;
                spritesheet.sprite_data[gfx_y * 128 + x] = (uint8_t)n;
                x++;
            }
            gfx_y++;
        } else if (sec == Section::Map) {
            if (map_y >= 64) continue;
            int x = 0, half = -1;
            for (size_t i = 0; i < line.size() && x < 128; i++) {
                int n = hex_nibble(line[i]);
                if (n < 0) continue;
                if (half < 0) { half = n; }
                else {
                    map_data[map_y * 128 + x] = (uint8_t)((half << 4) | n);
                    x++; half = -1;
                }
            }
            map_y++;
        } else if (sec == Section::Gff) {
            int half = -1;
            for (size_t i = 0; i < line.size() && gff_i < 256; i++) {
                int n = hex_nibble(line[i]);
                if (n < 0) continue;
                if (half < 0) { half = n; }
                else {
                    spritesheet.flags[gff_i++] = (uint8_t)((half << 4) | n);
                    half = -1;
                }
            }
        }
    }

    // Extended map (gfx lower half shares map data)
    for (int i = 32; i < 64; i++) {
        mapParser((const uint8_t*)spritesheet.sprite_data + (i * 256), i, map_data);
    }

    printf("Loaded: %s\n", path.c_str());
    return true;
}

// ─── Map to RGBA texture (bypasses 128x128 frontbuffer) ───
// Full map is 128x64 tiles, each 8x8 pixels = 1024x512 pixels max
static const int MAP_TEX_W = 128 * 8; // 1024
static const int MAP_TEX_H = 64 * 8;  // 512

static inline void color_to_rgba(color_t c, uint8_t* out) {
    out[0] = (uint8_t)((c >> 11) << 3);
    out[1] = (uint8_t)(((c >> 5) & 0x3f) << 2);
    out[2] = (uint8_t)((c & 0x1f) << 3);
    out[3] = 255;
}

// Render full map to map_rgba buffer at 1:1 (1 pixel per sprite pixel)
static void update_map_texture() {
    if (!map_rgba) {
        map_rgba = new uint8_t[MAP_TEX_W * MAP_TEX_H * 4];
    }
    // Clear to background color (color 0)
    color_t bg = palette[0];
    uint8_t bg_rgba[4];
    color_to_rgba(bg, bg_rgba);
    for (int i = 0; i < MAP_TEX_W * MAP_TEX_H; i++) {
        memcpy(map_rgba + i * 4, bg_rgba, 4);
    }

    // Draw each map tile
    for (int ty = 0; ty < 64; ty++) {
        for (int tx = 0; tx < 128; tx++) {
            uint8_t spr = map_data[tx + ty * 128];
            if (spr == 0) continue;
            // Sprite position in spritesheet: 16 sprites per row, each 8x8
            int sx0 = (spr % 16) * 8;
            int sy0 = (spr / 16) * 8;
            int px0 = tx * 8;
            int py0 = ty * 8;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    palidx_t idx = spritesheet.sprite_data[(sy0 + py) * 128 + (sx0 + px)];
                    if (idx == 0) continue; // transparent
                    color_t c = palette[idx];
                    int off = ((py0 + py) * MAP_TEX_W + (px0 + px)) * 4;
                    color_to_rgba(c, map_rgba + off);
                }
            }
        }
    }

    if (!gMapTexture) {
        glGenTextures(1, &gMapTexture);
        glBindTexture(GL_TEXTURE_2D, gMapTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border_color[] = { 0, 0, 0, 1 };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MAP_TEX_W, MAP_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, map_rgba);
    } else {
        glBindTexture(GL_TEXTURE_2D, gMapTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MAP_TEX_W, MAP_TEX_H, GL_RGBA, GL_UNSIGNED_BYTE, map_rgba);
    }
}

// ─── Spritesheet to RGBA texture ───
static GLuint gSpritesheetTexture = 0;
static uint8_t spritesheet_rgba[128 * 128 * 4];

static void update_spritesheet_texture() {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            palidx_t idx = spritesheet.sprite_data[y * 128 + x];
            color_t c = palette[idx];
            int off = (y * 128 + x) * 4;
            spritesheet_rgba[off + 0] = (uint8_t)((c >> 11) << 3);
            spritesheet_rgba[off + 1] = (uint8_t)(((c >> 5) & 0x3f) << 2);
            spritesheet_rgba[off + 2] = (uint8_t)((c & 0x1f) << 3);
            spritesheet_rgba[off + 3] = 255;
        }
    }
    if (!gSpritesheetTexture) {
        glGenTextures(1, &gSpritesheetTexture);
        glBindTexture(GL_TEXTURE_2D, gSpritesheetTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, spritesheet_rgba);
    } else {
        glBindTexture(GL_TEXTURE_2D, gSpritesheetTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, spritesheet_rgba);
    }
}

// ─── Main ───

int main(int argc, char** argv) {
    if (!init_video()) return 1;
    init_audio();
    engine_init();

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;

    ImGui_ImplSDL2_InitForOpenGL(gWindow, gGLContext);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Scan cartridges
    // Try multiple paths to find carts directory
    std::string carts_dir = "carts";
    if (!fs::exists(carts_dir)) {
        // Try relative to executable
        carts_dir = "C:/Users/77533/Documents/Pico8Player/carts";
    }
    std::vector<std::string> levels = list_p8_files(carts_dir);
    int current_level = -1;
    std::string current_file;
    bool file_loaded = false;

    // If command line arg provided
    if (argc >= 2) {
        current_file = fs::path(argv[1]).generic_string();
        auto it = std::find(levels.begin(), levels.end(), current_file);
        if (it != levels.end()) current_level = (int)std::distance(levels.begin(), it);
        if (load_p8_file(current_file)) {
            file_loaded = true;
            update_spritesheet_texture();
            update_map_texture();
        }
    } else if (!levels.empty()) {
        current_level = 0;
        current_file = levels[0];
        if (load_p8_file(current_file)) {
            file_loaded = true;
            update_spritesheet_texture();
            update_map_texture();
        }
    }

    // Map view state
    int map_x = 0, map_y = 0;
    float map_drag_accum_x = 0.0f, map_drag_accum_y = 0.0f;
    static const z8::fix32 zoom_levels[] = {
        z8::fix32(1) / 16, // 0.5px
        z8::fix32(1) / 8,  // 1px
        z8::fix32(1) / 4,  // 2px
        z8::fix32(1) / 2,  // 4px
        z8::fix32(1),      // 8px  (default)
        z8::fix32(2),      // 16px
        z8::fix32(4),      // 32px
        z8::fix32(8),      // 64px
        z8::fix32(16),     // 128px
    };
    const int zoom_count = (int)(sizeof(zoom_levels) / sizeof(zoom_levels[0]));
    int zoom_idx = 4;

    // File browser state
    std::string browse_dir = carts_dir;
    bool show_spritesheet = true;
    bool show_map = true;
    bool show_files = true;
    bool show_info = true;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) done = true;
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ─── Menu Bar ───
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Directory...")) {
                    // Simple: let user type a path
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "ESC")) done = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Cartridge Files", nullptr, &show_files);
                ImGui::MenuItem("Map View", nullptr, &show_map);
                ImGui::MenuItem("Spritesheet", nullptr, &show_spritesheet);
                ImGui::MenuItem("Info", nullptr, &show_info);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // ─── Cartridge Files Panel ───
        if (show_files) {
            ImGui::Begin("Cartridge Files", &show_files);

            // Directory input
            char dir_buf[512];
            strncpy(dir_buf, browse_dir.c_str(), sizeof(dir_buf) - 1);
            dir_buf[sizeof(dir_buf) - 1] = '\0';
            if (ImGui::InputText("Directory", dir_buf, sizeof(dir_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                browse_dir = dir_buf;
                levels = list_p8_files(browse_dir);
                current_level = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                levels = list_p8_files(browse_dir);
            }

            ImGui::Separator();
            ImGui::Text("%d cartridge(s) found", (int)levels.size());

            // File list
            if (ImGui::BeginChild("FileList", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < (int)levels.size(); i++) {
                    bool is_selected = (i == current_level);
                    std::string label = filename_only(levels[i]);
                    if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        current_level = i;
                        current_file = levels[i];
                        if (load_p8_file(current_file)) {
                            file_loaded = true;
                            map_x = 0;
                            map_y = 0;
                            update_spritesheet_texture();
                            update_map_texture();
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // ─── Map View ───
        if (show_map) {
            ImGui::Begin("Map View", &show_map);
            if (file_loaded && gMapTexture) {
                // Zoom controls
                const char* zoom_labels[] = { "x1/16", "x1/8", "x0.25", "x0.5", "x1", "x2", "x4", "x8", "x16" };
                ImGui::SliderInt("Zoom", &zoom_idx, 0, zoom_count - 1, zoom_labels[zoom_idx]);
                if (ImGui::Button("Reset")) { map_x = 0; map_y = 0; zoom_idx = 4; }

                // Mouse wheel zoom when hovering over this window
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel > 0.0f && zoom_idx < zoom_count - 1) zoom_idx++;
                    if (wheel < 0.0f && zoom_idx > 0) zoom_idx--;
                }

                ImGui::Separator();

                // Calculate display
                const float zoom_f = (float)zoom_levels[zoom_idx];
                const float map_img_w = MAP_TEX_W * zoom_f;
                const float map_img_h = MAP_TEX_H * zoom_f;

                ImVec2 avail = ImGui::GetContentRegionAvail();
                ImVec2 cur = ImGui::GetCursorPos();

                // UV from tile pan offset
                float uv_x0 = (float)(map_x * 8) / MAP_TEX_W;
                float uv_y0 = (float)(map_y * 8) / MAP_TEX_H;
                float uv_x1 = uv_x0 + avail.x / map_img_w;
                float uv_y1 = uv_y0 + avail.y / map_img_h;

                // Display (CLAMP_TO_BORDER fills out-of-range UVs with black)
                ImGui::Image((ImTextureID)(intptr_t)gMapTexture, avail,
                             ImVec2(uv_x0, uv_y0), ImVec2(uv_x1, uv_y1));

                // Overlay invisible button for drag interaction (covers full area)
                ImGui::SetCursorPos(cur);
                ImGui::InvisibleButton("map_drag", avail);

                // Mouse drag to pan map (sub-tile smooth panning via float offsets)
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    // Convert screen pixels to tile offset
                    float tile_screen_px = 8.0f * zoom_f;
                    map_drag_accum_x -= delta.x;
                    map_drag_accum_y -= delta.y;
                    if (fabsf(map_drag_accum_x) >= tile_screen_px) {
                        int step = (int)(map_drag_accum_x / tile_screen_px);
                        map_x += step;
                        map_drag_accum_x -= step * tile_screen_px;
                    }
                    if (fabsf(map_drag_accum_y) >= tile_screen_px) {
                        int step = (int)(map_drag_accum_y / tile_screen_px);
                        map_y += step;
                        map_drag_accum_y -= step * tile_screen_px;
                    }
                } else {
                    map_drag_accum_x = 0.0f;
                    map_drag_accum_y = 0.0f;
                }
            } else {
                ImGui::TextWrapped("No cartridge loaded. Select a .p8 file from the Cartridge Files panel.");
            }
            ImGui::End();
        }

        // ─── Spritesheet View ───
        if (show_spritesheet) {
            ImGui::Begin("Spritesheet", &show_spritesheet);
            if (file_loaded && gSpritesheetTexture) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float scale = std::min(avail.x / 128.0f, avail.y / 128.0f);
                if (scale < 1.0f) scale = 1.0f;
                ImGui::Image((ImTextureID)(intptr_t)gSpritesheetTexture, ImVec2(128 * scale, 128 * scale));
            } else {
                ImGui::TextWrapped("No spritesheet loaded.");
            }
            ImGui::End();
        }

        // ─── Info Panel ───
        if (show_info) {
            ImGui::Begin("Info", &show_info);
            ImGui::Text("Current File: %s", file_loaded ? current_file.c_str() : "(none)");
            if (file_loaded) {
                // Count non-zero sprites
                int sprite_count = 0;
                for (int s = 0; s < 256; s++) {
                    bool has_data = false;
                    int sx = (s % 16) * 8, sy = (s / 16) * 8;
                    for (int py = 0; py < 8 && !has_data; py++)
                        for (int px = 0; px < 8 && !has_data; px++)
                            if (spritesheet.sprite_data[(sy + py) * 128 + sx + px] != 0)
                                has_data = true;
                    if (has_data) sprite_count++;
                }

                int map_tile_count = 0;
                for (int i = 0; i < 64 * 128; i++)
                    if (map_data[i] != 0) map_tile_count++;

                ImGui::Text("Non-empty sprites: %d / 256", sprite_count);
                ImGui::Text("Non-empty map tiles: %d / %d", map_tile_count, 64 * 128);
                ImGui::Text("Map position: (%d, %d)", map_x, map_y);
                static const char* zoom_names[] = {"x0.25","x0.5","x1","x2","x4"};
                ImGui::Text("Zoom: %s", zoom_names[zoom_idx]);
            }
            ImGui::Separator();
            ImGui::Text("FPS: %.1f", io.Framerate);
            ImGui::End();
        }

        // ─── Render ───
        ImGui::Render();
        int display_w, display_h;
        SDL_GL_GetDrawableSize(gWindow, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(gWindow);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    video_close();
    return 0;
}
