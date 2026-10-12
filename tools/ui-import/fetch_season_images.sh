#!/usr/bin/env bash
# The season / challenge images Krunker v7.1.4 loaded from its CDN (UTILS.assetsUrl) and its
# reconstructed tree lacks, fetched into ui/ (committed like the rest of ui/img):
#   - img/chals/icon_<IC>.png (challenge cards: windows[32].genChallengeCard,
#     windows[5].genDailyCard, the end screen's .endChalImg), img/chals/hed_{1,2,4}.png (the
#     Challenges window's section headers), img/chals/<weapon.src | challSrc>.png (the Weapon
#     Challenges cards);
#   - img/battlepass/logo_white.svg and krunkies.svg (the end screen's #endBPLayout).
# Sources: the Wayback Machine where it has a copy (the hed_* files, the two SVGs), else
# https://assets.krunker.io/img/chals/ as it serves them today (2026-10-12; Last-Modified 2021-2023,
# ETag = MD5 of the file, the same files v7.1.4 loaded). Not served anywhere: icon_21 and the
# cards of weapons 13, 18, 20, 21, 23-28 (v7 shows a broken image for them too). Static images
# only, sequential GETs, every file checked against its pinned SHA-256.
#
#   tools/ui-import/fetch_season_images.sh [UI_DIR]   (default: ui/ of this checkout)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
UI=${1:-$HERE/../../ui}

# destination under ui/ | URL | sha256
FILES="
img/chals/hed_1.png|https://web.archive.org/web/20241104194847id_/https://assets.krunker.io/img/chals/hed_1.png|864ef222a21207580ec6f6aaf882aeedc77247154cc9e071c8b23584053164b3
img/chals/hed_2.png|https://web.archive.org/web/20241104194936id_/https://assets.krunker.io/img/chals/hed_2.png|1a612e9b21aebd1969722b7030678e143522fb09a59f7df440103eec5b546a10
img/chals/hed_4.png|https://web.archive.org/web/20241104194913id_/https://assets.krunker.io/img/chals/hed_4.png|e7f985c052d6783c9d94ccf78341208e674c6fa6641badd66c10bb38e4b2c108
img/battlepass/logo_white.svg|https://web.archive.org/web/20231101163313id_/https://krunker.io/img/battlepass/logo_white.svg|7e7709f9109085d2b26de73b17b4050a006719c2cf17b945ad4d5c3effc93185
img/battlepass/krunkies.svg|https://web.archive.org/web/20231101163318id_/https://krunker.io/img/battlepass/krunkies.svg|0ba5d33d7c583fac561bdfc98c17408fd73504bcaed536976b3c6b9b100bf896
img/chals/icon_0.png|https://assets.krunker.io/img/chals/icon_0.png|f02ed7e7b4bb01330b77520694797074370dce0617a7cef0353ff8d7dfa5995b
img/chals/icon_1.png|https://assets.krunker.io/img/chals/icon_1.png|9671f2d4184639b62362d2c24eeb7c8a76a2ea7e39116f0ad4aef1c7e248fa94
img/chals/icon_2.png|https://assets.krunker.io/img/chals/icon_2.png|f8cac409e07539f0ab73e62430d59b506fcff739c11d6c9be9379e2fce04af0d
img/chals/icon_3.png|https://assets.krunker.io/img/chals/icon_3.png|9a7ab01f64bbe8746d02ae3d977dc5a1b7a14aa521cfd5beb2c883c087d10050
img/chals/icon_4.png|https://assets.krunker.io/img/chals/icon_4.png|ce6247c59c3015501397bdd916140ddfec3fb80d7aca0c5370f06638d3549c8f
img/chals/icon_5.png|https://assets.krunker.io/img/chals/icon_5.png|1e845332fbbceb60e3d204d10abc0c542c6e3ce740cc452d27baaaff3d0d814f
img/chals/icon_6.png|https://assets.krunker.io/img/chals/icon_6.png|df95eba52f8e714a783555a274f755199925fd21b2438e4e10caf06400130b76
img/chals/icon_7.png|https://assets.krunker.io/img/chals/icon_7.png|dd29569827d08bdf5ae04a43d463205992354b43ad72d16465bf8f2d30c4527e
img/chals/icon_8.png|https://assets.krunker.io/img/chals/icon_8.png|9683b30002d74023292e73ed0f616dfee85486ae8f7a52a2b41571bef350d82f
img/chals/icon_9.png|https://assets.krunker.io/img/chals/icon_9.png|d5193c6b1839635ad2f72f4f8daa6f71487451d51d32a0383e31e1a7290eed71
img/chals/icon_10.png|https://assets.krunker.io/img/chals/icon_10.png|f3c359d59f5a041679192413ebed4469d86a16a5ff0fb719889385bfa32754dc
img/chals/icon_11.png|https://assets.krunker.io/img/chals/icon_11.png|2766feb1165ab25fefbb589f1b2ab9866bf775f1238556f6b057530335e5b98c
img/chals/icon_12.png|https://assets.krunker.io/img/chals/icon_12.png|d9a18a73a5d243b969473e2a6b554fd202ec825b560984a7debe65914a9c2338
img/chals/icon_13.png|https://assets.krunker.io/img/chals/icon_13.png|30f75f560da71fd3a017f5e9412c0fbc0a37231d4bac2e9b188f32d9a447566e
img/chals/icon_14.png|https://assets.krunker.io/img/chals/icon_14.png|32df3005738da465188699df02a0c647b7cdb26268cb78cf40b374480bb5c65a
img/chals/icon_15.png|https://assets.krunker.io/img/chals/icon_15.png|6f7e3261033bdcf88b5614244c9c8638391ed7f13a782f8d6357fb05e2f82aff
img/chals/icon_16.png|https://assets.krunker.io/img/chals/icon_16.png|e2fb0caae210d6ca00c689107841dc740f35720ae08147381e2adfe6665a44d7
img/chals/icon_17.png|https://assets.krunker.io/img/chals/icon_17.png|4fc07a2af192396e2d4b93fd17c5868761f98e5c3d28c93757dc5068b32c9ab3
img/chals/icon_18.png|https://assets.krunker.io/img/chals/icon_18.png|ca9076fa4dd48f86ff96185902cec4251911111fad8a26df56a4ff0ded850632
img/chals/icon_19.png|https://assets.krunker.io/img/chals/icon_19.png|3d61d20da9d4fe1ace7b46e09f63c70b08bd866001c19397cf3dac5048db3c3d
img/chals/icon_20.png|https://assets.krunker.io/img/chals/icon_20.png|40eeee4a077363979701d1c2e225bc0ce8d6b682fcd16e44a2f76e9cc6a5b6f7
img/chals/icon_22.png|https://assets.krunker.io/img/chals/icon_22.png|af417595fd95fa7f3f6b88e2719eeda0ce422c046f0962b4e1e6fc62d90dd266
img/chals/icon_23.png|https://assets.krunker.io/img/chals/icon_23.png|bd6f365e4e372a7b32f11a178fd594d9cf8f29232a8b3855c2e4bbd2a9c7c120
img/chals/icon_24.png|https://assets.krunker.io/img/chals/icon_24.png|abe5715df1eed8b9a520b730b848d2900d2dcd0ecbb8a31ec83c586edd070c8c
img/chals/icon_25.png|https://assets.krunker.io/img/chals/icon_25.png|2f6def7518aad9466761068e7e45fcebe1ee622784a492803ae249b7fc1b4675
img/chals/icon_26.png|https://assets.krunker.io/img/chals/icon_26.png|ca1f7d871c931f218495c170cd1ac671f5ac849ffdcb9a8caf60885afa3c5213
img/chals/icon_27.png|https://assets.krunker.io/img/chals/icon_27.png|b237d58e401f32b396e8da02778d033f1d854e574acaf4de79a3623253408606
img/chals/icon_28.png|https://assets.krunker.io/img/chals/icon_28.png|67fb1163ff00f2b6cb77ea2b0743464d54a916eeea30ef10928392da46ed04f1
img/chals/icon_29.png|https://assets.krunker.io/img/chals/icon_29.png|9b75264a4df72cb22677849eb0ecbadf783178aa28d68144f794eaba3ab2ea75
img/chals/icon_30.png|https://assets.krunker.io/img/chals/icon_30.png|11f1a86b34c0376efc78b37ad3bbbd1bfb398704dcc6bbab93d0496ce2effc69
img/chals/icon_31.png|https://assets.krunker.io/img/chals/icon_31.png|c4456e167f356a9d8dfe4e19ece4f4d692bcf048e9dbf518bc3453a93491489f
img/chals/icon_32.png|https://assets.krunker.io/img/chals/icon_32.png|6f4ab7e7d8d8455119661ee0b2b2ae5ff1b8085cbc7a797858a13c586acfd6ff
img/chals/icon_33.png|https://assets.krunker.io/img/chals/icon_33.png|09bcaa26a0ba2a4240d3cdd4ed0457e39c885b138f07f782a8aaf04eea56e7da
img/chals/icon_34.png|https://assets.krunker.io/img/chals/icon_34.png|0a762f79936d5022699fd8ebabcd7dc01a3e5b55dde153669849d3c87b3b4d29
img/chals/icon_35.png|https://assets.krunker.io/img/chals/icon_35.png|7516820ddbcca9b46ff710f83c6cb015cfd1a62269f65f338eaa4783b75d1d31
img/chals/weapon_1.png|https://assets.krunker.io/img/chals/weapon_1.png|be77286d4509782ae3c45bef50d668ae319412659dc1ff2d743c7a4566c08385
img/chals/weapon_2.png|https://assets.krunker.io/img/chals/weapon_2.png|7f9e8e6b7b8acd889d8d3595b30b5c6705ecff1b81fd53ee0d7175a72ac0a5cd
img/chals/weapon_3.png|https://assets.krunker.io/img/chals/weapon_3.png|3468a9bcb46cfaeb46826c60731a754e84685c8a3944c1746b0d43487915b201
img/chals/weapon_4.png|https://assets.krunker.io/img/chals/weapon_4.png|8a32018e5a93b0116298ff5a1f4ba78266a218acf6eae5881ff0fb639e4c0997
img/chals/weapon_5.png|https://assets.krunker.io/img/chals/weapon_5.png|272b5013af619152120098183a70f4ef944ceacca9686790b445aae9f6b19cf5
img/chals/weapon_6.png|https://assets.krunker.io/img/chals/weapon_6.png|cb37fdb07bb6a761013e83ef8285904266834213ece8b1abd1f0247512f0eed4
img/chals/weapon_7.png|https://assets.krunker.io/img/chals/weapon_7.png|de21e4833164b821fa8956f9a4764677facb2bdda8e2eaec1b6fce6b6bc46732
img/chals/weapon_8.png|https://assets.krunker.io/img/chals/weapon_8.png|874e50dd1897388c9f8bd09f8121d21db22a83e5872e4decb20842e688bf7bbf
img/chals/weapon_9.png|https://assets.krunker.io/img/chals/weapon_9.png|67378667c90f2389c1f7632510cf4fc27bcb7f0e254df8d9928e0cae9e3b1710
img/chals/weapon_10.png|https://assets.krunker.io/img/chals/weapon_10.png|90f003c62245ac7bd5921d16f93f33f0ebb2f6204b68b8e4239d590703fabaaa
img/chals/weapon_11.png|https://assets.krunker.io/img/chals/weapon_11.png|80f75562f006f6c4de736f29dbedf6a09598058ea9406ca9af7a2a2fa2fef8bd
img/chals/weapon_12.png|https://assets.krunker.io/img/chals/weapon_12.png|7dae5688a26a97711762ff1d772593231cc2335f04c313ade8be7d43bfa68bd4
img/chals/weapon_14.png|https://assets.krunker.io/img/chals/weapon_14.png|1ed727f478742ce8457a5988720ae3790b993d84babf2b309da1697952f71127
img/chals/weapon_15.png|https://assets.krunker.io/img/chals/weapon_15.png|e6ff1459693d73336af0e2a2950b297067081f287f682e8f7dbc5960b09a4554
img/chals/weapon_16.png|https://assets.krunker.io/img/chals/weapon_16.png|0642f8416c4157429d8e1271b9635cd7f074f01b31602ec9fb4d7677ae3c9835
img/chals/weapon_17.png|https://assets.krunker.io/img/chals/weapon_17.png|c10b507ddf7260dadfe51d807316901931db2ac17ebeffcec74b0fc058aeca76
img/chals/weapon_19.png|https://assets.krunker.io/img/chals/weapon_19.png|cefe92e223c3a747e6f22001a73f08a5b5c7d7e884e22400a9a52ce6f2b5ff8a
img/chals/weapon_22.png|https://assets.krunker.io/img/chals/weapon_22.png|b9a5f07e4847efefcbf44f9aeeb2c6b37fba014939fccac313bfb1d6d1ddbc60
img/chals/weapon_29.png|https://assets.krunker.io/img/chals/weapon_29.png|f4e5467cf1a1e77f1bb49329e431146e7a947754e0e50c1110738c50b85dfe6d
img/chals/weapon_k.png|https://assets.krunker.io/img/chals/weapon_k.png|0719c7dbf0a3bbb716aeb7a2ded06364ecb23797e9c5599ca201ef18a2f4c9a6
"

while IFS='|' read -r rel url sum; do
    [ -n "$rel" ] || continue
    dest="$UI/$rel"
    if [ -s "$dest" ] && [ "$(sha256sum "$dest" | cut -d' ' -f1)" = "$sum" ]; then continue; fi
    mkdir -p "$(dirname "$dest")"
    tmp=$(mktemp "$(dirname "$dest")/.dl.XXXXXX")
    curl -fsSL --retry 3 -m 120 -o "$tmp" "$url"
    if [ "$(sha256sum "$tmp" | cut -d' ' -f1)" != "$sum" ]; then
        rm -f "$tmp"
        echo "fetch_season_images: $rel: SHA-256 mismatch ($url)" >&2
        exit 1
    fi
    mv "$tmp" "$dest"
    sleep 1 # a polite pace
done <<< "$FILES"
echo "season images in $UI"
