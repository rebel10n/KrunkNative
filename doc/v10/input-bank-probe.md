# Measuring Krunker's input bank (the lag-switch fix)

Krunker v10.0.7 "Fixed lag switching letting players teleport". The bundle only declares the player's
`inputBank` / `inputBankAt` and zeroes `inputBankAt` at every spawn (deob-10.0.7.js:171267-171268, :171545); the
code that reads them is on Krunker's game server, which is not shipped. The other limits the bundle does show stay
as they were: each input's delta is clamped to `[deltaMlt, Math.round(1000 / 30) = 33 ms] / deltaDiv` in procInputs
(:16404, :172014; KN `game_constants.max_delta`), and `inputFreq` is reset on the server's player check
(`players.resetFreq`, :176781-176784, :195310). Box Office's bank (`shared/src/player.c`, `PLAYER_INPUT_BANK_MAX`
0.5 s, `PLAYER_INPUT_BANK_RATE` 1.5) is therefore KN's own reconstruction. Searched 2026-11-19 with no numbers found:
the bundle, the live i18n, krunker.io/docs/versions.txt ("UPDATE 10.0.7" names the fix only), the older client
trees (Krunker_v3.1.1, Krunker_v7.1.4, game.leak) and public pages.

The probe measures the real values on Krunker's official server from the Box Office client. It needs a person,
because the matchmaker's Turnstile captcha is always solved by a person (official-captcha.md). Nothing runs unless
`KRUNKNATIVE_INPUT_BANK_PROBE` is set, and it only acts on the official-server link, never on Box Office servers.

## What it does

`client/src/net/krunker_probe.c` (header `client/include/krunker_probe.h`). Three seconds after the first server
position it walks the local player through a fixed schedule, with the movement keys driven by the probe
(`krunker_link_input_fields`):

- for every length L in the list: a **control** walk forward of L (frames sent as they are made), 5 s standing,
  a **held** walk back of L, then 1.5 s standing. During a held walk `out_flush` (krunker_net.c) keeps every
  outbound frame queued, which is exactly what a lag switch does. When the walk ends they leave in one burst.
- for every rest g in the optional gap list: two held walks of the longest length with g standing between them
  (the refill).

Every server position (`"l"`), Input Failed (`"f"`), step, hold and release is written to a TSV.
`tools/krunker/input_bank_probe.py` compares each held walk's server displacement with its control walk's:

- covered = L × held / control is the walk time the server let through.
- Up to the cap, covered ≈ L. Above it, covered stays at the cap.
- A refill pair drains the bank with its first burst. The bank then refills over the rest plus the second walk's
  length, so the second walk's covered time over (rest + length) is the refill rate, wherever it stays below the cap.

The whole schedule with the default lists takes about 2 minutes. The player stays near where it started (forward,
then back) and stands still between walks; holds are at most 2 s.

## Steps (after the user's go)

1. Build the native client (`make`), on Linux or macOS. The probe uses the native socket; the Windows socket and
   the web build don't hold frames.
2. On krunker.io (or the client's krunker.io hosting), create a **private custom game** on an open, flat map
   (no walls within a few metres of the spawn). This keeps the walks clear and other players out of it. Note its
   game id (the `?game=` part of its link).
3. Run, with the window on your own display (the captcha needs you):

   ```sh
   KRUNKNATIVE_INPUT_BANK_PROBE=100,250,500,750,1000,1500,2000 \
   KRUNKNATIVE_INPUT_BANK_PROBE_GAPS=250,500,1000,2000 \
   KRUNKNATIVE_INPUT_BANK_PROBE_LOG=$HOME/.cache/input_bank_probe.tsv \
     ./cmake-build-debug/client_bin --krunker <gameId>
   ```

   Solve the captcha if it appears, click to play, spawn, and take your hands off the keyboard and mouse. The log
   prints `[probe] input-bank probe armed` and, about two minutes later, `[probe] input-bank probe done`.
4. `python3 -I tools/krunker/input_bank_probe.py ~/.cache/input_bank_probe.tsv` prints the cap and the refill.
   Put them in `PLAYER_INPUT_BANK_MAX` / `PLAYER_INPUT_BANK_RATE` (shared/src/player.c) with the run's date as
   their source, then run `proc_input_test` and `input_bank_test`.

If the table says the control walks barely moved, the spawn was against a wall: move to an open spot and run again.
An Input Failed line in the output means the server dropped a burst outright (its own lag-switch answer); the
script lists those times too.
