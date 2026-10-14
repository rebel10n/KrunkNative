// WP-B table test: every Krunker v7.1.4 class, weapon, effect, streak and attachment field the
// native tables carry, against the JSON fixtures tools/kref/v7-dump.js writes from the v7 source
// (shared/tests/data/v7/*.json). Times are compared in seconds (JS ms / 1000), recoil per s
// (JS per ms * 1000), as KrunkNative stores them. Run from the repository root.
//
// With the argument "v10" it checks the v10.0.2 ruleset instead: the same checks on
// shared/tests/data/v10/{weapons,classes}.json (tools/kref/v10-fixtures.py from the statically
// extracted v10 bundle; classConfig / weaponConfig hold the same keys in v10, so the same field
// table applies) and the v10 ruleset's own weapons / classes. The v10 class table carries the
// v10-only cls hurtboxScale (1.0) and the v10 weapon table the v10-only Boulder / Compressor.
#include <shared.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *g_version = "v7"; // "v7" (default) or "v10": which ruleset and fixtures
static int g_weapon_count = WEAPON_COUNT;

static cJSON *load_fixture(const char *name) {
    // v10 has its own weapons.json / classes.json; the effects, streaks, attachments, scopes and
    // upgradeWeapons tables are the v7 ones under v10 too (the v10 extractor diff finds no change;
    // upgrades: v10's table only renames the entries and drops their anim / funcOnHit, and
    // weapon_upgraded() builds from v7's base table, shared/src/weapons/upgraded.c), so those
    // fixtures come from data/v7.
    static const char *const from_v7[] = {"effects", "streaks", "attach", "aimRecticles", "upgradeWeapons"};
    const char *dir = g_version;
    for (size_t i = 0; i < sizeof(from_v7) / sizeof(from_v7[0]); i++) {
        if (!strcmp(name, from_v7[i])) dir = "v7";
    }
    char path[256];
    for (int up = 0; up < 2; up++) {
        snprintf(path, sizeof(path), "%sshared/tests/data/%s/%s.json", up ? "../" : "", dir, name);
        size_t length;
        unsigned char *data;
        if (read_file(path, &data, &length)) continue;
        cJSON *parsed = cJSON_Parse((char *) data);
        free(data);
        if (parsed) return parsed;
    }
    printf("missing fixture %s/%s.json (run from the repository root; v7: node tools/kref/v7-dump.js,"
           " v10: tools/kref/v10-fixtures.py)\n", dir, name);
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

// JS key -> float field of Weapon, with the unit conversion.
typedef enum { UNIT_SAME, UNIT_MS, UNIT_X1000 } Unit;
typedef struct {
    const char *js;
    size_t offset;
    Unit unit;
    double def;   // the C value when the JS key is absent (or the v7 default the C table leaves at 0)
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
};

static double js_to_c(const double v, const Unit unit) {
    return unit == UNIT_MS ? v / 1000.0 : unit == UNIT_X1000 ? v * 1000.0 : v;
}

static void check_weapon_entry(const Weapon *w, const int id, const cJSON *js, const int upgraded) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(js, "name"));
    CHECK(w && name && !strcmp(w->name, name), "weapon %d is %s, v7 %s", id, w ? w->name : "(null)", name ? name : "?");
    if (!w) return;

    const char *src = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(js, "src"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(js, "icon"));
    const char *sound = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(js, "sound"));
    CHECK((!src && !w->src) || (src && w->src && !strcmp(src, w->src)), "%s src", w->name);
    CHECK(icon && w->icon && !strcmp(icon, w->icon), "%s icon", w->name);
    // sound: no v7 weapon has an empty one; v10's Boulder ships 'sound': "" (it never fires), which
    // KrunkNative keeps as NULL (no sound to play)
    CHECK((sound && w->sound && !strcmp(sound, w->sound)) || (sound && !sound[0] && !w->sound), "%s sound %s (v7 %s)", w->name,
          w->sound ? w->sound : "(null)", sound ? sound : "?");

    for (size_t i = 0; i < sizeof(k_weapon_floats) / sizeof(k_weapon_floats[0]); i++) {
        const FloatField *f = &k_weapon_floats[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(js, f->js);
        const double expect = cJSON_IsNumber(v) ? js_to_c(v->valuedouble, f->unit) : f->def;
        const float have = *(const float *) ((const char *) w + f->offset);
        CHECK(near(have, expect), "%s %s = %g, v7 %g", w->name, f->js, (double) have, expect);
    }

    CHECK(w->ammo == (unsigned int) num(js, "ammo", 0), "%s ammo %u, v7 %g", w->name, w->ammo, num(js, "ammo", 0));
    CHECK(w->shots == (unsigned int) num(js, "shots", 0), "%s shots", w->name);
    CHECK(w->burst == flag(js, "burst") && (!w->burst || w->burst_count == (unsigned int) num(js, "burstC", 0)), "%s burst", w->name);
    CHECK(w->type == (int) num(js, "type", 0), "%s type", w->name);
    CHECK(w->min_rec == (int) num(js, "minRec", 0), "%s minRec", w->name);
    if (!upgraded) CHECK(w->mag_reserve == (int) num(js, "magReserve", 0), "%s magReserve", w->name); // upgrades: not ported
    const cJSON *kill = cJSON_GetObjectItemCaseSensitive(js, "kill");
    CHECK(w->kill_score == (cJSON_IsArray(kill) ? (int) cJSON_GetArrayItem(kill, 1)->valuedouble : 0), "%s kill score %d", w->name, w->kill_score);
    CHECK(near(w->leg_mlt ? w->leg_mlt : 0.5, num(js, "lsMlt", 0.5)), "%s lsMlt %g", w->name, (double) w->leg_mlt);
    CHECK(near(w->move_spread ? w->move_spread : 1.0, num(js, "movSpread", 1.0)), "%s movSpread %g", w->name, (double) w->move_spread);
    // chrgMlt: 0 in the C table means the 0.9 default. v10's Compressor ships an explicit 'chrgMlt': 0
    // (v10:101887) but never charges (chrgTime 0), so the two encodings cannot differ there.
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
    // v10.0.2 weaponConfig keys (no v7 weapon has them, so these are no-ops under v7)
    CHECK(!!w->no_dmg == flag(js, "noDmg") && !!w->phys_pull == flag(js, "physPull") && !!w->non_obj_impulse == flag(js, "nonObjImpulse") &&
          !!w->r_shoot == flag(js, "rShoot"), "%s noDmg / physPull / nonObjImpulse / rShoot", w->name);
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

    // cSpread: both v7 patterns, flattened
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
        CHECK(!w->custom_spread, "%s has a cSpread v7 lacks", w->name);
    }
}

static void check_weapon(const int id, const cJSON *js) {
    check_weapon_entry(rules_weapon(id), id, js, 0);
}

static void check_class(const int id, const cJSON *js) {
    const ClassConfig *c = &rules_classes()[id];
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(js, "name"));
    CHECK(name && c->name && !strcmp(c->name, name), "class %d is %s, v7 %s", id, c->name, name ? name : "?");
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
    // v10.0.2 classConfig (v10:17699): the hit boxes' scale. The v7 fixtures have no such key, so the
    // check is a no-op there (v7's class table leaves the field 0 = 1)
    CHECK(near(c->hurtbox_scale ? c->hurtbox_scale : 1.0, num(js, "hurtboxScale", 1)), "%s hurtboxScale %g", c->name, (double) c->hurtbox_scale);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "v10")) {
        g_version = "v10";
        CHECK(ruleset_set_current(RULESET_V10_0_2), "the v10 ruleset is built");
        CHECK(rules_hit_model() == HIT_MODEL_V10 && ruleset_current()->ranked.kr_mlt == 2.0f, "the v10 ruleset's hit model / ranked rules");
        g_weapon_count = rules_krunker_weapon_count(); // 31: Boulder and Compressor included
    } else {
        CHECK(rules_hit_model() == HIT_MODEL_V7 && ruleset_current()->ranked.kr_mlt == 1.0f, "the v7 ruleset's hit model / ranked rules");
    }
    cJSON *weapons = load_fixture("weapons");
    cJSON *classes = load_fixture("classes");
    cJSON *effects = load_fixture("effects");
    cJSON *streaks = load_fixture("streaks");
    cJSON *attach = load_fixture("attach");
    cJSON *scopes = load_fixture("aimRecticles");
    cJSON *upgrades = load_fixture("upgradeWeapons");

    CHECK(cJSON_GetArraySize(weapons) == g_weapon_count, "%s weapon count %d, %d", g_version, g_weapon_count, cJSON_GetArraySize(weapons));
    for (int i = 0; i < g_weapon_count && i < cJSON_GetArraySize(weapons); i++) check_weapon(i, cJSON_GetArrayItem(weapons, i));

    // upgradeWeapons.js (krumMachine): the same checks on the upgraded table
    CHECK(cJSON_GetArraySize(upgrades) == WEAPON_COUNT, "upgraded weapon count");
    for (int i = 0; i < WEAPON_COUNT && i < cJSON_GetArraySize(upgrades); i++) check_weapon_entry(weapon_upgraded(i), i, cJSON_GetArrayItem(upgrades, i), 1);
    cJSON_Delete(upgrades);

    CHECK(cJSON_GetArraySize(classes) == CLASS_COUNT, "class count");
    for (int i = 0; i < CLASS_COUNT && i < cJSON_GetArraySize(classes); i++) check_class(i, cJSON_GetArrayItem(classes, i));

    CHECK(cJSON_GetArraySize(effects) == EFFECT_COUNT, "effect count");
    for (int i = 0; i < EFFECT_COUNT; i++) {
        const cJSON *e = cJSON_GetArrayItem(effects, i);
        const EffectConfig *c = &g_effects[i];
        CHECK(!strcmp(c->name, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "name"))), "effect %d name", i);
        CHECK(!strcmp(c->key, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "key"))), "%s key", c->name);
        CHECK(near(c->timer, num(e, "timer", 0)) && near(c->def, num(e, "def", 0)) && c->max == (int) num(e, "max", 0) &&
              near(c->mlt, num(e, "mlt", 0)) && near(c->hp, num(e, "hp", 0)) && c->medal == (int) num(e, "mid", 0), "%s values", c->name);
    }

    CHECK(cJSON_GetArraySize(streaks) == STREAK_COUNT, "streak count");
    for (int i = 0; i < STREAK_COUNT; i++) {
        const cJSON *s = cJSON_GetArrayItem(streaks, i);
        const StreakConfig *c = &g_streaks[i];
        CHECK(!strcmp(c->name, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "name"))), "streak %d name", i);
        CHECK(c->slot == (int) num(s, "slot", -1) && c->kills == (int) num(s, "kills", -1), "%s slot / kills", c->name);
        CHECK(c->req_account == flag(s, "reqAccount") && c->no_customs == flag(s, "noCustoms") && c->dev_only == flag(s, "devOnly"), "%s flags", c->name);
    }

    CHECK(cJSON_GetArraySize(attach) == ATTACHMENT_COUNT, "attachment count");
    for (int i = 0; i < ATTACHMENT_COUNT; i++) {
        const cJSON *a = cJSON_GetArrayItem(attach, i);
        const WeaponAttachment *c = &g_attachments[i];
        CHECK(!strcmp(c->name, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "name"))) &&
              !strcmp(c->src, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "src"))), "attachment %d name / src", i);
        CHECK(near(c->aim_offset_y, num(a, "aimOffY", 0)) && near(c->scale, num(a, "scale", 0)) && near(c->zoom_mlt, num(a, "zoomMlt", 0)) &&
              near(c->z_recoil, num(a, "zRec", 0)) && near(c->z_offset, num(a, "zOff", 0)) && near(c->exp_scale, num(a, "expScale", 0)) &&
              c->min_rec == (int) num(a, "minRec", 0), "%s values", c->name);
    }
    // Acog: zoom x1.35 on the AR (1.6 -> 2.16), yOff per weapon
    CHECK(near(weapon_aim_zoom(rules_weapon(1), &g_attachments[ATTACHMENT_ACOG]), 1.6 * 1.35) &&
          near(weapon_aim_zoom(rules_weapon(1), &g_attachments[0]), 1.6), "Acog zoom");
    CHECK(near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 1), 0.01) &&
          near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 14), -0.01) &&
          near(weapon_attachment_y_offset(&g_attachments[ATTACHMENT_ACOG], 3), 0.0), "Acog yOff");
    // The MG lost forceAttach in v7: the player's sight applies
    CHECK(weapon_attachment(rules_weapon(6), ATTACHMENT_ACOG) == &g_attachments[ATTACHMENT_ACOG], "MG takes the Acog");

    CHECK(cJSON_GetArraySize(scopes) == SCOPE_OVERLAY_COUNT, "scope overlay count");
    for (int i = 0; i < SCOPE_OVERLAY_COUNT && i < cJSON_GetArraySize(scopes); i++) {
        const cJSON *s = cJSON_GetArrayItem(scopes, i);
        CHECK(!strcmp(g_scope_overlays[i].name, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "name"))) &&
              g_scope_overlays[i].id == (int) num(s, "id", -1), "scope overlay %d", i);
    }
    cJSON_Delete(scopes);

    cJSON_Delete(weapons);
    cJSON_Delete(classes);
    cJSON_Delete(effects);
    cJSON_Delete(streaks);
    cJSON_Delete(attach);

    if (failures) {
        printf("weapons_v7_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("weapons_v7_test: %d checks OK\n", checks);
    return 0;
}
