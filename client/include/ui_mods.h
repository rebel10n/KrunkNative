#pragma once
// The Krunker v3.1.1 offline build's page mods (jail tree mod/mods.ts) that sit around the
// developer console (client/src/ui/ui_mods.c): window.devConsoleEnabled / devConsoleKey /
// cycleCommandKey, the rows mods.ts adds to the settings window ("Force Offline Mode",
// "Enable Developer Console (`)") and to the Change Controls window ("Developer Tools":
// Developer Console, Bhop Mode, Cycle Map (/cycle)) with their "Press any Key" capture,
// and the document keydown that toggles the console, sends "cycle" (F8) and cycles the
// bhop mode (B, ui_bhop_toggle). Binds are KeyboardEvent.code strings like the mod's.
#include <client.h>

int  ui_mods_dev_console_enabled(void);      // window.devConsoleEnabled (kro_setngss_devConsoleEnabled, default true)
const char *ui_mods_dev_console_key(void);   // window.devConsoleKey (krk_dev_console_bind, default "Backquote"; "" unbound)
int  ui_mods_force_offline(void);            // window.forceOfflineMode (krk_force_offline_sandbox, default true)

// GLFW key -> KeyboardEvent.code ("KeyA", "Digit1", "Backquote", "F8", ...), "" unknown.
const char *ui_mods_key_code(int glfw_key);
// GLFW key (+ GLFW mods) -> KeyboardEvent.key on a US layout ("a", "A", "1", "F9", "Enter"), "" unknown.
const char *ui_mods_key_value(int glfw_key, int mods);
int  ui_mods_code_matches(const char *code, int glfw_key); // event.code === code
const char *ui_mods_format_key_bind(const char *code); // formatKeyBind: "`", "F8", "B", "Unbound"

// mods.ts document keydown (+ the bind rows' capture). Called from ui_console_key_event
// with `typing` = focus is in another text field. 1 = consumed.
int  ui_mods_key_event(Client*, int key, int action, int mods, int typing);
int  ui_mods_capturing(void);                // a mod bind row waits for "Press any Key"
// Left button on the injected rows (data-kn-pointer): CSS px; 1 = consumed.
int  ui_mods_pointer(int down, float x, float y);
// syncDevConsoleInterface: (re)inserts the rows whenever their windows are shown and
// mirrors the switches. Per frame from ui_console_tick().
void ui_mods_tick(Client*);
