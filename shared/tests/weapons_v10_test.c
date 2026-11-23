// Krunker v10.0.7 table test: every class, weapon, effect, streak, attachment, scope, upgraded-weapon and game-mode
// field KrunkNative's tables carry (shared/src/rules/v10/, shared/src/weapons/, shared/src/effects.c, shared/src/modes/),
// against fixtures dumped from the decoded 10.0.7 bundle: shared/tests/data/v10/*.json, written by
// tools/kref/v10-fixtures.py from deob-10.0.7.js (weaponConfig :99659, classConfig :102638, effects :164539,
// upgradeWeapons :177550, attachments :180663, streaks :180987, scope overlays :202747, modes :106124; the tables
// are the same as 10.0.3's, whose lines the older comments below cite: weaponConfig :112381, classConfig :115405, effects
// :182046, upgradeWeapons :199770, attachments :202939, streaks :203338, scope overlays :227658; the
// sources and how to regenerate them: shared/tests/data/v10/README.md), and the physics constants and limits
// against its config object (config.json, deob-10.0.7.js:19503; 10.0.3 :22367). Times are compared in seconds
// (JS ms / 1000), recoil per s (JS per ms * 1000), as KrunkNative stores them. KrunkNative's own entries
// (weapons from WEAPON_ID_KN_BASE, classes from KN_CLASS_BASE: the Nettspend) are not in the bundle and
// not compared. Run from the repository root (or its build directory).
#include <shared.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map_validate.h>
#include <ruleset.h>
#include <triggers.h>
#include <voice.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static cJSON *load_fixture(const char *name) {
    char path[256];
    for (int up = 0; up < 2; up++) {
        snprintf(path, sizeof(path), "%sshared/tests/data/v10/%s.json", up ? "../" : "", name);
        size_t length;
        unsigned char *data;
        if (read_file(path, &data, &length)) continue;
        cJSON *parsed = cJSON_Parse((char *) data);
        free(data);
        if (parsed) return parsed;
    }
    printf("missing fixture shared/tests/data/v10/%s.json (run from the repository root; regenerate with"
           " python3 -I tools/kref/v10-fixtures.py)\n", name);
    exit(1);
}

static double num(const cJSON *obj, const char *key, const double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    return def;
}

static int flag(const cJSON *obj, const char *key) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static int near(const double a, const double b) {
    return fabs(a - b) <= 1e-5 * fmax(1.0, fabs(b));
}

static const char *str(const cJSON *obj, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

// JS key -> float field of Weapon, with the unit conversion.
typedef enum { UNIT_SAME, UNIT_MS, UNIT_X1000 } Unit;
typedef struct {
    const char *js;
    size_t offset;
    Unit unit;
    double def;   // the C value when the JS key is absent
} FloatField;

#define WF(js, field, unit) {js, offsetof(Weapon, field), unit, 0.0}
static const FloatField k_weapon_floats[] = {
    WF("burstR", burst_rate, UNIT_MS), WF("swapTime", swap_time, UNIT_MS), WF("reload", reload_time, UNIT_MS),
    WF("aimSpd", aim_speed, UNIT_MS), WF("spdMlt", speed_mlt, UNIT_SAME), WF("jumMlt", jump_mlt, UNIT_SAME),
    WF("dmg", damage, UNIT_SAME), WF("hsMlt", headshot_mlt, UNIT_SAME), WF("pierce", pierce, UNIT_SAME),
    WF("range", range, UNIT_SAME), WF("dropStart", drop_start, UNIT_SAME), WF("dmgDrop", damage_drop, UNIT_SAME),
    WF("rate", rate, UNIT_MS), WF("spread", spread, UNIT_SAME), WF("minSpread", min_spread, UNIT_SAME),
    WF("recoil", recoil, UNIT_X1000), WF("recoilY", recoil_y, UNIT_SAME), WF("recoilR", recoil_r, UNIT_SAME),
    WF("recover", recover, UNIT_SAME), WF("recoverY", recover_y, UNIT_SAME), WF("recoverF", recover_f, UNIT_SAME),
    WF("physPow", physical_power, UNIT_SAME), WF("physRang", physical_range, UNIT_SAME), WF("zoom", zoom, UNIT_SAME),
    WF("innac", inaccuracy, UNIT_SAME), WF("recoilYM", recoil_y_mlt, UNIT_SAME), WF("recoilZ", recoil_z, UNIT_SAME),
    WF("recoilZM", recoil_z_mlt, UNIT_SAME), WF("zRot", z_rotation, UNIT_SAME), WF("zRM", z_rotation_mlt, UNIT_SAME),
    WF("zLnM", z_lean_mlt, UNIT_SAME), WF("yRot", y_rotation, UNIT_SAME), WF("rotOff", rotation_offset, UNIT_SAME),
    WF("rotOffA", rotation_offset_aim, UNIT_SAME), WF("aimOffY", aim_offset_y, UNIT_SAME), WF("jYMlt", jump_y_mlt, UNIT_SAME),
    WF("jumpYM", jump_bob_y_mlt, UNIT_SAME), WF("landBob", land_bob, UNIT_SAME), WF("aimRecMlt", aim_recoil_mlt, UNIT_SAME),
    WF("leanMlt", lean_mlt, UNIT_SAME), WF("inspectR", inspect_rotation, UNIT_SAME), WF("inspectM", inspect_mlt, UNIT_SAME),
    WF("insX", inspect_x, UNIT_SAME), WF("swapWiggle", swap_wiggle, UNIT_SAME), WF("aimLean", aim_lean, UNIT_SAME),
    WF("cLean", crouch_lean, UNIT_SAME), WF("cRot", crouch_rotation, UNIT_SAME), WF("cDrop", crouch_drop, UNIT_SAME),
    WF("attachYOff", attach_y_offset, UNIT_SAME), WF("attachZOff", attach_z_offset, UNIT_SAME), WF("scale", scale, UNIT_SAME),
    WF("holdW", hold_width, UNIT_SAME), WF("hDstOff", hold_distance_offset, UNIT_SAME),
    WF("adSpread", aim_spread, UNIT_SAME), WF("chrgTime", charge_time, UNIT_MS), WF("chrgDec", charge_decay, UNIT_SAME),
    WF("fullChrgMlt", full_charge_mlt, UNIT_SAME), WF("deployRang", deploy_range, UNIT_SAME), WF("grapRang", grapple_range, UNIT_SAME),
    WF("shine", shine, UNIT_SAME), WF("aimY", aim_y, UNIT_SAME),
};

// The view-model vectors: JS x / y / z keys -> a vec3 of Weapon (absent = 0)
typedef struct {
    const char *js[3];
    size_t offset;
} VecField;

static const VecField k_weapon_vecs[] = {
    {{"xOff", "yOff", "zOff"}, offsetof(Weapon, offset)},
    {{"xOrg", "yOrg", "zOrg"}, offsetof(Weapon, origin)},
    {{"leftHoldX", "leftHoldY", "leftHoldZ"}, offsetof(Weapon, left_hold)},
    {{"rightHoldX", "rightHoldY", "rightHoldZ"}, offsetof(Weapon, right_hold)},
};

static double js_to_c(const double v, const Unit unit) {
    return unit == UNIT_MS ? v / 1000.0 : unit == UNIT_X1000 ? v * 1000.0 : v;
}

// The view-model fields: offsets, holds, rot, recoilAnim, inspAnim
static void check_weapon_view(const Weapon *w, const cJSON *js) {
    for (size_t i = 0; i < sizeof(k_weapon_vecs) / sizeof(k_weapon_vecs[0]); i++) {
        const VecField *f = &k_weapon_vecs[i];
        const vec3 have = *(const vec3 *) ((const char *) w + f->offset);
        CHECK(near(have.x, num(js, f->js[0], 0)) && near(have.y, num(js, f->js[1], 0)) && near(have.z, num(js, f->js[2], 0)),
              "%s %s / %s / %s = %g %g %g, v10.0.7 %g %g %g", w->name, f->js[0], f->js[1], f->js[2], (double) have.x, (double) have.y,
              (double) have.z, num(js, f->js[0], 0), num(js, f->js[1], 0), num(js, f->js[2], 0));
    }

    // rot: the three entries that set it (Bomb, Build Tool, Boulder) are [0, Math.PI / 2, -0.4]
    // (deob-10.0.3.js:114225, 114390, 115264; the fixture holds PI / 2 computed)
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(js, "rot");
    CHECK(w->has_rotation == cJSON_IsArray(rot), "%s rot present %d", w->name, w->has_rotation);
    if (cJSON_IsArray(rot)) {
        const float have[3] = {w->rotation.x, w->rotation.y, w->rotation.z};
        CHECK(cJSON_GetArraySize(rot) == 3, "%s rot length", w->name);
        for (int k = 0; k < 3 && k < cJSON_GetArraySize(rot); k++) {
            const cJSON *v = cJSON_GetArrayItem(rot, k);
            CHECK(cJSON_IsNumber(v) && near(have[k], v->valuedouble), "%s rot[%d] = %g", w->name, k, (double) have[k]);
        }
    }

    // recoilAnim {time, aimTime, recoilTweenY / R / YM / Z}: the targets present set recoil_anim_mask
    const cJSON *ra = cJSON_GetObjectItemCaseSensitive(js, "recoilAnim");
    CHECK(w->has_recoil_anim == cJSON_IsObject(ra), "%s recoilAnim present %d", w->name, w->has_recoil_anim);
    if (cJSON_IsObject(ra)) {
        static const char *const tweens[] = {"recoilTweenY", "recoilTweenR", "recoilTweenYM", "recoilTweenZ"};
        const float have[] = {w->recoil_tween_y, w->recoil_tween_r, w->recoil_tween_ym, w->recoil_tween_z};
        unsigned int mask = 0;
        for (int k = 0; k < 4; k++) {
            if (cJSON_GetObjectItemCaseSensitive(ra, tweens[k])) mask |= 1u << k;
            CHECK(near(have[k], num(ra, tweens[k], 0)), "%s recoilAnim.%s = %g, v10.0.7 %g", w->name, tweens[k], (double) have[k],
                  num(ra, tweens[k], 0));
        }
        CHECK(w->recoil_anim_mask == mask, "%s recoilAnim targets 0x%x, v10.0.3 0x%x", w->name, w->recoil_anim_mask, mask);
        CHECK(near(w->recoil_anim_time, num(ra, "time", 0) / 1000.0) && near(w->recoil_anim_aim_time, num(ra, "aimTime", 0) / 1000.0),
              "%s recoilAnim time / aimTime = %g / %g", w->name, (double) w->recoil_anim_time, (double) w->recoil_anim_aim_time);
    }

    // flap {src, rot, scl, xOff, yOff, zOff} (the Sniper Rifle and its upgrade)
    const cJSON *flap = cJSON_GetObjectItemCaseSensitive(js, "flap");
    CHECK(!!w->flap.src == cJSON_IsObject(flap), "%s flap present %d", w->name, !!w->flap.src);
    if (cJSON_IsObject(flap) && w->flap.src) {
        const char *src = str(flap, "src");
        CHECK(src && !strcmp(w->flap.src, src), "%s flap.src %s", w->name, w->flap.src);
        CHECK(near(w->flap.rot, num(flap, "rot", 0)) && near(w->flap.scl, num(flap, "scl", 0)), "%s flap rot / scl", w->name);
        CHECK(near(w->flap.offset.x, num(flap, "xOff", 0)) && near(w->flap.offset.y, num(flap, "yOff", 0)) &&
                  near(w->flap.offset.z, num(flap, "zOff", 0)),
              "%s flap offset %g %g %g", w->name, (double) w->flap.offset.x, (double) w->flap.offset.y, (double) w->flap.offset.z);
    }

    // inspAnim {arZ, arX, arY, apZ, apX, apY, wpZ, wpY, wrY} (the Combat Knife)
    const cJSON *ia = cJSON_GetObjectItemCaseSensitive(js, "inspAnim");
    CHECK(w->has_inspect_anim == cJSON_IsObject(ia), "%s inspAnim present %d", w->name, w->has_inspect_anim);
    if (cJSON_IsObject(ia)) {
        static const char *const keys[9] = {"arZ", "arX", "arY", "apZ", "apX", "apY", "wpZ", "wpY", "wrY"};
        for (int k = 0; k < 9; k++) {
            CHECK(near(w->inspect_anim[k], num(ia, keys[k], 0)), "%s inspAnim.%s = %g", w->name, keys[k], (double) w->inspect_anim[k]);
        }
    }
}

static void check_weapon_entry(const Weapon *w, const int id, const cJSON *js, const int upgraded) {
    const char *name = str(js, "name");
    CHECK(w && name && !strcmp(w->name, name), "weapon %d is %s, v10.0.7 %s", id, w ? w->name : "(null)", name ? name : "?");
    if (!w) return;

    const char *src = str(js, "src");
    const char *icon = str(js, "icon");
    const char *sound = str(js, "sound");
    CHECK((!src && !w->src) || (src && w->src && !strcmp(src, w->src)), "%s src", w->name);
    CHECK(icon && w->icon && !strcmp(icon, w->icon), "%s icon", w->name);
    // sound: the Boulder ships 'sound': "" (deob-10.0.3.js:115240; it never fires), which KrunkNative keeps as
    // NULL (no sound to play)
    CHECK((sound && w->sound && !strcmp(sound, w->sound)) || (sound && !sound[0] && !w->sound), "%s sound %s (v10.0.7 %s)", w->name,
          w->sound ? w->sound : "(null)", sound ? sound : "?");

    for (size_t i = 0; i < sizeof(k_weapon_floats) / sizeof(k_weapon_floats[0]); i++) {
        const FloatField *f = &k_weapon_floats[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(js, f->js);
        const double expect = cJSON_IsNumber(v) ? js_to_c(v->valuedouble, f->unit) : f->def;
        const float have = *(const float *) ((const char *) w + f->offset);
        CHECK(near(have, expect), "%s %s = %g, v10.0.7 %g", w->name, f->js, (double) have, expect);
    }

    CHECK(w->ammo == (unsigned int) num(js, "ammo", 0), "%s ammo %u, v10.0.7 %g", w->name, w->ammo, num(js, "ammo", 0));
    CHECK(w->shots == (unsigned int) num(js, "shots", 0), "%s shots", w->name);
    CHECK(w->burst == flag(js, "burst") && (!w->burst || w->burst_count == (unsigned int) num(js, "burstC", 0)), "%s burst", w->name);
    CHECK(w->type == (int) num(js, "type", 0), "%s type", w->name);
    CHECK(w->min_rec == (int) num(js, "minRec", 0), "%s minRec", w->name);
    if (!upgraded) CHECK(w->mag_reserve == (int) num(js, "magReserve", 0), "%s magReserve", w->name); // upgrades: not ported
    const cJSON *kill = cJSON_GetObjectItemCaseSensitive(js, "kill");
    CHECK(w->kill_score == (cJSON_IsArray(kill) ? (int) cJSON_GetArrayItem(kill, 1)->valuedouble : 0), "%s kill score %d", w->name, w->kill_score);
    CHECK(near(w->leg_mlt ? w->leg_mlt : 0.5, num(js, "lsMlt", 0.5)), "%s lsMlt %g", w->name, (double) w->leg_mlt);
    CHECK(near(w->move_spread ? w->move_spread : 1.0, num(js, "movSpread", 1.0)), "%s movSpread %g", w->name, (double) w->move_spread);
    // chrgMlt: 0 in the C table means the 0.9 default. The Compressor ships an explicit 'chrgMlt': 0
    // (deob-10.0.3.js:115316) but never charges (chrgTime 0), so the two encodings cannot differ there.
    const double chrg_mlt = num(js, "chrgMlt", 0.9);
    const int chrg_unused = chrg_mlt == 0.0 && !w->charge_mlt && !w->charge_time;
    CHECK(chrg_unused || near(w->charge_mlt ? w->charge_mlt : 0.9, chrg_mlt), "%s chrgMlt", w->name);
    CHECK(w->effect_on_hit == (cJSON_GetObjectItemCaseSensitive(js, "effectOnHit") ? (int) num(js, "effectOnHit", 0) + 1 : 0), "%s effectOnHit", w->name);
    CHECK(w->deployable == (cJSON_GetObjectItemCaseSensitive(js, "deployable") ? (int) num(js, "deployable", 0) + 1 : 0), "%s deployable", w->name);

    static const struct { const char *js; int bit; } flags[] = {{"secondary", 0}, {"noSpread", 1}, {"akimbo", 2}, {"noAim", 3}, {"nAuto", 4},
        {"nInsp", 5}, {"canThrow", 6}, {"melee", 7}, {"equipment", 8}, {"bomb", 9}, {"animWhileAim", 10}, {"scope", 11}, {"noHeadShot", 12},
        {"forceAttach", 13}, {"ovrChrg", 14}, {"shootChrg", 15}, {"rateChrg", 16}, {"fullChrgSht", 17}, {"bypassChrg", 18}, {"noComp", 19},
        {"customOnly", 20}, {"noReload", 21}, {"noSwap", 22}, {"noJump", 23}, {"removeOnEmpty", 24}, {"noProjSpread", 25}, {"build", 26},
        {"noCross", 27}, {"canUpgrade", 28}};
    const int have[] = {w->secondary, w->no_spread, w->akimbo, w->no_aim, w->no_auto, w->no_inspect, w->can_throw, w->melee, w->equipment,
        w->bomb, w->animate_while_aiming, w->scope, w->no_headshot, w->force_attach, w->over_charge, w->shoot_charge, w->rate_charge,
        w->full_charge_shot, w->bypass_charge, w->no_comp, w->custom_only, w->no_reload, w->no_swap, w->no_jump, w->remove_on_empty,
        w->no_proj_spread, w->build, w->no_cross, w->can_upgrade};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        CHECK(!!have[flags[i].bit] == flag(js, flags[i].js), "%s %s = %d", w->name, flags[i].js, have[flags[i].bit]);
    }
    // the weaponConfig keys v10 added (no v7.1.4 weapon has them): the Compressor's, the Boulder's gravMlt,
    // the Sniper Rifle / AWP Blaster trail and the Charge Rifle trailChrg
    CHECK(!!w->no_dmg == flag(js, "noDmg") && !!w->phys_pull == flag(js, "physPull") && !!w->non_obj_impulse == flag(js, "nonObjImpulse") &&
          !!w->r_shoot == flag(js, "rShoot"), "%s noDmg / physPull / nonObjImpulse / rShoot", w->name);
    CHECK(!!w->trail == flag(js, "trail") && !!w->trail_charge == flag(js, "trailChrg"), "%s trail / trailChrg", w->name);
    CHECK(near(w->gravity_mlt, num(js, "gravMlt", 0)), "%s gravMlt %g", w->name, (double) w->gravity_mlt);
    CHECK(w->has_attach == (cJSON_GetObjectItemCaseSensitive(js, "attach") != NULL && !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(js, "attach"))), "%s attach", w->name);

    // pj_* (projectile_config): every weapon with a projectile block
    const ProjectileConfig *pj = &w->projectile_config;
    CHECK(near(pj->speed, num(js, "pj_spd", 0)), "%s pj_spd", w->name);
    if (pj->speed > 0.0f) {
        CHECK(near(pj->gravity, num(js, "pj_grav", 0)), "%s pj_grav", w->name);
        CHECK(near(pj->range, num(js, "pj_range", 0)), "%s pj_range", w->name);
        CHECK(near(pj->damage, num(js, "pj_dmg", 0)), "%s pj_dmg", w->name);
        CHECK(near(pj->explode, num(js, "pj_explode", 0)), "%s pj_explode", w->name);
        CHECK(near(pj->explode_damage, num(js, "pj_eDmg", 0)), "%s pj_eDmg", w->name);
        CHECK(pj->no_impact == flag(js, "pj_nImp") && pj->dont_pred == flag(js, "pj_dontPred"), "%s pj flags", w->name);
        CHECK(w->projectile_disable == flag(js, "pj_disable"), "%s pj_disable", w->name);
    }

    // cSpread: both patterns, flattened
    const cJSON *cs = cJSON_GetObjectItemCaseSensitive(js, "cSpread");
    if (cJSON_IsArray(cs)) {
        unsigned int pair = 0;
        int ok = 1;
        const cJSON *pattern;
        cJSON_ArrayForEach(pattern, cs) {
            for (int k = 0; k + 1 < cJSON_GetArraySize(pattern); k += 2, pair++) {
                ok &= pair < w->custom_spread_count && near(w->custom_spread[pair].x, cJSON_GetArrayItem(pattern, k)->valuedouble) &&
                      near(w->custom_spread[pair].y, cJSON_GetArrayItem(pattern, k + 1)->valuedouble);
            }
        }
        CHECK(ok && pair == w->custom_spread_count, "%s cSpread", w->name);
    } else {
        CHECK(!w->custom_spread, "%s has a cSpread v10.0.7 lacks", w->name);
    }

    if (!upgraded) check_weapon_view(w, js); // upgrades keep their base weapon's view model (upgraded.c)
}

static void check_class(const int id, const cJSON *js) {
    const ClassConfig *c = &rules_classes()[id];
    const char *name = str(js, "name");
    CHECK(name && c->name && !strcmp(c->name, name), "class %d is %s, v10.0.7 %s", id, c->name, name ? name : "?");
    CHECK(c->loadout && c->loadout[0] == (int) cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(js, "loadout"), 0)->valuedouble, "%s loadout", c->name);
    CHECK(c->secondary == flag(js, "secondary"), "%s secondary", c->name);
    CHECK(c->wall_jump == flag(js, "wallJ"), "%s wallJ", c->name);
    CHECK(c->wall_jump_max == (int) num(js, "wallJM", 0), "%s wallJM", c->name);
    CHECK(c->health == (int) num(js, "health", 0), "%s health %d", c->name, c->health);
    CHECK(c->health_segments == (int) num(js, "segs", 0), "%s segs", c->name);
    CHECK(near(c->regen, num(js, "regen", 0.1)), "%s regen", c->name);
    CHECK(near(c->speed, num(js, "speed", 1)), "%s speed %g", c->name, (double) c->speed);
    CHECK(near(c->throw_charge, num(js, "throwChrg", 900) / 1000.0), "%s throwChrg", c->name);
    CHECK(near(c->slide_time_mlt ? c->slide_time_mlt : 1.0, num(js, "slT", 1)), "%s slT", c->name);
    CHECK(near(c->slide_accel_mlt ? c->slide_accel_mlt : 1.0, num(js, "slA", 1)), "%s slA", c->name);
    CHECK(near(c->melee_rate ? c->melee_rate : 1.0, num(js, "melRate", 1)), "%s melRate", c->name);
    CHECK(near(c->turn_speed ? c->turn_speed : 1.0, num(js, "trnSpd", 1)), "%s trnSpd", c->name);
    CHECK(near(c->hitbox_pad_w, num(js, "hbS", 0)) && near(c->hitbox_pad_h, num(js, "hbH", 0)), "%s hbS / hbH", c->name);
    CHECK(c->min_rec == (int) num(js, "minRec", 0), "%s minRec", c->name);
    CHECK(c->hide == flag(js, "hide") && c->no_mobile == flag(js, "noMobile") && c->no_sleeve == flag(js, "noSleeve"), "%s flags", c->name);
    CHECK(c->face == (int) num(js, "face", 0), "%s face", c->name);
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(js, "colors");
    for (int k = 0; k < 6; k++) CHECK(c->colors[k] == (int) cJSON_GetArrayItem(colors, k)->valuedouble, "%s colour %d", c->name, k);
    const cJSON *txts = cJSON_GetObjectItemCaseSensitive(js, "txts");
    CHECK(c->text_count == (cJSON_IsArray(txts) ? cJSON_GetArraySize(txts) : 0), "%s txts", c->name);
    // classConfig hurtboxScale: the hit boxes' scale (no 10.0.3 class sets it: 1)
    CHECK(near(c->hurtbox_scale ? c->hurtbox_scale : 1.0, num(js, "hurtboxScale", 1)), "%s hurtboxScale %g", c->name, (double) c->hurtbox_scale);
}

// ---- the game-mode list (modes.json: deob-10.0.7.js:106124-106681) against the v10 ruleset's GameModeConfig table
// (shared/src/modes/modes.c). Absent keys are null / false / 0; times are seconds in C (JS ms / 1000; forceSpawn is
// already seconds in JS). A function key is compared by presence: KrunkNative ports each hook as a C function.
static int truthy(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

static int has_key(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return v && !cJSON_IsNull(v);
}

static int same_str(const char *have, const cJSON *obj, const char *key) {
    const char *want = str(obj, key);
    return (!have && !want) || (have && want && !strcmp(have, want));
}

// a NULL-terminated C string list against a JS string array (NULL / absent = none)
static int same_str_list(const char *const *have, const cJSON *obj, const char *key) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(obj, key);
    int n = 0;
    for (; have && have[n]; n++) {
        const cJSON *v = cJSON_GetArrayItem(list, n);
        if (!cJSON_IsString(v) || strcmp(v->valuestring, have[n])) return 0;
    }
    return n == (cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0);
}

static int same_int_list(const int *have, const int count, const cJSON *obj, const char *key) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (count != (cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0)) return 0;
    for (int i = 0; i < count; i++) {
        if (have[i] != (int) cJSON_GetArrayItem(list, i)->valuedouble) return 0;
    }
    return 1;
}

static void check_mode(const GameModeConfig *m, const int id, const cJSON *js) {
    CHECK(m->id == id && same_str(m->key, js, "id") && same_str(m->name, js, "name") && same_str(m->alias, js, "alias"),
          "mode %d is %s / %s, v10.0.7 %s", id, m->key, m->name, str(js, "id"));
    const char *k = m->key;

    const struct { const char *js; int have; } flags[] = {
        {"realMov", m->real_movement}, {"noWeap", m->no_weapons}, {"noRegen", m->no_regen}, {"noReloads", m->no_reloads},
        {"noPickups", m->no_pickups}, {"noStreaks", m->no_streaks}, {"noClan", m->no_clan}, {"teams", m->teams},
        {"dmgTeam", m->dmg_team}, {"site", m->site}, {"flags", m->flags}, {"killConv", m->kill_on_convert},
        {"friendly", m->friendly}, {"objective", m->objective}, {"objWep", m->obj_weapon}, {"noScoreC", m->no_score_c},
        {"noScoreK", m->no_score_k}, {"noWinner", m->no_winner}, {"noKick", m->no_kick}, {"noReward", m->no_reward},
        {"noAssists", m->no_assists}, {"noKillFeed", m->no_kill_feed}, {"noDeadChat", m->no_dead_chat},
        {"hideNames", m->hide_names}, {"showTeam", m->show_team}, {"teamSort", m->team_sort}, {"fakeNames", m->fake_names},
        {"timed", m->timed}, {"showMS", m->show_ms}, {"keepLoadout", m->keep_loadout}, {"canTrade", m->can_trade},
        {"preventCustomGames", m->prevent_custom}, {"noFreeCam", m->no_free_cam}, {"party", m->party}, {"dom", m->dom},
        {"oneflag", m->oneflag}, {"rush", m->rush}, {"deposit", m->deposit}, {"noKnives", m->no_knives},
        {"noMelee", m->no_melee}, {"noSliding", m->no_sliding}, {"noWallJ", m->no_wall_jump}, {"frSlid", m->fr_slid},
        {"wallJ", m->has_wall_jump}, {"doubleXP", m->double_xp}, {"bighead", m->bighead}, {"noMinimap", m->no_minimap},
        {"botsAsAI", m->bots_as_ai}, {"botBypassClass", m->bot_bypass_class}, {"noClientKick", m->no_client_kick},
        {"noHPbars", m->no_hp_bars}, {"logTime", m->log_time}, {"ownTime", m->own_time}, {"allowReset", m->allow_reset},
        {"showHour", m->show_hour}, {"prop", m->prop}, {"noClassScr", m->no_class_scr}, {"teamSee", m->team_see},
        {"finiteAmmo", m->finite_ammo}, {"unrestrictedWeapons", m->unrestricted_weapons}, {"forceCharge", m->force_charge},
        {"forceOldScoreboard", m->force_old_scoreboard}, {"noScoreXP", m->no_score_xp}, {"noProfileStats", m->no_profile_stats},
        {"nonObjImpulse", m->non_obj_impulse}, {"canSlide", m->stalker_v10}, {"convTeam", m->convert_team},
        {"ammoLimit", m->has_ammo_limit}, {"impulseMlt", m->has_impulse_mlt}, {"skyCol", m->has_sky_col},
        {"fogC", m->has_fog_col}, {"fogD", m->has_fog_dist}, {"lightCol", m->has_light_col}, {"ambCol", m->has_amb_col},
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        const int want = !strcmp(flags[i].js, "ammoLimit") || !strcmp(flags[i].js, "impulseMlt") || !strcmp(flags[i].js, "skyCol") ||
                                 !strcmp(flags[i].js, "fogC") || !strcmp(flags[i].js, "fogD") || !strcmp(flags[i].js, "lightCol") ||
                                 !strcmp(flags[i].js, "ambCol")
                             ? has_key(js, flags[i].js)
                             : truthy(js, flags[i].js);
        CHECK(!!flags[i].have == want, "mode %s %s = %d, v10.0.7 %d", k, flags[i].js, !!flags[i].have, want);
    }

    // numbers: {JS key, the C value, JS -> C scale, the C value when the key is absent}
    const struct { const char *js; double have; double scale; double def; } nums[] = {
        {"gameTime", m->game_time, 0.001, MODE_GAME_TIME_CONFIG}, {"forceSpawn", m->force_spawn, 1, 0}, {"minPlayers", m->min_players, 1, 0},
        {"lives", m->lives, 1, 0}, {"scoreLimit", m->score_limit, 1, 0}, {"objScore", m->obj_score, 1, 0}, {"tagLimit", m->tag_limit, 1, 0},
        {"rewardMlt", m->reward_mlt, 1, 0}, {"sendR", m->send_rate, 1, 0}, {"forceClass", m->force_class, 1, -1},
        {"maxHealth", m->health, 1, 0}, {"ammoLimit", m->ammo_limit, 1, 0}, {"hitBoxPad", m->hitbox_pad, 1, 1},
        {"impulseMlt", m->impulse_mlt, 1, 0}, {"ambInd", m->amb_ind, 1, 0}, {"skyCol", m->sky_col, 1, 0}, {"fogC", m->fog_col, 1, 0},
        {"fogD", m->fog_dist, 1, 0}, {"lightCol", m->light_col, 1, 0}, {"ambCol", m->amb_col, 1, 0},
        {"pointsPerHit", m->points_per_hit, 1, 0}, {"starterScore", m->starter_score, 1, 0}, {"regenMlt", m->regen_mlt, 1, 0},
        {"multiTeam", m->multi_team, 1, 0}, {"captureTime", m->capture_time, 0.001, 0}, {"lockoutMlt", m->lockout_mlt, 1, 0},
        {"perPlayerMlt", m->per_player_mlt, 1, 0}, {"killMlt", m->kill_mlt, 1, 0}, {"hillMlt", m->hill_mlt, 1, 0},
        {"carrierMlt", m->carrier_mlt, 1, 0}, {"teamCarrierMlt", m->team_carrier_mlt, 1, 0}, {"nonCarrierMlt", m->non_carrier_mlt, 1, 0},
        {"tagHoldLimit", m->tag_hold_limit, 1, 0}, {"maxPerCluster", m->max_per_cluster, 1, 0},
        {"krankedTimer", m->kranked_timer, 0.001, 0}, {"headMlt", m->head_mlt, 1, 0}, {"headMltMax", m->head_mlt_max, 1, 0},
        {"gravMlt", m->grav_mlt, 1, 0}, {"jumpMlt", m->jump_mlt, 1, 0}, {"groundAccel", m->ground_accel, 1, 0},
        {"airAccel", m->air_accel, 1, 0}, {"reloadMlt", m->reload_mlt, 1, 0}, {"wallJ", m->wall_jump, 1, 0},
        {"wallJM", m->wall_jump_max, 1, 0}, {"wallJD", m->wall_jump_d, 1, 0}, {"wallJV", m->wall_jump_v, 1, 0},
        {"wallJYV", m->wall_jump_yv, 1, 0}, {"explMlt", m->expl_mlt, 1, 0}, {"explSMlt", m->expl_size_mlt, 1, 0},
        {"randClassScr", m->rand_class_scr, 1, 0}, {"viewDis", m->view_dis, 1, 0}, {"nameDist", m->name_dist, 1, 0},
        {"maxNoMov", m->max_no_mov, 0.001, 0}, {"propSwitches", m->prop_switches, 1, 0}, {"specMaxD", m->spec_max_d, 1, 0},
        {"event", m->event, 1, 0}, {"convTeam", m->conv_team, 1, 0},
    };
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
        const double want = has_key(js, nums[i].js) ? num(js, nums[i].js, 0) * nums[i].scale : nums[i].def;
        CHECK(near(nums[i].have, want), "mode %s %s = %g, v10.0.7 %g", k, nums[i].js, nums[i].have, want);
    }
    const cJSON *bonuses = cJSON_GetObjectItemCaseSensitive(js, "bonuses");
    CHECK(near(m->bonus_speed, num(bonuses, "speed", 0)) && near(m->bonus_reload, num(bonuses, "reload", 0)) &&
              near(m->bonus_fire_rate, num(bonuses, "firerate", 0)), "mode %s bonuses", k);

    // per-team tables: teamClass / spdMlt {team: value}
    const cJSON *team_class = cJSON_GetObjectItemCaseSensitive(js, "teamClass"), *spd = cJSON_GetObjectItemCaseSensitive(js, "spdMlt");
    for (int t = 0; t < MODE_TEAMS; t++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", t);
        CHECK(m->team_class[t] == (int) num(team_class, key, -1) && near(m->speed_mlt[t], num(spd, key, 1)), "mode %s team %d class / spdMlt", k, t);
    }

    // strings and lists
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(js, "teamOptions"), *colors = cJSON_GetObjectItemCaseSensitive(js, "teamColors");
    for (int t = 0; t < 2; t++) {
        const cJSON *o = cJSON_GetArrayItem(opts, t), *c = cJSON_GetArrayItem(colors, t);
        CHECK((!m->team_options[t] && !o) || (m->team_options[t] && cJSON_IsString(o) && !strcmp(m->team_options[t], o->valuestring)),
              "mode %s teamOptions[%d]", k, t);
        CHECK((!m->team_colors[t] && !c) || (m->team_colors[t] && cJSON_IsString(c) && !strcmp(m->team_colors[t], c->valuestring)),
              "mode %s teamColors[%d]", k, t);
    }
    CHECK(same_str(m->sub, js, "sub") && same_str(m->conv_word, js, "convWord") && same_str(m->score_stat, js, "scoreStat") &&
              same_str(m->score_label, js, "scoreLabel"), "mode %s sub / convWord / scoreStat / scoreLabel", k);
    CHECK(same_str_list(m->end_stats, js, "endStats"), "mode %s endStats", k);
    CHECK(same_str_list(m->leader_stats, js, "leaderStats"), "mode %s leaderStats", k);
    CHECK(same_str_list(m->skip_stats, js, "skipStats"), "mode %s skipStats", k);
    CHECK(same_int_list(m->starting_loadout, m->starting_loadout_size, js, "startingLoadout"), "mode %s startingLoadout", k);
    CHECK(same_int_list(m->weapon_order, m->weapon_order_size, js, "weaponOrder"), "mode %s weaponOrder", k);
    CHECK(same_int_list(m->disabled_streaks, m->disabled_streak_count, js, "disabledStreaks"), "mode %s disabledStreaks", k);

    // waitTimers {time, msg, trigger, noJoin, noClear, comp, contTime, dontWait}
    const cJSON *timers = cJSON_GetObjectItemCaseSensitive(js, "waitTimers");
    CHECK(m->wait_timer_count == (cJSON_IsArray(timers) ? cJSON_GetArraySize(timers) : 0), "mode %s waitTimers count %d", k, m->wait_timer_count);
    for (int i = 0; i < m->wait_timer_count && i < cJSON_GetArraySize(timers); i++) {
        const ModeWaitTimer *w = &m->wait_timers[i];
        const cJSON *t = cJSON_GetArrayItem(timers, i);
        // One in the Chamber's second trigger is `function () {}` (deob-10.0.7.js:105200): KrunkNative has none
        const int empty_trigger = !strcmp(k, "oinc") && i == 1;
        CHECK(near(w->time, num(t, "time", 0) / 1000.0) && same_str(w->msg, t, "msg") &&
                  (!!w->trigger == has_key(t, "trigger") || empty_trigger) && w->no_join == flag(t, "noJoin") && w->no_clear == flag(t, "noClear") &&
                  w->comp == flag(t, "comp") && w->cont_time == flag(t, "contTime") && w->dont_wait == flag(t, "dontWait"),
              "mode %s waitTimers[%d] (%g s, %s)", k, i, (double) w->time, w->msg ? w->msg : "");
    }

    // the hooks
    const struct { const char *js; int have; } hooks[] = {
        {"condition", !!m->condition}, {"winCondition", !!m->win_condition}, {"endSort", !!m->end_sort}, {"gameStart", !!m->game_start},
        {"onKill", !!m->on_kill}, {"leaderSort", !!m->leader_sort}, {"update", !!m->update}, {"collisionCheck", !!m->collision_check},
        {"onPreEnd", !!m->on_pre_end}, {"onEnd", !!m->on_end}, {"onMelee", !!m->on_melee},
    };
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        // not hooks in KrunkNative, on purpose: Hardpoint's onKill computes a position and drops it (deob-10.0.7.js:104779-104786,
        // no effect); Zombies' onEnd is only the official database's best-wave call (:105950-105956, doc/v10/rules-1003.md)
        if ((!strcmp(k, "point") && !strcmp(hooks[i].js, "onKill")) || (!strcmp(k, "zom") && !strcmp(hooks[i].js, "onEnd"))) continue;
        CHECK(hooks[i].have == has_key(js, hooks[i].js), "mode %s %s hook %d, v10.0.7 %d", k, hooks[i].js, hooks[i].have, has_key(js, hooks[i].js));
    }
}

// ---- config (deob-10.0.7.js:19503-20147, 10.0.3 :22367-23014): GameConstants (constants_v10.c) and the limits KrunkNative keeps as
// #defines, against config.json. Per-ms values are stored per second (x 1000), times in seconds.
typedef struct {
    const char *js;
    size_t offset;
    Unit unit;
} ConstField;

#define CF(js, field, unit) {js, offsetof(GameConstants, field), unit}
static const ConstField k_constants[] = {
    CF("mouseSens", mouse_sensitivity, UNIT_SAME), CF("worldUV", world_uv_scale, UNIT_SAME), CF("playerHeight", player_height, UNIT_SAME),
    CF("borderH", border_height, UNIT_SAME), CF("cameraHeight", camera_height, UNIT_SAME), CF("climbHeight", climb_height, UNIT_SAME),
    CF("jumpVel", jump_velocity, UNIT_SAME), CF("aimJumpSlow", jump_aim_slow, UNIT_SAME), CF("jumpPush", jump_push, UNIT_SAME),
    CF("crouchJump", crouch_jump, UNIT_SAME), CF("crouchSpd", crouch_speed, UNIT_X1000), CF("crouchLean", crouch_lean, UNIT_SAME),
    CF("aimSlow", aim_slow, UNIT_SAME), CF("terrainSlideDecel", terrain_slide_decel, UNIT_SAME), CF("terrainDecel", terrain_decel, UNIT_SAME),
    CF("slideDecel", slide_decel, UNIT_SAME), CF("airDecel", air_decel, UNIT_SAME), CF("groundDecel", ground_decel, UNIT_SAME),
    CF("ladderDecel", ladder_decel, UNIT_SAME), CF("crouchDst", crouch_distance, UNIT_SAME), CF("gravity", gravity, UNIT_X1000),
    CF("armScale", arm_scale, UNIT_SAME), CF("armInset", arm_inset, UNIT_SAME), CF("chestWidth", chest_width, UNIT_SAME),
    CF("chestScale", chest_scale, UNIT_SAME), CF("legScale", leg_scale, UNIT_SAME), CF("legHeight", leg_height, UNIT_SAME),
    CF("headScale", head_scale, UNIT_SAME), CF("playerScale", player_scale, UNIT_SAME), CF("decelMin", min_decel, UNIT_SAME),
    CF("slideTime", slide_time, UNIT_MS), CF("playerSlideVelMlt", player_slide_velocity_mlt, UNIT_SAME),
    CF("playerTerrainSlideVelMlt", player_terrain_slide_velocity_mlt, UNIT_SAME), CF("idleAnimS", idle_anim_speed, UNIT_X1000),
    CF("interpolation", interpolation, UNIT_SAME), CF("leanSens", lean_sensitivity, UNIT_SAME), CF("leanMax", lean_max, UNIT_SAME),
    CF("leanPull", lean_pull, UNIT_SAME), CF("leanPullZ", lean_pull_z, UNIT_SAME), CF("leanMltZ", lean_mlt_z, UNIT_SAME),
    CF("bobPullY", bob_pull_y, UNIT_SAME), CF("bobPullZ", bob_pull_z, UNIT_SAME), CF("bobMltY", bob_mlt_y, UNIT_SAME),
    CF("bobMltZ", bob_mlt_z, UNIT_SAME), CF("crouchAnim", crouch_anim, UNIT_X1000), CF("collPad", collision_padding, UNIT_SAME),
    CF("boosterSpd", booster_speed, UNIT_X1000), CF("ladderSpeed", ladder_speed, UNIT_X1000), CF("ladderScale", ladder_scale, UNIT_SAME),
    CF("ladderWidth", ladder_width, UNIT_SAME), CF("deathY", death_y, UNIT_SAME), CF("recoilMlt", recoil_mlt, UNIT_SAME),
    CF("spreadAdj", spread_adjustment, UNIT_SAME), CF("uArmLength", upper_arm_length, UNIT_SAME), CF("lArmLength", lower_arm_length, UNIT_SAME),
    CF("armOff", arm_offset, UNIT_SAME), CF("animMult", anim_mlt, UNIT_SAME), CF("aimAnimMlt", aim_anim_mlt, UNIT_SAME),
    CF("crouchAnimMlt", crouch_anim_mlt, UNIT_SAME), CF("stepAnim", step_anim, UNIT_SAME), CF("stepPull", step_pull, UNIT_SAME),
    CF("diskSpin", disk_spin, UNIT_X1000), CF("wristCuff", wrist_cuff, UNIT_SAME), CF("wristScale", wrist_scale, UNIT_SAME),
};

static void check_config(const cJSON *config) {
    const GameConstants *k = &game_constants;
    for (size_t i = 0; i < sizeof(k_constants) / sizeof(k_constants[0]); i++) {
        const ConstField *f = &k_constants[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(config, f->js);
        const float have = *(const float *) ((const char *) k + f->offset);
        CHECK(cJSON_IsNumber(v) && near(have, js_to_c(v->valuedouble, f->unit)), "config.%s: KrunkNative %g, v10.0.7 %g", f->js,
              (double) have, cJSON_IsNumber(v) ? js_to_c(v->valuedouble, f->unit) : NAN);
    }
    // not config scalars: dltMx = Math.round(1000 / 30) (deob-10.0.3.js:19242), procInputs' 1 ms floor at deltaMlt 1
    // (:191276-191280), the grA / arA / crchSpd class-editor defaults 0.042 / 100, 0.0047 / 100, 0.3 (:20418, 20438,
    // 20465), and the six posters after "Random" in config.billboards (:20134)
    CHECK(near(k->max_delta, round(1000.0 / 30.0) / 1000.0) && near(k->min_delta, 0.001), "dltMx / delta floor");
    CHECK(near(k->player_speed, 0.042 / 100.0 * 1000.0) && near(k->air_speed, 0.0047 / 100.0 * 1000.0) && near(k->crouch_slow, 0.3),
          "grA / arA / crchSpd defaults");
    CHECK(k->billboard_count == 6, "billboard posters");

    const Ruleset *rules = ruleset_v10();
    CHECK(near(rules->end_timer, num(config, "endTimer", NAN) / 1000.0) && near(GAME_END_TIMER, num(config, "endTimer", NAN) / 1000.0),
          "config.endTimer");
    CHECK(near(rules->end_anim, num(config, "endAnim", NAN) / 1000.0), "config.endAnim");
    CHECK(rules->trigger_action_limit == (int) num(config, "triggerActionLimit", -1) && rules->entity_limit == (int) num(config, "entityLimit", -1) &&
          rules->archetype_limit == (int) num(config, "archetypeLimit", -1), "triggerActionLimit / entityLimit / archetypeLimit");
    CHECK(near(rules->light_unit_factor, num(config, "lightIntensityScaling", NAN)), "lightIntensityScaling");

    // chat, lag compensation, net rates (shared.h)
    CHECK(NET_CHAT_MAX_CHARS == (int) num(config, "chatMaxLength", -1), "chatMaxLength");
    CHECK(near(NET_CHAT_INTERVAL, num(config, "chatInterval", NAN) / 1000.0), "chatInterval");
    CHECK(near(GAME_STATE_HISTORY, num(config, "stateHistory", NAN) / 1000.0), "stateHistory");
    CHECK(NET_STATE_SEND_HZ == (int) num(config, "serverSendFrequency", -1) && near(GAME_LAG_COMP_SEND_RATE, num(config, "serverSendFrequency", NAN)),
          "serverSendFrequency");
    CHECK(NET_RECON_SEND_HZ == (int) num(config, "serverSyncFrequency", -1), "serverSyncFrequency");
    CHECK(near(num(config, "serverTickRate", NAN), 1000.0 / num(config, "serverTickFrequency", NAN)), "serverTickRate = 1000 / serverTickFrequency");
    CHECK(GAME_DEATH_DELAY == (float) (num(config, "deathDelay", NAN) / 1000.0) && GAME_FEED_TIMER == (float) (num(config, "feedTimer", NAN) / 1000.0) &&
          GAME_REGEN_DELAY == (float) (num(config, "regenDelay", NAN) / 1000.0) && near(GAME_INTERACT_TIMER, num(config, "interactTimer", NAN) / 1000.0),
          "deathDelay / feedTimer / regenDelay / interactTimer");

    // map validation (map_validate.h) and triggers (triggers.h)
    static const struct { const char *js; double have; } limits[] = {
        {"objectLimit", MAP_V7_OBJECT_LIMIT}, {"objectLimitF", MAP_V7_OBJECT_LIMIT_FEATURED}, {"objectColLimit", MAP_V7_COLLIDABLE_LIMIT},
        {"objectColLimitF", MAP_V7_COLLIDABLE_LIMIT_FEATURED}, {"spawnLimit", MAP_V7_SPAWN_LIMIT}, {"signLimit", MAP_V7_SIGN_LIMIT},
        {"signTextLimit", MAP_V7_SIGN_TEXT_LIMIT}, {"signFontMin", MAP_V7_SIGN_FONT_MIN}, {"signFontMax", MAP_V7_SIGN_FONT_MAX},
        {"wheelLimit", MAP_V7_WHEEL_LIMIT}, {"lbLimit", MAP_V7_LEADERBOARD_LIMIT}, {"aiLimit", MAP_V7_AI_LIMIT}, {"botLimit", MAP_V7_BOT_LIMIT},
        {"interfaceLimit", MAP_V7_INTERFACE_LIMIT}, {"triangleLimit", MAP_V7_TRIANGLE_LIMIT}, {"guiLimit", MAP_V7_GUI_LIMIT},
        {"positionRange", MAP_V7_POSITION_RANGE}, {"triggerLimit", TRIGGER_LIMIT}, {"interfaceIDMax", TRIGGER_INTERFACE_MAX},
        {"teamLimit", TRIGGER_TEAM_LIMIT}, {"custValLimit", TRIGGER_V7_CUSTOM_VALUE_LIMIT}, {"emitterVolMin", TRIGGER_EMITTER_VOLUME_MIN},
        {"emitterVolMax", TRIGGER_EMITTER_VOLUME_MAX}, {"emitterRateMin", TRIGGER_EMITTER_RATE_MIN}, {"emitterRateMax", TRIGGER_EMITTER_RATE_MAX},
        {"triggerActionLimit", TRIGGER_V10_ACTION_LIMIT}, {"voiceChatMaxLength", VOICE_CHAT_MAX_LENGTH}, {"voiceRate", VOICE_RATE_MS},
    };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        CHECK(near(limits[i].have, num(config, limits[i].js, NAN)), "config.%s: KrunkNative %g, v10.0.7 %g", limits[i].js, limits[i].have,
              num(config, limits[i].js, NAN));
    }
    CHECK(TRIGGER_V7_CHAIN_LIMIT == TRIGGER_LIMIT, "triggerLimit (chain)");
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    CHECK(!strcmp(ruleset_current()->version, "10.0.7") && ruleset_current()->ranked.kr_mlt == 2.0f, "the v10 ruleset's ranked rules");
    const int weapon_count = rules_krunker_weapon_count(); // 31: Boulder and Compressor included
    cJSON *weapons = load_fixture("weapons");
    cJSON *classes = load_fixture("classes");
    cJSON *effects = load_fixture("effects");
    cJSON *streaks = load_fixture("streaks");
    cJSON *attach = load_fixture("attach");
    cJSON *scopes = load_fixture("aimRecticles");
    cJSON *upgrades = load_fixture("upgradeWeapons");
    cJSON *config = load_fixture("config");

    check_config(config);
    cJSON_Delete(config);

    CHECK(cJSON_GetArraySize(weapons) == weapon_count && weapon_count == WEAPON_V10_COUNT, "weapon count %d, v10.0.7 %d", weapon_count,
          cJSON_GetArraySize(weapons));
    for (int i = 0; i < weapon_count && i < cJSON_GetArraySize(weapons); i++) check_weapon_entry(rules_weapon(i), i, cJSON_GetArrayItem(weapons, i), 0);
    // the ids between Krunker's table and KrunkNative's are holes
    for (int i = weapon_count; i < WEAPON_ID_KN_BASE; i++) CHECK(!rules_weapon(i), "weapon id %d is a hole", i);

    // upgradeWeapons (krumMachine): the same checks on the upgraded table
    CHECK(cJSON_GetArraySize(upgrades) == WEAPON_COUNT, "upgraded weapon count");
    for (int i = 0; i < WEAPON_COUNT && i < cJSON_GetArraySize(upgrades); i++) check_weapon_entry(weapon_upgraded(i), i, cJSON_GetArrayItem(upgrades, i), 1);
    cJSON_Delete(upgrades);

    // v10.0.7's 16 classes; KrunkNative's from KN_CLASS_BASE (the Nettspend) come after them and are not compared
    CHECK(cJSON_GetArraySize(classes) == CLASS_COUNT && rules_class_count() >= CLASS_COUNT, "class count %d, v10.0.7 %d", CLASS_COUNT,
          cJSON_GetArraySize(classes));
    for (int i = 0; i < CLASS_COUNT && i < cJSON_GetArraySize(classes); i++) check_class(i, cJSON_GetArrayItem(classes, i));

    CHECK(cJSON_GetArraySize(effects) == EFFECT_COUNT, "effect count");
    for (int i = 0; i < EFFECT_COUNT; i++) {
        const cJSON *e = cJSON_GetArrayItem(effects, i);
        const EffectConfig *c = &g_effects[i];
        CHECK(!strcmp(c->name, str(e, "name")), "effect %d name", i);
        CHECK(!strcmp(c->key, str(e, "key")), "%s key", c->name);
        CHECK(near(c->timer, num(e, "timer", 0)) && near(c->def, num(e, "def", 0)) && c->max == (int) num(e, "max", 0) &&
              near(c->mlt, num(e, "mlt", 0)) && near(c->hp, num(e, "hp", 0)) && c->medal == (int) num(e, "mid", 0), "%s values", c->name);
    }

    CHECK(cJSON_GetArraySize(streaks) == STREAK_COUNT, "streak count");
    for (int i = 0; i < STREAK_COUNT; i++) {
        const cJSON *s = cJSON_GetArrayItem(streaks, i);
        const StreakConfig *c = &g_streaks[i];
        CHECK(!strcmp(c->name, str(s, "name")), "streak %d name", i);
        CHECK(c->slot == (int) num(s, "slot", -1) && c->kills == (int) num(s, "kills", -1), "%s slot / kills", c->name);
        // devOnly: the Refill only; 10.0.3 dropped the Bombardment's (deob-10.0.3.js:203406, v7.1.4 had it)
        CHECK(c->req_account == flag(s, "reqAccount") && c->no_customs == flag(s, "noCustoms") && c->dev_only == flag(s, "devOnly"),
              "%s reqAccount / noCustoms / devOnly", c->name);
    }

    CHECK(cJSON_GetArraySize(attach) == ATTACHMENT_COUNT, "attachment count");
    for (int i = 0; i < ATTACHMENT_COUNT; i++) {
        const cJSON *a = cJSON_GetArrayItem(attach, i);
        const WeaponAttachment *c = &g_attachments[i];
        CHECK(!strcmp(c->name, str(a, "name")) && !strcmp(c->src, str(a, "src")), "attachment %d name / src", i);
        CHECK(near(c->aim_offset_y, num(a, "aimOffY", 0)) && near(c->scale, num(a, "scale", 0)) && near(c->zoom_mlt, num(a, "zoomMlt", 0)) &&
              near(c->z_recoil, num(a, "zRec", 0)) && near(c->z_offset, num(a, "zOff", 0)) && near(c->exp_scale, num(a, "expScale", 0)) &&
              c->min_rec == (int) num(a, "minRec", 0), "%s values", c->name);
        // yOff {weapon id: offset}: per-weapon sight height, 0 for the others
        const cJSON *y_off = cJSON_GetObjectItemCaseSensitive(a, "yOff"), *entry;
        cJSON_ArrayForEach(entry, y_off) {
            CHECK(near(weapon_attachment_y_offset(c, atoi(entry->string)), entry->valuedouble), "%s yOff[%s]", c->name, entry->string);
        }
    }
    // Acog: zoom x1.35 on the AR (1.6 -> 2.16), yOff per weapon
    CHECK(near(weapon_aim_zoom(rules_weapon(1), &g_attachments[ATTACHMENT_ACOG]), 1.6 * 1.35) &&
          near(weapon_aim_zoom(rules_weapon(1), &g_attachments[0]), 1.6), "Acog zoom");
    CHECK(near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 1), 0.01) &&
          near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 14), -0.01) &&
          near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 3), 0.0), "Acog yOff");
    // The MG has no forceAttach (since v7): the player's sight applies
    CHECK(weapon_attachment(rules_weapon(6), ATTACHMENT_ACOG) == &g_attachments[ATTACHMENT_ACOG], "MG takes the Acog");
    // v10.0.7's skin rule (deob-10.0.7.js:173380): (weapon.attach != null || skin.attachBypass) && !skin.noAttach.
    // The Shotgun has no attach; Vince Mastery's attachBypass mounts the pick on it. noAttach takes it off any weapon.
    const Weapon *shotgun = rules_weapon(5), *ar = rules_weapon(1);
    CHECK(shotgun && !shotgun->has_attach && !weapon_attachment(shotgun, 0) && !weapon_skin_attachment(shotgun, 0, 0, 0), "Shotgun: no sight");
    CHECK(weapon_skin_attachment(shotgun, ATTACHMENT_ACOG, 1, 0) == &g_attachments[ATTACHMENT_ACOG] &&
          weapon_skin_attachment(shotgun, 0, 1, 0) == &g_attachments[0], "attachBypass: the Shotgun takes the pick");
    CHECK(!weapon_skin_attachment(shotgun, 0, 1, 1), "attachBypass with noAttach: none");
    CHECK(ar && ar->has_attach && weapon_skin_attachment(ar, ATTACHMENT_ACOG, 0, 0) == &g_attachments[ATTACHMENT_ACOG] &&
          !weapon_skin_attachment(ar, ATTACHMENT_ACOG, 0, 1) && !weapon_skin_attachment(ar, 2, 1, 0) && !weapon_skin_attachment(NULL, 0, 1, 0),
          "noAttach: the AR without its sight; an index past attach[]: none");
    // the ADS zoom follows: the Acog's zoomMlt on the bypass Shotgun, none under noAttach (updateAim, :174581-174583)
    CHECK(near(weapon_aim_zoom(shotgun, weapon_skin_attachment(shotgun, ATTACHMENT_ACOG, 1, 0)), shotgun->zoom * 1.35) &&
          near(weapon_aim_zoom(ar, weapon_skin_attachment(ar, ATTACHMENT_ACOG, 0, 1)), ar->zoom), "skin sight zoom");

    CHECK(cJSON_GetArraySize(scopes) == SCOPE_OVERLAY_COUNT, "scope overlay count");
    for (int i = 0; i < SCOPE_OVERLAY_COUNT && i < cJSON_GetArraySize(scopes); i++) {
        const cJSON *s = cJSON_GetArrayItem(scopes, i);
        CHECK(!strcmp(g_scope_overlays[i].name, str(s, "name")) && g_scope_overlays[i].id == (int) num(s, "id", -1), "scope overlay %d", i);
    }
    cJSON_Delete(scopes);

    cJSON *modes = load_fixture("modes");
    CHECK(cJSON_GetArraySize(modes) == MODE_COUNT, "mode count %d, v10.0.7 %d", MODE_COUNT, cJSON_GetArraySize(modes));
    for (int i = 0; i < MODE_COUNT && i < cJSON_GetArraySize(modes); i++) check_mode(&ruleset_v10()->modes[i], i, cJSON_GetArrayItem(modes, i));
    cJSON_Delete(modes);

    cJSON_Delete(weapons);
    cJSON_Delete(classes);
    cJSON_Delete(effects);
    cJSON_Delete(streaks);
    cJSON_Delete(attach);

    if (failures) {
        printf("weapons_v10_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("weapons_v10_test: %d checks OK\n", checks);
    return 0;
}
