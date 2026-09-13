#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "hd_texture_pack.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
std::string key_name(uint32_t texture_hash, uint32_t palette_hash) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << texture_hash << '-'
        << palette_hash << ".png";
    return out.str();
}

std::string padded_key_name(uint32_t texture_hash, uint32_t palette_hash) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0')
        << texture_hash << '-' << palette_hash << ".png";
    return out.str();
}

void touch(const fs::path& path) {
    std::ofstream output(path, std::ios::binary);
    output.put('\0');
}

uint32_t png_crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void append_be32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void append_png_chunk(std::vector<uint8_t>& png, const char type[4],
                      const std::vector<uint8_t>& payload) {
    append_be32(png, static_cast<uint32_t>(payload.size()));
    const size_t crc_start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    append_be32(png, png_crc(png.data() + crc_start, png.size() - crc_start));
}

/* Generate an original, tiny RGBA PNG in the test itself. The single stored
 * DEFLATE block keeps the fixture independent of an encoder library. */
void write_test_png(const fs::path& path, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1u + width * 4u));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); /* PNG filter: None */
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(static_cast<uint8_t>(x * 3u));
            raw.push_back(static_cast<uint8_t>(y * 17u));
            raw.push_back(0xA5u);
            /* Exercise packs that use 7-bit alpha (127 = opaque),
             * not conventional 8-bit PNG opacity.  Keep the fixture aligned
             * with the shipping assets so the renderer's alpha contract stays
             * covered by the pack test. */
            raw.push_back(0x7Fu);
        }
    }
    check(raw.size() <= 65535u, "generated PNG fits one stored DEFLATE block");

    std::vector<uint8_t> zlib{0x78u, 0x01u, 0x01u};
    const uint16_t length = static_cast<uint16_t>(raw.size());
    const uint16_t inverse = static_cast<uint16_t>(~length);
    zlib.push_back(static_cast<uint8_t>(length));
    zlib.push_back(static_cast<uint8_t>(length >> 8));
    zlib.push_back(static_cast<uint8_t>(inverse));
    zlib.push_back(static_cast<uint8_t>(inverse >> 8));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t value : raw) { s1 = (s1 + value) % 65521u; s2 = (s2 + s1) % 65521u; }
    append_be32(zlib, (s2 << 16) | s1);

    std::vector<uint8_t> png{0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<uint8_t> ihdr;
    append_be32(ihdr, width); append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8u, 6u, 0u, 0u, 0u});
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib);
    append_png_chunk(png, "IEND", {});
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
}

/* Shared "one stored DEFLATE block" tail for the two PNG writers below --
 * same trick as write_test_png, just factored out since both a 4-channel
 * and a 1-channel writer need it. */
void write_raw_as_png(const fs::path& path, uint32_t width, uint32_t height,
                      uint8_t color_type, uint8_t channels,
                      const std::vector<uint8_t>& raw) {
    check(raw.size() <= 65535u, "generated PNG fits one stored DEFLATE block");
    std::vector<uint8_t> zlib{0x78u, 0x01u, 0x01u};
    const uint16_t length = static_cast<uint16_t>(raw.size());
    const uint16_t inverse = static_cast<uint16_t>(~length);
    zlib.push_back(static_cast<uint8_t>(length));
    zlib.push_back(static_cast<uint8_t>(length >> 8));
    zlib.push_back(static_cast<uint8_t>(inverse));
    zlib.push_back(static_cast<uint8_t>(inverse >> 8));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t value : raw) { s1 = (s1 + value) % 65521u; s2 = (s2 + s1) % 65521u; }
    append_be32(zlib, (s2 << 16) | s1);

    std::vector<uint8_t> png{0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<uint8_t> ihdr;
    append_be32(ihdr, width); append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8u, color_type, 0u, 0u, 0u});
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib);
    append_png_chunk(png, "IEND", {});
    (void)channels;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
}

void write_rgba_png(const fs::path& path, uint32_t width, uint32_t height,
                    const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1u + width * 4u));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t{y} * width * 4,
                   rgba.begin() + size_t{y + 1} * width * 4);
    }
    write_raw_as_png(path, width, height, 6u, 4u, raw);
}

void write_gray_png(const fs::path& path, uint32_t width, uint32_t height,
                    const std::vector<uint8_t>& gray) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1u + width));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), gray.begin() + size_t{y} * width,
                   gray.begin() + size_t{y + 1} * width);
    }
    write_raw_as_png(path, width, height, 0u, 1u, raw);
}

/* Same PS1 555-color convention hd_texture_pack.cpp's own
 * hd_texture_rgba5551_to_rgba8 uses (not directly callable here -- internal
 * linkage, and this is source code, not something to compile twice into the
 * test): all-zero word is the one transparent color, everything else opaque. */
void rgba5551_to_rgba8(uint16_t c, uint8_t out4[4]) {
    const unsigned r5 = c & 0x1Fu, g5 = (c >> 5) & 0x1Fu, b5 = (c >> 10) & 0x1Fu;
    out4[0] = static_cast<uint8_t>((r5 * 255u + 15u) / 31u);
    out4[1] = static_cast<uint8_t>((g5 * 255u + 15u) / 31u);
    out4[2] = static_cast<uint8_t>((b5 * 255u + 15u) / 31u);
    out4[3] = (c == 0) ? 0 : 255;
}

struct TempTree {
    fs::path path;

    TempTree() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("psxrecomp_hd_pack_" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Fixture {
    TempTree temp;
    fs::path root;
    fs::path replacements;
    std::vector<uint16_t> vram;
    std::vector<uint16_t> upload_words;
    std::vector<uint16_t> wrap_words;
    uint32_t upload_hash = 0;
    uint32_t wrap_hash = 0;
    uint32_t palette4_hash = 0;
    uint32_t palette8_hash = 0;

    Fixture()
        : root(temp.path / "pack"),
          replacements(root / "Example-texture-replacements"),
          vram(1024u * 512u, 0),
          upload_words(8u * 4u),
          wrap_words(4u * 4u) {
        fs::create_directories(replacements);
        for (unsigned i = 0; i < upload_words.size(); ++i)
            upload_words[i] = static_cast<uint16_t>(0x1200u + i * 17u);
        for (unsigned i = 0; i < wrap_words.size(); ++i)
            wrap_words[i] = static_cast<uint16_t>(0xA000u + i * 13u);
        upload_hash = hd_texture_crc32_words_le(upload_words.data(), upload_words.size());
        wrap_hash = hd_texture_crc32_words_le(wrap_words.data(), wrap_words.size());

        /* Both palettes wrap at the right edge, proving full-CLUT gathering. */
        for (unsigned i = 0; i < 16; ++i)
            vram[7u * 1024u + ((1020u + i) & 1023u)] =
                static_cast<uint16_t>(0x0100u + i * 3u);
        for (unsigned i = 0; i < 256; ++i)
            vram[8u * 1024u + ((900u + i) & 1023u)] =
                static_cast<uint16_t>(0x2000u + i * 5u);
        palette4_hash = hd_texture_hash_clut(
            vram.data(), vram.size(), 1020, 7, HD_TEXTURE_DEPTH_4BPP);
        palette8_hash = hd_texture_hash_clut(
            vram.data(), vram.size(), 900, 8, HD_TEXTURE_DEPTH_8BPP);

        /* Native upload is 8 words * 4 pixels/word by 4 rows. The 64x8 PNG is
         * a valid uniform 2x replacement for the live GL aspect check. */
        write_test_png(replacements / key_name(upload_hash, palette4_hash), 64, 8);
        touch(replacements / key_name(upload_hash, palette8_hash));
        touch(replacements / key_name(wrap_hash, 0));
        touch(replacements / "not-a-pack-key.png");
        touch(replacements / "123456789-1.png"); /* component is > 8 hex digits */

        std::ofstream hashes(root / "Hashes.ini");
        hashes << "[Textures]\n"
               << std::hex << upload_hash << '-' << palette4_hash
               << " = Test/Logical/Four bit.png\n"
               << "# comments and sections are ignored\n";
    }
};

HdTexturePack* open_pack(const fs::path& root) {
    HdTexturePack* pack = nullptr;
    char error[512]{};
    check(hd_texture_pack_create(root.string().c_str(), &pack,
                                 error, sizeof(error)) == 1,
          error[0] ? error : "pack opens");
    return pack;
}

HdTextureDrawQuery query4(const std::vector<uint16_t>& vram,
                          uint8_t u_first,
                          uint8_t u_last) {
    HdTextureDrawQuery query{};
    query.page_x = 10;
    query.page_y = 20;
    query.depth = HD_TEXTURE_DEPTH_4BPP;
    query.u_first = u_first;
    query.u_last = u_last;
    query.v_first = 0;
    query.v_last = 3;
    query.clut_x = 1020;
    query.clut_y = 7;
    query.vram = vram.data();
    query.vram_word_count = vram.size();
    return query;
}

void test_crc_exact_little_endian() {
    const uint16_t ascii_words[] = {0x3231, 0x3433, 0x3635, 0x3837};
    check(hd_texture_crc32_words_le(ascii_words, 4) == 0x9AE0DAAFu,
          "CRC32 is IEEE/zlib over explicit little-endian word bytes");
    check(hd_texture_crc32_words_le(nullptr, 0) == 0,
          "empty CRC32 is the IEEE empty-buffer value");
}

void test_scan_mapping(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;

    HdTexturePackInfo info{};
    hd_texture_pack_get_info(pack, &info);
    check(info.unique_key_count == 3,
          "scanner accepts only valid numeric PNG keys");
    check(info.ambiguous_key_count == 0, "fixture has no ambiguous aliases");
    check(info.logical_mapping_count == 1, "Hashes.ini logical map is parsed");
    check(std::string(info.replacement_root).find("-texture-replacements") !=
              std::string::npos,
          "pack root resolves its single replacement child");

    HdTexturePackEntry entry{};
    check(hd_texture_pack_lookup(pack, fixture.upload_hash,
                                 fixture.palette4_hash, &entry) ==
              HD_TEXTURE_LOOKUP_FOUND,
          "numeric texture/palette key lookup succeeds");
    check(std::string(entry.logical_path) == "Test/Logical/Four bit.png",
          "lookup exposes the logical path from Hashes.ini");
    hd_texture_pack_destroy(pack);

    /* The replacement directory is also a valid direct root; sibling
     * Hashes.ini remains discoverable. */
    pack = open_pack(fixture.replacements);
    if (pack) {
        check(hd_texture_pack_lookup(pack, fixture.upload_hash,
                                     fixture.palette4_hash, &entry) ==
                  HD_TEXTURE_LOOKUP_FOUND && entry.logical_path[0] != '\0',
              "direct replacement root finds sibling Hashes.ini");
        hd_texture_pack_destroy(pack);
    }

#ifdef _WIN32
    _putenv_s("PSXRECOMP_HD_TEXTURE_ROOT", fixture.root.string().c_str());
#else
    setenv("PSXRECOMP_HD_TEXTURE_ROOT", fixture.root.string().c_str(), 1);
#endif
    pack = nullptr;
    char error[128]{};
    check(hd_texture_pack_create(nullptr, &pack, error, sizeof(error)) == 1,
          "environment root is used when explicit path is absent");
    hd_texture_pack_destroy(pack);
#ifdef _WIN32
    _putenv_s("PSXRECOMP_HD_TEXTURE_ROOT", "");
#else
    unsetenv("PSXRECOMP_HD_TEXTURE_ROOT");
#endif
}

void test_palette_match_and_invalidation(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    uint32_t hash = 0;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), &hash) == 1 &&
              hash == fixture.upload_hash,
          "upload tracking returns exact content hash");

    HdTextureDrawQuery four = query4(fixture.vram, 0, 31);
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "4bpp page/depth/UV query matches a containing upload");
    check(match.entry.palette_hash == fixture.palette4_hash &&
              match.source_word_x == 0 && match.source_y == 0,
          "match reports palette key and upload-relative origin");

    /* The live GP0 adapter derives page base/depth from texpage. Page X/Y are
     * fixed hardware bases, so UV 40/20 reaches the upload at VRAM 10/20 in
     * 4bpp (four pixels per word). */
    check(hd_texture_pack_match_draw(
              pack, 0x0000u, 1020, 7, 40, 71, 20, 23,
              fixture.vram.data(), fixture.vram.size(), &match) ==
              HD_TEXTURE_LOOKUP_FOUND && match.source_word_x == 0 &&
              match.source_y == 0,
          "live texpage/CLUT/UV adapter resolves a 4bpp replacement hit");
    check(hd_texture_pack_match_draw(
              pack, 0x0180u, 1020, 7, 0, 0, 0, 0,
              fixture.vram.data(), fixture.vram.size(), &match) ==
              HD_TEXTURE_LOOKUP_ERROR,
          "reserved texpage depth fails closed");

    /* Entry 20 is outside a 16-color CLUT and must not alter a 4bpp match. */
    const size_t outside4 = 7u * 1024u + ((1020u + 20u) & 1023u);
    fixture.vram[outside4] ^= 0x55AAu;
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "4bpp hashes exactly 16 CLUT entries, not adjacent VRAM");
    fixture.vram[outside4] ^= 0x55AAu;
    const size_t inside4 = 7u * 1024u + ((1020u + 15u) & 1023u);
    fixture.vram[inside4] ^= 1u;
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_NONE,
          "changing the sixteenth 4bpp CLUT entry rejects the palette");
    fixture.vram[inside4] ^= 1u;

    HdTextureDrawQuery eight{};
    eight.page_x = 10;
    eight.page_y = 20;
    eight.depth = HD_TEXTURE_DEPTH_8BPP;
    eight.u_first = 0;
    eight.u_last = 15;
    eight.v_first = 0;
    eight.v_last = 3;
    eight.clut_x = 900;
    eight.clut_y = 8;
    eight.vram = fixture.vram.data();
    eight.vram_word_count = fixture.vram.size();
    check(hd_texture_pack_match(pack, &eight, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "8bpp query matches using the complete 256-entry CLUT");
    const size_t entry200 = 8u * 1024u + ((900u + 200u) & 1023u);
    fixture.vram[entry200] ^= 1u;
    check(hd_texture_pack_match(pack, &eight, &match) == HD_TEXTURE_LOOKUP_NONE,
          "changing CLUT entry 200 rejects an 8bpp palette");
    fixture.vram[entry200] ^= 1u;

    /* Intersect invalidation preserves disjoint surviving fragments. */
    hd_texture_pack_invalidate(pack, 14, 20, 4, 4);
    HdTextureDrawQuery left = query4(fixture.vram, 0, 15);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "non-intersected split of an upload remains matchable");
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_NONE,
          "query crossing an invalidated upload fragment falls back");
    hd_texture_pack_invalidate(pack, 11, 21, 1, 1);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "single-word intersect invalidation punches a real residency hole");

    hd_texture_pack_destroy(pack);
}

/* A recolorable master should let hd_texture_pack_match succeed for a
 * (texture_hash, palette_hash) pair that has NO exact-match PNG at all, by
 * synthesizing one from the master's reference upscale + the live CLUT this
 * query actually carries. Builds an hd.png where every native index cell is
 * a FLAT block of that index's reference color (no AI-hallucinated detail),
 * so the expected recolored output is exactly the live palette's color --
 * checkable byte-for-byte, not just "some plausible image came out". */
void test_recolorable_master(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    uint32_t hash = 0;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), &hash) == 1 &&
              hash == fixture.upload_hash,
          "recolor test: upload tracking returns exact content hash");

    constexpr uint32_t kNativeW = 32, kNativeH = 4, kScale = 2;
    std::vector<uint8_t> index(kNativeW * kNativeH);
    for (uint32_t y = 0; y < kNativeH; ++y)
        for (uint32_t word_col = 0; word_col < 8; ++word_col) {
            const uint16_t word = fixture.upload_words[y * 8 + word_col];
            for (uint32_t nib = 0; nib < 4; ++nib)
                index[y * kNativeW + word_col * 4 + nib] =
                    static_cast<uint8_t>((word >> (nib * 4)) & 0xFu);
        }

    /* Reference palette: the SAME 16 words fixture.palette4_hash already
     * hashes (vram row 7, wrapping at x=1020) -- a real, already-matchable
     * palette, just reused here as the master's reference instead of a live
     * query palette. */
    uint16_t ref_words[16];
    for (unsigned i = 0; i < 16; ++i)
        ref_words[i] = fixture.vram[7u * 1024u + ((1020u + i) & 1023u)];

    /* A brand-new live palette this pack has never seen a file for. */
    uint16_t live_words[16];
    for (unsigned i = 0; i < 16; ++i) {
        live_words[i] = static_cast<uint16_t>(0x0400u + i * 3u);
        fixture.vram[9u * 1024u + 500u + i] = live_words[i];
    }

    std::vector<uint8_t> hd_rgba(size_t{kNativeW} * kScale * kNativeH * kScale * 4, 0);
    const uint32_t hd_w = kNativeW * kScale;
    for (uint32_t ny = 0; ny < kNativeH; ++ny)
        for (uint32_t nx = 0; nx < kNativeW; ++nx) {
            uint8_t color[4];
            rgba5551_to_rgba8(ref_words[index[ny * kNativeW + nx]], color);
            for (uint32_t by = 0; by < kScale; ++by)
                for (uint32_t bx = 0; bx < kScale; ++bx) {
                    uint8_t* px = &hd_rgba[(size_t{(ny * kScale + by)} * hd_w +
                                            (nx * kScale + bx)) * 4];
                    std::memcpy(px, color, 4);
                }
        }

    char hash_hex[9];
    std::snprintf(hash_hex, sizeof(hash_hex), "%08x", fixture.upload_hash);
    const fs::path master_dir = fixture.replacements / "masters" / hash_hex;
    fs::create_directories(master_dir);
    write_rgba_png(master_dir / "hd.png", hd_w, kNativeH * kScale, hd_rgba);
    write_gray_png(master_dir / "index.png", kNativeW, kNativeH, index);
    {
        std::ofstream pal(master_dir / "palette.bin", std::ios::binary);
        uint32_t count = 16;
        pal.write(reinterpret_cast<const char*>(&count), 4);
        pal.write(reinterpret_cast<const char*>(ref_words), 32);
    }
    hd_texture_pack_destroy(pack);

    /* Reopen so the newly-written masters/ bundle gets picked up -- it's
     * only scanned during hd_texture_pack_create. */
    pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), &hash) == 1,
          "recolor test: re-tracking after reopen");

    HdTextureDrawQuery query{};
    query.page_x = 10; query.page_y = 20;
    query.depth = HD_TEXTURE_DEPTH_4BPP;
    query.u_first = 0; query.u_last = 31;
    query.v_first = 0; query.v_last = 3;
    query.clut_x = 500; query.clut_y = 9;
    query.vram = fixture.vram.data();
    query.vram_word_count = fixture.vram.size();

    /* No exact (texture_hash, palette_hash) file exists for this brand-new
     * live palette -- the match itself still misses, but must report WHICH
     * texture_hash it would have needed, so the caller can look for a live-
     * recolorable master instead of giving up. */
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_NONE,
          "no exact file for a never-seen palette -- still a miss");
    check(match.has_miss_texture_hash && match.miss_texture_hash == fixture.upload_hash,
          "miss still reports which texture_hash a recolorable master could serve");

    HdRecolorMasterInfo info{};
    check(hd_texture_pack_get_recolor_master(pack, match.miss_texture_hash, &info) == 1,
          "a recolorable master is found for the reported texture_hash");
    check(info.hd_width == hd_w && info.hd_height == kNativeH * kScale &&
              info.native_width == kNativeW && info.native_height == kNativeH &&
              info.palette_count == 16 && info.hd_rgba != nullptr && info.index != nullptr,
          "master info reports the expected dimensions and non-null buffers");

    /* THE core guarantee this whole feature exists for: given the master
     * and the live palette's hash (via its CLUT), reconstruct EXACTLY the
     * texture that palette specifies -- not an approximation. */
    std::vector<float> scale(size_t{info.palette_count} * 4), offset(size_t{info.palette_count} * 3);
    const uint32_t table_count = hd_texture_pack_compute_recolor_table(
        pack, match.miss_texture_hash, fixture.vram.data(), fixture.vram.size(),
        500, 9, HD_TEXTURE_DEPTH_4BPP, scale.data(), offset.data());
    check(table_count == 16, "recolor table is sized to the master's own palette_count");

    /* Apply the table exactly as the GPU shader will: for every HD pixel,
     * look up its native-resolution index and do out = hd*scale+offset. */
    auto apply_and_check = [&](uint32_t hx, uint32_t hy) {
        const uint32_t nx = hx / kScale, ny = hy / kScale;
        const uint8_t idx = index[ny * kNativeW + nx];
        const uint8_t* hd_px = &info.hd_rgba[(size_t{hy} * info.hd_width + hx) * 4];
        uint8_t got[4];
        for (int ch = 0; ch < 3; ++ch) {
            const float v = hd_px[ch] * scale[size_t{idx} * 4 + ch] + offset[size_t{idx} * 3 + ch] * 255.0f;
            got[ch] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        }
        /* Alpha depends only on scale.a (the LIVE palette's own opacity at
         * this index), never on hd_px[3] -- see compute_recolor_table's
         * comment for why multiplying by the master's own alpha would be
         * wrong. */
        got[3] = static_cast<uint8_t>(scale[size_t{idx} * 4 + 3] * 255.0f);
        uint8_t expected[4];
        rgba5551_to_rgba8(live_words[idx], expected);
        return std::memcmp(got, expected, 4) == 0;
    };
    check(apply_and_check(0, 0),
          "table applied to the master's top-left pixel reproduces the exact live-palette color");
    check(apply_and_check(20 * kScale, 2 * kScale),
          "table applied to an interior pixel also reproduces the exact live-palette color");

    hd_texture_pack_destroy(pack);
}

/* 2026-09-11 regression test: a master whose ref_palette_count (how many
 * REAL reference colors export_hd_pack.py recorded) is SMALLER than the
 * full legal index range for its bit depth -- exactly what a character/
 * monster SHP or WEP texture produces in practice, since its raw index
 * bytes are literal 0-255 values independent of how many colors that one
 * costume's own palette happens to define (confirmed live: texture_hash
 * 83059c83's real master has ref_palette_count 160, well short of 256, and
 * its index map legitimately contains values above that). Before this fix,
 * compute_recolor_table only ever wrote table entries up to ref_palette_
 * count, so gpu_gl_renderer.c's GPU-side lookup texture was never given
 * defined data for indices beyond it -- sampling one there read whatever a
 * PREVIOUS, unrelated draw's table happened to leave in that GL texture
 * column (that texture is shared and reused every recolor draw), reported
 * live as parts of a character rendering the wrong color and other parts
 * (a shield) going fully transparent. This proves: (a) the table is always
 * sized to the FULL 16-or-256 range regardless of ref_palette_count, (b) an
 * index within ref_palette_count still gets exact reference/live color
 * correction, (c) an index beyond it shows the master's own baked pixel
 * unmodified (identity, not garbage) while alpha still comes from the REAL
 * live palette, and (d) a live-palette entry of exactly zero still hides
 * that index even with no reference data for it. */
void test_recolorable_master_partial_palette(Fixture& fixture) {
    constexpr uint32_t kNativeW = 16, kNativeH = 4, kScale = 2;
    constexpr uint32_t kRefCount = 10;  /* deliberately < 16 */

    std::vector<uint16_t> words(4u * kNativeH);
    for (uint32_t row = 0; row < kNativeH; ++row)
        for (uint32_t word_col = 0; word_col < 4; ++word_col) {
            uint16_t word = 0;
            for (uint32_t nib = 0; nib < 4; ++nib) {
                const uint32_t texel = row * kNativeW + word_col * 4 + nib;
                const uint32_t idx = texel % 16u;  /* cycles 0..15, every value appears */
                word |= static_cast<uint16_t>(idx << (nib * 4));
            }
            words[row * 4 + word_col] = word;
        }
    uint32_t upload_hash = 0;
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 50, 100, 4, kNativeH,
                                       words.data(), words.size(), &upload_hash) == 1,
          "partial-palette recolor test: distinct upload tracks");

    std::vector<uint8_t> index(kNativeW * kNativeH);
    for (uint32_t row = 0; row < kNativeH; ++row)
        for (uint32_t word_col = 0; word_col < 4; ++word_col) {
            const uint16_t word = words[row * 4 + word_col];
            for (uint32_t nib = 0; nib < 4; ++nib)
                index[row * kNativeW + word_col * 4 + nib] =
                    static_cast<uint8_t>((word >> (nib * 4)) & 0xFu);
        }

    /* Only indices 0..kRefCount-1 get a REAL reference color. */
    uint16_t ref_words[kRefCount];
    for (unsigned i = 0; i < kRefCount; ++i)
        ref_words[i] = static_cast<uint16_t>(0x0200u + i * 5u);

    /* A brand-new live palette, all 16 slots -- index 12 deliberately zero
     * (transparent) to prove that's still honored with no reference data. */
    uint16_t live_words[16];
    for (unsigned i = 0; i < 16; ++i)
        live_words[i] = (i == 12) ? 0 : static_cast<uint16_t>(0x0500u + i * 7u);
    for (unsigned i = 0; i < 16; ++i)
        fixture.vram[11u * 1024u + 700u + i] = live_words[i];

    /* hd.png: indices < kRefCount render from ref_words (same convention as
     * test_recolorable_master); indices >= kRefCount get an arbitrary, fixed
     * "baked art" color that has no relationship to any palette -- there is
     * no reference for them, so the ONLY correct output is this exact color
     * passed through unmodified. */
    std::vector<uint8_t> hd_rgba(size_t{kNativeW} * kScale * kNativeH * kScale * 4, 0);
    const uint32_t hd_w = kNativeW * kScale;
    for (uint32_t ny = 0; ny < kNativeH; ++ny)
        for (uint32_t nx = 0; nx < kNativeW; ++nx) {
            const uint8_t idx = index[ny * kNativeW + nx];
            uint8_t color[4];
            if (idx < kRefCount) {
                rgba5551_to_rgba8(ref_words[idx], color);
            } else {
                color[0] = static_cast<uint8_t>(10 + idx * 7);
                color[1] = static_cast<uint8_t>(20 + idx * 11);
                color[2] = static_cast<uint8_t>(30 + idx * 13);
                color[3] = 255;
            }
            for (uint32_t by = 0; by < kScale; ++by)
                for (uint32_t bx = 0; bx < kScale; ++bx) {
                    uint8_t* px = &hd_rgba[(size_t{(ny * kScale + by)} * hd_w +
                                            (nx * kScale + bx)) * 4];
                    std::memcpy(px, color, 4);
                }
        }

    char hash_hex[9];
    std::snprintf(hash_hex, sizeof(hash_hex), "%08x", upload_hash);
    const fs::path master_dir = fixture.replacements / "masters" / hash_hex;
    fs::create_directories(master_dir);
    write_rgba_png(master_dir / "hd.png", hd_w, kNativeH * kScale, hd_rgba);
    write_gray_png(master_dir / "index.png", kNativeW, kNativeH, index);
    {
        std::ofstream pal(master_dir / "palette.bin", std::ios::binary);
        uint32_t count = kRefCount;
        pal.write(reinterpret_cast<const char*>(&count), 4);
        pal.write(reinterpret_cast<const char*>(ref_words), kRefCount * 2);
    }
    hd_texture_pack_destroy(pack);

    pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 50, 100, 4, kNativeH,
                                       words.data(), words.size(), &upload_hash) == 1,
          "partial-palette recolor test: re-tracking after reopen");

    HdTextureDrawQuery query{};
    query.page_x = 50; query.page_y = 100;
    query.depth = HD_TEXTURE_DEPTH_4BPP;
    query.u_first = 0; query.u_last = static_cast<uint8_t>(kNativeW - 1);
    query.v_first = 0; query.v_last = static_cast<uint8_t>(kNativeH - 1);
    query.clut_x = 700; query.clut_y = 11;
    query.vram = fixture.vram.data();
    query.vram_word_count = fixture.vram.size();

    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_NONE &&
              match.has_miss_texture_hash && match.miss_texture_hash == upload_hash,
          "partial-palette master: miss still reports the covering texture_hash");

    HdRecolorMasterInfo info{};
    check(hd_texture_pack_get_recolor_master(pack, upload_hash, &info) == 1 &&
              info.palette_count == 16,
          "partial-palette master: palette_count is the FULL 16, not ref_palette_count (10)");

    std::vector<float> scale(16u * 4), offset(16u * 3);
    const uint32_t table_count = hd_texture_pack_compute_recolor_table(
        pack, upload_hash, fixture.vram.data(), fixture.vram.size(),
        700, 11, HD_TEXTURE_DEPTH_4BPP, scale.data(), offset.data());
    check(table_count == 16, "partial-palette master: table is sized to the FULL 16 entries");

    auto sample_out = [&](uint8_t idx) {
        /* Any HD pixel whose index is idx works; use the first one. */
        for (uint32_t ny = 0; ny < kNativeH; ++ny)
            for (uint32_t nx = 0; nx < kNativeW; ++nx)
                if (index[ny * kNativeW + nx] == idx) {
                    const uint8_t* hd_px = &info.hd_rgba[
                        (size_t{ny * kScale} * info.hd_width + nx * kScale) * 4];
                    uint8_t got[4];
                    for (int ch = 0; ch < 3; ++ch) {
                        const float v = hd_px[ch] * scale[size_t{idx} * 4 + ch] +
                                        offset[size_t{idx} * 3 + ch] * 255.0f;
                        got[ch] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
                    }
                    got[3] = static_cast<uint8_t>(scale[size_t{idx} * 4 + 3] * 255.0f);
                    return std::array<uint8_t, 4>{got[0], got[1], got[2], got[3]};
                }
        return std::array<uint8_t, 4>{0, 0, 0, 0};
    };

    /* Indices within ref_palette_count: exact reference/live correction,
     * same guarantee as test_recolorable_master. */
    for (uint8_t idx = 0; idx < kRefCount; ++idx) {
        uint8_t expected[4];
        rgba5551_to_rgba8(live_words[idx], expected);
        const auto got = sample_out(idx);
        check(std::memcmp(got.data(), expected, 4) == 0,
              "partial-palette master: an index WITH a reference color reconstructs exactly");
    }

    /* Indices beyond ref_palette_count: identity passthrough of hd.png's own
     * baked pixel, gated only by the live palette's real opacity. Index 12's
     * live word is zero (transparent) -- must be hidden even with no
     * reference data; every other one here must be fully opaque. */
    for (uint8_t idx = static_cast<uint8_t>(kRefCount); idx < 16; ++idx) {
        const auto got = sample_out(idx);
        uint8_t expected_rgb[3] = {
            static_cast<uint8_t>(10 + idx * 7),
            static_cast<uint8_t>(20 + idx * 11),
            static_cast<uint8_t>(30 + idx * 13),
        };
        check(got[0] == expected_rgb[0] && got[1] == expected_rgb[1] && got[2] == expected_rgb[2],
              "partial-palette master: an index WITHOUT a reference color shows hd.png's own pixel unmodified");
        const uint8_t expected_alpha = (idx == 12) ? 0 : 255;
        check(got[3] == expected_alpha,
              "partial-palette master: alpha for a no-reference index still comes from the REAL live palette");
    }

    hd_texture_pack_destroy(pack);
}


void test_tracking_savestate_continuity(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), nullptr) == 1,
          "savestate fixture upload tracks");
    hd_texture_pack_invalidate(pack, 14, 20, 4, 4);
    const HdTextureDrawQuery left = query4(fixture.vram, 0, 15);
    const HdTextureDrawQuery full = query4(fixture.vram, 0, 31);
    HdTextureMatch before{};
    check(hd_texture_pack_match(pack, &left, &before) ==
              HD_TEXTURE_LOOKUP_FOUND,
          "savestate fixture retains a split upload fragment");

    uint8_t* state = nullptr;
    size_t state_size = 0;
    check(hd_texture_pack_tracking_state_save(
              pack, &state, &state_size) == 1 && state && state_size > 24,
          "upload residency serializes to a non-empty portable wire");
    check(hd_texture_pack_tracking_state_check(state, state_size) == 1,
          "serialized upload residency passes side-effect-free preflight");

    hd_texture_pack_reset_tracking(pack);
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "full-VRAM restore reset removes host upload identity");
    check(hd_texture_pack_tracking_state_load(pack, state, state_size) == 1,
          "saved upload residency restores after the VRAM reset");
    check(hd_texture_pack_tracking_upload_count(pack) == 1,
          "restore recovers the exact live upload count");
    check(hd_texture_pack_match(pack, &left, &match) ==
              HD_TEXTURE_LOOKUP_FOUND &&
              match.upload_serial == before.upload_serial,
          "restored tracker preserves replacement match and upload identity");
    check(hd_texture_pack_match(pack, &full, &match) == HD_TEXTURE_LOOKUP_NONE,
          "restored tracker preserves the invalidated residency hole");
    std::vector<uint8_t> corrupt(state, state + state_size);
    corrupt[20] = 1; /* reserved header word */
    check(hd_texture_pack_tracking_state_check(corrupt.data(), corrupt.size()) == 0,
          "corrupt tracker header fails closed");
    check(hd_texture_pack_tracking_state_check(state, state_size - 1) == 0,
          "truncated tracker wire fails closed");
    check(hd_texture_pack_tracking_state_load(
              pack, corrupt.data(), corrupt.size()) == 0 &&
              hd_texture_pack_match(pack, &left, &match) ==
                  HD_TEXTURE_LOOKUP_FOUND,
          "failed tracker load leaves the current residency untouched");
    hd_texture_pack_invalidate(pack, 10, 20, 1, 1);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "restored tracker rebuilds invalidation bounds before GPU draws");
    std::free(state);
    hd_texture_pack_destroy(pack);

    state = nullptr;
    state_size = 0;
    check(hd_texture_pack_tracking_state_save(
              nullptr, &state, &state_size) == 1 && state_size == 24 &&
              hd_texture_pack_tracking_state_check(state, state_size) == 1 &&
              hd_texture_pack_tracking_state_load(nullptr, state, state_size) == 1,
          "renderer-independent states carry a valid empty HD tracker");
    std::free(state);
}

void test_wrapping(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    uint32_t hash = 0;
    check(hd_texture_pack_track_upload(pack, 1022, 510, 4, 4,
                                       fixture.wrap_words.data(),
                                       fixture.wrap_words.size(), &hash) == 1,
          "upload crossing both VRAM edges is tracked");
    HdTextureDrawQuery query{};
    query.page_x = 1022;
    query.page_y = 510;
    query.depth = HD_TEXTURE_DEPTH_16BPP;
    query.u_first = 0;
    query.u_last = 3;
    query.v_first = 0;
    query.v_last = 3;
    query.vram = fixture.vram.data();
    query.vram_word_count = fixture.vram.size();
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "wrapped page/UV footprint is covered by one logical upload");
    check(match.entry.texture_hash == fixture.wrap_hash,
          "wrapped match preserves original logical upload hash");
    hd_texture_pack_invalidate(pack, 0, 0, 1, 1);
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_NONE,
          "physical-edge invalidation intersects a wrapped upload");
    hd_texture_pack_destroy(pack);
}

void test_ambiguity_fallback() {
    TempTree temp;
    const fs::path direct = temp.path / "ambiguous-texture-replacements";
    fs::create_directories(direct);
    uint16_t word = 0;
    uint32_t hash = 0;
    for (unsigned value = 0; value <= 0xFFFFu; ++value) {
        word = static_cast<uint16_t>(value);
        hash = hd_texture_crc32_words_le(&word, 1);
        if (hash != 0 && hash < 0x10000000u) break;
    }
    check(hash < 0x10000000u, "found a short numeric CRC for alias test");
    touch(direct / key_name(hash, 0));
    touch(direct / padded_key_name(hash, 0));

    HdTexturePack* pack = open_pack(direct);
    if (!pack) return;
    HdTexturePackEntry entry{};
    check(hd_texture_pack_lookup(pack, hash, 0, &entry) ==
              HD_TEXTURE_LOOKUP_AMBIGUOUS,
          "numeric filename aliases are rejected as ambiguous");

    std::vector<uint16_t> vram(1024u * 512u, 0);
    check(hd_texture_pack_track_upload(pack, 30, 40, 1, 1,
                                       &word, 1, nullptr) == 1,
          "ambiguous fixture upload tracks");
    HdTextureDrawQuery query{};
    query.page_x = 30;
    query.page_y = 40;
    query.depth = HD_TEXTURE_DEPTH_16BPP;
    query.vram = vram.data();
    query.vram_word_count = vram.size();
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) ==
              HD_TEXTURE_LOOKUP_AMBIGUOUS,
          "draw ambiguity fails closed instead of choosing enumeration order");
    hd_texture_pack_destroy(pack);
}

void test_bounded_async_decode(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), nullptr) == 1,
          "decode fixture upload tracks");
    int status = hd_texture_pack_request_decode(
        pack, fixture.upload_hash, fixture.palette4_hash);
    check(status == HD_TEXTURE_LOOKUP_NONE,
          "first decode request queues work instead of decoding synchronously");

    HdTexturePixels pixels{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        status = hd_texture_pack_acquire_decoded(
            pack, fixture.upload_hash, fixture.palette4_hash, &pixels);
        if (status == HD_TEXTURE_LOOKUP_FOUND) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    check(status == HD_TEXTURE_LOOKUP_FOUND,
          "bounded worker publishes a decoded replacement");
    check(pixels.rgba && pixels.width == 64 && pixels.height == 8 &&
              pixels.stride == 64u * 4u,
          "decoded lease exposes expected RGBA dimensions");
    hd_texture_pack_destroy(pack);
    check(pixels.rgba && pixels.rgba[2] == 0xA5u && pixels.rgba[3] == 0x7Fu,
          "decoded lease preserves the pack's 7-bit opaque alpha");
    hd_texture_pixels_release(&pixels);
}

} // namespace

int main() {
    test_crc_exact_little_endian();
    Fixture fixture;
    test_scan_mapping(fixture);
    test_palette_match_and_invalidation(fixture);
    test_recolorable_master(fixture);
    test_recolorable_master_partial_palette(fixture);
    test_tracking_savestate_continuity(fixture);
    test_wrapping(fixture);
    test_ambiguity_fallback();
    test_bounded_async_decode(fixture);

    if (failures) {
        std::fprintf(stderr, "test_hd_texture_pack: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("PASS: external HD texture pack scan/hash/tracker semantics");
    return 0;
}
