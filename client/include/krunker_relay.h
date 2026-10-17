#pragma once
// The krunker.io relay of the web build: whether the bridge (net-bridge) can carry this browser's
// krunker.io traffic. Asked once at start-up: GET <bridge>/healthz, whose JSON answers
// "krunkerRelay": true when the relay is live. An answer without the flag, or a failed request, counts
// as off. The bridge may not report it yet (the relay is not on main), so off is the default. Native
// builds join krunker.io directly: they never probe, and the answers are 0.
//
// The web Server Type row (seek_prefs.c) says plainly whether krunker.io is reachable. The web's seeks
// stay on Box Office servers either way (seek_prefs_servers): joining krunker.io from the browser is
// not built yet.

void krunker_relay_probe(void);   // start-up (backend_startup.c): web only, once; a no-op natively
int krunker_relay_on(void);       // 1 when the bridge reports the relay on (answered, flag true)
