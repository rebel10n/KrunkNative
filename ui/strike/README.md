# Krunker Strike UI (ui/strike/): provenance

The page is FRVR Krunker Strike 1.37.2's own `public/` tree (index.html, `assets/index-CJ_IUjn5.css`, `icons.css`, `img/`, `css/fonts/`), shown instead of `ui/index.html` when the process flavor is Strike (`client/src/ui/strike_ui.c`, `strike_ui_page_dir()`). The base page `ui/index.html` is not touched.

- `screens/*.html`: transcribed from FRVR's own rendered DOM in the offline sandbox (`tools/uidiff/capture-strike.js`) by `tools/strike/strike_screens.py`. `tools/strike/strike_overrides.py` writes the generated block of `css/kn-strike-overrides.css`; the rest of that file is hand-written engine gaps (`GAP` comments).
- Reference captures: `doc/ref/krunker-strike-1.37.2/`.

## Fetches from FRVR's CDN

GET only, about one request per second, exact paths, no cookies, no request body, nothing executed. Origin: `https://cdn.frvr.com/games/krunkerfrvr/<path>?build=1.37.2`. FRVR's gateway, director and API were never contacted. The per-file log is `~/.cache/kn-strike/live/_manifest.jsonl` (path, URL, status, sha256, size, time).

- Run 1, 150 paths from the screens' references: every request returned HTTP 404 on the CDN. No Internet Archive fallback was fetched for these paths.
- Run 2: 87 files returned HTTP 200 (the table below). Their list was not kept with the cache, so the table is the record.

| path under cdn/1.37.2/ | URL | sha256 |
|---|---|---|
| img/medals/badge_0.png | https://cdn.frvr.com/games/krunkerfrvr/img/medals/badge_0.png?build=1.37.2 | 46070c27245b4733b9249a2fc94d94e8c92d8a8193f3a98f6c3d4093451285fa |
| img/scroll_2.png | https://cdn.frvr.com/games/krunkerfrvr/img/scroll_2.png?build=1.37.2 | b7b7991a4ccf4af3e8ef22ff2051b4a496f2ae315cff8741ef7eb49a07b6addb |
| img/shop/filter/vignette.png | https://cdn.frvr.com/games/krunkerfrvr/img/shop/filter/vignette.png?build=1.37.2 | 36ff680552699709ca88b098615362d732b97d3e12642d83f3cf6dcab35eb1e5 |
| textures/classes/icon_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_0.png?build=1.37.2 | 3beffb5746ec2dfe015c6ebf08885597ac5ed7afaac4041c3f8f8b9c3f055de1 |
| textures/classes/icon_1.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_1.png?build=1.37.2 | 6ee63a17145b551ddc9c8a0f8e9b66b0cb5b4f6568f4a9ea2fee15abd150c3e5 |
| textures/classes/icon_2.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_2.png?build=1.37.2 | 69afc67586e72b0219b7ba6d1240e0badffd446f706270d7bf90b695652ffa8c |
| textures/classes/icon_4.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_4.png?build=1.37.2 | 4e473ef466272df42cb5ec323b54cbf4299e5aeec9c4d678d374c390ba498b92 |
| textures/classes/icon_5.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_5.png?build=1.37.2 | 273a2c4127d4a29bc466c4ef7a024e26390767bdd7d808a1e52d1e1be5cabab1 |
| textures/classes/icon_7.png | https://cdn.frvr.com/games/krunkerfrvr/textures/classes/icon_7.png?build=1.37.2 | b1d6c90f7bd1e90ab50352f2a5edbdc245de4b1844e5f44e936aec290307f5ec |
| textures/flag_b.png | https://cdn.frvr.com/games/krunkerfrvr/textures/flag_b.png?build=1.37.2 | bde3ab9d976e2946887b992f6bcdec45b65a800329cbbc8eeba9512ee96bcf8c |
| textures/flag_r.png | https://cdn.frvr.com/games/krunkerfrvr/textures/flag_r.png?build=1.37.2 | e4763608b324b2934eb7f9980d4b921363932dbab3b7f8787ae589bab463b5e6 |
| textures/flag_w.png | https://cdn.frvr.com/games/krunkerfrvr/textures/flag_w.png?build=1.37.2 | a200bca3b107ce95220fbe0d28b6b755793f2bffa6e6f3dbc15604122d224267 |
| textures/jugg_1.png | https://cdn.frvr.com/games/krunkerfrvr/textures/jugg_1.png?build=1.37.2 | 0b4998b48cdbdfb7355fd48ec56dd290310c4b646dd9cc1fc33e1a2fac0d8b30 |
| textures/jugg_2.png | https://cdn.frvr.com/games/krunkerfrvr/textures/jugg_2.png?build=1.37.2 | 0568201c79bd6d9afca562540eee64fb454daac8bc550225743869a40c3f2fa4 |
| textures/melee/icon_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/melee/icon_0.png?build=1.37.2 | e5c29c73b8ff4a99c9b082835060ed6c7210efe9b83dbce544c74cb22047abeb |
| textures/objective_1.png | https://cdn.frvr.com/games/krunkerfrvr/textures/objective_1.png?build=1.37.2 | f7d8a7eaf99e7e07d1ffcec805413c7b06703d899eb77fb20f8e55287c17a597 |
| textures/objective_6.png | https://cdn.frvr.com/games/krunkerfrvr/textures/objective_6.png?build=1.37.2 | d3c72ba165eb8f750a2510ef3fda4c08ad27af503bdd83b66704f1ccfbe971a3 |
| textures/previews/cosmetics/11_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/11_default.png?build=1.37.2 | 421d846411b95ad6f637919ce575fc28773a5c156550d78b5cdedb038bf44405 |
| textures/previews/cosmetics/12_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/12_default.png?build=1.37.2 | 421d846411b95ad6f637919ce575fc28773a5c156550d78b5cdedb038bf44405 |
| textures/previews/cosmetics/14_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/14_default.png?build=1.37.2 | 011979fde8fac63727d3bdbad42333f056fd69c3d09523c846cd54b2431ba1d0 |
| textures/previews/cosmetics/15_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/15_default.png?build=1.37.2 | a780c0867041733a7286aa3e327c60598887b480cd11ca7d0eb13a67c98bb6fb |
| textures/previews/cosmetics/1_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/1_default.png?build=1.37.2 | a780c0867041733a7286aa3e327c60598887b480cd11ca7d0eb13a67c98bb6fb |
| textures/previews/cosmetics/2_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/2_default.png?build=1.37.2 | 011979fde8fac63727d3bdbad42333f056fd69c3d09523c846cd54b2431ba1d0 |
| textures/previews/cosmetics/3_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/3_default.png?build=1.37.2 | 1b88ee5eee20cb7ff2b20da95f0efbff28b4ada35817e693e26faf54e1281fd1 |
| textures/previews/cosmetics/5_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/5_default.png?build=1.37.2 | 5d058af8ac1ebf96d5ac465265875e4d5bd56533dd7c2d66200cce8972013df6 |
| textures/previews/cosmetics/6_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/6_default.png?build=1.37.2 | 12be7c531f1733a00c3be9501a4c402bc2509a7cc0452a12849bf57ef925b99a |
| textures/previews/cosmetics/7_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/7_default.png?build=1.37.2 | d5184b56541452c6cfe16951b8cdc06222d0526620c2e64c3d97e4a77952f01d |
| textures/previews/cosmetics/8_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/8_default.png?build=1.37.2 | 7ec7c5031e7e951c6bed02f4070cf2f0e33afda4c157e0da610a4cae18f63a78 |
| textures/previews/cosmetics/9_default.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/cosmetics/9_default.png?build=1.37.2 | 421d846411b95ad6f637919ce575fc28773a5c156550d78b5cdedb038bf44405 |
| textures/previews/weapons/weapon_1.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_1.png?build=1.37.2 | 375f6f529c8dd32ee04cbcbbe7d01cc0b667965eaa77dc0f44b16fcc0b06e32c |
| textures/previews/weapons/weapon_10.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_10.png?build=1.37.2 | 85cd72553f8f08ebbf0489d7591f0b416b725284216317c2387741e91429f143 |
| textures/previews/weapons/weapon_11.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_11.png?build=1.37.2 | eb80ffd24be49ee81d155781e7d5648f73699d4f92cf76aec1a7c1b47851f95d |
| textures/previews/weapons/weapon_12.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_12.png?build=1.37.2 | ff090661eed4dd68d85766ecf14685d9b1b04a2a4e177ef6e2b78dcfc616c753 |
| textures/previews/weapons/weapon_14.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_14.png?build=1.37.2 | d6ffe3b0e80423e15b8656cba50285d97270998220f69ce8563666fa281fde8f |
| textures/previews/weapons/weapon_15.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_15.png?build=1.37.2 | f2e29adf3b150c1cf4a0264a41ce0d535dafffc542fde1c33b13dad2bcd120df |
| textures/previews/weapons/weapon_16.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_16.png?build=1.37.2 | fd2ed8f585f8b9f492dddf1d419f0955fb9ed199f39a0655f740f7f4010b897a |
| textures/previews/weapons/weapon_17.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_17.png?build=1.37.2 | 7fd00fa654b1e6c18b262ba4a6ddb99ea6868c461b9a64269004b8ff6c4d0a65 |
| textures/previews/weapons/weapon_18.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_18.png?build=1.37.2 | a2ae317dbd3f0d035a8738fe9d134e01a98ae6fc3456a0e2eea5f7da40ca113b |
| textures/previews/weapons/weapon_19.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_19.png?build=1.37.2 | 37f7d8175f8bbfe92d5666f4911dc5a29516b063be496df6bf145430061069ef |
| textures/previews/weapons/weapon_2.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_2.png?build=1.37.2 | 1807f66e93e204eb729edd1c6343d4104121ecf0dbc017cde22edb34b286cf5f |
| textures/previews/weapons/weapon_20.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_20.png?build=1.37.2 | 10366e5abc7d9c56ca059cc81c84ff489e6aa7edb0c3ff110908f368070ff90c |
| textures/previews/weapons/weapon_21.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_21.png?build=1.37.2 | e01b307faaf26d85244926c2ad353d1a539a83651821a37f4ee9fe5a1b4bdb0f |
| textures/previews/weapons/weapon_22.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_22.png?build=1.37.2 | a9cad237f36d05e5fe4eff16ae3cbfb7180a1526fa12a157df2473cbfc2164ce |
| textures/previews/weapons/weapon_23.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_23.png?build=1.37.2 | 3251c90515e2b0d7b8297e5115ed94d8eb8252911e0c7a421631b5bcff9da285 |
| textures/previews/weapons/weapon_24.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_24.png?build=1.37.2 | 68e8672aa8f657529aff93975fa1ee552123f8802e49b4f9eb671161ec53671f |
| textures/previews/weapons/weapon_25.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_25.png?build=1.37.2 | 45f4419853ca960b1830f5a80ed43e43830a79fdffa65d0496c78909b5c734d6 |
| textures/previews/weapons/weapon_28.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_28.png?build=1.37.2 | 883f0fdf2f72e6dbf26d3f98cdf12ff8f77b032bc4994fa065c12564ba7638ea |
| textures/previews/weapons/weapon_29.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_29.png?build=1.37.2 | d755ede65b3668371b9dc8315e7708519818a971793cb7cbe5565e9f00534cc6 |
| textures/previews/weapons/weapon_3.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_3.png?build=1.37.2 | 3b06364def5e2c67d5714cff7c6a42def4ca27215980c2f619fc13e716776ee1 |
| textures/previews/weapons/weapon_4.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_4.png?build=1.37.2 | a3095cf999f7078cf9dc39750e4fc3d4f9abb04c03f2d952cbd4b81ecf9a1e7d |
| textures/previews/weapons/weapon_5.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_5.png?build=1.37.2 | 7c01cc17f026e3f9e12d097848f41b1ec3e0e694ade3573045f7e6a0e7a1c53a |
| textures/previews/weapons/weapon_6.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_6.png?build=1.37.2 | bb9638ffc86b0d0e0bd416e187433900cad4e8dd64d08243d3fc6499d98254c5 |
| textures/previews/weapons/weapon_7.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_7.png?build=1.37.2 | acff75217eef7c8e8a5838b977c62ea1beb2c56ec4c1c403d0fe6cd7105c1cd7 |
| textures/previews/weapons/weapon_8.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_8.png?build=1.37.2 | 43772351cc070904f57667644de2c5f34fb8adbfed1f4bc60c87bfb1feefb62f |
| textures/previews/weapons/weapon_9.png | https://cdn.frvr.com/games/krunkerfrvr/textures/previews/weapons/weapon_9.png?build=1.37.2 | 024eac810f17a917d2996ddf1674156a51f865e6693b794e16a407853c3ee1ef |
| textures/reticles/reticle_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/reticles/reticle_0.png?build=1.37.2 | 8308703f2d011c4add7416fd068078cc95bafd72d3bc5f4e40653938888ad042 |
| textures/scopes/scope_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/scopes/scope_0.png?build=1.37.2 | 8600321e88d46bd1aead688f06e6ce2f66a8cbcdc7bd0725d58bbd5997a1624b |
| textures/water_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/water_0.png?build=1.37.2 | ed4249cbd0b314173fe14938eaa39db504ce085d7d35b60a3878162c3358dfaf |
| textures/weapons/icon_0.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_0.png?build=1.37.2 | 5cc4fa2a4474a2c6a2c99c5a7cc4f63a6b8fe871b9fb0eeb1c50d098054d32c8 |
| textures/weapons/icon_1.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_1.png?build=1.37.2 | af22e8914cbe98ee028aac8034f5f9be9682cd09514e3c2385d774309da4c358 |
| textures/weapons/icon_10.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_10.png?build=1.37.2 | cfe8b991e5c01c2a5f8be3c1f06b07d67c1204130f95ad2ba1ec10d6f45e9626 |
| textures/weapons/icon_11.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_11.png?build=1.37.2 | 4a7fb2d7f8c2f88858ea3c7bd4976ba2001bb0f04e29ea826c7edf5370113300 |
| textures/weapons/icon_12.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_12.png?build=1.37.2 | 8771adfc785f571b6fbbc9965b55ea8d8c4bce0307101350e02a34f0ad7bc9dc |
| textures/weapons/icon_14.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_14.png?build=1.37.2 | 5b272284287516ee539d3bfbebf05a4f3c408babdf36793704b5452fe71d10ce |
| textures/weapons/icon_15.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_15.png?build=1.37.2 | 6d1fc551f0a06a0134e80dccfc1a14fab552e699e75fe825d6c8238bdc89ccf7 |
| textures/weapons/icon_16.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_16.png?build=1.37.2 | 1300743971eefe6daaff99cba915051b07d4cad729349f282e4361cfef902594 |
| textures/weapons/icon_17.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_17.png?build=1.37.2 | 168c97df1ad8c0b768f6181dd6f1d523dab3713814b79d14448f2a66a74285e6 |
| textures/weapons/icon_18.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_18.png?build=1.37.2 | 95af3bd0cac6badf96d003e06d0633a2b0df2f3be551e77c3055ae1457989b4f |
| textures/weapons/icon_19.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_19.png?build=1.37.2 | 4649046715f1131efe1851c6be1f258370749fdde8c1e57be5f719b49db188e6 |
| textures/weapons/icon_2.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_2.png?build=1.37.2 | ff80fd90b53901f5bbac93f8e0cfda1b12f8ea3031673a8130ccf7484829ba7e |
| textures/weapons/icon_20.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_20.png?build=1.37.2 | 3e39ec6107d4bd916f01eb1a9aa0b1e21d8078a31a693f263fcf0ba518e71f32 |
| textures/weapons/icon_21.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_21.png?build=1.37.2 | 97aafc94029a96d93c2a5212f852ee3927e049986dd45f71110179585e51d3a7 |
| textures/weapons/icon_22.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_22.png?build=1.37.2 | fda46629905728134fe3a4dab21fbe48d89bd13ac9181b65482b12ced2652640 |
| textures/weapons/icon_23.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_23.png?build=1.37.2 | 4aa3c2e37851c40d9ac93d6fab54643dcfec4c842f2b9053fae4f3847daa0082 |
| textures/weapons/icon_24.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_24.png?build=1.37.2 | c982213d40754ce4c194c0cbd40f0760b89b6083fca8274e2f8ed0ff8a1ba16a |
| textures/weapons/icon_25.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_25.png?build=1.37.2 | 506240e3cc89559e9f5d3417007feb7a185fa06d7bc87d0f59a6d3ddb273f235 |
| textures/weapons/icon_26.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_26.png?build=1.37.2 | 61845a9c1de8ee07e049e8a0c45624e3859523e78ba4058899d8e56efe0d281f |
| textures/weapons/icon_27.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_27.png?build=1.37.2 | de45566880e6e8fb82ea998626044b734c69bae8a18bf11b80ec70726d6f2638 |
| textures/weapons/icon_28.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_28.png?build=1.37.2 | dbbb4a7a4d6f58dfa30d74be4ea89e8e871f79067d5dcc1d038a0570c2b05771 |
| textures/weapons/icon_29.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_29.png?build=1.37.2 | 6dc5089c01a24dc0abeb9cb939d1e022e2b7e01da7500cdf7c69e0217428c6fc |
| textures/weapons/icon_3.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_3.png?build=1.37.2 | 94f91dc9c7b82dbc7b21425052d89806536e70d265324cb3f1e8bb465413819d |
| textures/weapons/icon_4.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_4.png?build=1.37.2 | 3e968dcb92edb5467f9e8f4cdbba301eb33da17e47649d756ab2048191bad84b |
| textures/weapons/icon_5.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_5.png?build=1.37.2 | e06ab1f842fd28fcbfff31dc8c3252432676df8051b3806170c7c7dd59c9efac |
| textures/weapons/icon_6.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_6.png?build=1.37.2 | dd2ff88f9ecd3ffb2ff21ed141b35c6567d8d7a8e110a475b9c6be73483caeab |
| textures/weapons/icon_7.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_7.png?build=1.37.2 | 0f0c22d1ad8639e1ea5057d0648ef42938604891f0837c32dc21aff6bc291d33 |
| textures/weapons/icon_8.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_8.png?build=1.37.2 | bf996405bf38ca6f45a9fabd84eae456c47310f90b82906413e97ae746411edb |
| textures/weapons/icon_9.png | https://cdn.frvr.com/games/krunkerfrvr/textures/weapons/icon_9.png?build=1.37.2 | e05eb215c15c633c6919f543dca7fb68c3838a535eb752a6d53d2ac25de4dde0 |

Vendored into this tree (`ui/strike/cdn/`): the class and weapon icons the HUD and class selection read (`./cdn/textures/...`). The other fetched files stay in the cache until a screen uses them.

## Wired screens

STORE (Extras, `fg`), SPIN (`SA`), CUSTOMIZE_CONTROLS (`wb`), LEADERBOARD_PLAYERS (`YB`) and POPUP (`In`), captured from FRVR's own
DOM (tools/strike/strike_screens.py, doc/ref/krunker-strike-1.37.2/strike-<name>.*), are driven by `client/src/ui/strike_menu.c`:
Extras opens STORE, whose Spins section lists the shop's wheels (OC) and opens SPIN (the wheel drawn into the page's canvas by
`client/src/ui/spin_wheel.c`); a currency's "+" opens STORE on that currency's section; Settings > Game > Customize Controls opens
CUSTOMIZE_CONTROLS, where controls drag and the layout is saved (`mobile_layouts`, `client/src/ui/strike_touch.c`, which the in-game
controls use) and Reset restores and saves the defaults. POPUP is the frame other views open in.

The screens run on the signed-in account's Strike economy (`client/src/ui/strike_econ.c` over kn_brain's `/strike`
gateway): the account box and the currency headers show its wallet; the Spins section lists the catalogue's shop wheels
(`strike_economy()`, the KR and spin-ticket ones) and SPIN's 1x / 10x buttons spin on kn_brain, the wheel turning onto
the result as `aue` does, then the reward screen (REWARD, `Es`) shows what it won; Market and Currency list the shop's
products and buy them (KR only): a paid market item opens the product popup (`U2`) first, a purchase shows
"Purchasing..." (`ws`) and then its rewards on the reward screen; Customize's category boxes open
ITEM_SELECTION_SCREEN (transcribed from `T5` / `rde`) to equip and unequip the account's items, and its Skin Color box
opens the same screen's colour list (FRVR's six `xe.skinColors`, saved as `skinColIndex`); the
Battle Pass Missions tab lists the day's missions (a completed one is claimed from its card); Profile's Rewards Track
comes from the game config and Claim Rewards claims every reached level one after another (`Fy` / `UU`): a pick-one
level asks first (REWARD_CHOICE, `QC`), each claim's rewards show on the reward screen; a leaderboard opens in the POPUP frame from
`strikeAction('leaderboard[:board]')` (FRVR shows player boards only in its tournament popup; no button reaches it).
A scenario with `state.strike.account` keeps the capture sandbox's wheels (`capture-strike.js` SPIN_WHEELS) so its
pixels compare with the references. client/tests/strike_econ_ui_e2e.py runs it against a local kn_brain;
server/tests/strike_menu_screens_e2e.py opens each one and goes back.

## Transcribed screens (no capture: not pixel-verified)

MISSION_TAB_SCREEN (`UD`, the Battle Pass screen's Missions tab), REWARD_TO_CLAIM (`XB`, a reward card in the POPUP
frame), MATCH_DETAILS_SCREEN (`GB` / `Ny`, the round end's podium button) and PROGRESSION_TRACK (`KU`, PROFILE's Rewards
Track) have no sandbox capture. Their templates (`screens/<VIEW_ID>.html`, hand-written, not generated: the generator
never touches them) are transcribed from the Mithril views in `assets/index-Cz2uYXJQ.js`, read as text: the same
elements, classes, inline styles and strings FRVR's code would render, with `<!-- part NAME -->` sections for the row /
card components that client/src/ui/strike_meta.c fills. Nothing about them is checked against FRVR's own pixels.
Notes from the bundle:
- 1.37.2's stylesheet has no `.frvr-mission-card` or `.reward-to-claim` rules (neither has 1.27.0's or 1.7.0's): FRVR
  shows the mission cards and the reward card's wrapper unstyled, and so does this page.
- `KU` renders nothing until the game config's (`/v1/game-config` `accountProgression`) item definitions are in. The
  live client has no source for that config, so the track shows only in a `--render-scenario` with
  `state.strike.progression` (tools/uidiff/scenarios/strike/progression-track.json: capture-strike.js's PROGRESSION
  fixture, sample data). The single-track form is the one without a premium track.
- `XB` appears in FRVR's tournament rewards popup; KrunkNative has no hosted tournaments, so only a scenario opens it.
- FRVR has no "rooms" view: its rooms are the Discord voice channel's members. Their UI is ported over
  kn_brain's friends (client/src/ui/strike_social.c): the menu friends strip and Friends List (`S5`: `ede` / `sde`,
  `screens/MENU_FRIENDS.html`), Private Match's invite list (`Vhe` / `zhe`) and its Private Match button (`Whe`),
  INVITE_NOTIFICATION (`N5`), REPORT_PLAYER (`Hy`) and the POPUP_SUCCESS / POPUP_ERROR alerts (`XA` / `p0` over `Oue`,
  `screens/POPUP_ALERT.html`); transcribed, not pixel-verified. Friends are kn_brain's mutual follows, a friend's game
  counts only when it is a Strike game (presence gameFlavor), invites are the brain's game:invite, reports its "rpt".
  Profile pictures are the default (class icon 0); the platform invite dialog ("Invite Friends", "+ More Friends") has
  no KrunkNative counterpart and is not shown / does nothing.

## Images fetched for the transcribed screens and the friends panel (2026-10-15)

GET only, one request every 4 to 5 s, exact paths, no cookies, nothing executed. The bundle loads `./img/...` from the
page's own origin, which for the web build is `https://krunkerstrike.frvr.com/` (the live site serves the current
build's `public/` tree, so these are the files it serves today, not a 1.37.2 snapshot); `N.assetsUrl()` paths come from
`https://cdn.frvr.com/games/krunkerfrvr/<path>?build=1.37.2`. The Internet Archive had no capture of any of them
(~/.cache/kn-wt/strike-ui-port-fetch/manifest.jsonl, NO_SNAPSHOT), and cdn.frvr.com answers 404 for the `./img/` ones.

| path under ui/strike/ | URL | sha256 |
|---|---|---|
| img/currencies/krunkies.png | https://krunkerstrike.frvr.com/img/currencies/krunkies.png | 15e051c19960bc9838e63851d99f462be285eafd0ea0cb3b0ef021f296be3621 |
| img/frvr-ui/krunkies.svg | https://krunkerstrike.frvr.com/img/frvr-ui/krunkies.svg | b1c2d55a02f13b32577dc92cc84c567cfb337801afce89d5c0b2e7ef63f967e1 |
| img/frvr-ui/xp-icon.svg | https://krunkerstrike.frvr.com/img/frvr-ui/xp-icon.svg | e409063952b8c06fd4a94ecf450d07f9dbdc49ea3b50cbd2d775ed570b3bc4ea |
| img/victory-screen-bg.png | https://krunkerstrike.frvr.com/img/victory-screen-bg.png | 6bed743d09925967b357dbf2743c04b127e8b389a55b63da155c359046d313f8 |
| img/battlepass/skull_bg.svg | https://krunkerstrike.frvr.com/img/battlepass/skull_bg.svg | 56b57272ff882538c0d831302cc48ede7bd055edb642b4a68c3cfa5bbc9172fd |
| img/frvr-ui/check.svg | https://krunkerstrike.frvr.com/img/frvr-ui/check.svg | 7787d59d9f7c010ca116962767b0e1f5d93dee74a47fc6bd4af031f0cf239d48 |
| img/frvr-ui/warning-octagon-red.svg | https://krunkerstrike.frvr.com/img/frvr-ui/warning-octagon-red.svg | 57fd7a9fa6a85987eec0222316e2f934b187bc1684fa97d299641ef3fbe9cf52 |
| img/frvr-ui/xp-symbol.svg | https://krunkerstrike.frvr.com/img/frvr-ui/xp-symbol.svg | 95ea4f0cd95b2ceffa44b8fa9e93723ed2f444b2fdc3424cc147607c4e0e8b22 |
| img/frvr-ui/spin-ticket-1.png | https://krunkerstrike.frvr.com/img/frvr-ui/spin-ticket-1.png | 3a75b0f23fbcac8ce4e30b767d14c935de1e8da3268296b6da9f5d72623a0295 |
| img/frvr-ui/spin-ticket-2.png | https://krunkerstrike.frvr.com/img/frvr-ui/spin-ticket-2.png | a250ac4e8353b254690045d9fcf3181f905af8df1f80703ac76d7a219f11889d |
| img/frvr-ui/spin-ticket-3.png | https://krunkerstrike.frvr.com/img/frvr-ui/spin-ticket-3.png | dd4a79c14d726bda8a81834cdc78dee488b57808916e0b82472bcd87132780b1 |
| img/frvr-ui/spin-ticket-many.png | https://krunkerstrike.frvr.com/img/frvr-ui/spin-ticket-many.png | 4669ea87aa2e19da228b6b72d112f798557c2cae56fd366293ba22b2cf6b7a8c |
| img/right-arrow.svg | https://krunkerstrike.frvr.com/img/right-arrow.svg | 26d64434fe89fa056049e6b11e2328bcf9fdbc96ca8055b76180152c3c6ddd2f |
| img/player-count-symbol.svg | https://krunkerstrike.frvr.com/img/player-count-symbol.svg | e09094f26e46b982f5967d5d60d0887d79faf662834b0a797819f662ea3b5754 |
| img/gather-crew-promo.png | https://krunkerstrike.frvr.com/img/gather-crew-promo.png | 0803626d82890b3437e804de8b81485010b70decbdb97a93ef4e21bdc372e45b |
| img/frvr-ui/close-symbol.svg | https://krunkerstrike.frvr.com/img/frvr-ui/close-symbol.svg | df5933b70e7541dcb4f56a3278063b6210d4ae2edd1351d97a57099b44fa9b1c |
| img/frvr-ui/info-checkmark.svg | https://krunkerstrike.frvr.com/img/frvr-ui/info-checkmark.svg | 841e0e0b844e2c153ceef37b392b3131d0af8fe8c945f83f9f0c24a43adf99df |
| img/frvr-ui/info-close.svg | https://krunkerstrike.frvr.com/img/frvr-ui/info-close.svg | 07b8ba53d4492c5a6652417d4ad93ed5969766a48d6379c8e932a42db1964375 |
| cdn/img/chals/icon_0.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_0.png?build=1.37.2 | f02ed7e7b4bb01330b77520694797074370dce0617a7cef0353ff8d7dfa5995b |
| cdn/img/chals/icon_1.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_1.png?build=1.37.2 | 9671f2d4184639b62362d2c24eeb7c8a76a2ea7e39116f0ad4aef1c7e248fa94 |
| cdn/img/chals/icon_2.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_2.png?build=1.37.2 | f8cac409e07539f0ab73e62430d59b506fcff739c11d6c9be9379e2fce04af0d |
| cdn/img/chals/icon_3.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_3.png?build=1.37.2 | 9a7ab01f64bbe8746d02ae3d977dc5a1b7a14aa521cfd5beb2c883c087d10050 |
| cdn/img/chals/icon_4.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_4.png?build=1.37.2 | ce6247c59c3015501397bdd916140ddfec3fb80d7aca0c5370f06638d3549c8f |
| cdn/img/chals/icon_5.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_5.png?build=1.37.2 | 1e845332fbbceb60e3d204d10abc0c542c6e3ce740cc452d27baaaff3d0d814f |
| cdn/img/chals/icon_6.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_6.png?build=1.37.2 | df95eba52f8e714a783555a274f755199925fd21b2438e4e10caf06400130b76 |
| cdn/img/chals/icon_7.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_7.png?build=1.37.2 | dd29569827d08bdf5ae04a43d463205992354b43ad72d16465bf8f2d30c4527e |
| cdn/img/chals/icon_8.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_8.png?build=1.37.2 | 9683b30002d74023292e73ed0f616dfee85486ae8f7a52a2b41571bef350d82f |
| cdn/img/chals/icon_9.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_9.png?build=1.37.2 | d5193c6b1839635ad2f72f4f8daa6f71487451d51d32a0383e31e1a7290eed71 |
| cdn/img/chals/icon_10.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_10.png?build=1.37.2 | f3c359d59f5a041679192413ebed4469d86a16a5ff0fb719889385bfa32754dc |
| cdn/img/chals/icon_11.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_11.png?build=1.37.2 | 2766feb1165ab25fefbb589f1b2ab9866bf775f1238556f6b057530335e5b98c |
| cdn/img/chals/icon_12.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_12.png?build=1.37.2 | d9a18a73a5d243b969473e2a6b554fd202ec825b560984a7debe65914a9c2338 |
| cdn/img/chals/icon_13.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_13.png?build=1.37.2 | 30f75f560da71fd3a017f5e9412c0fbc0a37231d4bac2e9b188f32d9a447566e |
| cdn/img/chals/icon_14.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_14.png?build=1.37.2 | 32df3005738da465188699df02a0c647b7cdb26268cb78cf40b374480bb5c65a |
| cdn/img/chals/icon_15.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_15.png?build=1.37.2 | 6f7e3261033bdcf88b5614244c9c8638391ed7f13a782f8d6357fb05e2f82aff |
| cdn/img/chals/icon_16.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_16.png?build=1.37.2 | e2fb0caae210d6ca00c689107841dc740f35720ae08147381e2adfe6665a44d7 |
| cdn/img/chals/icon_17.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_17.png?build=1.37.2 | 4fc07a2af192396e2d4b93fd17c5868761f98e5c3d28c93757dc5068b32c9ab3 |
| cdn/img/chals/icon_18.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_18.png?build=1.37.2 | ca9076fa4dd48f86ff96185902cec4251911111fad8a26df56a4ff0ded850632 |
| cdn/img/chals/icon_19.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_19.png?build=1.37.2 | 3d61d20da9d4fe1ace7b46e09f63c70b08bd866001c19397cf3dac5048db3c3d |
| cdn/img/chals/icon_20.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_20.png?build=1.37.2 | 40eeee4a077363979701d1c2e225bc0ce8d6b682fcd16e44a2f76e9cc6a5b6f7 |
| cdn/img/chals/icon_22.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_22.png?build=1.37.2 | af417595fd95fa7f3f6b88e2719eeda0ce422c046f0962b4e1e6fc62d90dd266 |
| cdn/img/chals/icon_23.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_23.png?build=1.37.2 | bd6f365e4e372a7b32f11a178fd594d9cf8f29232a8b3855c2e4bbd2a9c7c120 |
| cdn/img/chals/icon_24.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_24.png?build=1.37.2 | abe5715df1eed8b9a520b730b848d2900d2dcd0ecbb8a31ec83c586edd070c8c |
| cdn/img/chals/icon_25.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_25.png?build=1.37.2 | 2f6def7518aad9466761068e7e45fcebe1ee622784a492803ae249b7fc1b4675 |
| cdn/img/chals/icon_26.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_26.png?build=1.37.2 | ca1f7d871c931f218495c170cd1ac671f5ac849ffdcb9a8caf60885afa3c5213 |
| cdn/img/chals/icon_27.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_27.png?build=1.37.2 | b237d58e401f32b396e8da02778d033f1d854e574acaf4de79a3623253408606 |
| cdn/img/chals/icon_28.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_28.png?build=1.37.2 | 67fb1163ff00f2b6cb77ea2b0743464d54a916eeea30ef10928392da46ed04f1 |
| cdn/img/chals/icon_29.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_29.png?build=1.37.2 | 9b75264a4df72cb22677849eb0ecbadf783178aa28d68144f794eaba3ab2ea75 |
| cdn/img/chals/icon_30.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_30.png?build=1.37.2 | 11f1a86b34c0376efc78b37ad3bbbd1bfb398704dcc6bbab93d0496ce2effc69 |
| cdn/img/chals/icon_31.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_31.png?build=1.37.2 | c4456e167f356a9d8dfe4e19ece4f4d692bcf048e9dbf518bc3453a93491489f |
| cdn/img/chals/icon_32.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_32.png?build=1.37.2 | 6f4ab7e7d8d8455119661ee0b2b2ae5ff1b8085cbc7a797858a13c586acfd6ff |
| cdn/img/chals/icon_33.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_33.png?build=1.37.2 | 09bcaa26a0ba2a4240d3cdd4ed0457e39c885b138f07f782a8aaf04eea56e7da |
| cdn/img/chals/icon_34.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_34.png?build=1.37.2 | 0a762f79936d5022699fd8ebabcd7dc01a3e5b55dde153669849d3c87b3b4d29 |
| cdn/img/chals/icon_35.png | https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_35.png?build=1.37.2 | 7516820ddbcca9b46ff710f83c6cb015cfd1a62269f65f338eaa4783b75d1d31 |

Not found: img/chals/icon_21.png (HTTP 404, https://cdn.frvr.com/games/krunkerfrvr/img/chals/icon_21.png?build=1.37.2) (Krunker's own set has no icon_21 either).
Referenced by the transcribed code (pa()'s feature rewards and the premium reward) but missing from the snapshot: img/premium/premium-logo.png, img/features/no-ads-unsolicited-logo.svg, img/features/xp-boost-logo.svg; fetched in the reward-flow table below.

## The reward flow (transcribed, 2026-10-15)

REWARD (`VC`, exported as `Es`), REWARD_CHOICE (`NB` / `QC`), INPUT_BLOCKER (`ws`) and the product popup
(`U2` in the POPUP frame, `screens/STORE_CONFIRM_PURCHASE.html`) are transcribed from the bundle's Mithril views
(`client/src/ui/strike_reward.c`; no sandbox capture of them exists, so they are not pixel-verified against FRVR's
runtime). `tools/strike/static_ref.py` renders a scenario's DOM in Chrome with FRVR's stylesheets only (no script runs:
every `<script>` is stripped and the page carries `script-src 'none'`), which checks the native engine against a
browser on the same markup. Differences, also listed at the top of `strike_reward.c`: the item viewer is `r1`'s 2D
form (the store thumbnail); a class reward's character (`yc`) is not drawn; a Dye shows its thumbnail, not its shirt
colour; KrunkNative's rarities past legendary take the legendary art and item power; the golden spin offered after a
heroic spin (`cwe`) is not opened. The native engine has no SVG `feGaussianBlur` (nanosvg) and no
`mix-blend-mode`, so the rarity art's glows and colour tint are approximated in `css/kn-strike-overrides.css` (GAP).

Images fetched for it: GET only, one request every 4.5 s, exact paths, no cookies, nothing executed. They are
`./img/...` paths of the page itself, so they come from the live site's origin (today's files, not a 1.37.2 snapshot);
every request answered HTTP 200.

| path under ui/strike/ | URL | sha256 |
|---|---|---|
| img/reward-bg/reward-bg-lights-non-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-lights-non-item.svg | 917932c683a449f39eb53d57a505032a10767dfb091d591b029bc29e51c6a105 |
| img/reward-bg/reward-bg-color-non-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-color-non-item.svg | ad2755f2c98378fbb2274a4bf21138fe85bf53851c610c472e643856e263c0c2 |
| img/reward-bg/reward-bg-lights-uncommon-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-lights-uncommon-item.svg | af6abfc56acb6f0405e8ef7184b6c6a0b9f5b90f8c22ea96a8cf27b81b5f76f2 |
| img/reward-bg/reward-bg-color-uncommon-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-color-uncommon-item.svg | 32c07cbd353f449e073e52306171a1136747fbc8fc0571996bdc482077da42aa |
| img/reward-bg/reward-bg-lights-rare-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-lights-rare-item.svg | 9387bf0d73a357e93377083c901ac1fd90dd636d3618902513aebd2e1c0ce883 |
| img/reward-bg/reward-bg-color-rare-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-color-rare-item.svg | 00c16f97f92b1dae4ba1dc4a604d118a6820941761fef3a62efce93ba242ecbd |
| img/reward-bg/reward-bg-lights-epic-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-lights-epic-item.svg | a81b5b27855685653f6d7f4b5b7abc8f4bbd52d1983e343f4937923a2c8637b8 |
| img/reward-bg/reward-bg-color-epic-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-color-epic-item.svg | 031d07beafa6b08f23ae62c3dadd5236bf987e93330d23bb07749f13c8140a1a |
| img/reward-bg/reward-bg-lights-legendary-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-lights-legendary-item.svg | 965d011fb4b840cfc166150736b813bea73c3dca69d28a04318ab4786cabc561 |
| img/reward-bg/reward-bg-color-legendary-item.svg | https://krunkerstrike.frvr.com/img/reward-bg/reward-bg-color-legendary-item.svg | c92c317deecce9cd8072e3c84d8ca542fb60da149479fb6683619801554f6c91 |
| img/store/store-lightbox-background.png | https://krunkerstrike.frvr.com/img/store/store-lightbox-background.png | 57ad731f757092f08a3f608bb55c5c02356125ce1746503d26a8fd06c5cd2f10 |
| img/green-arrow-up.svg | https://krunkerstrike.frvr.com/img/green-arrow-up.svg | e4a8f73d8c351bf7c7d466163944bedb9bb62352f17d3b71ef4d16f08634b057 |
| img/golden-banner-stars.svg | https://krunkerstrike.frvr.com/img/golden-banner-stars.svg | bcd05a0c756f8fb8f013f7fde45fb2a434c3e61ecac253b54632e747aa7f1ce2 |
| img/golden-ticket.svg | https://krunkerstrike.frvr.com/img/golden-ticket.svg | 3b9ad0a818842f6331faf906fb7e2b021910c839e6baa83b3db6f6b830a20b46 |

## Images fetched for the product popup, the reward art and the navigation icons (2026-10-15, round 2)

The screens above reference these paths but the 1.37.2 `public/img/` snapshot does not contain them: the product popup's
background (`.inspect-product__item-background:before`, FRVR's `U2` view), the reward coin (`img.reward-image`, a
Krunkie coin), the feature rewards (`pa()`: XP boost, no ads), the premium reward, and the back / home buttons
(`.frvr-icon-back` / `.frvr-icon-home`, the REWARD, SPIN, STORE and ITEM_SELECTION buttons). GET only, one request every
4.6 s, exact paths, no cookies, nothing executed (the SVGs contain no script). Every request answered HTTP 200 with the
listed size and sha256; the files are copied unchanged. client/tests/strike_econ_ui_e2e.py checks that the client logs no
"file not found" for an img/ path during its run.

| path under ui/strike/ | URL | sha256 |
|---|---|---|
| img/features/no-ads-unsolicited-logo.svg | https://krunkerstrike.frvr.com/img/features/no-ads-unsolicited-logo.svg | 094c14e154d281519ddb010ae1ce457793ed75e825ed3dfd05f3fb7eb05d255a |
| img/features/xp-boost-logo.svg | https://krunkerstrike.frvr.com/img/features/xp-boost-logo.svg | 4c1290574dc094c9a7b5853b21f4ce1b9a8ce506e6cbc44878fd833892783ac2 |
| img/frvr-ui/icon-krunkie-coin.svg | https://krunkerstrike.frvr.com/img/frvr-ui/icon-krunkie-coin.svg | 4d44f994233fdcddd989159074c095e8159839578139e88b523b4a9bff310870 |
| img/premium/premium-logo.png | https://krunkerstrike.frvr.com/img/premium/premium-logo.png | d13e605248060367e84400e6fd020b2b2753bc64328f3e588e780da4097753c1 |
| img/frvr-ui/inspect-product-background.png | https://krunkerstrike.frvr.com/img/frvr-ui/inspect-product-background.png | c396f9d447a153406bc288938ec6f965067a4dca41a5815d1ce13d1e51c514de |
| img/frvr-ui/icon-home.svg | https://krunkerstrike.frvr.com/img/frvr-ui/icon-home.svg | 0af0fd26dbd034393d0bf5f830bc636d06e86b62d4799bd8a10ef3a10d295563 |
| img/frvr-ui/back-icon.svg | https://krunkerstrike.frvr.com/img/frvr-ui/back-icon.svg | 39d8ba0766e2f280e995a12675cfe06a09039cf952061b0ad9250ede35d326a1 |

Still missing from the snapshot, and not used by any markup this port has: the Market decorations (`market-bg.png`,
`market-dealer.png`, `market-complexity.png`: STORE.html's Market products carry no `store-market__decoration` markup, so the
decoration FRVR draws around them is not ported yet), the tournament, seasonal, MVP and end-screen premium art, the speech
bubbles and the watch-ad icon. Their CSS references stay unresolved, as they were before.
