// The flavor gameplay deltas (shared/include/flavor_rules.h): read the current FlavorConfig and apply
// a knob only when it is non-neutral, so FLAVOR_BASE is the unchanged base game byte-for-byte.
#include <flavor_rules.h>
#include <flavor.h>

float flavor_scale_damage(const float damage) {
    const float mult = flavor_current()->damage_mult;
    if (mult == 1.0f || damage <= 0.0f) return damage; // base / neutral, or no outgoing damage
    return damage * mult;
}

float flavor_scale_regen(const float regen) {
    const float mult = flavor_current()->regen_mult;
    if (mult == 1.0f) return regen; // base / neutral: unchanged (Strike's 0.0 zeroes it)
    return regen * mult;
}
