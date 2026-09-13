/* psx_cheats.c - classic GameShark code interpreter + enabled-list
 * persistence. See psx_cheats.h for the instruction semantics this
 * implements and why the address needs no further translation. */

#include "psx_cheats.h"

#include "mod_plugins.h"

#include <stdio.h>
#include <string.h>

#define PSX_CHEATS_MAX (256)

static char s_config_path[1024];
static unsigned char s_enabled[PSX_CHEATS_MAX];

static int clamp_index(int index, int count) {
    if (index < 0 || index >= count) return -1;
    return index;
}

int psx_cheats_count(void) {
    int count = 0;
    psx_cheats_db(&count);
    return count;
}

const PsxCheatDef *psx_cheats_get(int index) {
    int count = 0;
    const PsxCheatDef *db = psx_cheats_db(&count);
    if (clamp_index(index, count) < 0) return NULL;
    return &db[index];
}

int psx_cheats_is_enabled(int index) {
    int count = psx_cheats_count();
    if (clamp_index(index, count) < 0 || index >= PSX_CHEATS_MAX) return 0;
    return s_enabled[index] ? 1 : 0;
}

static int find_index_by_id(const char *id) {
    int count = 0, i;
    const PsxCheatDef *db = psx_cheats_db(&count);
    if (!id || !id[0]) return -1;
    for (i = 0; i < count; i++) {
        if (strcmp(db[i].id, id) == 0) return i;
    }
    return -1;
}

static void save_enabled_list(void) {
    FILE *f;
    int count, i;
    if (!s_config_path[0]) return;
    f = fopen(s_config_path, "wb");
    if (!f) return;
    count = psx_cheats_count();
    for (i = 0; i < count && i < PSX_CHEATS_MAX; i++) {
        if (!s_enabled[i]) continue;
        fprintf(f, "%s\n", psx_cheats_get(i)->id);
    }
    fclose(f);
}

static void load_enabled_list(void) {
    FILE *f;
    char line[128];
    memset(s_enabled, 0, sizeof(s_enabled));
    if (!s_config_path[0]) return;
    f = fopen(s_config_path, "rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        int idx;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!line[0]) continue;
        idx = find_index_by_id(line);
        if (idx >= 0 && idx < PSX_CHEATS_MAX) s_enabled[idx] = 1;
    }
    fclose(f);
}

void psx_cheats_configure(const char *config_path) {
    s_config_path[0] = '\0';
    if (config_path && config_path[0])
        snprintf(s_config_path, sizeof(s_config_path), "%s", config_path);
    load_enabled_list();
}

void psx_cheats_set_enabled(int index, int enabled) {
    int count = psx_cheats_count();
    if (clamp_index(index, count) < 0 || index >= PSX_CHEATS_MAX) return;
    s_enabled[index] = enabled ? 1 : 0;
    save_enabled_list();
}

/* Returns the index, in `lines`, of the instruction right after the
 * conditional chain starting at `from` plus the one non-conditional
 * instruction that chain guards (itself 2 lines if it's a 0x50 slide) --
 * i.e. where execution resumes when a conditional in the chain is false.
 * See psx_cheats.h's file comment for why this mirrors DuckStation's
 * GetNextNonConditionalInstruction. */
static int is_conditional_code(uint32_t code) {
    return (code >= 0xD0 && code <= 0xD3) || (code >= 0xE0 && code <= 0xE3);
}

static int skip_guarded_block(const PsxCheatLine *lines, int count, int from) {
    int i = from;
    uint32_t code;
    while (i < count && is_conditional_code((lines[i][0] >> 24) & 0xFFu))
        i++;
    if (i >= count) return i;
    code = (lines[i][0] >> 24) & 0xFFu;
    i++;
    if (code == 0x50) i++; /* slide consumes an extra base-instruction line */
    return i;
}

static void apply_slide(const PsxCheatLine *lines, int count, int i) {
    uint32_t w1, w2, slide_count, addr_inc, base_w1, base_w2, base_code,
        base_addr, base_val;
    uint16_t val_inc;
    uint32_t n;
    if (i + 1 >= count) return; /* malformed: no base instruction follows */
    w1 = lines[i][0];
    w2 = lines[i][1];
    slide_count = (w1 >> 8) & 0xFFu;
    addr_inc = w1 & 0xFFu;
    val_inc = (uint16_t)(w2 & 0xFFFFu);
    base_w1 = lines[i + 1][0];
    base_w2 = lines[i + 1][1];
    base_code = (base_w1 >> 24) & 0xFFu;
    base_addr = base_w1 & 0x00FFFFFFu;
    base_val = base_w2;
    for (n = 0; n < slide_count; n++) {
        uint32_t addr = base_addr + n * addr_inc;
        uint32_t value = base_val + n * (uint32_t)val_inc;
        if (base_code == 0x30)
            psx_mod_write_byte(addr, (uint8_t)(value & 0xFFu));
        else if (base_code == 0x80)
            psx_mod_write_half(addr, (uint16_t)(value & 0xFFFFu));
        else
            psx_mod_write_word(addr, value);
    }
}

static void apply_lines(const PsxCheatLine *lines, int count) {
    int i = 0;
    while (i < count) {
        uint32_t w1 = lines[i][0];
        uint32_t w2 = lines[i][1];
        uint32_t code = (w1 >> 24) & 0xFFu;
        uint32_t addr = w1 & 0x00FFFFFFu;

        switch (code) {
        case 0x30: /* ConstantWrite8 */
            psx_mod_write_byte(addr, (uint8_t)(w2 & 0xFFu));
            i++;
            break;
        case 0x80: /* ConstantWrite16 */
            psx_mod_write_half(addr, (uint16_t)(w2 & 0xFFFFu));
            i++;
            break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            uint16_t cur = psx_mod_read_half(addr);
            uint16_t val = (uint16_t)(w2 & 0xFFFFu);
            int cond = code == 0xD0 ? (cur == val)
                     : code == 0xD1 ? (cur != val)
                     : code == 0xD2 ? (cur < val)
                                    : (cur > val);
            i = cond ? i + 1 : skip_guarded_block(lines, count, i + 1);
            break;
        }
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
            uint8_t cur = psx_mod_read_byte(addr);
            uint8_t val = (uint8_t)(w2 & 0xFFu);
            int cond = code == 0xE0 ? (cur == val)
                     : code == 0xE1 ? (cur != val)
                     : code == 0xE2 ? (cur < val)
                                    : (cur > val);
            i = cond ? i + 1 : skip_guarded_block(lines, count, i + 1);
            break;
        }
        case 0x50: /* Slide/repeater: consumes this line + the base line */
            apply_slide(lines, count, i);
            i += 2;
            break;
        default:
            /* Unsupported instruction type -- not used by any published
             * Vagrant Story code; skip rather than misinterpret. */
            i++;
            break;
        }
    }
}

void psx_cheats_apply_all(void) {
    int count = psx_cheats_count();
    int i;
    for (i = 0; i < count && i < PSX_CHEATS_MAX; i++) {
        const PsxCheatDef *def;
        if (!s_enabled[i]) continue;
        def = psx_cheats_get(i);
        if (!def || !def->lines || def->line_count <= 0) continue;
        apply_lines(def->lines, def->line_count);
    }
}
