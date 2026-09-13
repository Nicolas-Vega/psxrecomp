#ifndef PSX_CHEATS_H
#define PSX_CHEATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Classic PS1 GameShark code interpreter, scoped to the five instruction
 * types every code in Vagrant Story's published cheat list actually uses
 * (verified against DuckStation's src/core/cheats.cpp, the ground truth for
 * real-hardware GameShark semantics):
 *
 *   0x30 AAAAAA VVVV  - ConstantWrite8:  ram[addr]   = value & 0xFF
 *   0x80 AAAAAA VVVV  - ConstantWrite16: ram[addr:2] = value & 0xFFFF
 *   0xD0/D1/D2/D3     - Compare16 Equal/NotEqual/Less/Greater: if
 *                       ram[addr:2] compares true against value, execute the
 *                       next line; otherwise skip the rest of this AND-chain
 *                       of conditionals plus the one instruction they guard
 *                       (which may itself be a 2-line 0x50 slide).
 *   0xE0/E1/E2/E3     - Compare8 variants of the above, 8-bit width.
 *   0x50 NNAA VVVV    - Slide/repeater: repeats the FOLLOWING line NN times,
 *                       incrementing its address by AA and its value by VVVV
 *                       (truncated to the following line's write width) each
 *                       repeat. Used by the "give all items" style codes.
 *
 * A code's low 24 bits address PS1 RAM directly with no further masking:
 * PS1 RAM is physically mapped at address 0 and mirrored across
 * KUSEG/KSEG0/KSEG1, and every published code's 24-bit address already sits
 * below the 2MB (0x200000) RAM size, so the leading type byte (0x30/0x80/
 * 0xD0/...) is pure type discriminator, never part of the address -- exactly
 * how DuckStation itself decodes and applies these instructions with no
 * extra address translation.
 */

/* One [address_word, value_word] GameShark instruction line, exactly as
 * printed in a cheat list (e.g. {0x8006006E, 0x03E7}). */
typedef uint32_t PsxCheatLine[2];

typedef struct PsxCheatDef {
    const char *id;       /* stable key for persistence, e.g. "general.max_hp" */
    const char *category; /* display group, e.g. "GENERAL" */
    const char *name;      /* display name, e.g. "Max HP" */
    const PsxCheatLine *lines;
    int line_count;
} PsxCheatDef;

/* Built-in Vagrant Story GameShark database (psx_cheats_db.c), sourced from
 * the game's published cheat code list. Returns the array and, if count is
 * non-NULL, its length. */
const PsxCheatDef *psx_cheats_db(int *count);

/* Load/save the enabled-cheat list from a small sidecar text file (one
 * PsxCheatDef.id per line). Pass NULL/empty to keep enabled state in memory
 * only (nothing loads or persists) -- mirrors savestate_configure's
 * caller-supplies-the-path convention rather than resolving a path itself. */
void psx_cheats_configure(const char *config_path);

int psx_cheats_count(void);
const PsxCheatDef *psx_cheats_get(int index);
int psx_cheats_is_enabled(int index);
/* Toggling persists immediately (rewrites the whole sidecar file) so an
 * enabled cheat survives a crash or Alt+F4, not just a clean exit. */
void psx_cheats_set_enabled(int index, int enabled);

/* Re-applies every enabled cheat's writes. Call exactly once per guest
 * VBlank (see gpu.c, right beside mod_runtime_on_vblank()) so continuous
 * "always max HP"-style codes get re-poked every frame the same way real
 * GameShark hardware does, independent of host presentation/turbo/pacing. */
void psx_cheats_apply_all(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CHEATS_H */
