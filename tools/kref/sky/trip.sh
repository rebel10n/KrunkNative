#!/bin/bash
# trip.sh <out.png> <map>: before | after | real, rows per pose, labelled
cd /tmp/kn-sky-cap; m=$2; rows=()
for p in horizon up down; do
  magick \( before/$m-$p.png -resize 640x360! -fill white -undercolor '#0008' -pointsize 18 -annotate +8+22 "before $m $p" \) \
         \( after/$m-$p.png -resize 640x360! -fill white -undercolor '#0008' -pointsize 18 -annotate +8+22 "after" \) \
         \( real/$m-$p.png -resize 640x360! -fill white -undercolor '#0008' -pointsize 18 -annotate +8+22 "real v3.1.1" \) +append /tmp/kn-sky-cap/row-$p.png
  rows+=(/tmp/kn-sky-cap/row-$p.png)
done
magick "${rows[@]}" -append $1
