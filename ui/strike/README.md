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

## Not yet ported

Templates captured from FRVR's own DOM (tools/strike/strike_screens.py, doc/ref/krunker-strike-1.37.2/strike-<name>.*):
STORE (Extras, `fg`), SPIN (`SA`), CUSTOMIZE_CONTROLS (`wb`, Settings > Customize Controls), LEADERBOARD_PLAYERS (`YB`)
and POPUP (`In`, the empty popup frame). They are not wired: no C driver fills their live data yet, and
`strike_menu_template()` (client/src/ui/strike_menu.c) still returns NULL for them, so Extras, Customize Controls
and the other buttons that open them do nothing, as before. The captures use sample data (guest account,
fake tournament rows), so they are not shown to players.

Blocked captures (no template yet): MISSION_TAB_SCREEN (`UD`, the daily missions popup), REWARD_TO_CLAIM (`XB`) and
MATCH_DETAILS_SCREEN (`Ny` / `GB`). The capture states exist in tools/uidiff/capture-strike.js; they did not produce
references in this run (the duplicate writer and the load limit stopped the run). The seasonal progression track
(PROGRESSION_TRACK) and the rooms are not captured.

Images the new templates reference that are not in this tree: img/currencies/krunkies.png, img/frvr-ui/krunkies.svg,
img/frvr-ui/xp-icon.svg, img/victory-screen-bg.png. They are fetched from the Internet Archive's
discordsays.com capture (`cdnfrvr/games/krunkerfrvr/<path>`, the bundle's discord branch), one request per 25 s, GET
only; the rows are logged in the fetch job's manifest, not vendored here until they are checked.
