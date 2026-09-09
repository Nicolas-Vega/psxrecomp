#include "hd_texture_pack.h"

#ifndef HD_TEXTURE_PACK_DISABLE_PNG_DECODE
#include "../third_party/stb_image.h" /* declarations only; implementation is shared */
#endif
#include "../third_party/stb_image_write.h" /* declarations only; impl in psx_window_icon.cpp */

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr unsigned kVramWidth = 1024;
constexpr unsigned kVramHeight = 512;
/* GPU primitives invalidate VRAM constantly.  Keep a coarse spatial index so
 * the common case does not walk every historical upload.  The index is only
 * a broad phase; fragment rectangles remain authoritative below. */
constexpr unsigned kUploadIndexTileSize = 32;
constexpr unsigned kUploadIndexCols =
    (kVramWidth + kUploadIndexTileSize - 1) / kUploadIndexTileSize;
constexpr unsigned kUploadIndexRows =
    (kVramHeight + kUploadIndexTileSize - 1) / kUploadIndexTileSize;
constexpr size_t kUploadIndexCells =
    size_t{kUploadIndexCols} * kUploadIndexRows;
constexpr size_t kVramWords = size_t{kVramWidth} * kVramHeight;
constexpr size_t kDefaultDecodeBudget = size_t{64} * 1024 * 1024;
constexpr uint32_t kTrackingStateMagic = 0x31544448u; /* "HDT1" LE */
constexpr uint32_t kTrackingStateVersion = 1;
constexpr size_t kTrackingStateHeaderBytes = 24;
constexpr size_t kTrackingStateUploadBytes = 20;
constexpr size_t kTrackingStateFragmentBytes = 12;
constexpr size_t kMaxTrackingStateBytes = size_t{8} * 1024 * 1024;
constexpr uint32_t kMaxTrackingStateUploads = 8192;
constexpr uint32_t kMaxTrackingStateFragments = 262144;
#ifndef HD_TEXTURE_PACK_DISABLE_PNG_DECODE
constexpr size_t kMaxDecodeQueue = 32;
constexpr size_t kMaxPngFileBytes = size_t{64} * 1024 * 1024;
#endif

uint64_t make_key(uint32_t texture_hash, uint32_t palette_hash) {
    return (uint64_t{texture_hash} << 32) | palette_hash;
}

void write_error(char* error, size_t capacity, const std::string& message) {
    if (!error || capacity == 0) return;
    std::snprintf(error, capacity, "%s", message.c_str());
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string ascii_lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool ends_with_ci(const std::string& value, const char* suffix) {
    const std::string lower = ascii_lower(value);
    const std::string wanted = ascii_lower(suffix);
    return lower.size() >= wanted.size() &&
           lower.compare(lower.size() - wanted.size(), wanted.size(), wanted) == 0;
}

bool parse_hex_component(const std::string& text, uint32_t* out) {
    if (!out || text.empty() || text.size() > 8) return false;
    uint32_t value = 0;
    for (const unsigned char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

bool parse_key_text(const std::string& text,
                    uint32_t* texture_hash,
                    uint32_t* palette_hash) {
    const size_t dash = text.find('-');
    if (dash == std::string::npos || text.find('-', dash + 1) != std::string::npos)
        return false;
    return parse_hex_component(text.substr(0, dash), texture_hash) &&
           parse_hex_component(text.substr(dash + 1), palette_hash);
}

bool parse_png_filename(const fs::path& path,
                        uint32_t* texture_hash,
                        uint32_t* palette_hash) {
    if (ascii_lower(path.extension().string()) != ".png") return false;
    return parse_key_text(path.stem().string(), texture_hash, palette_hash);
}

const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < result.size(); ++i) {
            uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xEDB88320u : 0u);
            result[i] = value;
        }
        return result;
    }();
    return table;
}

uint32_t crc_byte(uint32_t crc, uint8_t value) {
    return crc_table()[(crc ^ value) & 0xFFu] ^ (crc >> 8);
}

struct Rect {
    unsigned x = 0;
    unsigned y = 0;
    unsigned width = 0;
    unsigned height = 0;
};

struct Fragment {
    Rect rect;
    unsigned source_x = 0;
    unsigned source_y = 0;
};

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

bool contains_point(const Rect& rect, unsigned x, unsigned y) {
    return x >= rect.x && x < rect.x + rect.width &&
           y >= rect.y && y < rect.y + rect.height;
}

std::vector<Fragment> physical_fragments(unsigned x,
                                         unsigned y,
                                         unsigned width,
                                         unsigned height) {
    std::vector<Fragment> result;
    if (width == 0 || height == 0 || width > kVramWidth || height > kVramHeight)
        return result;

    const unsigned px = x & (kVramWidth - 1);
    const unsigned py = y & (kVramHeight - 1);
    const unsigned first_w = std::min(width, kVramWidth - px);
    const unsigned first_h = std::min(height, kVramHeight - py);
    const std::array<std::pair<unsigned, unsigned>, 2> xs{{
        {px, first_w}, {0, width - first_w}
    }};
    const std::array<std::pair<unsigned, unsigned>, 2> ys{{
        {py, first_h}, {0, height - first_h}
    }};

    unsigned source_y = 0;
    for (const auto& yspan : ys) {
        if (yspan.second == 0) continue;
        unsigned source_x = 0;
        for (const auto& xspan : xs) {
            if (xspan.second != 0) {
                result.push_back({
                    {xspan.first, yspan.first, xspan.second, yspan.second},
                    source_x, source_y
                });
            }
            source_x += xspan.second;
        }
        source_y += yspan.second;
    }
    return result;
}

std::vector<Rect> physical_rects(unsigned x,
                                 unsigned y,
                                 unsigned width,
                                 unsigned height) {
    std::vector<Rect> result;
    for (const Fragment& fragment : physical_fragments(x, y, width, height))
        result.push_back(fragment.rect);
    return result;
}

std::vector<Fragment> subtract_fragment(const Fragment& original,
                                        const Rect& cut) {
    if (!intersects(original.rect, cut)) return {original};

    const unsigned left = std::max(original.rect.x, cut.x);
    const unsigned right = std::min(original.rect.x + original.rect.width,
                                    cut.x + cut.width);
    const unsigned top = std::max(original.rect.y, cut.y);
    const unsigned bottom = std::min(original.rect.y + original.rect.height,
                                     cut.y + cut.height);
    std::vector<Fragment> result;
    const auto add = [&](unsigned x, unsigned y, unsigned width, unsigned height) {
        if (width == 0 || height == 0) return;
        result.push_back({
            {x, y, width, height},
            original.source_x + x - original.rect.x,
            original.source_y + y - original.rect.y
        });
    };
    add(original.rect.x, original.rect.y, original.rect.width,
        top - original.rect.y);
    add(original.rect.x, bottom, original.rect.width,
        original.rect.y + original.rect.height - bottom);
    add(original.rect.x, top, left - original.rect.x, bottom - top);
    add(right, top, original.rect.x + original.rect.width - right, bottom - top);
    return result;
}

std::vector<Rect> subtract_rect(const Rect& original, const Rect& cut) {
    const Fragment source{original, 0, 0};
    std::vector<Rect> result;
    for (const Fragment& piece : subtract_fragment(source, cut))
        result.push_back(piece.rect);
    return result;
}

struct Upload {
    uint64_t serial = 0;
    uint32_t hash = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    /* Conservative broad phase for GPU-draw invalidation.  Fragment coverage
     * remains authoritative; this union only avoids walking unrelated uploads. */
    Rect bounds{};
    std::vector<Fragment> fragments;
    /* Runtime-only broad-phase membership; never serialized. */
    std::vector<uint16_t> index_tiles;
};

Rect fragment_bounds(const std::vector<Fragment>& fragments) {
    Rect bounds{};
    for (const Fragment& fragment : fragments) {
        if (bounds.width == 0 || bounds.height == 0) {
            bounds = fragment.rect;
            continue;
        }
        const unsigned right = std::max(bounds.x + bounds.width,
                                        fragment.rect.x + fragment.rect.width);
        const unsigned bottom = std::max(bounds.y + bounds.height,
                                         fragment.rect.y + fragment.rect.height);
        bounds.x = std::min(bounds.x, fragment.rect.x);
        bounds.y = std::min(bounds.y, fragment.rect.y);
        bounds.width = right - bounds.x;
        bounds.height = bottom - bounds.y;
    }
    return bounds;
}

void append_u16_le(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32_le(std::vector<uint8_t>& output, uint32_t value) {
    append_u16_le(output, static_cast<uint16_t>(value));
    append_u16_le(output, static_cast<uint16_t>(value >> 16));
}

void append_u64_le(std::vector<uint8_t>& output, uint64_t value) {
    append_u32_le(output, static_cast<uint32_t>(value));
    append_u32_le(output, static_cast<uint32_t>(value >> 32));
}

bool take_u16_le(const uint8_t** cursor, const uint8_t* end, uint16_t* out) {
    if (!cursor || !*cursor || !out || end - *cursor < 2) return false;
    const uint8_t* p = *cursor;
    *out = static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
    *cursor += 2;
    return true;
}

bool take_u32_le(const uint8_t** cursor, const uint8_t* end, uint32_t* out) {
    uint16_t low = 0, high = 0;
    if (!take_u16_le(cursor, end, &low) ||
        !take_u16_le(cursor, end, &high))
        return false;
    *out = static_cast<uint32_t>(low) |
           (static_cast<uint32_t>(high) << 16);
    return true;
}

bool take_u64_le(const uint8_t** cursor, const uint8_t* end, uint64_t* out) {
    uint32_t low = 0, high = 0;
    if (!take_u32_le(cursor, end, &low) ||
        !take_u32_le(cursor, end, &high))
        return false;
    *out = static_cast<uint64_t>(low) |
           (static_cast<uint64_t>(high) << 32);
    return true;
}

bool valid_fragment(const Upload& upload, const Fragment& fragment) {
    const Rect& rect = fragment.rect;
    return rect.width != 0 && rect.height != 0 &&
           rect.x < kVramWidth && rect.y < kVramHeight &&
           rect.width <= kVramWidth - rect.x &&
           rect.height <= kVramHeight - rect.y &&
           fragment.source_x < upload.width &&
           fragment.source_y < upload.height &&
           rect.width <= unsigned{upload.width} - fragment.source_x &&
           rect.height <= unsigned{upload.height} - fragment.source_y;
}

bool parse_tracking_state(const uint8_t* data, size_t size,
                          uint64_t* out_next_serial,
                          std::vector<Upload>* out_uploads) {
    if (!data || size < kTrackingStateHeaderBytes ||
        size > kMaxTrackingStateBytes || !out_next_serial)
        return false;
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    uint32_t magic = 0, version = 0, upload_count = 0, reserved = 0;
    uint64_t next_serial = 0, max_serial = 0;
    uint64_t total_fragments = 0;
    if (!take_u32_le(&cursor, end, &magic) ||
        !take_u32_le(&cursor, end, &version) ||
        !take_u64_le(&cursor, end, &next_serial) ||
        !take_u32_le(&cursor, end, &upload_count) ||
        !take_u32_le(&cursor, end, &reserved) ||
        magic != kTrackingStateMagic || version != kTrackingStateVersion ||
        next_serial == 0 || reserved != 0 ||
        upload_count > kMaxTrackingStateUploads)
        return false;

    std::vector<Upload> parsed;
    if (out_uploads) parsed.reserve(upload_count);
    std::vector<uint64_t> serials;
    serials.reserve(upload_count);
    for (uint32_t i = 0; i < upload_count; ++i) {
        Upload upload;
        uint32_t fragment_count = 0;
        if (!take_u64_le(&cursor, end, &upload.serial) ||
            !take_u32_le(&cursor, end, &upload.hash) ||
            !take_u16_le(&cursor, end, &upload.width) ||
            !take_u16_le(&cursor, end, &upload.height) ||
            !take_u32_le(&cursor, end, &fragment_count) ||
            upload.serial == 0 || upload.width == 0 || upload.height == 0 ||
            upload.width > kVramWidth || upload.height > kVramHeight ||
            fragment_count == 0 ||
            fragment_count > kMaxTrackingStateFragments - total_fragments)
            return false;
        total_fragments += fragment_count;
        upload.fragments.reserve(fragment_count);
        for (uint32_t j = 0; j < fragment_count; ++j) {
            Fragment fragment;
            uint16_t x = 0, y = 0, width = 0, height = 0;
            uint16_t source_x = 0, source_y = 0;
            if (!take_u16_le(&cursor, end, &x) ||
                !take_u16_le(&cursor, end, &y) ||
                !take_u16_le(&cursor, end, &width) ||
                !take_u16_le(&cursor, end, &height) ||
                !take_u16_le(&cursor, end, &source_x) ||
                !take_u16_le(&cursor, end, &source_y))
                return false;
            fragment.rect = Rect{x, y, width, height};
            fragment.source_x = source_x;
            fragment.source_y = source_y;
            if (!valid_fragment(upload, fragment)) return false;
            for (const Fragment& prior : upload.fragments)
                if (intersects(prior.rect, fragment.rect)) return false;
            upload.fragments.push_back(fragment);
        }
        upload.bounds = fragment_bounds(upload.fragments);
        if (std::find(serials.begin(), serials.end(), upload.serial) !=
            serials.end())
            return false;
        serials.push_back(upload.serial);
        max_serial = std::max(max_serial, upload.serial);
        if (out_uploads) parsed.push_back(std::move(upload));
    }
    if (cursor != end || next_serial <= max_serial) return false;
    *out_next_serial = next_serial;
    if (out_uploads) *out_uploads = std::move(parsed);
    return true;
}

bool covered_by_upload(const Upload& upload, const std::vector<Rect>& query) {
    for (const Rect& wanted : query) {
        std::vector<Rect> uncovered{wanted};
        for (const Fragment& fragment : upload.fragments) {
            std::vector<Rect> next;
            for (const Rect& piece : uncovered) {
                std::vector<Rect> remainder = subtract_rect(piece, fragment.rect);
                next.insert(next.end(), remainder.begin(), remainder.end());
            }
            uncovered.swap(next);
            if (uncovered.empty()) break;
        }
        if (!uncovered.empty()) return false;
    }
    return true;
}

struct EntryRecord {
    uint32_t texture_hash = 0;
    uint32_t palette_hash = 0;
    std::string replacement_path;
    std::string logical_path;
    bool ambiguous = false;
};

struct DecodedImage {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
};

enum class DecodeState { Queued, Loading, Ready, Failed };

struct DecodeItem {
    DecodeState state = DecodeState::Queued;
    std::string path;
    std::shared_ptr<DecodedImage> image;
    size_t bytes = 0;
    uint64_t last_used = 0;
};

struct DecodeCache {
    std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<uint64_t, DecodeItem> items;
    std::deque<uint64_t> queue;
    std::thread worker;
    size_t budget = kDefaultDecodeBudget;
    size_t used = 0;
    uint64_t tick = 0;
    bool started = false;
    bool stop = false;

    ~DecodeCache() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
};

struct PixelLease {
    std::shared_ptr<DecodedImage> image;
};

void evict_decode_cache(DecodeCache& cache, size_t incoming = 0) {
    while (cache.used + incoming > cache.budget) {
        auto victim = cache.items.end();
        for (auto it = cache.items.begin(); it != cache.items.end(); ++it) {
            if (it->second.state != DecodeState::Ready) continue;
            if (victim == cache.items.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == cache.items.end()) break;
        cache.used -= victim->second.bytes;
        cache.items.erase(victim);
    }
}

#ifndef HD_TEXTURE_PACK_DISABLE_PNG_DECODE
void decode_worker(DecodeCache* cache) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        uint64_t key = 0;
        std::string path;
        {
            std::unique_lock<std::mutex> lock(cache->mutex);
            cache->wake.wait(lock, [&] { return cache->stop || !cache->queue.empty(); });
            if (cache->stop) return;
            key = cache->queue.front();
            cache->queue.pop_front();
            auto it = cache->items.find(key);
            if (it == cache->items.end()) continue;
            it->second.state = DecodeState::Loading;
            path = it->second.path;
        }

        std::vector<uint8_t> encoded;
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            std::streamoff length = -1;
            if (input) length = static_cast<std::streamoff>(input.tellg());
            if (length > 0 && static_cast<uint64_t>(length) <= kMaxPngFileBytes &&
                static_cast<uint64_t>(length) <=
                    static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                encoded.resize(static_cast<size_t>(length));
                input.seekg(0);
                if (!input.read(reinterpret_cast<char*>(encoded.data()), length))
                    encoded.clear();
            }
        }
        int width = 0;
        int height = 0;
        int components = 0;
        stbi_uc* loaded = encoded.empty() ? nullptr : stbi_load_from_memory(
            encoded.data(), static_cast<int>(encoded.size()),
            &width, &height, &components, 4);
        std::shared_ptr<DecodedImage> decoded;
        size_t bytes = 0;
        if (loaded && width > 0 && height > 0 &&
            static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <=
                std::numeric_limits<size_t>::max() / 4) {
            bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            decoded = std::make_shared<DecodedImage>();
            decoded->width = static_cast<uint32_t>(width);
            decoded->height = static_cast<uint32_t>(height);
            decoded->rgba.assign(loaded, loaded + bytes);
        }
        if (loaded) stbi_image_free(loaded);

        std::lock_guard<std::mutex> lock(cache->mutex);
        auto it = cache->items.find(key);
        if (it == cache->items.end()) continue;
        if (!decoded || bytes > cache->budget) {
            it->second.state = DecodeState::Failed;
            continue;
        }
        evict_decode_cache(*cache, bytes);
        if (cache->used + bytes > cache->budget) {
            it->second.state = DecodeState::Failed;
            continue;
        }
        it->second.image = std::move(decoded);
        it->second.bytes = bytes;
        it->second.last_used = ++cache->tick;
        it->second.state = DecodeState::Ready;
        cache->used += bytes;
    }
}
#endif

bool directory_has_pack_png(const fs::path& directory) {
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        uint32_t texture_hash = 0;
        uint32_t palette_hash = 0;
        if (parse_png_filename(it->path(), &texture_hash, &palette_hash)) return true;
    }
    return false;
}

void parse_hashes_ini(const fs::path& path,
                      std::unordered_map<uint64_t, std::string>* mappings) {
    if (!mappings) return;
    std::ifstream input(path);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#' || stripped[0] == '[')
            continue;
        const size_t equal = stripped.find('=');
        if (equal == std::string::npos) continue;
        uint32_t texture_hash = 0;
        uint32_t palette_hash = 0;
        if (!parse_key_text(trim(stripped.substr(0, equal)),
                            &texture_hash, &palette_hash))
            continue;
        const std::string logical = trim(stripped.substr(equal + 1));
        if (!logical.empty())
            (*mappings)[make_key(texture_hash, palette_hash)] = logical;
    }
}

std::vector<Rect> query_rectangles(const HdTextureDrawQuery& query) {
    const unsigned pixels_per_word = query.depth == HD_TEXTURE_DEPTH_4BPP ? 4u :
                                     query.depth == HD_TEXTURE_DEPTH_8BPP ? 2u : 1u;
    std::vector<std::pair<unsigned, unsigned>> u_ranges;
    std::vector<std::pair<unsigned, unsigned>> v_ranges;
    if (query.u_first <= query.u_last)
        u_ranges.push_back({query.u_first, query.u_last});
    else {
        u_ranges.push_back({query.u_first, 255});
        u_ranges.push_back({0, query.u_last});
    }
    if (query.v_first <= query.v_last)
        v_ranges.push_back({query.v_first, query.v_last});
    else {
        v_ranges.push_back({query.v_first, 255});
        v_ranges.push_back({0, query.v_last});
    }

    std::vector<Rect> result;
    for (const auto& vrange : v_ranges) {
        for (const auto& urange : u_ranges) {
            const unsigned first_word = urange.first / pixels_per_word;
            const unsigned last_word = urange.second / pixels_per_word;
            std::vector<Rect> pieces = physical_rects(
                unsigned{query.page_x} + first_word,
                unsigned{query.page_y} + vrange.first,
                last_word - first_word + 1,
                vrange.second - vrange.first + 1);
            result.insert(result.end(), pieces.begin(), pieces.end());
        }
    }
    return result;
}

void fill_entry(const EntryRecord& record, HdTexturePackEntry* entry) {
    if (!entry) return;
    entry->texture_hash = record.texture_hash;
    entry->palette_hash = record.palette_hash;
    entry->replacement_path = record.replacement_path.c_str();
    entry->logical_path = record.logical_path.c_str();
}

} // namespace

struct HdTexturePack {
    std::string asset_root;
    std::string replacement_root;
    std::unordered_map<uint64_t, EntryRecord> entries;
    size_t replacement_file_count = 0;
    size_t ambiguous_key_count = 0;
    size_t logical_mapping_count = 0;
    uint64_t next_upload_serial = 1;
    std::vector<Upload> uploads;
    std::array<std::vector<uint64_t>, kUploadIndexCells> upload_index;
    std::unordered_map<uint64_t, Upload*> upload_by_serial;
    /* Conservative broad phase for the GPU primitive hot path.  Most scene
     * draws target frame-buffer VRAM and cannot affect texture uploads; avoid
     * allocating cut/candidate vectors for those draws.  This union may stay
     * larger after invalidation, but is reset with tracking and therefore can
     * only cause extra work, never a missed invalidation. */
    Rect upload_bounds{};
    DecodeCache decode;
    /* Lazily-built stable ordering over entries, for indexed enumeration
     * (hd_texture_pack_get_entry) -- a boot-time preload pass walking every
     * loaded entry needs index-based access an unordered_map doesn't give
     * directly. Rebuilt whenever the entry count changes (only happens
     * during hd_texture_pack_create, before any indexed access). */
    mutable std::vector<uint64_t> entry_keys_cache;
};

unsigned upload_index_cell(unsigned tx, unsigned ty) {
    return ty * kUploadIndexCols + tx;
}

void upload_index_clear(HdTexturePack* pack) {
    if (!pack) return;
    for (auto& bucket : pack->upload_index) bucket.clear();
    pack->upload_by_serial.clear();
    for (Upload& upload : pack->uploads) upload.index_tiles.clear();
}

void upload_index_remove(HdTexturePack* pack, Upload& upload) {
    if (!pack) return;
    for (const uint16_t tile : upload.index_tiles) {
        auto& bucket = pack->upload_index[tile];
        bucket.erase(std::remove(bucket.begin(), bucket.end(), upload.serial),
                     bucket.end());
    }
    upload.index_tiles.clear();
    pack->upload_by_serial.erase(upload.serial);
}

void upload_index_add(HdTexturePack* pack, Upload& upload) {
    if (!pack || upload.fragments.empty() || upload.bounds.width == 0 ||
        upload.bounds.height == 0)
        return;
    const unsigned first_x = upload.bounds.x / kUploadIndexTileSize;
    const unsigned first_y = upload.bounds.y / kUploadIndexTileSize;
    const unsigned last_x = std::min(
        kUploadIndexCols - 1,
        (upload.bounds.x + upload.bounds.width - 1) / kUploadIndexTileSize);
    const unsigned last_y = std::min(
        kUploadIndexRows - 1,
        (upload.bounds.y + upload.bounds.height - 1) / kUploadIndexTileSize);
    pack->upload_by_serial[upload.serial] = &upload;
    upload.index_tiles.reserve((last_x - first_x + 1) *
                               (last_y - first_y + 1));
    for (unsigned ty = first_y; ty <= last_y; ++ty) {
        for (unsigned tx = first_x; tx <= last_x; ++tx) {
            const unsigned tile = upload_index_cell(tx, ty);
            pack->upload_index[tile].push_back(upload.serial);
            upload.index_tiles.push_back(static_cast<uint16_t>(tile));
        }
    }
}

void upload_index_refresh(HdTexturePack* pack, Upload& upload) {
    upload_index_remove(pack, upload);
    upload_index_add(pack, upload);
}

void upload_index_rebuild(HdTexturePack* pack) {
    if (!pack) return;
    for (auto& bucket : pack->upload_index) bucket.clear();
    pack->upload_by_serial.clear();
    pack->upload_bounds = Rect{};
    for (Upload& upload : pack->uploads) {
        upload.index_tiles.clear();
        upload_index_add(pack, upload);
        if (pack->upload_bounds.width == 0 || pack->upload_bounds.height == 0) {
            pack->upload_bounds = upload.bounds;
        } else {
            const unsigned right = std::max(
                pack->upload_bounds.x + pack->upload_bounds.width,
                upload.bounds.x + upload.bounds.width);
            const unsigned bottom = std::max(
                pack->upload_bounds.y + pack->upload_bounds.height,
                upload.bounds.y + upload.bounds.height);
            pack->upload_bounds.x = std::min(pack->upload_bounds.x, upload.bounds.x);
            pack->upload_bounds.y = std::min(pack->upload_bounds.y, upload.bounds.y);
            pack->upload_bounds.width = right - pack->upload_bounds.x;
            pack->upload_bounds.height = bottom - pack->upload_bounds.y;
        }
    }
}

void upload_index_collect(const HdTexturePack* pack,
                          const std::vector<Rect>& query,
                          std::vector<uint64_t>* out_serials) {
    if (!pack || !out_serials) return;
    out_serials->clear();
    for (const Rect& rect : query) {
        if (rect.width == 0 || rect.height == 0) continue;
        const unsigned first_x = rect.x / kUploadIndexTileSize;
        const unsigned first_y = rect.y / kUploadIndexTileSize;
        const unsigned last_x = std::min(
            kUploadIndexCols - 1,
            (rect.x + rect.width - 1) / kUploadIndexTileSize);
        const unsigned last_y = std::min(
            kUploadIndexRows - 1,
            (rect.y + rect.height - 1) / kUploadIndexTileSize);
        for (unsigned ty = first_y; ty <= last_y; ++ty) {
            for (unsigned tx = first_x; tx <= last_x; ++tx) {
                const auto& bucket = pack->upload_index[
                    upload_index_cell(tx, ty)];
                out_serials->insert(out_serials->end(), bucket.begin(),
                                    bucket.end());
            }
        }
    }
    std::sort(out_serials->begin(), out_serials->end());
    out_serials->erase(std::unique(out_serials->begin(), out_serials->end()),
                       out_serials->end());
}

extern "C" {

int hd_texture_pack_create(const char* explicit_root,
                           HdTexturePack** out_pack,
                           char* error,
                           size_t error_capacity) {
    if (out_pack) *out_pack = nullptr;
    if (error && error_capacity) error[0] = '\0';
    if (!out_pack) {
        write_error(error, error_capacity, "out_pack is null");
        return 0;
    }

    try {
        const char* selected = explicit_root;
        if (!selected || !selected[0]) selected = std::getenv("PSXRECOMP_HD_TEXTURE_ROOT");
        if (!selected || !selected[0]) {
            write_error(error, error_capacity,
                        "HD asset root is unset (PSXRECOMP_HD_TEXTURE_ROOT)");
            return 0;
        }

        std::error_code ec;
        fs::path input = fs::absolute(fs::path(selected), ec).lexically_normal();
        if (ec || !fs::is_directory(input, ec)) {
            write_error(error, error_capacity, "HD asset root is not a directory");
            return 0;
        }

        fs::path replacement;
        fs::path hashes_ini;
        if (directory_has_pack_png(input)) {
            replacement = input;
            hashes_ini = fs::is_regular_file(input / "Hashes.ini", ec)
                ? input / "Hashes.ini" : input.parent_path() / "Hashes.ini";
        } else if (fs::is_regular_file(input / "Hashes.ini", ec)) {
            std::vector<fs::path> children;
            for (fs::directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec) &&
                    ends_with_ci(it->path().filename().string(), "-texture-replacements"))
                    children.push_back(it->path());
            }
            std::sort(children.begin(), children.end());
            if (children.size() != 1) {
                write_error(error, error_capacity,
                            "pack root must contain exactly one *-texture-replacements directory");
                return 0;
            }
            replacement = children.front();
            hashes_ini = input / "Hashes.ini";
        } else {
            write_error(error, error_capacity,
                        "no numeric PNGs or Hashes.ini + replacement directory found");
            return 0;
        }

        auto pack = std::make_unique<HdTexturePack>();
        pack->asset_root = input.string();
        pack->replacement_root = replacement.lexically_normal().string();

        std::unordered_map<uint64_t, std::string> mappings;
        parse_hashes_ini(hashes_ini, &mappings);
        pack->logical_mapping_count = mappings.size();

        std::vector<fs::path> files;
        for (fs::directory_iterator it(replacement, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            uint32_t texture_hash = 0;
            uint32_t palette_hash = 0;
            if (parse_png_filename(it->path(), &texture_hash, &palette_hash))
                files.push_back(it->path());
        }
        if (ec) {
            write_error(error, error_capacity, "failed to scan replacement directory");
            return 0;
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            write_error(error, error_capacity, "replacement directory has no numeric PNGs");
            return 0;
        }

        for (const fs::path& path : files) {
            uint32_t texture_hash = 0;
            uint32_t palette_hash = 0;
            if (!parse_png_filename(path, &texture_hash, &palette_hash)) continue;
            ++pack->replacement_file_count;
            const uint64_t key = make_key(texture_hash, palette_hash);
            auto found = pack->entries.find(key);
            if (found != pack->entries.end()) {
                if (!found->second.ambiguous) {
                    found->second.ambiguous = true;
                    ++pack->ambiguous_key_count;
                }
                continue;
            }
            EntryRecord record;
            record.texture_hash = texture_hash;
            record.palette_hash = palette_hash;
            record.replacement_path = path.lexically_normal().string();
            const auto mapping = mappings.find(key);
            if (mapping != mappings.end()) record.logical_path = mapping->second;
            pack->entries.emplace(key, std::move(record));
        }

        *out_pack = pack.release();
        return 1;
    } catch (const std::exception& exception) {
        write_error(error, error_capacity, exception.what());
        return 0;
    }
}

void hd_texture_pack_destroy(HdTexturePack* pack) {
    delete pack;
}

void hd_texture_pack_get_info(const HdTexturePack* pack,
                              HdTexturePackInfo* out_info) {
    if (!out_info) return;
    std::memset(out_info, 0, sizeof(*out_info));
    if (!pack) return;
    out_info->asset_root = pack->asset_root.c_str();
    out_info->replacement_root = pack->replacement_root.c_str();
    out_info->replacement_file_count = pack->replacement_file_count;
    out_info->unique_key_count = pack->entries.size() - pack->ambiguous_key_count;
    out_info->ambiguous_key_count = pack->ambiguous_key_count;
    out_info->logical_mapping_count = pack->logical_mapping_count;
}

size_t hd_texture_pack_entry_count(const HdTexturePack* pack) {
    return pack ? pack->entries.size() : 0;
}

int hd_texture_pack_get_entry(const HdTexturePack* pack, size_t index,
                              HdTexturePackEntry* out_entry) {
    if (!pack || index >= pack->entries.size()) return 0;
    if (pack->entry_keys_cache.size() != pack->entries.size()) {
        pack->entry_keys_cache.clear();
        pack->entry_keys_cache.reserve(pack->entries.size());
        for (const auto& kv : pack->entries) pack->entry_keys_cache.push_back(kv.first);
    }
    const auto found = pack->entries.find(pack->entry_keys_cache[index]);
    if (found == pack->entries.end()) return 0;
    if (out_entry) fill_entry(found->second, out_entry);
    return 1;
}

int hd_texture_pack_lookup(const HdTexturePack* pack,
                           uint32_t texture_hash,
                           uint32_t palette_hash,
                           HdTexturePackEntry* out_entry) {
    if (out_entry) std::memset(out_entry, 0, sizeof(*out_entry));
    if (!pack) return HD_TEXTURE_LOOKUP_ERROR;
    const auto found = pack->entries.find(make_key(texture_hash, palette_hash));
    if (found == pack->entries.end()) return HD_TEXTURE_LOOKUP_NONE;
    if (found->second.ambiguous) return HD_TEXTURE_LOOKUP_AMBIGUOUS;
    fill_entry(found->second, out_entry);
    return HD_TEXTURE_LOOKUP_FOUND;
}

uint32_t hd_texture_crc32_words_le(const uint16_t* words, size_t word_count) {
    if (!words && word_count != 0) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < word_count; ++i) {
        crc = crc_byte(crc, static_cast<uint8_t>(words[i] & 0xFFu));
        crc = crc_byte(crc, static_cast<uint8_t>(words[i] >> 8));
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t hd_texture_hash_clut(const uint16_t* vram,
                              size_t vram_word_count,
                              uint16_t clut_x,
                              uint16_t clut_y,
                              uint8_t depth) {
    if (depth == HD_TEXTURE_DEPTH_16BPP) return 0;
    if (!vram || vram_word_count < kVramWords) return 0;
    const unsigned count = depth == HD_TEXTURE_DEPTH_4BPP ? 16u :
                           depth == HD_TEXTURE_DEPTH_8BPP ? 256u : 0u;
    if (count == 0) return 0;
    std::array<uint16_t, 256> palette{};
    const unsigned y = clut_y & (kVramHeight - 1);
    for (unsigned i = 0; i < count; ++i)
        palette[i] = vram[y * kVramWidth + ((unsigned{clut_x} + i) &
                                            (kVramWidth - 1))];
    return hd_texture_crc32_words_le(palette.data(), count);
}

void hd_texture_pack_invalidate(HdTexturePack* pack,
                                uint16_t x,
                                uint16_t y,
                                uint16_t width_words,
                                uint16_t height) {
    if (!pack || width_words == 0 || height == 0 ||
        width_words > kVramWidth || height > kVramHeight)
        return;
    bool erased_upload = false;
    if (pack->upload_bounds.width == 0 || pack->upload_bounds.height == 0)
        return;
    if ((unsigned{x} + width_words <= kVramWidth) &&
        (unsigned{y} + height <= kVramHeight)) {
        const Rect cut{unsigned{x}, unsigned{y}, width_words, height};
        if (!intersects(pack->upload_bounds, cut)) return;
    }
    const std::vector<Rect> cuts = physical_rects(x, y, width_words, height);
    std::vector<uint64_t> candidate_serials;
    upload_index_collect(pack, cuts, &candidate_serials);
    for (const uint64_t serial : candidate_serials) {
        auto indexed = pack->upload_by_serial.find(serial);
        if (indexed == pack->upload_by_serial.end() || !indexed->second)
            continue;
        Upload& upload = *indexed->second;
        for (const Rect& cut : cuts) {
            /* Most primitives are outside every tracked upload.  The union is
             * conservative, so this can only skip work that is provably
             * unrelated; individual fragments remain the source of truth. */
            if (!intersects(upload.bounds, cut)) continue;
            std::vector<Fragment> next;
            next.reserve(upload.fragments.size() + 4);
            for (const Fragment& fragment : upload.fragments) {
                if (!intersects(fragment.rect, cut)) {
                    next.push_back(fragment);
                    continue;
                }
                std::vector<Fragment> pieces = subtract_fragment(fragment, cut);
                next.insert(next.end(), pieces.begin(), pieces.end());
            }
            upload.fragments.swap(next);
            upload.bounds = fragment_bounds(upload.fragments);
            if (upload.fragments.empty()) {
                upload_index_remove(pack, upload);
            } else {
                upload_index_refresh(pack, upload);
            }
        }
    }
    const auto old_size = pack->uploads.size();
    pack->uploads.erase(
        std::remove_if(pack->uploads.begin(), pack->uploads.end(),
                       [](const Upload& upload) { return upload.fragments.empty(); }),
        pack->uploads.end());
    erased_upload = old_size != pack->uploads.size();
    /* Vector erasure can move surviving Upload objects, invalidating the
     * pointer map.  Rebuild only on this uncommon path; ordinary primitive
     * invalidation updates only the few intersecting uploads. */
    if (erased_upload) upload_index_rebuild(pack);
}

/* Temporary stage-by-stage diagnostics (2026-09-07): a live A/B against real
 * game data measured 0 matches out of 80k+ attempts despite successful
 * uploads and a loaded pack, with no way to tell from outside which of the
 * three sequential filters below (broad-phase / hash-key / full-coverage)
 * was the one rejecting everything. See gpu_hd_texture_pack_diag_stats. */
uint64_t g_hd_pack_diag_no_candidates = 0;     /* upload_index_collect found nothing */
uint64_t g_hd_pack_diag_no_hash_match = 0;     /* had candidates, none had this (hash,palette) key */
uint64_t g_hd_pack_diag_not_covered = 0;       /* key matched, covered_by_upload rejected it */
/* Raw numeric snapshot of the first query + first tracked upload, so a
 * suspected coordinate-space mismatch between the two can be read directly
 * instead of inferred from aggregate pass/fail counts. */
unsigned g_hd_pack_diag_query_x = 0, g_hd_pack_diag_query_y = 0,
        g_hd_pack_diag_query_w = 0, g_hd_pack_diag_query_h = 0;
int g_hd_pack_diag_query_captured = 0;
unsigned g_hd_pack_diag_upload_x = 0, g_hd_pack_diag_upload_y = 0,
        g_hd_pack_diag_upload_w = 0, g_hd_pack_diag_upload_h = 0;
int g_hd_pack_diag_upload_captured = 0;
/* Rolling "last successful match" snapshot: which replacement file and query
 * rect a real draw resolved to, overwritten every match so it can be read
 * while a suspect texture is on screen right now instead of only at boot. */
/* Ring of the last N successful matches (not just the most recent one), so
 * a single static on-screen frame that mixes correct and wrong replacement
 * images across its several glyph/quad draws (confirmed live: a dialogue
 * text bubble showing some correct characters and some from an unrelated
 * screen at the same time) can be inspected as a SET instead of only ever
 * seeing whichever draw happened to run last. The frame keeps re-drawing
 * every refresh while on screen, so querying this at any moment reflects a
 * stable, representative sample of that frame's actual draws. */
#define HD_PACK_DIAG_RING_CAP 400
struct HdPackDiagRingEntry {
    char path[256];
    unsigned texhash = 0, palhash = 0;
    unsigned qx = 0, qy = 0, qw = 0, qh = 0;
    uint64_t upload_serial = 0;
    unsigned upload_w = 0, upload_h = 0, upload_fragments = 0;
    unsigned anchor_x = 0, anchor_y = 0, anchor_found = 0;
    unsigned source_word_x = 0, source_y = 0;
    unsigned clut_x = 0, clut_y = 0, depth = 0;
};
HdPackDiagRingEntry g_hd_pack_diag_ring[HD_PACK_DIAG_RING_CAP];
int g_hd_pack_diag_ring_pos = 0;
uint64_t g_hd_pack_diag_ring_total = 0;

char g_hd_pack_diag_last_path[512] = {0};
unsigned g_hd_pack_diag_last_texhash = 0, g_hd_pack_diag_last_palhash = 0;
unsigned g_hd_pack_diag_last_qx = 0, g_hd_pack_diag_last_qy = 0,
        g_hd_pack_diag_last_qw = 0, g_hd_pack_diag_last_qh = 0;
uint64_t g_hd_pack_diag_last_upload_serial = 0;

/* 2026-09-07 investigation: is the game's real font atlas (known-good
 * texture_hash 0xF98B3634, confirmed against the pack's own documented
 * bubble/menu-text replacements) EVER tracked as an upload at all, and does
 * anything ever touch the shared VRAM word-region (320,0)-(384,48) where a
 * stale boot-time credits-screen upload (0xF4F1A2B4) keeps winning every
 * match? Two independent counters so "never tracked at all" (a write path
 * we don't hook) can be told apart from "tracked but never wins the match"
 * (a residency/candidate-selection bug). */
uint64_t g_hd_pack_diag_font_hash_seen = 0;      /* track_upload calls whose hash == 0xF98B3634 */
uint64_t g_hd_pack_diag_font_region_touches = 0; /* track_upload calls intersecting (320,0,64,48) */
uint32_t g_hd_pack_diag_font_region_last_hash = 0;
uint64_t g_hd_pack_diag_font_region_last_serial = 0;

/* Ring of the last N successful hd_texture_pack_match_fused calls (2026-09-08
 * multi-entry-candidate investigation): the plain match's ring above only
 * records PLAIN matches, so it says nothing about what the fused path
 * actually resolved for an animated (mouth/eye "gif") region once it stopped
 * bailing out on a second candidate. A single "last match" snapshot turned
 * out not to be enough on its own: the fused path also handles small,
 * frequent UI elements (dialogue-box glyphs, HUD icons), so the single most
 * recent call is as likely to be one of those as the specific face draw
 * being investigated (confirmed live: a "last" snapshot queried while a
 * character's face with the suspected seam was on screen turned out to be a
 * 7x14-texel glyph-sized region instead). A ring mirrors g_hd_pack_diag_ring
 * above so every fused match from one held frame can be inspected as a set
 * and filtered by piece size to find the actual character draw. */
#define HD_FUSED_DIAG_MAX_ENTRIES 4
#define HD_FUSED_DIAG_MAX_PIECES 24
#define HD_FUSED_DIAG_RING_CAP 64
struct HdFusedDiagEntry {
    char path[256] = {0};
    unsigned texhash = 0, palhash = 0;
    uint64_t serial = 0;
    unsigned upload_w = 0, upload_h = 0, upload_fragments = 0;
};
struct HdFusedDiagPiece {
    unsigned x = 0, y = 0, width = 0, height = 0;
    int has_hd = 0, hd_entry_idx = -1;
    unsigned hd_src_x = 0, hd_src_y = 0;
};
struct HdFusedDiagRingEntry {
    unsigned qx = 0, qy = 0, qw = 0, qh = 0;
    int entry_count = 0;
    HdFusedDiagEntry entries[HD_FUSED_DIAG_MAX_ENTRIES];
    int piece_count = 0;
    HdFusedDiagPiece pieces[HD_FUSED_DIAG_MAX_PIECES];
};
HdFusedDiagRingEntry g_hd_fused_diag_ring[HD_FUSED_DIAG_RING_CAP];
int g_hd_fused_diag_ring_pos = 0;
uint64_t g_hd_fused_diag_total = 0;

/* Ring of the last ~32 hd_texture_pack_match_fused FAILURES, with a reason
 * code -- 2026-09-08 investigation into why a specific character's mouth
 * region sometimes falls through to fully-native (visible via the
 * PSXRECOMP_HD_TEXTURE_DEBUG_MISSING violet overlay) instead of fusing, even
 * though the same face upload/entry fuses successfully for smaller queries
 * elsewhere on the same head (per the success ring above, and per
 * gpu_hd_texture_pack_match_stats' no_candidates counter reading 0 all
 * session -- ruling out "this VRAM region is untracked"). Reasons:
 * 1=UV-wrapped/empty query (out of scope by design), 2=ambiguous pack entry,
 * 3=more than max_entries distinct candidate uploads, 4=no candidate's hash
 * matched any pack entry, 5=more pieces than max_pieces, 6=fewer than 2
 * pieces (nothing to fuse -- would have matched or gone plain-native
 * anyway). */
#define HD_FUSED_FAIL_RING_CAP 32
struct HdFusedFailEntry {
    unsigned qx = 0, qy = 0, qw = 0, qh = 0;
    int reason = 0;
};
HdFusedFailEntry g_hd_fused_fail_ring[HD_FUSED_FAIL_RING_CAP];
int g_hd_fused_fail_ring_pos = 0;
uint64_t g_hd_fused_fail_total = 0;
uint64_t g_hd_fused_fail_reason_counts[8] = {0};

static void hd_fused_record_fail(int reason, unsigned qx, unsigned qy, unsigned qw, unsigned qh) {
    HdFusedFailEntry& e = g_hd_fused_fail_ring[g_hd_fused_fail_ring_pos];
    e.qx = qx; e.qy = qy; e.qw = qw; e.qh = qh; e.reason = reason;
    g_hd_fused_fail_ring_pos = (g_hd_fused_fail_ring_pos + 1) % HD_FUSED_FAIL_RING_CAP;
    ++g_hd_fused_fail_total;
    if (reason >= 0 && reason < 8) ++g_hd_fused_fail_reason_counts[reason];
}

int hd_texture_pack_track_upload(HdTexturePack* pack,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width_words,
                                 uint16_t height,
                                 const uint16_t* words,
                                 size_t word_count,
                                 uint32_t* out_texture_hash) {
    if (out_texture_hash) *out_texture_hash = 0;
    const size_t required = size_t{width_words} * height;
    if (!pack || !words || width_words == 0 || height == 0 ||
        width_words > kVramWidth || height > kVramHeight || word_count < required)
        return 0;
    const uint32_t hash = hd_texture_crc32_words_le(words, required);
    hd_texture_pack_invalidate(pack, x, y, width_words, height);
    Upload upload;
    upload.serial = pack->next_upload_serial++;
    upload.hash = hash;
    upload.width = width_words;
    upload.height = height;
    upload.fragments = physical_fragments(x, y, width_words, height);
    upload.bounds = fragment_bounds(upload.fragments);
    if (pack->upload_bounds.width == 0 || pack->upload_bounds.height == 0) {
        pack->upload_bounds = upload.bounds;
    } else {
        const unsigned right = std::max(
            pack->upload_bounds.x + pack->upload_bounds.width,
            upload.bounds.x + upload.bounds.width);
        const unsigned bottom = std::max(
            pack->upload_bounds.y + pack->upload_bounds.height,
            upload.bounds.y + upload.bounds.height);
        pack->upload_bounds.x = std::min(pack->upload_bounds.x, upload.bounds.x);
        pack->upload_bounds.y = std::min(pack->upload_bounds.y, upload.bounds.y);
        pack->upload_bounds.width = right - pack->upload_bounds.x;
        pack->upload_bounds.height = bottom - pack->upload_bounds.y;
    }
    if (!g_hd_pack_diag_upload_captured) {
        g_hd_pack_diag_upload_x = upload.bounds.x;
        g_hd_pack_diag_upload_y = upload.bounds.y;
        g_hd_pack_diag_upload_w = upload.bounds.width;
        g_hd_pack_diag_upload_h = upload.bounds.height;
        g_hd_pack_diag_upload_captured = 1;
    }
    if (hash == 0xF98B3634u) ++g_hd_pack_diag_font_hash_seen;
    if (intersects(upload.bounds, Rect{320, 0, 64, 48})) {
        ++g_hd_pack_diag_font_region_touches;
        g_hd_pack_diag_font_region_last_hash = hash;
        g_hd_pack_diag_font_region_last_serial = upload.serial;
    }
    pack->uploads.push_back(std::move(upload));
    /* A push may reallocate the vector, so refresh all broad-phase pointers
     * after adding an upload.  Upload creation is far less frequent than GPU
     * primitive submission. */
    upload_index_rebuild(pack);
    if (out_texture_hash) *out_texture_hash = hash;
    return 1;
}

void hd_texture_pack_reset_tracking(HdTexturePack* pack) {
    if (pack) {
        pack->uploads.clear();
        upload_index_clear(pack);
        pack->upload_bounds = Rect{};
    }
}

int hd_texture_pack_tracking_state_save(const HdTexturePack* pack,
                                        uint8_t** out_data,
                                        size_t* out_size) {
    if (!out_data || !out_size) return 0;
    *out_data = nullptr;
    *out_size = 0;
    try {
        const uint64_t next_serial = pack ? pack->next_upload_serial : 1;
        const size_t upload_count = pack ? pack->uploads.size() : 0;
        if (next_serial == 0 || upload_count > kMaxTrackingStateUploads)
            return 0;
        size_t fragment_count = 0;
        if (pack) {
            for (const Upload& upload : pack->uploads) {
                if (upload.serial == 0 || upload.serial >= next_serial ||
                    upload.width == 0 || upload.height == 0 ||
                    upload.width > kVramWidth || upload.height > kVramHeight ||
                    upload.fragments.empty())
                    return 0;
                if (upload.fragments.size() >
                    kMaxTrackingStateFragments - fragment_count)
                    return 0;
                fragment_count += upload.fragments.size();
                for (const Fragment& fragment : upload.fragments)
                    if (!valid_fragment(upload, fragment)) return 0;
            }
        }
        if (upload_count >
                (std::numeric_limits<size_t>::max() - kTrackingStateHeaderBytes) /
                    kTrackingStateUploadBytes ||
            fragment_count >
                (std::numeric_limits<size_t>::max() - kTrackingStateHeaderBytes -
                 upload_count * kTrackingStateUploadBytes) /
                    kTrackingStateFragmentBytes)
            return 0;
        const size_t bytes = kTrackingStateHeaderBytes +
            upload_count * kTrackingStateUploadBytes +
            fragment_count * kTrackingStateFragmentBytes;
        if (bytes > kMaxTrackingStateBytes) return 0;

        std::vector<uint8_t> wire;
        wire.reserve(bytes);
        append_u32_le(wire, kTrackingStateMagic);
        append_u32_le(wire, kTrackingStateVersion);
        append_u64_le(wire, next_serial);
        append_u32_le(wire, static_cast<uint32_t>(upload_count));
        append_u32_le(wire, 0);
        if (pack) {
            for (const Upload& upload : pack->uploads) {
                append_u64_le(wire, upload.serial);
                append_u32_le(wire, upload.hash);
                append_u16_le(wire, upload.width);
                append_u16_le(wire, upload.height);
                append_u32_le(wire,
                              static_cast<uint32_t>(upload.fragments.size()));
                for (const Fragment& fragment : upload.fragments) {
                    append_u16_le(wire, static_cast<uint16_t>(fragment.rect.x));
                    append_u16_le(wire, static_cast<uint16_t>(fragment.rect.y));
                    append_u16_le(wire,
                                  static_cast<uint16_t>(fragment.rect.width));
                    append_u16_le(wire,
                                  static_cast<uint16_t>(fragment.rect.height));
                    append_u16_le(wire,
                                  static_cast<uint16_t>(fragment.source_x));
                    append_u16_le(wire,
                                  static_cast<uint16_t>(fragment.source_y));
                }
            }
        }
        if (wire.size() != bytes) return 0;
        uint8_t* output = static_cast<uint8_t*>(std::malloc(bytes));
        if (!output) return 0;
        std::memcpy(output, wire.data(), bytes);
        *out_data = output;
        *out_size = bytes;
        return 1;
    } catch (...) {
        return 0;
    }
}

int hd_texture_pack_tracking_state_check(const uint8_t* data, size_t size) {
    try {
        uint64_t next_serial = 0;
        return parse_tracking_state(data, size, &next_serial, nullptr) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int hd_texture_pack_tracking_state_load(HdTexturePack* pack,
                                        const uint8_t* data,
                                        size_t size) {
    try {
        uint64_t next_serial = 0;
        std::vector<Upload> uploads;
        if (!parse_tracking_state(data, size, &next_serial, &uploads)) return 0;
        if (pack) {
            pack->next_upload_serial = next_serial;
            pack->uploads.swap(uploads);
            upload_index_rebuild(pack);
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

size_t hd_texture_pack_tracking_upload_count(const HdTexturePack* pack) {
    return pack ? pack->uploads.size() : 0;
}

/* ---- Missing-texture export -----------------------------------------------
 * When a draw's (texture_hash, palette_hash) has no entry in the active
 * pack, dump the native VRAM content it WOULD need as a plain RGBA PNG named
 * the same way the pack itself names replacements (<texhash>-<palhash>.png),
 * so a later pass can eyeball what's missing, upscale/redraw it by hand, and
 * drop the result straight into the pack under that exact filename. Purely
 * an authoring aid -- never consulted for matching, and a decode/write
 * failure here must never affect the native draw itself. */
namespace {

void hd_texture_rgba5551_to_rgba8(uint16_t c, uint8_t* out4) {
    const unsigned r5 = c & 0x1Fu;
    const unsigned g5 = (c >> 5) & 0x1Fu;
    const unsigned b5 = (c >> 10) & 0x1Fu;
    /* Hard cutout, matching hd_texture_hash_clut's own palette convention:
     * the all-zero color (bits 0-14 AND the STP bit) is the one true
     * "transparent" entry; every other color -- STP included -- is opaque
     * for a plain reference-image export like this one. */
    out4[0] = static_cast<uint8_t>((r5 * 255u + 15u) / 31u);
    out4[1] = static_cast<uint8_t>((g5 * 255u + 15u) / 31u);
    out4[2] = static_cast<uint8_t>((b5 * 255u + 15u) / 31u);
    out4[3] = (c == 0) ? 0 : 255;
}

/* Decodes the query's exact WORD-space rect (as computed by
 * query_rectangles/hd_texture_pack_match, same convention as first_x/first_y
 * there) into a top-left-origin RGBA8 image. Output width is in TEXELS
 * (rect.width words * pixels-per-word for the depth), height in scanlines. */
void hd_texture_decode_query_rgba(const HdTextureDrawQuery& query, const Rect& rect,
                                  std::vector<uint8_t>* out_rgba,
                                  unsigned* out_w, unsigned* out_h) {
    const unsigned pixels_per_word = query.depth == HD_TEXTURE_DEPTH_4BPP ? 4u :
                                     query.depth == HD_TEXTURE_DEPTH_8BPP ? 2u : 1u;
    const unsigned w = rect.width * pixels_per_word;
    const unsigned h = rect.height;
    *out_w = w; *out_h = h;
    out_rgba->assign(size_t{w} * h * 4u, 0);
    if (w == 0 || h == 0) return;

    std::array<uint16_t, 256> palette{};
    unsigned clut_count = 0;
    if (query.depth != HD_TEXTURE_DEPTH_16BPP) {
        clut_count = query.depth == HD_TEXTURE_DEPTH_4BPP ? 16u : 256u;
        const unsigned cy = query.clut_y & (kVramHeight - 1);
        for (unsigned i = 0; i < clut_count; ++i)
            palette[i] = query.vram[cy * kVramWidth +
                                    ((unsigned{query.clut_x} + i) & (kVramWidth - 1))];
    }

    for (unsigned row = 0; row < h; ++row) {
        const unsigned vy = (rect.y + row) & (kVramHeight - 1);
        for (unsigned col = 0; col < w; ++col) {
            const unsigned word_x = (rect.x + col / pixels_per_word) & (kVramWidth - 1);
            const uint16_t word = query.vram[vy * kVramWidth + word_x];
            uint8_t* px = out_rgba->data() + (size_t{row} * w + col) * 4u;
            if (query.depth == HD_TEXTURE_DEPTH_16BPP) {
                hd_texture_rgba5551_to_rgba8(word, px);
            } else if (query.depth == HD_TEXTURE_DEPTH_8BPP) {
                const unsigned idx = (word >> ((col % 2u) * 8u)) & 0xFFu;
                hd_texture_rgba5551_to_rgba8(idx < clut_count ? palette[idx] : 0, px);
            } else {
                const unsigned idx = (word >> ((col % 4u) * 4u)) & 0xFu;
                hd_texture_rgba5551_to_rgba8(idx < clut_count ? palette[idx] : 0, px);
            }
        }
    }
}

void hd_texture_pack_export_missing(const HdTexturePack* pack,
                                    const HdTextureDrawQuery& query,
                                    uint32_t texture_hash,
                                    uint32_t palette_hash,
                                    const Rect& rect) {
    if (!pack || pack->replacement_root.empty()) return;
    /* Per-process dedup: a missing texture gets queried again every frame
     * it's drawn, but decode+encode+disk-existence-check on every one of
     * those would be wasteful -- only the first sighting per session does
     * any of that work. */
    static std::mutex s_export_mutex;
    static std::unordered_set<uint64_t> s_exported;
    const uint64_t key = make_key(texture_hash, palette_hash);
    {
        std::lock_guard<std::mutex> lock(s_export_mutex);
        if (!s_exported.insert(key).second) return;
    }

    std::error_code ec;
    const fs::path dir = fs::path(pack->replacement_root) / "missing_textures";
    fs::create_directories(dir, ec);
    char name[64];
    std::snprintf(name, sizeof(name), "%08x-%08x.png", texture_hash, palette_hash);
    const fs::path path = dir / name;
    if (fs::exists(path, ec)) return; /* already captured in an earlier session */

    unsigned w = 0, h = 0;
    std::vector<uint8_t> rgba;
    hd_texture_decode_query_rgba(query, rect, &rgba, &w, &h);
    if (w == 0 || h == 0) return;
    stbi_write_png(path.string().c_str(), static_cast<int>(w), static_cast<int>(h),
                   4, rgba.data(), static_cast<int>(w) * 4);
}

} // namespace

int hd_texture_pack_match(HdTexturePack* pack,
                          const HdTextureDrawQuery* query,
                          HdTextureMatch* out_match) {
    if (out_match) std::memset(out_match, 0, sizeof(*out_match));
    if (!pack || !query || query->depth > HD_TEXTURE_DEPTH_16BPP ||
        !query->vram || query->vram_word_count < kVramWords)
        return HD_TEXTURE_LOOKUP_ERROR;
    const std::vector<Rect> wanted = query_rectangles(*query);
    if (wanted.empty()) return HD_TEXTURE_LOOKUP_ERROR;
    const uint32_t palette_hash = hd_texture_hash_clut(
        query->vram, query->vram_word_count, query->clut_x, query->clut_y,
        query->depth);

    if (!g_hd_pack_diag_query_captured && !wanted.empty()) {
        g_hd_pack_diag_query_x = wanted[0].x;
        g_hd_pack_diag_query_y = wanted[0].y;
        g_hd_pack_diag_query_w = wanted[0].width;
        g_hd_pack_diag_query_h = wanted[0].height;
        g_hd_pack_diag_query_captured = 1;
    }
    std::vector<uint64_t> candidate_serials;
    upload_index_collect(pack, wanted, &candidate_serials);
    if (candidate_serials.empty()) ++g_hd_pack_diag_no_candidates;
    bool diag_saw_hash_match = false;
    const Upload* candidate = nullptr;
    const EntryRecord* candidate_entry = nullptr;
    for (const uint64_t serial : candidate_serials) {
        const auto indexed = pack->upload_by_serial.find(serial);
        if (indexed == pack->upload_by_serial.end() || !indexed->second)
            continue;
        const Upload& upload = *indexed->second;
        const auto record = pack->entries.find(make_key(upload.hash, palette_hash));
        if (record == pack->entries.end()) continue;
        diag_saw_hash_match = true;
        if (!covered_by_upload(upload, wanted)) continue;
        if (record->second.ambiguous) return HD_TEXTURE_LOOKUP_AMBIGUOUS;
        if (candidate) return HD_TEXTURE_LOOKUP_AMBIGUOUS;
        candidate = &upload;
        candidate_entry = &record->second;
    }
    if (!candidate_serials.empty() && !diag_saw_hash_match) ++g_hd_pack_diag_no_hash_match;
    if (diag_saw_hash_match && !candidate) ++g_hd_pack_diag_not_covered;
    if (!candidate || !candidate_entry) {
        /* Missing-texture export: only worth the extra covered_by_upload
         * pass on the (presumably rare) miss path -- the common matched
         * path above never pays for it. Picks the first tracked upload that
         * actually covers this query, so the exported PNG is named with
         * the exact (hash, palette) pair the pack would need. */
        for (const uint64_t serial : candidate_serials) {
            const auto indexed = pack->upload_by_serial.find(serial);
            if (indexed == pack->upload_by_serial.end() || !indexed->second) continue;
            const Upload& upload = *indexed->second;
            if (!covered_by_upload(upload, wanted)) continue;
            hd_texture_pack_export_missing(pack, *query, upload.hash, palette_hash, wanted[0]);
            break;
        }
        return HD_TEXTURE_LOOKUP_NONE;
    }

    const unsigned pixels_per_word = query->depth == HD_TEXTURE_DEPTH_4BPP ? 4u :
                                     query->depth == HD_TEXTURE_DEPTH_8BPP ? 2u : 1u;
    const unsigned first_x = (unsigned{query->page_x} +
                              query->u_first / pixels_per_word) &
                             (kVramWidth - 1);
    const unsigned first_y = (unsigned{query->page_y} + query->v_first) &
                             (kVramHeight - 1);
    uint16_t source_word_x = 0, source_y = 0;
    bool anchor_found = false;
    for (const Fragment& fragment : candidate->fragments) {
        if (contains_point(fragment.rect, first_x, first_y)) {
            source_word_x = static_cast<uint16_t>(fragment.source_x + first_x - fragment.rect.x);
            source_y = static_cast<uint16_t>(fragment.source_y + first_y - fragment.rect.y);
            anchor_found = true;
            break;
        }
    }

    {
        std::snprintf(g_hd_pack_diag_last_path, sizeof(g_hd_pack_diag_last_path),
                      "%s", candidate_entry->replacement_path.c_str());
        g_hd_pack_diag_last_texhash = candidate->hash;
        g_hd_pack_diag_last_palhash = palette_hash;
        g_hd_pack_diag_last_qx = wanted[0].x;
        g_hd_pack_diag_last_qy = wanted[0].y;
        g_hd_pack_diag_last_qw = wanted[0].width;
        g_hd_pack_diag_last_qh = wanted[0].height;
        g_hd_pack_diag_last_upload_serial = candidate->serial;

        HdPackDiagRingEntry& ring = g_hd_pack_diag_ring[g_hd_pack_diag_ring_pos];
        std::snprintf(ring.path, sizeof(ring.path), "%s", candidate_entry->replacement_path.c_str());
        ring.texhash = candidate->hash;
        ring.palhash = palette_hash;
        ring.qx = wanted[0].x; ring.qy = wanted[0].y;
        ring.qw = wanted[0].width; ring.qh = wanted[0].height;
        ring.upload_serial = candidate->serial;
        ring.upload_w = candidate->width; ring.upload_h = candidate->height;
        ring.upload_fragments = static_cast<unsigned>(candidate->fragments.size());
        ring.anchor_x = first_x; ring.anchor_y = first_y;
        ring.anchor_found = anchor_found ? 1u : 0u;
        ring.source_word_x = source_word_x; ring.source_y = source_y;
        ring.clut_x = query->clut_x; ring.clut_y = query->clut_y; ring.depth = query->depth;
        g_hd_pack_diag_ring_pos = (g_hd_pack_diag_ring_pos + 1) % HD_PACK_DIAG_RING_CAP;
        ++g_hd_pack_diag_ring_total;
    }
    if (out_match) {
        fill_entry(*candidate_entry, &out_match->entry);
        out_match->upload_serial = candidate->serial;
        out_match->upload_width_words = candidate->width;
        out_match->upload_height = candidate->height;
        out_match->source_word_x = source_word_x;
        out_match->source_y = source_y;
    }
    return HD_TEXTURE_LOOKUP_FOUND;
}

namespace {
/* Out-parameter instead of returning Rect: this sits inside the file's
 * extern "C" block, where a C++-only return type by value triggers
 * -Wreturn-type-c-linkage (see hd_texture_rgba5551_to_rgba8 above for the
 * same fix). *out is left zeroed when a/b do not intersect. */
void rect_intersection(const Rect& a, const Rect& b, Rect* out) {
    if (!intersects(a, b)) { *out = Rect{}; return; }
    const unsigned x = std::max(a.x, b.x);
    const unsigned y = std::max(a.y, b.y);
    const unsigned right = std::min(a.x + a.width, b.x + b.width);
    const unsigned bottom = std::min(a.y + a.height, b.y + b.height);
    *out = Rect{x, y, right - x, bottom - y};
}
} // namespace

/* Fused-page support (opt-in -- see gpu_hd_texture_fusion_set in gpu.c).
 * hd_texture_pack_match above requires one upload's fragments to FULLY
 * cover the query and gives up entirely otherwise (g_hd_pack_diag_not_covered),
 * which is exactly the case that produces a visible seam live: some
 * triangles of one mesh draw through the HD pipeline, the rest fall back to
 * native, and the two are two separate GL draw calls with different
 * shaders/blend state meeting at a hard edge. Real Beetle PSX HW's Vulkan
 * renderer avoids this by compositing every upload a draw's sample region
 * touches into one "fused page" texture before issuing a single draw (see
 * HD_TEXTURE_CACHE.md's "Fused-page path"/FusedPage) -- this is the same
 * idea. Handles one wanted rect (no UV wrap) and up to max_entries distinct
 * partially-covering uploads/entries (a single upload split into many
 * fragments by VRAM-invalidation history, or genuinely different uploads
 * both touching the query, are both routine -- confirmed live on a
 * character head texture with 7 surviving fragments that exceeded the
 * original single-candidate/4-piece v1 scope and still showed the seam
 * with fusion on, 2026-09-08). Candidates are resolved newest-upload-first
 * (see the serial sort below -- VRAM is last-write-wins): each one's
 * fragments claim whatever part of the STILL-uncovered region they overlap,
 * so two candidates' fragments covering the same pixels (rare) never
 * double-count -- the newer candidate wins. Reports each covered (HD, tagged with which
 * candidate/entry it came from) and uncovered (native-VRAM) piece so
 * gpu_gl_renderer.c can build the composite and draw once. Returns 0 (not
 * applicable -- caller keeps today's behavior unchanged) for anything
 * outside that scope: UV-wrapped queries, more than max_entries distinct
 * uploads, an ambiguous entry, or more pieces than max_pieces. */
int hd_texture_pack_match_fused(HdTexturePack* pack,
                                const HdTextureDrawQuery* query,
                                HdTexturePackEntry* out_entries,
                                uint16_t* out_upload_width_words,
                                uint16_t* out_upload_height,
                                int max_entries,
                                int* out_entry_count,
                                HdFusedPiece* out_pieces,
                                int max_pieces,
                                int* out_count) {
    if (out_count) *out_count = 0;
    if (out_entry_count) *out_entry_count = 0;
    if (!pack || !query || !out_entries || !out_pieces || max_pieces <= 0 ||
        max_entries <= 0 || query->depth > HD_TEXTURE_DEPTH_16BPP ||
        !query->vram || query->vram_word_count < kVramWords)
        return 0;
    const std::vector<Rect> wanted = query_rectangles(*query);
    if (wanted.size() != 1 || wanted[0].width == 0 || wanted[0].height == 0) {
        hd_fused_record_fail(1, wanted.empty() ? 0 : wanted[0].x, wanted.empty() ? 0 : wanted[0].y,
                             wanted.empty() ? 0 : wanted[0].width, wanted.empty() ? 0 : wanted[0].height);
        return 0; /* UV wrap or empty query: out of scope */
    }
    const Rect& want = wanted[0];
    const uint32_t palette_hash = hd_texture_hash_clut(
        query->vram, query->vram_word_count, query->clut_x, query->clut_y,
        query->depth);

    std::vector<uint64_t> candidate_serials;
    upload_index_collect(pack, wanted, &candidate_serials);

    struct Candidate {
        const Upload* upload;
        const EntryRecord* entry;
    };
    std::vector<Candidate> candidates;
    for (const uint64_t serial : candidate_serials) {
        const auto indexed = pack->upload_by_serial.find(serial);
        if (indexed == pack->upload_by_serial.end() || !indexed->second) continue;
        const Upload& upload = *indexed->second;
        if (!intersects(upload.bounds, want)) continue;
        const auto record = pack->entries.find(make_key(upload.hash, palette_hash));
        if (record == pack->entries.end()) continue;
        if (record->second.ambiguous) {
            hd_fused_record_fail(2, want.x, want.y, want.width, want.height);
            return 0;
        }
        if (static_cast<int>(candidates.size()) >= max_entries) {
            hd_fused_record_fail(3, want.x, want.y, want.width, want.height);
            return 0; /* too many distinct uploads: stay out of scope */
        }
        candidates.push_back({&upload, &record->second});
    }
    if (candidates.empty()) {
        hd_fused_record_fail(4, want.x, want.y, want.width, want.height);
        return 0;
    }

    /* upload_index_collect returns serials sorted ASCENDING (oldest first),
     * but VRAM semantics are last-write-wins: when two uploads' fragments
     * both still overlap the same pixels (a coarse broad-phase tile match,
     * or fragments an older upload kept after being partially overwritten),
     * the newest upload's content is what's actually in VRAM there. Sort
     * newest-first so it claims the overlap ahead of any older upload --
     * without this an older upload could steal pixels out from under a
     * newer, correct one, which read live as a misaligned patch (2026-09-08,
     * mouth region of a character face after enabling multi-entry fusion). */
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.upload->serial > b.upload->serial;
              });

    int count = 0;
    /* Each candidate's fragments claim whatever part of the still-uncovered
     * region they overlap, in order; whatever no candidate covers becomes
     * the native pieces at the end. Mirrors covered_by_upload's rectangle-
     * subtraction technique, generalized to multiple uploads in sequence. */
    std::vector<Rect> remaining{want};
    for (size_t ci = 0; ci < candidates.size() && !remaining.empty(); ++ci) {
        const Upload& upload = *candidates[ci].upload;
        std::vector<Rect> next_remaining;
        for (const Rect& region : remaining) {
            std::vector<Rect> uncovered{region};
            for (const Fragment& fragment : upload.fragments) {
                Rect covered;
                rect_intersection(region, fragment.rect, &covered);
                if (covered.width != 0 && covered.height != 0) {
                    if (count >= max_pieces) {
                        hd_fused_record_fail(5, want.x, want.y, want.width, want.height);
                        return 0;
                    }
                    out_pieces[count].x = static_cast<uint16_t>(covered.x);
                    out_pieces[count].y = static_cast<uint16_t>(covered.y);
                    out_pieces[count].width = static_cast<uint16_t>(covered.width);
                    out_pieces[count].height = static_cast<uint16_t>(covered.height);
                    out_pieces[count].has_hd = 1;
                    out_pieces[count].hd_entry_idx = static_cast<int>(ci);
                    out_pieces[count].hd_src_x = fragment.source_x + (covered.x - fragment.rect.x);
                    out_pieces[count].hd_src_y = fragment.source_y + (covered.y - fragment.rect.y);
                    ++count;
                }
                std::vector<Rect> next_uncovered;
                for (const Rect& u : uncovered) {
                    std::vector<Rect> remainder = subtract_rect(u, fragment.rect);
                    next_uncovered.insert(next_uncovered.end(), remainder.begin(), remainder.end());
                }
                uncovered.swap(next_uncovered);
                if (uncovered.empty()) break;
            }
            next_remaining.insert(next_remaining.end(), uncovered.begin(), uncovered.end());
        }
        remaining.swap(next_remaining);
    }
    /* Whatever no candidate's fragments covered: native pieces. */
    for (const Rect& piece : remaining) {
        if (piece.width == 0 || piece.height == 0) continue;
        if (count >= max_pieces) {
            hd_fused_record_fail(5, want.x, want.y, want.width, want.height);
            return 0;
        }
        out_pieces[count].x = static_cast<uint16_t>(piece.x);
        out_pieces[count].y = static_cast<uint16_t>(piece.y);
        out_pieces[count].width = static_cast<uint16_t>(piece.width);
        out_pieces[count].height = static_cast<uint16_t>(piece.height);
        out_pieces[count].has_hd = 0;
        out_pieces[count].hd_entry_idx = -1;
        out_pieces[count].hd_src_x = 0;
        out_pieces[count].hd_src_y = 0;
        ++count;
    }
    if (count < 2) {
        hd_fused_record_fail(6, want.x, want.y, want.width, want.height);
        return 0; /* nothing to fuse, or this fully resolves to plain native/HD (should not happen: the caller only reaches this after the plain matcher already failed) */
    }

    for (size_t ci = 0; ci < candidates.size(); ++ci) {
        fill_entry(*candidates[ci].entry, &out_entries[ci]);
        out_upload_width_words[ci] = candidates[ci].upload->width;
        out_upload_height[ci] = candidates[ci].upload->height;
    }
    if (out_entry_count) *out_entry_count = static_cast<int>(candidates.size());
    if (out_count) *out_count = count;

    {
        HdFusedDiagRingEntry& ring = g_hd_fused_diag_ring[g_hd_fused_diag_ring_pos];
        ring.qx = want.x; ring.qy = want.y;
        ring.qw = want.width; ring.qh = want.height;
        const int ecap = std::min<int>(static_cast<int>(candidates.size()), HD_FUSED_DIAG_MAX_ENTRIES);
        for (int ci = 0; ci < ecap; ci++) {
            HdFusedDiagEntry& e = ring.entries[ci];
            std::snprintf(e.path, sizeof(e.path), "%s", candidates[ci].entry->replacement_path.c_str());
            e.texhash = candidates[ci].upload->hash;
            e.palhash = palette_hash;
            e.serial = candidates[ci].upload->serial;
            e.upload_w = candidates[ci].upload->width;
            e.upload_h = candidates[ci].upload->height;
            e.upload_fragments = static_cast<unsigned>(candidates[ci].upload->fragments.size());
        }
        ring.entry_count = ecap;
        const int pcap = std::min(count, HD_FUSED_DIAG_MAX_PIECES);
        for (int pi = 0; pi < pcap; pi++) {
            HdFusedDiagPiece& p = ring.pieces[pi];
            p.x = out_pieces[pi].x; p.y = out_pieces[pi].y;
            p.width = out_pieces[pi].width; p.height = out_pieces[pi].height;
            p.has_hd = out_pieces[pi].has_hd;
            p.hd_entry_idx = out_pieces[pi].hd_entry_idx;
            p.hd_src_x = static_cast<unsigned>(out_pieces[pi].hd_src_x);
            p.hd_src_y = static_cast<unsigned>(out_pieces[pi].hd_src_y);
        }
        ring.piece_count = pcap;
        g_hd_fused_diag_ring_pos = (g_hd_fused_diag_ring_pos + 1) % HD_FUSED_DIAG_RING_CAP;
        ++g_hd_fused_diag_total;
    }
    return 1;
}

static void hd_diag_json_escape(const char* in, char* out, size_t out_cap); /* fwd, defined below */

int hd_texture_pack_diag_dump_fused_last(char* out, size_t out_capacity) {
    if (!out || out_capacity == 0) return 0;
    int pos = std::snprintf(out, out_capacity, "\"total\":%llu,\"recent\":[",
                            (unsigned long long)g_hd_fused_diag_total);
    const int n = g_hd_fused_diag_total < HD_FUSED_DIAG_RING_CAP
                      ? static_cast<int>(g_hd_fused_diag_total)
                      : HD_FUSED_DIAG_RING_CAP;
    const int start = g_hd_fused_diag_total < HD_FUSED_DIAG_RING_CAP
                          ? 0
                          : g_hd_fused_diag_ring_pos;
    for (int k = 0; k < n && pos < (int)out_capacity - 256; k++) {
        const HdFusedDiagRingEntry& r = g_hd_fused_diag_ring[(start + k) % HD_FUSED_DIAG_RING_CAP];
        pos += std::snprintf(out + pos, out_capacity - pos,
            "%s{\"query\":{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u},\"entries\":[",
            k ? "," : "", r.qx, r.qy, r.qw, r.qh);
        for (int i = 0; i < r.entry_count && pos < (int)out_capacity - 256; i++) {
            const HdFusedDiagEntry& e = r.entries[i];
            char escaped_path[512];
            hd_diag_json_escape(e.path, escaped_path, sizeof(escaped_path));
            pos += std::snprintf(out + pos, out_capacity - pos,
                "%s{\"path\":\"%s\",\"texhash\":%u,\"palhash\":%u,\"serial\":%llu,"
                "\"upload_w\":%u,\"upload_h\":%u,\"upload_fragments\":%u}",
                i ? "," : "", escaped_path, e.texhash, e.palhash, (unsigned long long)e.serial,
                e.upload_w, e.upload_h, e.upload_fragments);
        }
        pos += std::snprintf(out + pos, out_capacity - pos, "],\"pieces\":[");
        for (int i = 0; i < r.piece_count && pos < (int)out_capacity - 256; i++) {
            const HdFusedDiagPiece& p = r.pieces[i];
            pos += std::snprintf(out + pos, out_capacity - pos,
                "%s{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"has_hd\":%d,\"entry_idx\":%d,"
                "\"hd_src_x\":%u,\"hd_src_y\":%u}",
                i ? "," : "", p.x, p.y, p.width, p.height, p.has_hd, p.hd_entry_idx,
                p.hd_src_x, p.hd_src_y);
        }
        pos += std::snprintf(out + pos, out_capacity - pos, "]}");
    }
    pos += std::snprintf(out + pos, out_capacity - pos, "]");
    return 1;
}

int hd_texture_pack_diag_dump_fused_fails(char* out, size_t out_capacity) {
    if (!out || out_capacity == 0) return 0;
    int pos = std::snprintf(out, out_capacity,
        "\"total\":%llu,\"by_reason\":{"
        "\"uv_wrap\":%llu,\"ambiguous\":%llu,\"too_many_entries\":%llu,"
        "\"no_hash_match\":%llu,\"too_many_pieces\":%llu,\"nothing_to_fuse\":%llu},"
        "\"recent\":[",
        (unsigned long long)g_hd_fused_fail_total,
        (unsigned long long)g_hd_fused_fail_reason_counts[1],
        (unsigned long long)g_hd_fused_fail_reason_counts[2],
        (unsigned long long)g_hd_fused_fail_reason_counts[3],
        (unsigned long long)g_hd_fused_fail_reason_counts[4],
        (unsigned long long)g_hd_fused_fail_reason_counts[5],
        (unsigned long long)g_hd_fused_fail_reason_counts[6]);
    const int n = g_hd_fused_fail_total < HD_FUSED_FAIL_RING_CAP
                      ? static_cast<int>(g_hd_fused_fail_total)
                      : HD_FUSED_FAIL_RING_CAP;
    const int start = g_hd_fused_fail_total < HD_FUSED_FAIL_RING_CAP
                          ? 0
                          : g_hd_fused_fail_ring_pos;
    for (int k = 0; k < n && pos < (int)out_capacity - 128; k++) {
        const HdFusedFailEntry& e = g_hd_fused_fail_ring[(start + k) % HD_FUSED_FAIL_RING_CAP];
        pos += std::snprintf(out + pos, out_capacity - pos,
            "%s{\"reason\":%d,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
            k ? "," : "", e.reason, e.qx, e.qy, e.qw, e.qh);
    }
    pos += std::snprintf(out + pos, out_capacity - pos, "]");
    return 1;
}

/* C-callable wrapper around hd_texture_decode_query_rgba, for the native
 * pieces hd_texture_pack_match_fused reports (VRAM-absolute word/texel rect,
 * same coordinate space): decodes straight from query->vram, no upload/pack
 * lookup involved. out_rgba must be at least width*pixels_per_word*height*4
 * bytes (4bpp/8bpp/16bpp -> up to 4x/2x/1x width in texels; the caller in
 * gpu_gl_renderer.c already knows depth when sizing its buffer). */
int hd_texture_pack_decode_native_rgba(const HdTextureDrawQuery* query,
                                       uint16_t rect_x, uint16_t rect_y,
                                       uint16_t rect_w, uint16_t rect_h,
                                       uint8_t* out_rgba, uint32_t out_capacity,
                                       uint32_t* out_w, uint32_t* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!query || !out_rgba || query->depth > HD_TEXTURE_DEPTH_16BPP ||
        !query->vram || query->vram_word_count < kVramWords)
        return 0;
    const Rect rect{rect_x, rect_y, rect_w, rect_h};
    std::vector<uint8_t> rgba;
    unsigned w = 0, h = 0;
    hd_texture_decode_query_rgba(*query, rect, &rgba, &w, &h);
    if (w == 0 || h == 0 || rgba.size() > out_capacity) return 0;
    std::memcpy(out_rgba, rgba.data(), rgba.size());
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 1;
}

int hd_texture_pack_diag_dump_uploads(const HdTexturePack* pack, char* out,
                                      size_t out_capacity) {
    if (!out || out_capacity == 0) return 0;
    out[0] = '\0';
    if (!pack) return 0;
    size_t pos = 0;
    for (const Upload& upload : pack->uploads) {
        const int n = std::snprintf(
            out + pos, pos < out_capacity ? out_capacity - pos : 0,
            "%s{\"serial\":%llu,\"hash\":%u,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
            pos == 0 ? "" : ",",
            (unsigned long long)upload.serial, upload.hash,
            upload.bounds.x, upload.bounds.y, upload.bounds.width,
            upload.bounds.height);
        if (n < 0) break;
        pos += static_cast<size_t>(n);
        if (pos >= out_capacity) break;
    }
    return 1;
}

void hd_texture_pack_diag_font_region_stats(uint64_t out[5]) {
    out[0] = g_hd_pack_diag_font_hash_seen;
    out[1] = g_hd_pack_diag_font_region_touches;
    out[2] = g_hd_pack_diag_font_region_last_hash;
    out[3] = g_hd_pack_diag_font_region_last_serial;
    out[4] = 0;
}

/* Minimal JSON string escaping (backslash + quote only -- Windows paths are
 * the only untrusted-ish content here, and never contain control chars or
 * other JSON-special bytes in practice). */
static void hd_diag_json_escape(const char* in, char* out, size_t out_cap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 2 < out_cap; ++p) {
        if (*p == '\\' || *p == '"') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = '\0';
}

int hd_texture_pack_diag_dump_ring(char* out, size_t out_capacity) {
    if (!out || out_capacity == 0) return 0;
    out[0] = '\0';
    size_t pos = 0;
    const int n = g_hd_pack_diag_ring_total < HD_PACK_DIAG_RING_CAP
                      ? static_cast<int>(g_hd_pack_diag_ring_total)
                      : HD_PACK_DIAG_RING_CAP;
    /* Oldest-to-newest: if the ring hasn't wrapped yet, start at 0; once it
     * has, the oldest surviving entry is right where the next write will
     * land. */
    const int start = g_hd_pack_diag_ring_total < HD_PACK_DIAG_RING_CAP
                           ? 0
                           : g_hd_pack_diag_ring_pos;
    for (int k = 0; k < n; k++) {
        const HdPackDiagRingEntry& e = g_hd_pack_diag_ring[(start + k) % HD_PACK_DIAG_RING_CAP];
        char escaped_path[512];
        hd_diag_json_escape(e.path, escaped_path, sizeof(escaped_path));
        const int written = std::snprintf(
            out + pos, pos < out_capacity ? out_capacity - pos : 0,
            "%s{\"path\":\"%s\",\"texhash\":%u,\"palhash\":%u,"
            "\"qx\":%u,\"qy\":%u,\"qw\":%u,\"qh\":%u,\"serial\":%llu,"
            "\"upload_w\":%u,\"upload_h\":%u,\"upload_fragments\":%u,"
            "\"anchor_x\":%u,\"anchor_y\":%u,\"anchor_found\":%u,"
            "\"source_word_x\":%u,\"source_y\":%u}",
            pos == 0 ? "" : ",", escaped_path, e.texhash, e.palhash,
            e.qx, e.qy, e.qw, e.qh, (unsigned long long)e.upload_serial,
            e.upload_w, e.upload_h, e.upload_fragments,
            e.anchor_x, e.anchor_y, e.anchor_found,
            e.source_word_x, e.source_y);
        if (written < 0) break;
        pos += static_cast<size_t>(written);
        if (pos >= out_capacity) break;
    }
    return 1;
}

void hd_texture_pack_diag_last_match(char* out_path, size_t path_capacity,
                                     unsigned out_info[7]) {
    if (out_path && path_capacity)
        std::snprintf(out_path, path_capacity, "%s", g_hd_pack_diag_last_path);
    if (out_info) {
        out_info[0] = g_hd_pack_diag_last_texhash;
        out_info[1] = g_hd_pack_diag_last_palhash;
        out_info[2] = g_hd_pack_diag_last_qx;
        out_info[3] = g_hd_pack_diag_last_qy;
        out_info[4] = g_hd_pack_diag_last_qw;
        out_info[5] = g_hd_pack_diag_last_qh;
        out_info[6] = static_cast<unsigned>(g_hd_pack_diag_last_upload_serial);
    }
}

void hd_texture_pack_diag_stats(uint64_t out[3]) {
    out[0] = g_hd_pack_diag_no_candidates;
    out[1] = g_hd_pack_diag_no_hash_match;
    out[2] = g_hd_pack_diag_not_covered;
}

void hd_texture_pack_diag_first_rects(unsigned out[10]) {
    out[0] = g_hd_pack_diag_query_x;
    out[1] = g_hd_pack_diag_query_y;
    out[2] = g_hd_pack_diag_query_w;
    out[3] = g_hd_pack_diag_query_h;
    out[4] = (unsigned)g_hd_pack_diag_query_captured;
    out[5] = g_hd_pack_diag_upload_x;
    out[6] = g_hd_pack_diag_upload_y;
    out[7] = g_hd_pack_diag_upload_w;
    out[8] = g_hd_pack_diag_upload_h;
    out[9] = (unsigned)g_hd_pack_diag_upload_captured;
}

int hd_texture_pack_match_draw(HdTexturePack* pack,
                               uint16_t texpage,
                               uint16_t clut_x,
                               uint16_t clut_y,
                               uint8_t u_first,
                               uint8_t u_last,
                               uint8_t v_first,
                               uint8_t v_last,
                               const uint16_t* vram,
                               size_t vram_word_count,
                               HdTextureMatch* out_match) {
    const uint8_t depth = static_cast<uint8_t>((texpage >> 7) & 3u);
    if (depth > HD_TEXTURE_DEPTH_16BPP) {
        if (out_match) std::memset(out_match, 0, sizeof(*out_match));
        return HD_TEXTURE_LOOKUP_ERROR;
    }
    HdTextureDrawQuery query{};
    query.page_x = static_cast<uint16_t>((texpage & 0xFu) * 64u);
    query.page_y = static_cast<uint16_t>(((texpage >> 4) & 1u) * 256u);
    query.depth = depth;
    query.u_first = u_first; query.u_last = u_last;
    query.v_first = v_first; query.v_last = v_last;
    query.clut_x = clut_x; query.clut_y = clut_y;
    query.vram = vram; query.vram_word_count = vram_word_count;
    return hd_texture_pack_match(pack, &query, out_match);
}

/* Texpage-based convenience wrapper, same role as hd_texture_pack_match_draw
 * above but for the fused-page path -- keeps the texpage decode in one
 * place instead of duplicating it in gpu.c. */
int hd_texture_pack_match_fused_draw(HdTexturePack* pack,
                                     uint16_t texpage,
                                     uint16_t clut_x,
                                     uint16_t clut_y,
                                     uint8_t u_first,
                                     uint8_t u_last,
                                     uint8_t v_first,
                                     uint8_t v_last,
                                     const uint16_t* vram,
                                     size_t vram_word_count,
                                     HdTexturePackEntry* out_entries,
                                     uint16_t* out_upload_width_words,
                                     uint16_t* out_upload_height,
                                     int max_entries,
                                     int* out_entry_count,
                                     HdFusedPiece* out_pieces,
                                     int max_pieces,
                                     int* out_count,
                                     HdTextureDrawQuery* out_query) {
    const uint8_t depth = static_cast<uint8_t>((texpage >> 7) & 3u);
    if (out_count) *out_count = 0;
    if (out_entry_count) *out_entry_count = 0;
    if (depth > HD_TEXTURE_DEPTH_16BPP) return 0;
    HdTextureDrawQuery query{};
    query.page_x = static_cast<uint16_t>((texpage & 0xFu) * 64u);
    query.page_y = static_cast<uint16_t>(((texpage >> 4) & 1u) * 256u);
    query.depth = depth;
    query.u_first = u_first; query.u_last = u_last;
    query.v_first = v_first; query.v_last = v_last;
    query.clut_x = clut_x; query.clut_y = clut_y;
    query.vram = vram; query.vram_word_count = vram_word_count;
    if (out_query) *out_query = query; /* caller needs it again for decode_native_rgba */
    return hd_texture_pack_match_fused(pack, &query, out_entries, out_upload_width_words,
                                       out_upload_height, max_entries, out_entry_count,
                                       out_pieces, max_pieces, out_count);
}

void hd_texture_pack_set_decode_budget(HdTexturePack* pack,
                                       size_t budget_bytes) {
    if (!pack) return;
    std::lock_guard<std::mutex> lock(pack->decode.mutex);
    pack->decode.budget = budget_bytes;
    evict_decode_cache(pack->decode);
}

int hd_texture_pack_request_decode(HdTexturePack* pack,
                                   uint32_t texture_hash,
                                   uint32_t palette_hash) {
#ifdef HD_TEXTURE_PACK_DISABLE_PNG_DECODE
    (void)pack; (void)texture_hash; (void)palette_hash;
    return HD_TEXTURE_LOOKUP_ERROR;
#else
    if (!pack) return HD_TEXTURE_LOOKUP_ERROR;
    const uint64_t key = make_key(texture_hash, palette_hash);
    const auto record = pack->entries.find(key);
    if (record == pack->entries.end() || record->second.ambiguous)
        return HD_TEXTURE_LOOKUP_ERROR;

    std::lock_guard<std::mutex> lock(pack->decode.mutex);
    auto found = pack->decode.items.find(key);
    if (found != pack->decode.items.end()) {
        if (found->second.state == DecodeState::Ready) {
            found->second.last_used = ++pack->decode.tick;
            return HD_TEXTURE_LOOKUP_FOUND;
        }
        return found->second.state == DecodeState::Failed
            ? HD_TEXTURE_LOOKUP_ERROR : HD_TEXTURE_LOOKUP_NONE;
    }
    if (pack->decode.budget == 0) return HD_TEXTURE_LOOKUP_ERROR;
    if (pack->decode.queue.size() >= kMaxDecodeQueue)
        return HD_TEXTURE_LOOKUP_NONE;
    DecodeItem item;
    item.path = record->second.replacement_path;
    pack->decode.items.emplace(key, std::move(item));
    pack->decode.queue.push_back(key);
    if (!pack->decode.started) {
        pack->decode.started = true;
        pack->decode.worker = std::thread(decode_worker, &pack->decode);
    }
    pack->decode.wake.notify_one();
    return HD_TEXTURE_LOOKUP_NONE;
#endif
}

int hd_texture_pack_acquire_decoded(HdTexturePack* pack,
                                    uint32_t texture_hash,
                                    uint32_t palette_hash,
                                    HdTexturePixels* out_pixels) {
    if (out_pixels) std::memset(out_pixels, 0, sizeof(*out_pixels));
    if (!pack || !out_pixels) return HD_TEXTURE_LOOKUP_ERROR;
    std::lock_guard<std::mutex> lock(pack->decode.mutex);
    const auto found = pack->decode.items.find(make_key(texture_hash, palette_hash));
    if (found == pack->decode.items.end()) return HD_TEXTURE_LOOKUP_NONE;
    if (found->second.state == DecodeState::Failed) return HD_TEXTURE_LOOKUP_ERROR;
    if (found->second.state != DecodeState::Ready || !found->second.image)
        return HD_TEXTURE_LOOKUP_NONE;
    PixelLease* lease = new (std::nothrow) PixelLease{found->second.image};
    if (!lease) return HD_TEXTURE_LOOKUP_ERROR;
    found->second.last_used = ++pack->decode.tick;
    out_pixels->rgba = lease->image->rgba.data();
    out_pixels->width = lease->image->width;
    out_pixels->height = lease->image->height;
    out_pixels->stride = lease->image->width * 4;
    out_pixels->lease = lease;
    return HD_TEXTURE_LOOKUP_FOUND;
}

void hd_texture_pixels_release(HdTexturePixels* pixels) {
    if (!pixels) return;
    delete static_cast<PixelLease*>(pixels->lease);
    std::memset(pixels, 0, sizeof(*pixels));
}

} // extern "C"
