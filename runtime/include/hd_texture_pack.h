#ifndef PSXRECOMP_HD_TEXTURE_PACK_H
#define PSXRECOMP_HD_TEXTURE_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* External Beetle-style texture-pack loader and VRAM upload tracker.
 * The pack payload is never copied into or owned by the executable. */
typedef struct HdTexturePack HdTexturePack;

enum HdTextureDepth {
    HD_TEXTURE_DEPTH_4BPP = 0,
    HD_TEXTURE_DEPTH_8BPP = 1,
    HD_TEXTURE_DEPTH_16BPP = 2
};

enum HdTextureLookupStatus {
    HD_TEXTURE_LOOKUP_ERROR = -1,
    HD_TEXTURE_LOOKUP_NONE = 0,
    HD_TEXTURE_LOOKUP_FOUND = 1,
    HD_TEXTURE_LOOKUP_AMBIGUOUS = 2
};

typedef struct HdTexturePackInfo {
    const char* asset_root;       /* valid until hd_texture_pack_destroy */
    const char* replacement_root; /* valid until hd_texture_pack_destroy */
    size_t replacement_file_count;
    size_t unique_key_count;
    size_t ambiguous_key_count;
    size_t logical_mapping_count;
} HdTexturePackInfo;

typedef struct HdTexturePackEntry {
    uint32_t texture_hash;
    uint32_t palette_hash;
    const char* replacement_path; /* valid until hd_texture_pack_destroy */
    const char* logical_path;     /* empty when Hashes.ini has no mapping */
} HdTexturePackEntry;

/* A UV interval is inclusive. first > last means that the 8-bit texture
 * coordinate wraps through 255 -> 0. page_x/page_y are VRAM word coordinates.
 * vram must contain the complete 1024x512 native PSX VRAM image. */
typedef struct HdTextureDrawQuery {
    uint16_t page_x;
    uint16_t page_y;
    uint8_t depth; /* enum HdTextureDepth */
    uint8_t u_first;
    uint8_t u_last;
    uint8_t v_first;
    uint8_t v_last;
    uint16_t clut_x;
    uint16_t clut_y;
    const uint16_t* vram;
    size_t vram_word_count;
} HdTextureDrawQuery;

typedef struct HdTextureMatch {
    HdTexturePackEntry entry;
    uint64_t upload_serial;
    uint16_t upload_width_words;
    uint16_t upload_height;
    uint16_t source_word_x; /* first queried word within the original upload */
    uint16_t source_y;
} HdTextureMatch;

/* explicit_root wins. When it is NULL/empty, PSXRECOMP_HD_TEXTURE_ROOT is
 * consulted. Accepts either:
 *   root/Hashes.ini + exactly one <name>-texture-replacements directory, or
 *   the replacement directory directly (Hashes.ini is read from it or parent).
 * Returns 1 on success and leaves *out_pack NULL on failure. */
int hd_texture_pack_create(const char* explicit_root,
                           HdTexturePack** out_pack,
                           char* error,
                           size_t error_capacity);
void hd_texture_pack_destroy(HdTexturePack* pack);
void hd_texture_pack_get_info(const HdTexturePack* pack,
                              HdTexturePackInfo* out_info);

/* Indexed access over every loaded entry (0..hd_texture_pack_entry_count-1,
 * arbitrary but stable within a pack's lifetime), for a boot-time preload
 * pass that decodes/uploads every replacement PNG up front instead of the
 * first time gameplay actually draws it -- see gpu_gl_renderer.c's
 * gpu_hd_texture_preload_active. Returns 0 for an out-of-range index or a
 * null pack. */
size_t hd_texture_pack_entry_count(const HdTexturePack* pack);
int hd_texture_pack_get_entry(const HdTexturePack* pack, size_t index,
                              HdTexturePackEntry* out_entry);

/* Key-only lookup. Ambiguous numeric aliases (for example 1-2.png and
 * 00000001-2.png) deliberately return AMBIGUOUS instead of selecting by
 * directory enumeration order. */
int hd_texture_pack_lookup(const HdTexturePack* pack,
                           uint32_t texture_hash,
                           uint32_t palette_hash,
                           HdTexturePackEntry* out_entry);

/* Standard reflected IEEE CRC-32 (polynomial 0xEDB88320, initial/final XOR
 * 0xFFFFFFFF), feeding each uint16_t explicitly low byte then high byte. */
uint32_t hd_texture_crc32_words_le(const uint16_t* words, size_t word_count);

/* Hashes the complete live CLUT: 16 entries for 4bpp or 256 for 8bpp. CLUT X
 * wraps at 1024 and Y wraps at 512. 16bpp has no palette and returns zero. */
uint32_t hd_texture_hash_clut(const uint16_t* vram,
                              size_t vram_word_count,
                              uint16_t clut_x,
                              uint16_t clut_y,
                              uint8_t depth);

/* Upload bytes are row-major logical upload words, independent of destination
 * wrapping. Tracking invalidates/splits every intersected older residency.
 * Width <= 1024 and height <= 512; X/Y may wrap. */
int hd_texture_pack_track_upload(HdTexturePack* pack,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width_words,
                                 uint16_t height,
                                 const uint16_t* words,
                                 size_t word_count,
                                 uint32_t* out_texture_hash);
void hd_texture_pack_invalidate(HdTexturePack* pack,
                                uint16_t x,
                                uint16_t y,
                                uint16_t width_words,
                                uint16_t height);
void hd_texture_pack_reset_tracking(HdTexturePack* pack);

/* Beetle-style upload residency is host metadata, but it is required for an
 * HD replacement to keep matching after a savestate restores the same VRAM.
 * The portable little-endian state below records only upload identity and the
 * surviving residency fragments; decoded images and GL objects remain caches.
 * save allocates with malloc (caller frees). A NULL pack serializes a valid
 * empty tracker so states remain renderer-independent. check is side-effect
 * free; load has strong replacement semantics and also accepts a NULL pack
 * after validating the wire. */
int hd_texture_pack_tracking_state_save(const HdTexturePack* pack,
                                        uint8_t** out_data,
                                        size_t* out_size);
int hd_texture_pack_tracking_state_check(const uint8_t* data,
                                         size_t size);
int hd_texture_pack_tracking_state_load(HdTexturePack* pack,
                                        const uint8_t* data,
                                        size_t size);
size_t hd_texture_pack_tracking_upload_count(const HdTexturePack* pack);

/* Matches only when one uniquely-keyed tracked upload still contains every
 * word touched by the texture page/depth/UV query. Any pack-key or residency
 * ambiguity falls back explicitly via HD_TEXTURE_LOOKUP_AMBIGUOUS. */
int hd_texture_pack_match(HdTexturePack* pack,
                          const HdTextureDrawQuery* query,
                          HdTextureMatch* out_match);

/* Temporary stage-by-stage match diagnostics (see hd_texture_pack.cpp's
 * comment above hd_texture_pack_match): out[] = {no_candidates (broad-phase
 * spatial index found no overlapping upload at all), no_hash_match (had
 * candidates, but none had this exact (upload_hash, palette_hash) key in the
 * pack), not_covered (a key matched, but covered_by_upload rejected full
 * coverage)}. Global counters, not per-pack -- fine for single-pack-instance
 * diagnosis, not meant to ship long-term. */
void hd_texture_pack_diag_stats(uint64_t out[3]);
/* Raw (x,y,width,height) of the first query rect and first tracked upload's
 * bounds, in the same word/row units both are supposed to share -- read this
 * directly when the aggregate counts above point at a coordinate-space
 * mismatch between tracking and matching. out[] = {query_x, query_y,
 * query_w, query_h, query_captured, upload_x, upload_y, upload_w, upload_h,
 * upload_captured}; a *_captured of 0 means neither has happened yet. */
void hd_texture_pack_diag_first_rects(unsigned out[10]);
/* Rolling snapshot of the most recent successful match (overwritten every
 * match, not just the first), so the exact replacement file backing a
 * suspect on-screen texture can be identified while it is still visible.
 * out_info[] = {texture_hash, palette_hash, query_x, query_y, query_w,
 * query_h, upload_serial}. */
void hd_texture_pack_diag_last_match(char* out_path, size_t path_capacity,
                                     unsigned out_info[7]);
/* Dumps the last ~24 successful matches (oldest to newest) as a JSON array
 * body of {path,texhash,palhash,qx,qy,qw,qh,serial} objects -- a snapshot of
 * the SET of replacement images a single on-screen frame is drawing from
 * right now, not just whichever draw happened to run last. See
 * hd_texture_pack_diag_last_match's header comment for why "last match
 * only" wasn't enough to see a frame that mixes correct and wrong images
 * across its own draws. */
int hd_texture_pack_diag_dump_ring(char* out, size_t out_capacity);
/* 2026-09-07 investigation: out[] = {font_hash_seen (track_upload calls
 * whose hash was the known-good font atlas 0xF98B3634), region_touches
 * (track_upload calls intersecting the shared VRAM word-region
 * (320,0,64,48) where a stale boot-time upload keeps winning every match),
 * region_last_hash, region_last_serial, reserved}. */
void hd_texture_pack_diag_font_region_stats(uint64_t out[5]);
/* Dumps every currently-tracked upload as a JSON array body (no brackets) of
 * {serial,hash,x,y,w,h} objects into out. Only meant for a small number of
 * uploads (this session's whole-run investigation never exceeded a few
 * dozen); truncates silently if out_capacity is too small. Returns 0 only
 * for a null pack/out. */
int hd_texture_pack_diag_dump_uploads(const HdTexturePack* pack, char* out,
                                      size_t out_capacity);

/* 2026-09-08 multi-entry-candidate investigation: ring of the last ~64
 * successful hd_texture_pack_match_fused calls -- which candidate upload(s)
 * each picked and the exact pieces it resolved. hd_texture_pack_diag_dump_ring
 * above only records PLAIN (non-fused) matches, so it says nothing about an
 * animated (mouth/eye "gif") region that only ever resolves through the
 * fused path. A ring (not just the last call) matters here because the fused
 * path also handles small, frequent UI elements (dialogue-box glyphs, HUD
 * icons) -- the single most recent call is as likely to be one of those as
 * the specific character draw being investigated. Writes a JSON object BODY
 * (no surrounding braces) with "total"/"recent" (array of
 * {query,entries,pieces}) keys into out; truncates silently if out_capacity
 * is too small. Returns 0 only for a null/zero-capacity out. */
int hd_texture_pack_diag_dump_fused_last(char* out, size_t out_capacity);

/* Ring of the last ~32 hd_texture_pack_match_fused FAILURES with a reason
 * code (1=UV-wrapped/empty query, 2=ambiguous pack entry, 3=more than
 * max_entries distinct candidate uploads, 4=no candidate's hash matched any
 * pack entry, 5=more pieces than max_pieces, 6=fewer than 2 pieces), plus
 * cumulative per-reason counts -- see hd_texture_pack_match_fused's
 * g_hd_fused_fail_ring comment (hd_texture_pack.cpp) for why this exists:
 * the success ring alone can't tell you why a SPECIFIC query never shows up
 * in it. Writes a JSON object BODY (no braces) with
 * "total"/"by_reason"/"recent" keys into out; truncates silently if
 * out_capacity is too small. */
int hd_texture_pack_diag_dump_fused_fails(char* out, size_t out_capacity);

/* Fused-page support (opt-in, see gpu_hd_texture_fusion_set): a piece of a
 * partially-HD-covered query, in VRAM-absolute word(x)/texel(y) coordinates
 * -- the same space HdTextureDrawQuery's page_x/page_y + u/v use. has_hd
 * selects which of the two ways to fill it: the shared replacement PNG at
 * (hd_src_x, hd_src_y) within THAT PNG's own texel space (its width/height
 * come back in out_upload_width_words[hd_entry_idx]/out_upload_height[
 * hd_entry_idx]), or a hd_texture_pack_decode_native_rgba() call using this
 * rect directly. hd_entry_idx indexes out_entries[] -- pieces from
 * DIFFERENT uploads/entries are common (see hd_texture_pack_match_fused's
 * comment) and each keeps its own entry, not a single shared one. */
typedef struct HdFusedPiece {
    uint16_t x, y, width, height;
    int has_hd;
    uint32_t hd_src_x, hd_src_y;
    int hd_entry_idx; /* valid when has_hd; index into out_entries[]/out_upload_*[] */
} HdFusedPiece;

/* See hd_texture_pack.cpp's comment above the implementation for the exact
 * scope (one wanted rect -- no UV wrap -- and up to max_entries distinct
 * partially-covering uploads, each contributing whichever of its own
 * fragments overlap the query; the FIRST candidate's fragments claim
 * territory first, later candidates only cover what's left uncovered, so
 * two candidates' fragments overlapping the same pixels never double-count).
 * Anything outside that scope (UV wrap, more than max_entries distinct
 * uploads, an ambiguous entry, more pieces than max_pieces) returns 0 so the
 * caller keeps today's single-shot hd_texture_pack_match/
 * hd_texture_pack_match_draw behavior unchanged. On success (1),
 * *out_count pieces are written to out_pieces (capacity max_pieces) and
 * *out_entry_count entries are written to out_entries/out_upload_width_words/
 * out_upload_height (capacity max_entries), one per distinct replacement PNG
 * referenced by at least one has_hd piece. */
int hd_texture_pack_match_fused(HdTexturePack* pack,
                                const HdTextureDrawQuery* query,
                                HdTexturePackEntry* out_entries,
                                uint16_t* out_upload_width_words,
                                uint16_t* out_upload_height,
                                int max_entries,
                                int* out_entry_count,
                                HdFusedPiece* out_pieces,
                                int max_pieces,
                                int* out_count);

/* Texpage-based convenience wrapper, same role as hd_texture_pack_match_draw
 * for the fused-page path -- keeps the texpage decode in one place instead
 * of duplicating it in gpu.c. out_query receives the HdTextureDrawQuery this
 * built (page_x/page_y/depth/clut/vram) so the caller can pass it straight
 * to hd_texture_pack_decode_native_rgba for each has_hd==0 piece without
 * re-deriving page_x/page_y/depth from texpage itself. */
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
                                     HdTextureDrawQuery* out_query);

/* Decodes native VRAM content for an arbitrary VRAM-absolute rect (an
 * HdFusedPiece with has_hd == 0, or any other word/texel rect in the same
 * space) straight from query->vram -- no upload/pack lookup involved.
 * out_rgba must be at least rect_w*pixels_per_word(query->depth)*rect_h*4
 * bytes; returns 0 (leaving out_rgba untouched) if out_capacity is too
 * small. out_w/out_h report the actual decoded size in texels. */
int hd_texture_pack_decode_native_rgba(const HdTextureDrawQuery* query,
                                       uint16_t rect_x, uint16_t rect_y,
                                       uint16_t rect_w, uint16_t rect_h,
                                       uint8_t* out_rgba, uint32_t out_capacity,
                                       uint32_t* out_w, uint32_t* out_h);

/* Live GP0 draw adapter: derives page_x/page_y/depth from the PS1 texpage word
 * and applies the same deterministic residency/CLUT/UV match above. This keeps
 * the renderer's texpage interpretation in the focused unit-test surface. */
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
                               HdTextureMatch* out_match);

/* Optional, bounded, single-worker on-demand PNG decode cache. The request is
 * asynchronous and low-priority on Windows. request_decode returns FOUND when
 * already ready, NONE when queued/in flight, and ERROR for an absent,
 * ambiguous, failed, or over-budget image. acquire_decoded returns FOUND only
 * when ready. A lease keeps pixels alive across cache eviction and pack
 * destruction; always release it with hd_texture_pixels_release. */
typedef struct HdTexturePixels {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void* lease;
} HdTexturePixels;

void hd_texture_pack_set_decode_budget(HdTexturePack* pack,
                                       size_t budget_bytes);
int hd_texture_pack_request_decode(HdTexturePack* pack,
                                   uint32_t texture_hash,
                                   uint32_t palette_hash);
int hd_texture_pack_acquire_decoded(HdTexturePack* pack,
                                    uint32_t texture_hash,
                                    uint32_t palette_hash,
                                    HdTexturePixels* out_pixels);
void hd_texture_pixels_release(HdTexturePixels* pixels);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_HD_TEXTURE_PACK_H */
