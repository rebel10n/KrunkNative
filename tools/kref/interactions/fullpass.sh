#!/bin/bash
# Native capture pass at 1280x720 (fresh config). Each step starts from the menu with no window open.
T=$(dirname "$0")/nat.sh; N=$(dirname "$0")/ncap.sh; export DISPLAY=:77
rm -rf ${KN_CFG:-/tmp/kn-menus-cfg}; mkdir -p ${KN_OUT:-/tmp/kn-menus-ref}/ours; $T stop; $T start
CLOSE=0 $N menu
$N w5-account 40 95; $N w14-store 40 190; $N w33-challenges 40 285; $N w10-games 40 480
$N w10-tab1 40 480 533 192; $N w10-tab5 40 480 823 192; $N w10-tab3 40 480 653 351; $N w10-tab0 40 480 453 358
$N w4-mods 40 575; $N w1-settings 40 670
$N host 466 614; $N w2-servers 578 614; $N w27-ranked 675 614; $N join 838 614
$N w25-client 1153 20
$N w21-advertise 1042 695; $N w29-support 1122 695; $N w35-changelog 1250 695
CLOSE=0 $N kpd 1034 441; CLOSE=0 $N kpd-closed 300 600
$N w3-loadout 1132 443
$N w3-skincolor 1132 443 762 466
CLOSE=0 $N w6-class 1132 443 830 208
$N w6-pick-runngun 638 233
$N tmp 1132 443 830 229 492 233
$N w15-skins 1132 443 830 229
$N w31-attach 1132 443 830 250
$N w32-reticle 1132 443 830 271
$N w20-secondary 1132 443 830 293
$N w26-melee 1132 443 830 314
$N w39-face 1132 443 830 335
$N w16-hat 1132 443 830 357
$N w17-back 1132 443 830 378
$N w38-waist 1132 443 830 399
$N w34-dye 1132 443 830 421
$N w9-spray 1132 443 830 442
# real: selectClass(0);selectMelee(-2);selectHat(-2);selectAttachment(-1);selectReticle(3);selectSpray(-2)
CLOSE=0 $N tmp 1132 443 830 314 580 360
CLOSE=0 $N tmp 830 357 580 360
CLOSE=0 $N tmp 830 250 470 360
CLOSE=0 $N tmp 830 271 580 290
$N w3-after-random 830 442 476 240
