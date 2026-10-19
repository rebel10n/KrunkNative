#pragma once
// The krunker.io relay of the web build: whether the bridge (net-bridge) can carry this browser's
// krunker.io traffic. Asked once at start-up: GET <bridge>/healthz, whose JSON answers
// "krunkerRelay": true when the relay is live. An answer without the flag, or a failed request, counts
// as off. The bridge may not report it yet (the relay is not on main), so off is the default. Native
// builds join krunker.io directly: they never probe, and the answers are 0.
//
// The web build shows no krunker.io state any more (it plays krunk.io only: no Server Type row, no krunker.io
// tab, seek_prefs_servers): joining krunker.io from the browser is not built yet.

void krunker_relay_probe(void);   // start-up (backend_startup.c): web only, once; a no-op natively
int krunker_relay_on(void);       // 1 when the bridge reports the relay on (answered, flag true)
