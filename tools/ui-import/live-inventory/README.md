# Inventory of the files the v7.1.4 tree lacks (input of `fetch_live_assets.py fetch`)

Read-only over the reference tree; outputs go wherever you point them (use `~/.cache`).

```sh
T=~/.cache/kn-live-inv; mkdir -p $T
# 1. static: every path v7.1.4's data tables / code literals / the 68 catalogue maps reference
node -r ./v7env.js inventory.js > $T/cand.tsv
# 2. dynamic, v7: v7's own client (headless Chromium) on every map, asset requests logged
node v7probe.js $T/probe.jsonl ../../../assets/maps/*.json ../../../assets/maps/custom/*.json
# 3. dynamic, KN: client_bin on every map with failed opens logged (LD_PRELOAD shim)
gcc -shared -fPIC -O2 -o $T/misslog.so misslog.c -ldl
#    env -u DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_AUDIO_BACKEND=null MISSLOG=$T/runs/<map>.miss \
#        LD_PRELOAD=$T/misslog.so KRUNKNATIVE_SCREENSHOT=/dev/null client_bin --offline <map.json>
# 4. merge: what assets/ (and ui/img) lack, with category, availability and referencing maps
python3 -I merge.py $T/cand.tsv $T/probe.jsonl $T/runs $T/missing.tsv
python3 -I ../fetch_live_assets.py fetch --list <(grep -v '^#' $T/missing.tsv | cut -f1,2) \
    --stage $T/fetched --manifest $T/fetched.tsv --write-lock ../live_assets.lock
```

`v7probe.js` uses `CDP_PORT` / `HTTP_PORT` (pick free ones) and `SETTLE` (ms per map).
