#pragma once
// Gameplay deltas a product flavor (shared/include/flavor.h) layers over the Krunker ruleset,
// centralised so every consumer reads the current FlavorConfig the same way. Each helper is a strict
// no-op for FLAVOR_BASE and for any flavor whose knob is still at its neutral value, so a base build
// (and the official/web packages) runs the unchanged base-game maths byte-for-byte: the identity
// path takes no multiply at all. Gate Strike-only behaviour here, never by indexing a flavor table.
//
// Pure C; part of the shared core linked by server_bin, client_bin and the tests.

// Scale dealer-dealt outgoing COMBAT damage by the flavor's damage_mult (Strike's hardcore feel).
// `damage` is the positive, pre-round damage one player deals another; healing (<= 0) and
// environmental damage with no dealer are not scaled, so callers pass only dealer-dealt positive
// damage. Returns `damage` unchanged when the multiplier is neutral.
float flavor_scale_damage(float damage);

// Scale a passive health-regen amount by the flavor's regen_mult (Strike disables passive regen with
// 0.0). Composes with the mode regenMlt already applied by the caller. Returns `regen` unchanged when
// the multiplier is neutral; a 0.0 multiplier yields 0 so the caller's `regen > 0` guard skips it.
float flavor_scale_regen(float regen);
