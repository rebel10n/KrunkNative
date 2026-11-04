# rt-materials: the ray tracer's material classes

`assets/materials/rt_materials.json` gives map textures, prop models, weapons, player parts and
paint shaders a physical material class (metalness, roughness, reflectivity) for the ray tracer
(`client/src/rt/rt_materials.c`, `doc/raytracing.md`). Krunker's content carries no material maps,
so the table is hand-authored from contact sheets of the actual textures. It is read only while
ray tracing is on; `rtMode 0` never sees it.

## Regenerating the contact sheets

```sh
python3 tools/rt-materials/contact_sheet.py            # -> tools/rt-materials/sheets/*.png, draft.json
python3 tools/rt-materials/contact_sheet.py --out ~/.cache/kn-rt-sheets --draft ~/.cache/draft.json
```

Needs Pillow (`python3 -m pip install pillow`); without it the sheets come from ImageMagick's
`magick montage` and carry no proposed-class labels. Three sheets: `map-and-props.png` (every
`assets/textures/*.png`: the v7 map texture table's `<src>_<variant>` files and the prefab models'
`<model>[_<variant>]` textures, `_e` emissive maps left out), `weapons.png` (`weapon_<id>.png`, one
per weapon src) and `melee.png` (`melee_<id>.png`). Each cell shows the thumbnail, the file name and
the tool's proposal: a name keyword (`metal`, `brick`, `crate`, `neon`, ...) first, else colour
statistics (desaturated mid grey leans metal, warm brown wood, saturated bright emissive, alpha
glass). `draft.json` groups files into families (`brick_0_7` -> `brick_0*`) with the majority
proposal. The proposals are a starting point only: the shipped table was corrected by eye (weapon
atlases are too busy for colour statistics, "lines Pipes" is metal, "metal Brick" is stone, ...).

`sheets/` and `draft.json` are git-ignored.

## JSON conventions

```
{ "classes":  { "<class>": { "metalness": 0..1, "roughness": 0..1, "reflect": 0..4, "note": "..." } },
  "textures": { "<texture base name or glob>": "<class>" },
  "models":   { "<model path glob relative to assets>": "<class>" },
  "weapons":  { "<weapon id>": "<class>" },
  "parts":    { "player_body" | "player_head" | "player_arms" | "player_legs" | "cosmetic": "<class>" },
  "paint":    { "<paintShader 0..5>": "<class>" },
  "default":  "<class>" }
```

- **Precedence**: paint > weapons > models > textures > parts > default. A held gun is classed by its
  weapon id; the `weapon_<id>` texture rules only reach pickups, showcases and other model paths.
- **Texture keys** are the texture file's base name without directory or extension: `wall_0`,
  `wall_0_3` (variant 3 of the WALL texture), `weapon_2`, `melee_0_5`, `crate_0_1`.
- **Model keys** are assets-relative paths: `models/weapons/weapon_2.obj`, `models/hats/*`.
- **Globs** use `*` only. Within a section the **first matching rule in file order wins**, so an
  exception must sit above the glob it overrides (`"metal_0_1": "stone"` before `"metal_0*": "metal"`).
- Keys starting with `_` (`_doc`, `_comment...`) are ignored: their values are not class names.
- Unknown classes drop the rule; an empty or missing table turns classes off (logged once).
- Weapon ids are the v10 table's (`shared/src/rules/v10/weapons_v10.c`); Box Office weapons start
  at 64. Paint shader ids: 0 Spectrum, 1 Synth, 2 Gold, 3 Silver, 4 Prism, 5 Codefall.
- Classes: `metal`, `dark_metal`, `chrome`, `gold`, `paint_fx`, `wood`, `cloth`, `skin`, `plastic`,
  `stone` (default), `glass`, `window`, `water`, `emissive`, `foliage`. Add a class by adding an entry;
  `reflect` is the tracer's reflection intensity (0 = no reflection ray), `roughness` the GGX roughness.

Edit the table live: the console command `rt_materials_reload` re-reads it. Check the log line
`[rt] materials: N classes, N texture, N model, N weapon, N part, N paint rules` after a load.
