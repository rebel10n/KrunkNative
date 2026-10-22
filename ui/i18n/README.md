# ui/i18n — the string tables

Krunker v10's translations (the English table is v10.0.3's), one JSON object per language (`key -> value`, in the source file's
key order), read by `client/src/ui/i18n.c` (`client/include/i18n.h`) on both platforms.

## Provenance

| File | Source |
|---|---|
| `en.json` | The Krunker v10.0.3 client bundle's inline `translations.en` (`this.translations.en = this.parseText(...)`), extracted by `tools/ui-import/v10_bundle.js` from the bundle after a deobfuscation-only webcrack pass (doc/v10/ui-1003-diff.md). 2174 keys: the bundle's 2176 less `matchmaker.CaptchaRequired` and `matchmaker.RecaptchaFailed` (removed in 9234f121). The six keys 10.0.3 added (`purchase.*`, `settings.quality.noVideoSkins`, `windows.skins.painted`) are in the table; no C code shows the purchase or paint strings yet. |
| `es de nl kr pr jp fr fi zh zhtr pl hi ru it tg tr .json` | `https://krunker.io/i18n/<code>.js?v=i41Nu` (v10's `getTranslations(code)` URL), fetched 2026-10-14 through the proxy (GET, no login), converted by `tools/i18n-import/i18n_import.py convert` (`export default \`...\`;` modules in v10's "key value" line format: first space ends the key, a repeated key keeps its last value). 1008 to 1217 keys each; every other key falls back to English, as in v10. |
| `kn/en.json` | Strings Krunker Box Office added (`kn.*` keys), generated from the C (`I18N("kn....", "English")`) by `tools/i18n-import/i18n_lint.py --write-kn`. |
| `kn/<code>.json` | Translations of the `kn.*` strings. **None ship yet**: every `kn.*` string shows in English in every language. |

Refresh: `python3 -I tools/i18n-import/i18n_import.py fetch --out <dir>` then `convert --src <dir> --en ui/i18n/en.json --ui ui`,
then `fonts --ui ui` (the bundled font subsets follow the tables' characters).

## What v10 does, and what this port keeps

- **Languages** (`config.langNames`, `game.deob.js:18773`, the order of the Settings select):
  en English, es Español, de Deutsch, nl Nederlands, kr 한국어, pr Português, jp 日本語, fr Français,
  fi Suomi, zh 简体中文, zhtr 繁體中文, pl Polski, hi हिन्दी, ru Pусский, it Italiano, tg Tagalog, tr Türkçe.
  The option texts are each language's own name and are never translated. None is right-to-left: v10 has
  no RTL handling and neither does the port.
- **Setting**: Settings > General > Localization > "Language" (`settings.list.lang`, label key
  `settings.local.lang`), stored as `krk_lang` (`currentStorage`; natively
  `$XDG_CONFIG_HOME/krunknative/krk_lang.json`, web `localStorage`).
- **Default**: `en` unless `krk_lang` names a supported language (`setLocaleFromStorage`). v10 reads no
  `navigator.language` and no system locale, so neither does the port. `KRUNKNATIVE_LANG=<code>`
  (native, tests and tools) overrides the stored choice for one run.
- **Lookup** (`get` / `t`): the language's value, else English's, else `UNLOCALIZED STRING`; an empty
  value counts as missing. `has` / `h`: the same without the last fallback. In C: `I18N(key, english)`
  falls back to the English written next to the key instead of `UNLOCALIZED STRING`.
- **Placeholders**: `{0}`..`{9}` (`/\{(\d)\}/g`), replaced by the arguments, `""` when one is missing
  (`I18N_F`). There are no plural forms and no named placeholders in v10.
- **Markup**: some values hold HTML (`<br>`, `<span>`); they are written with innerHTML, as v10 does.
- **Switching**: v10 sets the locale and closes the open window. The port re-renders in place: the page's
  markup (`index.html`, the captured window templates) is re-translated (`i18n_translate_tree`), the open
  window (Settings excepted, which is translated in place) and the HUD are rebuilt by their modules'
  `i18n_on_change` listeners. No restart.
- **The captured markup**: v10's Svelte menu renders `$i18n.t(key)`; the port's `index.html` and
  `ui/windows/` are the English capture, so `i18n_translate_tree()` maps each text node / `title` /
  `placeholder` back to its key (values with markup per text segment, never values with `{n}`), and
  records the key on the owner element (`data-kn-i18n`) so later switches stay on the same key. Text v10
  writes as plain English literals (`"Quick Match"`, `"Lost Connection"`, ...) has no key and stays
  English in every language, as in v10.

## Adding strings

- A string v10 localises: `I18N("<v10 key>", "<its en value>")` / `I18N_F(buf, "<key>", "<en value>", args...)`.
- A string only Krunker Box Office has: `I18N("kn.<area>.<name>", "English")`, markup and handlers kept
  out of the text (`{0}`/`{1}` for tags), then `i18n_lint.py --write-kn`.
- `python3 -I tools/i18n-import/i18n_lint.py` checks every pair against the tables (`server/tests/i18n_e2e.py`
  runs it). `--find "text"` / `--key k` look keys up.
- Translations of `kn.*` strings go to `kn/<code>.json`. Never add machine translations silently: a file
  with unreviewed strings carries `"_review": "<who/what made them; not reviewed by a native speaker>"`
  at the top and the PR says so.

## Fonts (native engine)

The browser draws every language from its own fonts on the web. The native engine (FreeType, no system
font matching) uses the page's fonts, then a per-glyph fallback chain (`client/src/html/engine/text.c`):
`ui/css/fonts/kn-symbols.ttf`, a system Latin font (DejaVu / Liberation / Noto Sans), **the bundled script
faces below**, the system's Noto Sans CJK, a monospace font. The bundled faces make every shipped
language render on a machine without CJK / Devanagari fonts:

| File | Size | Source (OFL 1.1, `ui/css/fonts/kn-noto-OFL.txt`) | Characters |
|---|---|---|---|
| `kn-noto-cjk-jp.otf` | 109 KB | Noto Sans CJK JP 2.004, weight 400 | the `jp` table's kana / kanji + kana and CJK punctuation blocks (639) |
| `kn-noto-cjk-sc.otf` | 143 KB | Noto Sans CJK SC | the `zh` table's (787) |
| `kn-noto-cjk-tc.otf` | 170 KB | Noto Sans CJK TC | the `zhtr` table's (807) |
| `kn-noto-cjk-kr.otf` | 57 KB | Noto Sans CJK KR | the `kr` table's Hangul (539) |
| `kn-noto-devanagari.ttf` | 9 KB | Noto Sans Devanagari 2.006, weight 400 | the `hi` table's (67) |

Total 493 KB, made by `i18n_import.py fonts` (fontTools instancer + subsetter, no OpenType layout
tables: the engine has no shaper). Characters outside the subsets (player chat in these scripts) use the
system's fonts. The Language setting sets `<html lang>` (`ja`, `zh-CN`, `zh-TW`, `ko`, `hi`, ...), which
moves that language's face to the front of the chain natively (Han characters in Japanese, Simplified or
Traditional forms) and picks the browser's fonts on the web. `text-transform` case-maps Latin-1 /
Extended-A / Additional, Greek and Cyrillic.

## Gaps

- Devanagari is drawn without an OpenType shaper (the engine has no HarfBuzz): marks overlay their base
  and the vowel sign I is moved before its consonant cluster, but conjuncts, half forms and reph are not
  formed (a virama stays visible). Readable, not typographically correct. The web build is unaffected.
- Turkish dotted / dotless i under `text-transform: uppercase` follows the generic rule (i -> I).
- `kn.*` strings have no translations yet (English everywhere).
- The hub / editor / viewer pages get their markup translated, but the strings their own C writes are
  English.
- `<select>` / `<input>` widget text is painted by the widget code: the glyph probe
  (`html_dom_missing_glyphs`) does not see it.
- The markup mapping is by text: a string v10 hardcodes in English that happens to equal a table value
  (the clan window's "Members" / "Stats" / "Requests" tabs, the Change Controls tab names) is translated
  here although v10 leaves it English.
- Data the C UI takes from shared tables stays English: the Object Manager's tool names, hints and field
  labels (`shared/`), map / mode / class / weapon / skin names (v10 does not translate those either).
- Server settings in the Host window: in English the rows keep KrunkNative's own names (v7 wording);
  other languages show v10's `server.config.<var>` translation when the table has one.
- Text v10 writes as English literals stays English in every language (parity): Svelte labels ("Quick
  Match", "Find Game", "Guide", the Social hub), the error screen's "Lost Connection", the death and end
  screens' labels, the Battle Pass window, and so on.
