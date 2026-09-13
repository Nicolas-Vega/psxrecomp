/* psx_cheats_db.c - built-in Vagrant Story GameShark cheat database.
 *
 * Transcribed from the game's published cheat code list (Cheat Code
 * Central's "Vagrant Story Cheats & Secrets for PlayStation"). Every code
 * uses only the five instruction types psx_cheats.c's interpreter
 * implements (0x30, 0x80, 0xD0-D3, 0xE0-E3, 0x50) -- see psx_cheats.h for
 * their semantics.
 *
 * Each cheat gets its own small static line array plus one CHEAT() row so a
 * transcription mistake is easy to spot and fix in isolation rather than
 * hunting through one giant flattened table.
 */

#include "psx_cheats.h"

#include <stddef.h>

#define GENERAL "GENERAL"
#define BATTLE_ABILITY "BATTLE ABILITY"
#define DEFENSE_ABILITY "DEFENSE ABILITY"
#define BA_DAGGER "BREAK ART: DAGGER"
#define BA_SWORD "BREAK ART: SWORD"
#define BA_GREAT_SWORD "BREAK ART: GREAT SWORD"
#define BA_AXE_MACE "BREAK ART: AXE & MACE"
#define BA_GREAT_AXE "BREAK ART: GREAT AXE"
#define BA_STAFF "BREAK ART: STAFF"
#define BA_HEAVY_MACE "BREAK ART: HEAVY MACE"
#define BA_POLEARM "BREAK ART: POLEARM"
#define BA_CROSSBOW "BREAK ART: CROSSBOW"
#define BA_UNARMED "BREAK ART: UNARMED"
#define CHARACTER "CHARACTER"
#define TELEPORT "TELEPORT LOCATIONS"

/* ---- General ---- */
static const PsxCheatLine k_general_max_hp[] = {{0x8006006E, 0x03E7}};
static const PsxCheatLine k_general_max_mp[] = {{0x80060072, 0x03E7}};
static const PsxCheatLine k_general_risk_zero[] = {{0x3011FA60, 0x0000}};
static const PsxCheatLine k_general_body_excellent[] = {{0x3012006C, 0x00C8}};
static const PsxCheatLine k_general_right_arm_excellent[] = {{0x3011FEB4, 0x00C8}};
static const PsxCheatLine k_general_left_arm_excellent[] = {{0x3011FDD8, 0x00C8}};
static const PsxCheatLine k_general_head_excellent[] = {{0x3011FF90, 0x00C8}};
static const PsxCheatLine k_general_legs_excellent[] = {{0x30120148, 0x00C8}};
static const PsxCheatLine k_general_teleport_spell[] = {{0x8006164C, 0x0105}};
static const PsxCheatLine k_general_all_battle_abilities_combo[] = {{0x80060062, 0xFFFF}};
static const PsxCheatLine k_general_playtime_zero[] = {
    {0xD004267E, 0x0062}, {0x80042682, 0xACA0}};
static const PsxCheatLine k_general_speed_mode[] = {
    {0xD001F8BC, 0xFFE9}, {0x8001FE8C, 0x0001}};
static const PsxCheatLine k_general_all_normal_key_items[] = {
    {0x50002004, 0x0001}, {0x80060F68, 0x0145},
    {0x50002004, 0x0000}, {0x80060F6A, 0x0064},
    {0x50002004, 0x0001}, {0x80060FE8, 0x01CA},
    {0x50002004, 0x0000}, {0x80060FEA, 0x0001}};
static const PsxCheatLine k_general_all_misc_items[] = {
    {0xD00F4920, 0x1000}, {0x5000A704, 0x0001}, {0x801F5580, 0x0143},
    {0xD00F4920, 0x1000}, {0x5000A704, 0x0000}, {0x801F5582, 0x0064}};
static const PsxCheatLine k_general_moon_jump[] = {
    {0xE005E1C0, 0x0082}, {0x301203AE, 0x00DA},
    {0xE01203AE, 0x00C0}, {0x301203AE, 0x0000}};
static const PsxCheatLine k_general_save_anywhere[] = {
    {0xD0109FBC, 0x4E6B}, {0x80109FC6, 0x2400}};
static const PsxCheatLine k_general_infinite_bonus_time[] = {
    {0x8005046C, 0x6320}, {0x8005046E, 0x0063}};
static const PsxCheatLine k_general_start_in_final_area[] = {
    {0x800F1AB0, 0x0419}, {0x800F1AB6, 0x1C0C}};
static const PsxCheatLine k_general_quick_ending[] = {
    {0xD005E1C0, 0x0005}, {0x800F1A48, 0x0002},
    {0xD005E1C0, 0x0005}, {0x800F1AB0, 0x003C}};
static const PsxCheatLine k_general_warp_final_boss[] = {
    {0xD005E1C0, 0x000A}, {0x800F1A48, 0x0002},
    {0xD005E1C0, 0x000A}, {0x800F1AB0, 0x011B}};
static const PsxCheatLine k_general_all_magic[] = {
    {0x50003334, 0x0000}, {0x8004C4E0, 0x9000},
    {0x50001B34, 0x0000}, {0x8004CF70, 0x8000}};

/* ---- Battle Abilities ---- */
static const PsxCheatLine k_ba_have_all[] = {
    {0x50002034, 0x0000}, {0x8004BE60, 0x9000}};
static const PsxCheatLine k_ba_heavy_shot[] = {{0x8004BE60, 0x9000}};
static const PsxCheatLine k_ba_gain_life[] = {{0x8004BE94, 0x9000}};
static const PsxCheatLine k_ba_mind_assault[] = {{0x8004BEC8, 0x9000}};
static const PsxCheatLine k_ba_gain_magic[] = {{0x8004BEFC, 0x9000}};
static const PsxCheatLine k_ba_raging_ache[] = {{0x8004BF30, 0x9000}};
static const PsxCheatLine k_ba_mind_ache[] = {{0x8004BF64, 0x9000}};
static const PsxCheatLine k_ba_temper[] = {{0x8004BF98, 0x9000}};
static const PsxCheatLine k_ba_crimson_pain[] = {{0x8004BFCC, 0x9000}};
static const PsxCheatLine k_ba_instill[] = {{0x8004C000, 0x9000}};
static const PsxCheatLine k_ba_phantom_pain[] = {{0x8004C034, 0x9000}};
static const PsxCheatLine k_ba_paralysis_pulse[] = {{0x8004C068, 0x9000}};
static const PsxCheatLine k_ba_numbing_claw[] = {{0x8004C0D0, 0x9000}};
static const PsxCheatLine k_ba_dulling_impact[] = {{0x8004C104, 0x9000}};
static const PsxCheatLine k_ba_snake_venom[] = {{0x8004C16C, 0x9000}};

/* ---- Defense Abilities ---- */
static const PsxCheatLine k_da_ward[] = {{0x8004C208, 0x9000}};
static const PsxCheatLine k_da_siphon_soul[] = {{0x8004C23C, 0x9000}};
static const PsxCheatLine k_da_reflect_magic[] = {{0x8004C270, 0x9000}};
static const PsxCheatLine k_da_reflect_damage[] = {{0x8004C2A4, 0x9000}};
static const PsxCheatLine k_da_absorb_magic[] = {{0x8004C2D8, 0x9000}};
static const PsxCheatLine k_da_absorb_damage[] = {{0x8004C30C, 0x9000}};
static const PsxCheatLine k_da_impact_guard[] = {{0x8004C340, 0x9000}};
static const PsxCheatLine k_da_wind_break[] = {{0x8004C374, 0x9000}};
static const PsxCheatLine k_da_fire_proof[] = {{0x8004C3A8, 0x9000}};
static const PsxCheatLine k_da_terra_ward[] = {{0x8004C3DC, 0x9000}};
static const PsxCheatLine k_da_aqua_ward[] = {{0x8004C410, 0x9000}};
static const PsxCheatLine k_da_shadow_guard[] = {{0x8004C444, 0x9000}};
static const PsxCheatLine k_da_demonscale[] = {{0x8004C478, 0x9000}};
static const PsxCheatLine k_da_phantom_shield[] = {{0x8004C4AC, 0x9000}};

/* ---- Break Arts: Dagger ---- */
static const PsxCheatLine k_dagger_whistle_sting[] = {{0x8004DF48, 0xC000}};
static const PsxCheatLine k_dagger_shadoweave[] = {{0x8004DF7C, 0xC000}};
static const PsxCheatLine k_dagger_double_fang[] = {{0x8004DFB0, 0xC000}};
static const PsxCheatLine k_dagger_wyrm_scorn[] = {{0x8004DFE4, 0xC000}};

/* ---- Break Arts: Sword ---- */
static const PsxCheatLine k_sword_rending_gale[] = {{0x8004E018, 0xC000}};
static const PsxCheatLine k_sword_vile_scar[] = {{0x8004E04C, 0xC000}};
static const PsxCheatLine k_sword_cherry_ronde[] = {{0x8004E080, 0xC000}};
static const PsxCheatLine k_sword_papillon_reel[] = {{0x8004E0B4, 0xC000}};

/* ---- Break Arts: Great Sword ---- */
static const PsxCheatLine k_gsword_sunder[] = {{0x8004E0E8, 0xC000}};
static const PsxCheatLine k_gsword_thunderweave[] = {{0x8004E11C, 0xC000}};
static const PsxCheatLine k_gsword_swallow_slash[] = {{0x8004E150, 0xC000}};
static const PsxCheatLine k_gsword_advent_sign[] = {{0x8004E184, 0xC000}};

/* ---- Break Arts: Axe and Mace ---- */
static const PsxCheatLine k_axemace_mistral_edge[] = {{0x8004E1B8, 0xC000}};
static const PsxCheatLine k_axemace_glacial_gale[] = {{0x8004E1EC, 0xC000}};
static const PsxCheatLine k_axemace_killer_mantis[] = {{0x8004E220, 0xC000}};
static const PsxCheatLine k_axemace_black_nebula[] = {{0x8004E254, 0xC000}};

/* ---- Break Arts: Great Axe ---- */
static const PsxCheatLine k_gaxe_bear_claw[] = {{0x8004E288, 0xC000}};
static const PsxCheatLine k_gaxe_accursed_umbra[] = {{0x8004E2BC, 0xC000}};
static const PsxCheatLine k_gaxe_iron_ripper[] = {{0x8004E2F0, 0xC000}};
static const PsxCheatLine k_gaxe_emetic_bomb[] = {{0x8004E324, 0xC000}};

/* ---- Break Arts: Staff ---- */
static const PsxCheatLine k_staff_sirocco[] = {{0x8004E358, 0xC000}};
static const PsxCheatLine k_staff_riskbreak[] = {{0x8004E38C, 0xC000}};
static const PsxCheatLine k_staff_gravis_aether[] = {{0x8004E3C0, 0xC000}};
static const PsxCheatLine k_staff_trinity_pulse[] = {{0x8004E3F4, 0xC000}};

/* ---- Break Arts: Heavy Mace ---- */
static const PsxCheatLine k_hmace_bonecrusher[] = {{0x8004E428, 0xC000}};
static const PsxCheatLine k_hmace_quickshock[] = {{0x8004E45C, 0xC000}};
static const PsxCheatLine k_hmace_ignis_wheel[] = {{0x8004E490, 0xC000}};
static const PsxCheatLine k_hmace_hex_flux[] = {{0x8004E4C4, 0xC000}};

/* ---- Break Arts: Polearm ---- */
static const PsxCheatLine k_polearm_ruination[] = {{0x8004E4F8, 0xC000}};
static const PsxCheatLine k_polearm_scythe_wind[] = {{0x8004E52C, 0xC000}};
static const PsxCheatLine k_polearm_giga_tempest[] = {{0x8004E560, 0xC000}};
static const PsxCheatLine k_polearm_spiral_scourge[] = {{0x8004E594, 0xC000}};

/* ---- Break Arts: Crossbow ---- */
static const PsxCheatLine k_crossbow_brimstone_hail[] = {{0x8004E5C8, 0xC000}};
static const PsxCheatLine k_crossbow_heavens_scorn[] = {{0x8004E5FC, 0xC000}};
static const PsxCheatLine k_crossbow_death_wail[] = {{0x8004E630, 0xC000}};
static const PsxCheatLine k_crossbow_sanctus_flare[] = {{0x8004E664, 0xC000}};

/* ---- Break Arts: Unarmed ---- */
static const PsxCheatLine k_unarmed_lotus_palm[] = {{0x8004E698, 0xC000}};
static const PsxCheatLine k_unarmed_vertigo[] = {{0x8004E6CC, 0xC000}};
static const PsxCheatLine k_unarmed_vermillion_aura[] = {{0x8004E700, 0xC000}};
static const PsxCheatLine k_unarmed_retribution[] = {{0x8004E734, 0xC000}};

/* ---- Character ---- */
static const PsxCheatLine k_char_infinite_hp[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA58, 0x03E7}, {0x8006006C, 0x03E7}};
static const PsxCheatLine k_char_infinite_mp[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA5C, 0x03E7}, {0x80060070, 0x03E7}};
static const PsxCheatLine k_char_quicken_x2[] = {
    {0xE011FA73, 0x0012}, {0x3011FA73, 0x001C}};
static const PsxCheatLine k_char_fast_speed[] = {
    {0xE011FA73, 0x0012}, {0x3011FA73, 0x007F}};
static const PsxCheatLine k_char_hold_circle_999_damage[] = {
    {0xD005E1C0, 0x0020}, {0x801FBC84, 0x03E7}};
static const PsxCheatLine k_char_max_strength[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA64, 0x03E7}};
static const PsxCheatLine k_char_super_strength[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA62, 0x2690}};
static const PsxCheatLine k_char_max_intelligence[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA68, 0x03E7}};
static const PsxCheatLine k_char_super_intelligence[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA66, 0x2690}};
static const PsxCheatLine k_char_max_agility[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA6C, 0x03E7}};
static const PsxCheatLine k_char_super_agility[] = {
    {0xD011FA2E, 0x8011}, {0x8011FA6A, 0x2690}};

/* ---- Teleportation ---- */
static const PsxCheatLine k_tp_all_locations[] = {
    {0x50002401, 0x0000}, {0x300616EE, 0x0001}};
static const PsxCheatLine k_tp_workers_breakroom[] = {{0x300616EE, 0x0001}};
static const PsxCheatLine k_tp_wine_guild_hall[] = {{0x300616EF, 0x0001}};
static const PsxCheatLine k_tp_black_market[] = {{0x300616F0, 0x0001}};
static const PsxCheatLine k_tp_hall_of_revenge[] = {{0x300616F1, 0x0001}};
static const PsxCheatLine k_tp_withered_spring[] = {{0x300616F2, 0x0001}};
static const PsxCheatLine k_tp_work_of_art_workshop[] = {{0x300616F3, 0x0001}};
static const PsxCheatLine k_tp_advent_ground[] = {{0x300616F4, 0x0001}};
static const PsxCheatLine k_tp_rue_vermillion[] = {{0x300616F5, 0x0001}};
static const PsxCheatLine k_tp_magic_hammer_workshop[] = {{0x300616F6, 0x0001}};
static const PsxCheatLine k_tp_the_crossing[] = {{0x300616F7, 0x0001}};
static const PsxCheatLine k_tp_dark_tunnel[] = {{0x300616F8, 0x0001}};
static const PsxCheatLine k_tp_rue_bouquet[] = {{0x300616F9, 0x0001}};
static const PsxCheatLine k_tp_sunless_way[] = {{0x300616FA, 0x0001}};
static const PsxCheatLine k_tp_faerie_circle[] = {{0x300616FB, 0x0001}};
static const PsxCheatLine k_tp_forest_river[] = {{0x300616FC, 0x0001}};
static const PsxCheatLine k_tp_wood_gate[] = {{0x300616FD, 0x0001}};
static const PsxCheatLine k_tp_valdiman_gates[] = {{0x300616FE, 0x0001}};
static const PsxCheatLine k_tp_warriors_rest[] = {{0x300616FF, 0x0001}};
static const PsxCheatLine k_tp_keanes_workshop[] = {{0x30061700, 0x0001}};
static const PsxCheatLine k_tp_sinners_corner[] = {{0x30061701, 0x0001}};
static const PsxCheatLine k_tp_crumbling_market[] = {{0x30061702, 0x0001}};
static const PsxCheatLine k_tp_treaty_room[] = {{0x30061703, 0x0001}};
static const PsxCheatLine k_tp_bandits_hollow[] = {{0x30061704, 0x0001}};
static const PsxCheatLine k_tp_ore_road[] = {{0x30061705, 0x0001}};
static const PsxCheatLine k_tp_auction_block[] = {{0x30061706, 0x0001}};
static const PsxCheatLine k_tp_way_down[] = {{0x30061707, 0x0001}};
static const PsxCheatLine k_tp_rue_lejour[] = {{0x30061708, 0x0001}};
static const PsxCheatLine k_tp_kesch_bridge[] = {{0x30061709, 0x0001}};
static const PsxCheatLine k_tp_metal_works_workshop[] = {{0x3006170A, 0x0001}};
static const PsxCheatLine k_tp_junction_point_workshop[] = {{0x3006170B, 0x0001}};
static const PsxCheatLine k_tp_dark_coast[] = {{0x3006170C, 0x0001}};
static const PsxCheatLine k_tp_plateia_lumitar[] = {{0x3006170D, 0x0001}};
static const PsxCheatLine k_tp_sin_and_punishment[] = {{0x3006170E, 0x0001}};
static const PsxCheatLine k_tp_the_atrium[] = {{0x3006170F, 0x0001}};
static const PsxCheatLine k_tp_gods_hands_workshop[] = {{0x30061710, 0x0001}};

#define CHEAT(cheat_id, cat, display_name, arr) \
    { cheat_id, cat, display_name, arr, (int)(sizeof(arr) / sizeof((arr)[0])) }

static const PsxCheatDef k_cheats[] = {
    /* General */
    CHEAT("general.max_hp", GENERAL, "Max HP", k_general_max_hp),
    CHEAT("general.max_mp", GENERAL, "Max MP", k_general_max_mp),
    CHEAT("general.risk_zero", GENERAL, "Risk Always at Zero", k_general_risk_zero),
    CHEAT("general.body_excellent", GENERAL, "Body Always at Excellent", k_general_body_excellent),
    CHEAT("general.right_arm_excellent", GENERAL, "Right Arm Always at Excellent", k_general_right_arm_excellent),
    CHEAT("general.left_arm_excellent", GENERAL, "Left Arm Always at Excellent", k_general_left_arm_excellent),
    CHEAT("general.head_excellent", GENERAL, "Head Always at Excellent", k_general_head_excellent),
    CHEAT("general.legs_excellent", GENERAL, "Legs Always at Excellent", k_general_legs_excellent),
    CHEAT("general.teleport_spell", GENERAL, "Activate Teleport Spell", k_general_teleport_spell),
    CHEAT("general.all_battle_abilities_combo", GENERAL, "Learn All Battle Abilities in One Combo", k_general_all_battle_abilities_combo),
    CHEAT("general.playtime_zero", GENERAL, "Play Time Always 00:00:00", k_general_playtime_zero),
    CHEAT("general.speed_mode", GENERAL, "Speed Mode", k_general_speed_mode),
    CHEAT("general.all_normal_key_items", GENERAL, "All Normal & Key Items", k_general_all_normal_key_items),
    CHEAT("general.all_misc_items", GENERAL, "All Misc. Items in Container", k_general_all_misc_items),
    CHEAT("general.moon_jump", GENERAL, "Moon Jump (R2 + Square)", k_general_moon_jump),
    CHEAT("general.save_anywhere", GENERAL, "Save Anywhere", k_general_save_anywhere),
    CHEAT("general.infinite_bonus_time", GENERAL, "Infinite Bonus Time", k_general_infinite_bonus_time),
    CHEAT("general.start_in_final_area", GENERAL, "Start Game in Final Area", k_general_start_in_final_area),
    CHEAT("general.quick_ending", GENERAL, "Quick Ending (L1 + L2)", k_general_quick_ending),
    CHEAT("general.warp_final_boss", GENERAL, "Warp to Final Boss (R1 + R2)", k_general_warp_final_boss),
    CHEAT("general.all_magic", GENERAL, "Have All Magic", k_general_all_magic),

    /* Battle Abilities */
    CHEAT("battle.have_all", BATTLE_ABILITY, "Have All Battle Abilities", k_ba_have_all),
    CHEAT("battle.heavy_shot", BATTLE_ABILITY, "Heavy Shot", k_ba_heavy_shot),
    CHEAT("battle.gain_life", BATTLE_ABILITY, "Gain Life", k_ba_gain_life),
    CHEAT("battle.mind_assault", BATTLE_ABILITY, "Mind Assault", k_ba_mind_assault),
    CHEAT("battle.gain_magic", BATTLE_ABILITY, "Gain Magic", k_ba_gain_magic),
    CHEAT("battle.raging_ache", BATTLE_ABILITY, "Raging Ache", k_ba_raging_ache),
    CHEAT("battle.mind_ache", BATTLE_ABILITY, "Mind Ache", k_ba_mind_ache),
    CHEAT("battle.temper", BATTLE_ABILITY, "Temper", k_ba_temper),
    CHEAT("battle.crimson_pain", BATTLE_ABILITY, "Crimson Pain", k_ba_crimson_pain),
    CHEAT("battle.instill", BATTLE_ABILITY, "Instill", k_ba_instill),
    CHEAT("battle.phantom_pain", BATTLE_ABILITY, "Phantom Pain", k_ba_phantom_pain),
    CHEAT("battle.paralysis_pulse", BATTLE_ABILITY, "Paralysis Pulse", k_ba_paralysis_pulse),
    CHEAT("battle.numbing_claw", BATTLE_ABILITY, "Numbing Claw", k_ba_numbing_claw),
    CHEAT("battle.dulling_impact", BATTLE_ABILITY, "Dulling Impact", k_ba_dulling_impact),
    CHEAT("battle.snake_venom", BATTLE_ABILITY, "Snake Venom", k_ba_snake_venom),

    /* Defense Abilities */
    CHEAT("defense.ward", DEFENSE_ABILITY, "Ward", k_da_ward),
    CHEAT("defense.siphon_soul", DEFENSE_ABILITY, "Siphon Soul", k_da_siphon_soul),
    CHEAT("defense.reflect_magic", DEFENSE_ABILITY, "Reflect Magic", k_da_reflect_magic),
    CHEAT("defense.reflect_damage", DEFENSE_ABILITY, "Reflect Damage", k_da_reflect_damage),
    CHEAT("defense.absorb_magic", DEFENSE_ABILITY, "Absorb Magic", k_da_absorb_magic),
    CHEAT("defense.absorb_damage", DEFENSE_ABILITY, "Absorb Damage", k_da_absorb_damage),
    CHEAT("defense.impact_guard", DEFENSE_ABILITY, "Impact Guard", k_da_impact_guard),
    CHEAT("defense.wind_break", DEFENSE_ABILITY, "Wind Break", k_da_wind_break),
    CHEAT("defense.fire_proof", DEFENSE_ABILITY, "Fire Proof", k_da_fire_proof),
    CHEAT("defense.terra_ward", DEFENSE_ABILITY, "Terra Ward", k_da_terra_ward),
    CHEAT("defense.aqua_ward", DEFENSE_ABILITY, "Aqua Ward", k_da_aqua_ward),
    CHEAT("defense.shadow_guard", DEFENSE_ABILITY, "Shadow Guard", k_da_shadow_guard),
    CHEAT("defense.demonscale", DEFENSE_ABILITY, "Demonscale", k_da_demonscale),
    CHEAT("defense.phantom_shield", DEFENSE_ABILITY, "Phantom Shield", k_da_phantom_shield),

    /* Break Arts: Dagger */
    CHEAT("break.dagger.whistle_sting", BA_DAGGER, "Whistle Sting", k_dagger_whistle_sting),
    CHEAT("break.dagger.shadoweave", BA_DAGGER, "Shadoweave", k_dagger_shadoweave),
    CHEAT("break.dagger.double_fang", BA_DAGGER, "Double Fang", k_dagger_double_fang),
    CHEAT("break.dagger.wyrm_scorn", BA_DAGGER, "Wyrm Scorn", k_dagger_wyrm_scorn),

    /* Break Arts: Sword */
    CHEAT("break.sword.rending_gale", BA_SWORD, "Rending Gale", k_sword_rending_gale),
    CHEAT("break.sword.vile_scar", BA_SWORD, "Vile Scar", k_sword_vile_scar),
    CHEAT("break.sword.cherry_ronde", BA_SWORD, "Cherry Ronde", k_sword_cherry_ronde),
    CHEAT("break.sword.papillon_reel", BA_SWORD, "Papillon Reel", k_sword_papillon_reel),

    /* Break Arts: Great Sword */
    CHEAT("break.gsword.sunder", BA_GREAT_SWORD, "Sunder", k_gsword_sunder),
    CHEAT("break.gsword.thunderweave", BA_GREAT_SWORD, "Thunderweave", k_gsword_thunderweave),
    CHEAT("break.gsword.swallow_slash", BA_GREAT_SWORD, "Swallow Slash", k_gsword_swallow_slash),
    CHEAT("break.gsword.advent_sign", BA_GREAT_SWORD, "Advent Sign", k_gsword_advent_sign),

    /* Break Arts: Axe and Mace */
    CHEAT("break.axemace.mistral_edge", BA_AXE_MACE, "Mistral Edge", k_axemace_mistral_edge),
    CHEAT("break.axemace.glacial_gale", BA_AXE_MACE, "Glacial Gale", k_axemace_glacial_gale),
    CHEAT("break.axemace.killer_mantis", BA_AXE_MACE, "Killer Mantis", k_axemace_killer_mantis),
    CHEAT("break.axemace.black_nebula", BA_AXE_MACE, "Black Nebula", k_axemace_black_nebula),

    /* Break Arts: Great Axe */
    CHEAT("break.gaxe.bear_claw", BA_GREAT_AXE, "Bear Claw", k_gaxe_bear_claw),
    CHEAT("break.gaxe.accursed_umbra", BA_GREAT_AXE, "Accursed Umbra", k_gaxe_accursed_umbra),
    CHEAT("break.gaxe.iron_ripper", BA_GREAT_AXE, "Iron Ripper", k_gaxe_iron_ripper),
    CHEAT("break.gaxe.emetic_bomb", BA_GREAT_AXE, "Emetic Bomb", k_gaxe_emetic_bomb),

    /* Break Arts: Staff */
    CHEAT("break.staff.sirocco", BA_STAFF, "Sirocco", k_staff_sirocco),
    CHEAT("break.staff.riskbreak", BA_STAFF, "Riskbreak", k_staff_riskbreak),
    CHEAT("break.staff.gravis_aether", BA_STAFF, "Gravis Aether", k_staff_gravis_aether),
    CHEAT("break.staff.trinity_pulse", BA_STAFF, "Trinity Pulse", k_staff_trinity_pulse),

    /* Break Arts: Heavy Mace */
    CHEAT("break.hmace.bonecrusher", BA_HEAVY_MACE, "Bonecrusher", k_hmace_bonecrusher),
    CHEAT("break.hmace.quickshock", BA_HEAVY_MACE, "Quickshock", k_hmace_quickshock),
    CHEAT("break.hmace.ignis_wheel", BA_HEAVY_MACE, "Ignis Wheel", k_hmace_ignis_wheel),
    CHEAT("break.hmace.hex_flux", BA_HEAVY_MACE, "Hex Flux", k_hmace_hex_flux),

    /* Break Arts: Polearm */
    CHEAT("break.polearm.ruination", BA_POLEARM, "Ruination", k_polearm_ruination),
    CHEAT("break.polearm.scythe_wind", BA_POLEARM, "Scythe Wind", k_polearm_scythe_wind),
    CHEAT("break.polearm.giga_tempest", BA_POLEARM, "Giga Tempest", k_polearm_giga_tempest),
    CHEAT("break.polearm.spiral_scourge", BA_POLEARM, "Spiral Scourge", k_polearm_spiral_scourge),

    /* Break Arts: Crossbow */
    CHEAT("break.crossbow.brimstone_hail", BA_CROSSBOW, "Brimstone Hail", k_crossbow_brimstone_hail),
    CHEAT("break.crossbow.heavens_scorn", BA_CROSSBOW, "Heaven's Scorn", k_crossbow_heavens_scorn),
    CHEAT("break.crossbow.death_wail", BA_CROSSBOW, "Death Wail", k_crossbow_death_wail),
    CHEAT("break.crossbow.sanctus_flare", BA_CROSSBOW, "Sanctus Flare", k_crossbow_sanctus_flare),

    /* Break Arts: Unarmed */
    CHEAT("break.unarmed.lotus_palm", BA_UNARMED, "Lotus Palm", k_unarmed_lotus_palm),
    CHEAT("break.unarmed.vertigo", BA_UNARMED, "Vertigo", k_unarmed_vertigo),
    CHEAT("break.unarmed.vermillion_aura", BA_UNARMED, "Vermillion Aura", k_unarmed_vermillion_aura),
    CHEAT("break.unarmed.retribution", BA_UNARMED, "Retribution", k_unarmed_retribution),

    /* Character */
    CHEAT("character.infinite_hp", CHARACTER, "Infinite HP", k_char_infinite_hp),
    CHEAT("character.infinite_mp", CHARACTER, "Infinite MP", k_char_infinite_mp),
    CHEAT("character.quicken_x2", CHARACTER, "Quicken x2 Permanent", k_char_quicken_x2),
    CHEAT("character.fast_speed", CHARACTER, "Fast Speed", k_char_fast_speed),
    CHEAT("character.hold_circle_999_damage", CHARACTER, "Hold Circle While Attacking for 999 Damage", k_char_hold_circle_999_damage),
    CHEAT("character.max_strength", CHARACTER, "Max Strength", k_char_max_strength),
    CHEAT("character.super_strength", CHARACTER, "Super Strength", k_char_super_strength),
    CHEAT("character.max_intelligence", CHARACTER, "Max Intelligence", k_char_max_intelligence),
    CHEAT("character.super_intelligence", CHARACTER, "Super Intelligence", k_char_super_intelligence),
    CHEAT("character.max_agility", CHARACTER, "Max Agility", k_char_max_agility),
    CHEAT("character.super_agility", CHARACTER, "Super Agility", k_char_super_agility),

    /* Teleportation */
    CHEAT("teleport.all_locations", TELEPORT, "All Locations Unlocked", k_tp_all_locations),
    CHEAT("teleport.workers_breakroom", TELEPORT, "Worker's Breakroom", k_tp_workers_breakroom),
    CHEAT("teleport.wine_guild_hall", TELEPORT, "Wine Guild Hall", k_tp_wine_guild_hall),
    CHEAT("teleport.black_market", TELEPORT, "Black Market", k_tp_black_market),
    CHEAT("teleport.hall_of_revenge", TELEPORT, "Hall of Revenge", k_tp_hall_of_revenge),
    CHEAT("teleport.withered_spring", TELEPORT, "The Withered Spring", k_tp_withered_spring),
    CHEAT("teleport.work_of_art_workshop", TELEPORT, "Work of Art Workshop", k_tp_work_of_art_workshop),
    CHEAT("teleport.advent_ground", TELEPORT, "Advent Ground", k_tp_advent_ground),
    CHEAT("teleport.rue_vermillion", TELEPORT, "Rue Vermillion", k_tp_rue_vermillion),
    CHEAT("teleport.magic_hammer_workshop", TELEPORT, "Magic Hammer Workshop", k_tp_magic_hammer_workshop),
    CHEAT("teleport.the_crossing", TELEPORT, "The Crossing", k_tp_the_crossing),
    CHEAT("teleport.dark_tunnel", TELEPORT, "The Dark Tunnel", k_tp_dark_tunnel),
    CHEAT("teleport.rue_bouquet", TELEPORT, "Rue Bouquet", k_tp_rue_bouquet),
    CHEAT("teleport.sunless_way", TELEPORT, "The Sunless Way", k_tp_sunless_way),
    CHEAT("teleport.faerie_circle", TELEPORT, "The Faerie Circle", k_tp_faerie_circle),
    CHEAT("teleport.forest_river", TELEPORT, "Forest River", k_tp_forest_river),
    CHEAT("teleport.wood_gate", TELEPORT, "The Wood Gate", k_tp_wood_gate),
    CHEAT("teleport.valdiman_gates", TELEPORT, "Valdiman Gates", k_tp_valdiman_gates),
    CHEAT("teleport.warriors_rest", TELEPORT, "The Warrior's Rest", k_tp_warriors_rest),
    CHEAT("teleport.keanes_workshop", TELEPORT, "Keane's Workshop", k_tp_keanes_workshop),
    CHEAT("teleport.sinners_corner", TELEPORT, "Sinner's Corner", k_tp_sinners_corner),
    CHEAT("teleport.crumbling_market", TELEPORT, "Crumbling Market", k_tp_crumbling_market),
    CHEAT("teleport.treaty_room", TELEPORT, "Treaty Room", k_tp_treaty_room),
    CHEAT("teleport.bandits_hollow", TELEPORT, "Bandit's Hollow", k_tp_bandits_hollow),
    CHEAT("teleport.ore_road", TELEPORT, "The Ore Road", k_tp_ore_road),
    CHEAT("teleport.auction_block", TELEPORT, "The Auction Block", k_tp_auction_block),
    CHEAT("teleport.way_down", TELEPORT, "Way Down", k_tp_way_down),
    CHEAT("teleport.rue_lejour", TELEPORT, "Rue Lejour", k_tp_rue_lejour),
    CHEAT("teleport.kesch_bridge", TELEPORT, "Kesch Bridge", k_tp_kesch_bridge),
    CHEAT("teleport.metal_works_workshop", TELEPORT, "Metal Works Workshop", k_tp_metal_works_workshop),
    CHEAT("teleport.junction_point_workshop", TELEPORT, "Junction Point Workshop", k_tp_junction_point_workshop),
    CHEAT("teleport.dark_coast", TELEPORT, "The Dark Coast", k_tp_dark_coast),
    CHEAT("teleport.plateia_lumitar", TELEPORT, "Plateia Lumitar", k_tp_plateia_lumitar),
    CHEAT("teleport.sin_and_punishment", TELEPORT, "Sin and Punishment", k_tp_sin_and_punishment),
    CHEAT("teleport.the_atrium", TELEPORT, "The Atrium", k_tp_the_atrium),
    CHEAT("teleport.gods_hands_workshop", TELEPORT, "God's Hands Workshop", k_tp_gods_hands_workshop),
};

#undef CHEAT

const PsxCheatDef *psx_cheats_db(int *count) {
    if (count) *count = (int)(sizeof(k_cheats) / sizeof(k_cheats[0]));
    return k_cheats;
}
