#ifndef PSX_WS_CULL_DETECT_H
#define PSX_WS_CULL_DETECT_H
/* ============================================================================
 * Generic widescreen SCREEN-EXTENT CULL detector ([widescreen.cull]
 * auto_screen_x)
 *
 * Self-contained (depends only on <stdint.h>) so ONE implementation is shared
 * verbatim by the recompiler (C++) and the runtime interpreter
 * (C) — the widen must be identical on every execution path or the backends
 * diverge (same contract as ws_backdrop_detect.h).
 *
 * WHAT IT FINDS
 * -------------
 * A GTE render funnel trivially rejects a primitive whose projected vertices
 * all fall outside the screen. Titles express the horizontal reject three ways
 * (immediates are per-game: Tomba tests X < 0x140/0x141 on a 320 display, Ape
 * Escape tests X < 0x181 on its 368 display — configured via
 * [widescreen.cull] screen_w_imms / screen_h_imms):
 *
 *  (1) UNSIGNED PER-VERTEX (Tomba, Ape sprite path):
 *          sltiu v, SX, W          ; SX loaded lhu / masked andi 0xFFFF
 *      One compare covers both edges (off-left SX wraps to a large unsigned).
 *      Widened by psx_ws_cull_sltiu(): window [-m, W+m).
 *
 *  (2) SIGNED MIN/MAX (Ape tri/quad funnel):
 *          bltz maxSX, reject      ; all verts left of 0
 *          _slti v, minSX, W       ; (delay slot) all verts right of W-1
 *      The right bound widens via psx_ws_cull_slti() (W+m); the LEFT bound is
 *      the bare bltz, widened via psx_ws_cull_bltz() (reject only when
 *      maxSX < -m). The X pair is discriminated from the identically-shaped Y
 *      pair by the delay-slot immediate (W-set vs H-set).
 *
 *  (3) SIGNED CENTER±HALFWIDTH (Ape billboard funnel):
 *          addu t, cx, r
 *          bltz t, reject          ; cx + r < 0   (fully left)
 *          _<any>
 *          subu t, cx, r
 *          slti v, t, W            ; cx - r >= W  (fully right)
 *      The bltz is recognised by the addu immediately before it whose (rs,rt)
 *      pair reappears in a subu feeding a W-immediate slti a few words later.
 *
 * A function/window QUALIFIES only when it contains BOTH a width compare
 * (slti/sltiu, imm in the W set) and a height compare (imm in the H set) — the
 * screen-extent signature. A lone width-immediate compare elsewhere stays
 * vanilla.
 *
 * All widen helpers are identity at margin 0 (4:3 / boot / menu / FMV), so the
 * transformed code is bit-for-bit the vanilla verdict outside native-wide.
 * ========================================================================== */
#include <stdint.h>

static inline int psx_ws_cull_imm_in(uint32_t imm, const uint32_t *set, int n) {
    for (int i = 0; i < n; i++)
        if (set[i] == imm) return 1;
    return 0;
}

/* is words[i] a slti (0x0A) or sltiu (0x0B) with an immediate in `set`? */
static inline int psx_ws_cull_is_slt_imm(uint32_t w, const uint32_t *set, int n) {
    uint32_t op = w >> 26;
    if (op != 0x0Au && op != 0x0Bu) return 0;
    return psx_ws_cull_imm_in(w & 0xFFFFu, set, n);
}

/* Screen-extent signature over an instruction window: at least one W compare
 * AND one H compare (slti or sltiu). Pure — identical verdicts on the
 * recompiler's full-function scan and the runtime's ±window scan. */
static inline int psx_ws_cull_scan(const uint32_t *words, int n,
                                   const uint32_t *w_imms, int nw,
                                   const uint32_t *h_imms, int nh) {
    int has_x = 0, has_y = 0;
    for (int i = 0; i < n; i++) {
        uint32_t w = words[i];
        uint32_t op = w >> 26;
        if (op != 0x0Au && op != 0x0Bu) continue;
        uint32_t im = w & 0xFFFFu;
        if (psx_ws_cull_imm_in(im, w_imms, nw)) has_x = 1;
        else if (psx_ws_cull_imm_in(im, h_imms, nh)) has_y = 1;
        if (has_x && has_y) return 1;
    }
    return 0;
}

/* Classify words[idx] as an X LEFT-EDGE reject bltz (idiom 2 or 3 above).
 * Caller must already have qualified the surrounding window via
 * psx_ws_cull_scan. Returns 1 iff the bltz belongs to a width reject. */
static inline int psx_ws_cull_bltz_here(const uint32_t *words, int n, int idx,
                                        const uint32_t *w_imms, int nw) {
    if (idx < 0 || idx >= n) return 0;
    uint32_t w = words[idx];
    if ((w >> 26) != 0x01u || ((w >> 16) & 0x1Fu) != 0x00u) return 0;  /* bltz only */
    uint32_t bltz_rs = (w >> 21) & 0x1Fu;

    /* Idiom 2: the width slti/sltiu sits in the bltz's delay slot. (The Y pair
     * has the same shape with an H immediate — excluded by the W-set test.) */
    if (idx + 1 < n && psx_ws_cull_is_slt_imm(words[idx + 1], w_imms, nw))
        return 1;

    /* Idiom 3: addu t,x,y immediately before; a subu with the same (x,y) pair
     * within the next few words feeds a W-immediate slti/sltiu right after. */
    if (idx >= 1) {
        uint32_t p = words[idx - 1];
        if ((p >> 26) == 0x00u && (p & 0x3Fu) == 0x21u &&          /* addu */
            ((p >> 11) & 0x1Fu) == bltz_rs) {                       /* rd feeds bltz */
            uint32_t ax = (p >> 21) & 0x1Fu, ay = (p >> 16) & 0x1Fu;
            for (int j = idx + 1; j <= idx + 4 && j < n; j++) {
                uint32_t s = words[j];
                if ((s >> 26) == 0x00u && (s & 0x3Fu) == 0x23u &&   /* subu */
                    ((s >> 21) & 0x1Fu) == ax && ((s >> 16) & 0x1Fu) == ay) {
                    for (int k = j + 1; k <= j + 2 && k < n; k++)
                        if (psx_ws_cull_is_slt_imm(words[k], w_imms, nw))
                            return 1;
                }
            }
        }
    }
    return 0;
}

/* Classify words[idx] as an X LOWER-EDGE ("< 0") companion of a LATER W-bound
 * compare on the SAME register within the window (idiom 4, found on Vagrant
 * Story): rather than the min/max-plus-bltz shapes idioms 2/3 assume, this
 * title splits the on-screen test into two independent per-axis SLTI compares
 * per vertex slot -- `slti v, sx, 0` (is this coordinate negative) followed
 * later by `slti v, sx, W` (is it past the right edge), same source register,
 * same vertex slot, reused for the Y axis with an H compare right before it in
 * the exact same shape (`slti v, sy, 0` before `slti v, sy, H`). Recognising
 * the bare zero-immediate compare on its own would be far too broad -- it is
 * an extremely common idiom for unrelated sign tests -- so the discriminator
 * is pairing it to a LATER width-bound compare on the IDENTICAL register
 * within the qualifying window. A title whose zero-compare register instead
 * reappears against an H immediate is the Y companion and must NOT widen
 * (native-wide only widens horizontally); this function only ever matches the
 * W-paired (X) case. */
static inline int psx_ws_cull_slti_zero_here(const uint32_t *words, int n, int idx,
                                           const uint32_t *w_imms, int nw) {
    if (idx < 0 || idx >= n) return 0;
    uint32_t w = words[idx];
    if ((w >> 26) != 0x0Au) return 0;      /* slti only (signed compare) */
    if ((w & 0xFFFFu) != 0u) return 0;     /* zero immediate only */
    uint32_t rs = (w >> 21) & 0x1Fu;
    if (rs == 0u) return 0;                /* $zero can't hold a coordinate */
    for (int j = idx + 1; j < n && j <= idx + 48; j++) {
        uint32_t s = words[j];
        if ((s >> 26) != 0x0Au) continue;
        if (((s >> 21) & 0x1Fu) != rs) continue;
        if (psx_ws_cull_imm_in(s & 0xFFFFu, w_imms, nw)) return 1;
        break; /* first later slti on this register decides it; don't skip past it */
    }
    return 0;
}

#endif /* PSX_WS_CULL_DETECT_H */
