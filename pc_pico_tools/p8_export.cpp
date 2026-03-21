#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static void usage() {
	std::fprintf(stderr,
		"Usage:\n"
		"  p8_export <cart.p8> [--out-dir <dir>]\n"
		"\n"
		"Outputs:\n"
		"  <out-dir>/<cart>.lua\n"
		"  <out-dir>/<cart>_sprites.png\n"
		"  <out-dir>/<cart>_map.csv\n"
		"  <out-dir>/<cart>_map.json\n"
		"  <out-dir>/<cart>_map.bin\n"
		"  <out-dir>/<cart>_gff.json\n"
		"  <out-dir>/<cart>_gff.bin\n"
	);
}

static bool write_text_file(const fs::path& p, const std::string& s) {
	std::ofstream out(p, std::ios::binary);
	if (!out.good()) return false;
	out.write(s.data(), (std::streamsize)s.size());
	return out.good();
}

static bool write_bin_file(const fs::path& p, const uint8_t* data, size_t len) {
	std::ofstream out(p, std::ios::binary);
	if (!out.good()) return false;
	out.write((const char*)data, (std::streamsize)len);
	return out.good();
}

int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 2;
	}

	fs::path in_path;
	fs::path out_dir;

	in_path = fs::path(argv[1]);
	for (int i = 2; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--out-dir" && i + 1 < argc) {
			out_dir = fs::path(argv[++i]);
		} else if (a == "-h" || a == "--help") {
			usage();
			return 0;
		} else {
			std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
			usage();
			return 2;
		}
	}

	if (out_dir.empty()) {
		out_dir = in_path.parent_path();
		if (out_dir.empty()) out_dir = ".";
	}
	out_dir = fs::absolute(out_dir);

	std::ifstream in(in_path, std::ios::in);
	if (!in.good()) {
		std::fprintf(stderr, "Failed to open: %s\n", in_path.string().c_str());
		return 1;
	}

	// 解析结果
	std::string lua_code;
	std::vector<uint8_t> gfx(128 * 128, 0);
	std::vector<uint8_t> map(64 * 128, 0);
	std::vector<uint8_t> gff(256, 0);
	bool have_lua = false;
	bool have_gfx = false;
	bool have_map = false;
	bool have_gff = false;

	enum class Section { None, Lua, Gfx, Map, Gff };
	Section sec = Section::None;
	int gfx_y = 0;
	int map_y = 0;
	int gff_i = 0;
	std::ostringstream lua;

	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();

		if (line.rfind("__", 0) == 0) {
			if (line == "__lua__") {
				sec = Section::Lua;
				have_lua = true;
				continue;
			}
			if (line == "__gfx__") {
				sec = Section::Gfx;
				have_gfx = true;
				gfx_y = 0;
				continue;
			}
			if (line == "__map__") {
				sec = Section::Map;
				have_map = true;
				map_y = 0;
				continue;
			}
			if (line == "__gff__") {
				sec = Section::Gff;
				have_gff = true;
				gff_i = 0;
				continue;
			}
			sec = Section::None;
			continue;
		}

		if (sec == Section::Lua) {
			lua << line << "\n";
		} else if (sec == Section::Gfx) {
			if (gfx_y >= 128) continue;
			int x = 0;
			for (size_t i = 0; i < line.size() && x < 128; i++) {
				int n = hex_nibble(line[i]);
				if (n < 0) continue;
				gfx[gfx_y * 128 + x] = (uint8_t)n;
				x++;
			}
			gfx_y++;
		} else if (sec == Section::Map) {
			if (map_y >= 64) continue;
			int x = 0;
			int half = -1;
			for (size_t i = 0; i < line.size() && x < 128; i++) {
				const int n = hex_nibble(line[i]);
				if (n < 0) continue;
				if (half < 0) {
					half = n;
				} else {
					map[map_y * 128 + x] = (uint8_t)((half << 4) | n);
					x++;
					half = -1;
				}
			}
			map_y++;
		} else if (sec == Section::Gff) {
			int half = -1;
			for (size_t i = 0; i < line.size() && gff_i < 256; i++) {
				const int n = hex_nibble(line[i]);
				if (n < 0) continue;
				if (half < 0) {
					half = n;
				} else {
					gff[gff_i++] = (uint8_t)((half << 4) | n);
					half = -1;
				}
			}
		}
	}

	lua_code = lua.str();

	const fs::path base_name = in_path.stem();
	const fs::path out_lua = out_dir / (base_name.string() + ".lua");
	const fs::path out_png = out_dir / (base_name.string() + "_sprites.png");
	const fs::path out_map_csv = out_dir / (base_name.string() + "_map.csv");
	const fs::path out_map_json = out_dir / (base_name.string() + "_map.json");
	const fs::path out_map_bin = out_dir / (base_name.string() + "_map.bin");
	const fs::path out_gff_json = out_dir / (base_name.string() + "_gff.json");
	const fs::path out_gff_bin = out_dir / (base_name.string() + "_gff.bin");

	// 确保输出目录存在（先建目录再写文件）
	std::error_code ec;
	fs::create_directories(out_dir, ec);
	if (ec) {
		std::fprintf(stderr, "Failed to create out dir: %s (%s)\n", out_dir.string().c_str(), ec.message().c_str());
		return 1;
	}
	if (!fs::exists(out_dir)) {
		std::fprintf(stderr, "Out dir does not exist: %s\n", out_dir.string().c_str());
		return 1;
	}

	// 输出 Lua
	if (!have_lua) {
		std::fprintf(stderr, "Warning: no __lua__ section found.\n");
	} else {
		if (!write_text_file(out_lua, lua_code)) {
			std::fprintf(stderr, "Failed to write: %s\n", out_lua.string().c_str());
			return 1;
		}
	}

	// 生成“完整地图”（0..31 来自 __map__；32..63 来自 gfx 共享区域）
	// PICO-8 内存里，map 的下半部分与 gfx 共用；这里按同样规则还原。
	if (!have_map) {
		std::fprintf(stderr, "Warning: no __map__ section found (map will be zeros except shared part).\n");
	}
	if (!have_gfx) {
		std::fprintf(stderr, "Warning: no __gfx__ section found (shared map part and sprites will be blank).\n");
	} else {
		for (int row = 32; row < 64; row++) {
			const int base = row * 256; // 每行 map 128 byte -> 256 个 nibble（来自 gfx 两行像素）
			for (int x = 0; x < 128; x++) {
				const uint8_t lo = (uint8_t)(gfx[base + x * 2 + 0] & 0x0F);
				const uint8_t hi = (uint8_t)(gfx[base + x * 2 + 1] & 0x0F);
				map[row * 128 + x] = (uint8_t)((hi << 4) | lo);
			}
		}
	}

	// 输出 map.bin
	if (!write_bin_file(out_map_bin, map.data(), map.size())) {
		std::fprintf(stderr, "Failed to write: %s\n", out_map_bin.string().c_str());
		return 1;
	}

	// 输出 map.csv（64 行，每行 128 个逗号分隔的十进制）
	{
		std::ofstream out(out_map_csv, std::ios::binary);
		if (!out.good()) {
			std::fprintf(stderr, "Failed to write: %s\n", out_map_csv.string().c_str());
			return 1;
		}
		for (int y = 0; y < 64; y++) {
			for (int x = 0; x < 128; x++) {
				out << (unsigned int)map[y * 128 + x];
				if (x != 127) out << ",";
			}
			out << "\n";
		}
	}

	// 输出 map.json
	{
		std::ofstream out(out_map_json, std::ios::binary);
		if (!out.good()) {
			std::fprintf(stderr, "Failed to write: %s\n", out_map_json.string().c_str());
			return 1;
		}
		out << "{\n";
		out << "  \"width\": 128,\n";
		out << "  \"height\": 64,\n";
		out << "  \"data\": [\n";
		for (int y = 0; y < 64; y++) {
			out << "    [";
			for (int x = 0; x < 128; x++) {
				out << (unsigned int)map[y * 128 + x];
				if (x != 127) out << ", ";
			}
			out << "]";
			if (y != 63) out << ",";
			out << "\n";
		}
		out << "  ]\n";
		out << "}\n";
	}

	// 输出 gff.bin（256 bytes）
	if (!have_gff) {
		std::fprintf(stderr, "Warning: no __gff__ section found (gff will be zeros).\n");
	}
	if (!write_bin_file(out_gff_bin, gff.data(), gff.size())) {
		std::fprintf(stderr, "Failed to write: %s\n", out_gff_bin.string().c_str());
		return 1;
	}

	// 输出 gff.json（256 个整数）
	{
		std::ofstream out(out_gff_json, std::ios::binary);
		if (!out.good()) {
			std::fprintf(stderr, "Failed to write: %s\n", out_gff_json.string().c_str());
			return 1;
		}
		out << "{\n";
		out << "  \"count\": 256,\n";
		out << "  \"flags\": [";
		for (int i = 0; i < 256; i++) {
			out << (unsigned int)gff[i];
			if (i != 255) out << ", ";
		}
		out << "]\n";
		out << "}\n";
	}

	// PICO-8 默认调色板（RGB888）
	static const uint8_t pal[16][3] = {
		{0, 0, 0},       {29, 43, 83},    {126, 37, 83},  {0, 135, 81},
		{171, 82, 54},   {95, 87, 79},    {194, 195, 199},{255, 241, 232},
		{255, 0, 77},    {255, 163, 0},   {255, 236, 39}, {0, 228, 54},
		{41, 173, 255},  {131, 118, 156}, {255, 119, 168},{255, 204, 170},
	};

	// 输出 PNG（RGBA）
	std::vector<uint8_t> rgba(128 * 128 * 4);
	for (int i = 0; i < 128 * 128; i++) {
		const uint8_t idx = (uint8_t)(gfx[i] & 0x0F);
		rgba[i * 4 + 0] = pal[idx][0];
		rgba[i * 4 + 1] = pal[idx][1];
		rgba[i * 4 + 2] = pal[idx][2];
		rgba[i * 4 + 3] = 255;
	}

	if (!stbi_write_png(out_png.string().c_str(), 128, 128, 4, rgba.data(), 128 * 4)) {
		std::fprintf(stderr, "Failed to write png: %s\n", out_png.string().c_str());
		return 1;
	}

	std::printf("OK\n");
	if (have_lua) std::printf("  lua: %s\n", out_lua.string().c_str());
	std::printf("  png: %s\n", out_png.string().c_str());
	std::printf("  map: %s\n", out_map_csv.string().c_str());
	std::printf("       %s\n", out_map_json.string().c_str());
	std::printf("       %s\n", out_map_bin.string().c_str());
	std::printf("  gff: %s\n", out_gff_json.string().c_str());
	std::printf("       %s\n", out_gff_bin.string().c_str());
	return 0;
}

