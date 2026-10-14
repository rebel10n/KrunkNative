#pragma once
// Localisation: Krunker v10.0.2's i18n object (live bundle game.deob.js:197726-197788) in C.
//
// Tables: ui/i18n/<code>.json (tools/i18n-import: en = the bundle's inline translations.en, the
// others = the files v10 fetches as i18n/<code>.js?v=i41Nu), plus ui/i18n/kn/<code>.json, the
// strings Krunker Box Office added (`kn.*` keys; English in kn/en.json, see ui/i18n/README.md).
// Natively they are read from the page tree (ui_default_path(), KRUNKNATIVE_UI_DIR first: the
// launcher installs ui/ with the executables, so a language is never fetched natively; a missing
// file logs and falls back to en). On the web en is preloaded (MEMFS /ui/i18n, like the window
// templates: v10 has it inline) and the others are fetched over HTTP relative to the page when
// picked (emscripten_async_wget_data): the language switches when its table has arrived.
//
// v10's rules, kept:
//   - languages: config.langNames (17, i18n_lang()), the Settings > General > Localization
//     "Language" select (settings.list.lang, key settings.local.lang);
//   - locale: "en" unless the stored `krk_lang` (currentStorage) names a supported one
//     (setLocaleFromStorage); v10 never reads navigator.language or the OS locale;
//   - get(key) / t(key): the locale's value, else en's, else "UNLOCALIZED STRING" (an empty value
//     counts as missing: JS `||`); has(key): the same without the last fallback (NULL);
//   - placeholders: /\{(\d)\}/g -> args[d] ("" when absent). No plural forms, no named
//     placeholders, no RTL (none of the 17 languages is right-to-left).
// KrunkNative differences (intentional): picking a language re-renders the page in place (v10
// closes the open window and the rest follows on the next render); the menu markup, which v10
// renders through Svelte with $i18n.t(key), is the captured English page here, so
// i18n_translate_tree() maps its text back to keys (below).
#include <html_dom.h>
#include <stddef.h>

#define I18N_DEFAULT_LOCALE "en"
#define I18N_STORAGE_KEY "krk_lang"           // v10 saveLocale(): currentStorage.setItem("krk_lang", ...)
#define I18N_UNLOCALIZED "UNLOCALIZED STRING" // v10 get() with no value in either table
#define I18N_LANG_COUNT 17

typedef struct {
    const char *code; // v10's code ("kr" Korean, "jp" Japanese, "pr" Portuguese, "tg" Tagalog, "zhtr" Traditional Chinese)
    const char *name; // langNames: the option text (each language's own name, never translated)
    const char *bcp47; // the <html lang> value for the code (font selection on the web, CJK glyph forms natively)
} I18nLang;

const I18nLang *i18n_lang(int index);          // 0..I18N_LANG_COUNT-1 in v10's order, NULL outside
const I18nLang *i18n_lang_find(const char *code); // NULL when unsupported

// Loads en (and the stored locale) on first use; every other function calls it.
void i18n_init(void);
const char *i18n_locale(void);                  // the active locale ("en" ...)
// setLocale(code) + saveLocale(): an unsupported code is ignored. Returns 1 when the active
// locale changed now; 0 unchanged, or (web) the table is on its way: the change and the
// listeners follow when it arrives.
int i18n_set_locale(const char *code);

const char *i18n_t(const char *key);            // get(key) without arguments; never NULL
const char *i18n_has(const char *key);          // has(key): NULL when neither table has it
// get(key, args...): {0}..{9} replaced (argv[d], "" when d >= argc or argv[d] NULL). Returns out.
const char *i18n_format(char *out, size_t n, const char *key, int argc, const char *const *argv);
// The C UI's form: I18N("menu.btn.shop", "Shop") = i18n_tr(key, english): get(key), else the
// English the code was written with (no table: a broken install still reads). The English is
// v10's en value for Krunker keys and ui/i18n/kn/en.json's for kn.* keys; i18n_lint.py checks
// every pair against the tables, so the C keeps showing what each string says.
const char *i18n_tr(const char *key, const char *english);
#define I18N(key, english) i18n_tr((key), (english))
// I18N_F(buf, "timer.end", "Next Round in {0}", time): the same with placeholders
const char *i18n_trf(char *out, size_t n, const char *key, const char *english, int argc, const char *const *argv);
// replacePlaceholders on any text (a value the caller already looked up).
const char *i18n_replace(char *out, size_t n, const char *text, int argc, const char *const *argv);
#define I18N_ARGV(...) ((const char *const[]){__VA_ARGS__})
#define I18N_ARGC(...) ((int) (sizeof(I18N_ARGV(__VA_ARGS__)) / sizeof(const char *)))
// I18N_FMT(buf, "player.killstreak", name, streak): i18n_format into a char array
#define I18N_FMT(buf, key, ...) i18n_format((buf), sizeof(buf), (key), I18N_ARGC(__VA_ARGS__), I18N_ARGV(__VA_ARGS__))
#define I18N_F(buf, key, english, ...) i18n_trf((buf), sizeof(buf), (key), (english), I18N_ARGC(__VA_ARGS__), I18N_ARGV(__VA_ARGS__))

// The page's own markup (ui/index.html, the captured ui/windows templates, markup the C UI built
// from English literals): every text node and translatable attribute (title, placeholder, the
// value of button inputs) under `root` (0 = the document) whose text is a table value, in the
// language the page currently shows or in English, is replaced by that key's value in the
// active locale. Values with markup match per text segment (the same tag structure in both
// languages); values with {n} placeholders never match (the C UI formats those itself).
// Skipped: translate="no" / .notranslate subtrees, icon-font ligatures, script / style /
// textarea, and the containers of player-made text (chat, names, lists of servers / maps).
// Returns the number of strings replaced.
int i18n_translate_tree(HtmlElem root);

// Locale changes (after the table arrived): called in registration order, after the document
// was translated in place. Modules re-render what they build from i18n_t() (open window, HUD).
typedef void (*I18nListener)(void *user);
void i18n_on_change(I18nListener fn, void *user);
unsigned i18n_generation(void);                 // bumps on every locale change (cache keys)

// Tests / tools: how many keys the locale's table has (-1 not loaded); 1 when `code`'s table is
// loaded (or failed for good: then lookups fall back to en).
int i18n_table_size(const char *code);
int i18n_ready(const char *code);
void i18n_shutdown(void);                       // frees every table (process exit / tests)
