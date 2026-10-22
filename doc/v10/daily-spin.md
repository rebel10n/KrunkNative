# Daily Spin (v10.0.3)

The sidebar's Daily Spin item (`#dailySpinDiv`) as Krunker v10.0.3 has it, and what Krunker
Box Office does on each server type. Reference: `~/.cache/kn-v103/deob-10.0.3.js` (the readable
bundle; line numbers and byte offsets are into that file). `game.js` in
`/home/user/jails/kru/src/Krunker_v10.0.3/` is the same code string-encoded (its `$dailySpinState`
block starts at byte 6656308, the `dailySpinDiv` markup at byte 112137), so the deob offsets are
the ones to cite. Read only.

## v10.0.3 click handler

| What | Where (deob-10.0.3.js) |
|---|---|
| `$dailySpinState` store, default `{scoreEarned 0, maxScore 5000, timeRemaining 0, isClaimed false, stage 0}` | line 134522 (byte 5921356) |
| progress fetch `iìíîíïî()`: `getProgress()`, then `store.set` from `data` (score, maxScore, timeRemaining, claimed, stage); `catch {}` | lines 134529-134545 (byte 5921495) |
| API call `GET /daily-spin/progress` (dailySpin.getProgress) | line 29931-29933 (byte 1140620) |
| progress fetch triggers: sidebar mount when `$platform === "frvr"` | lines 164146-164153 (bytes 7923137, 7923345) |
| progress fetch at the end of a match (victory flow) | line 257588 (byte 12940458) |
| `$account` gate: the item exists only when signed in (`IIîíiîí = !$account`) | lines 164103, 164183 (byte 7924837) |
| claimable: `!signedOut && !isClaimed && scoreEarned >= maxScore` | line 164127 (byte 7922268) |
| tiers: `(stage + 1) % 7 == 0` Heroic 7993, `% 3 == 0` Elite 8068, else Starter 8067 | lines 164104-164123 |
| the click handler `IIïîîíì` | lines 164131-164152 (byte 7922603) |
| `playSelect()` at the top of the handler (`iiîïîiì()`, the sound helper; no argument) | line 164132; helper at lines 158419-158423 (byte 7495280) |
| claim guard: `if (claimable && !claiming)` | line 164133 (byte 7922666) |
| claim: `await rpc(DAILY_SPIN_CLAIM)`, then `store.update(isClaimed: true)` | lines 164135-164142 (byte 7922817) |
| the item's `onclick`: `__click = IIïîîíì` | line 164185 (byte 7924964) |
| rpc names: `DAILY_SPIN_CLAIM = "dsl"` (111109, byte 4645760), `DAILY_SPIN_CLAIM_RESPONSE = "dslr"` (111361, byte 4652726) | |

The click handler, in full:

```
async function IIïîîíì() {
  iiîïîiì();                                   // window.playSelect() : the click sound, always
  if (claimable && !claiming) {                // IÎiììîî && !IÏiïîïí
    claiming = true;
    try { await rpc(DAILY_SPIN_CLAIM); store.update(isClaimed = true); }
    catch {} finally { claiming = false; }
  }
}                                              // locked: nothing else. No popup, no message, no request
```

## What v10 does

- **Locked** (score below maxScore, or already claimed): the click plays the select sound and
  nothing else. No claim is sent, no popup opens, no text is shown. v10.0.3 has no "Play more"
  text at all (`grep -i "play more"` on the bundle: 0 hits).
- **Unlocked** (`FREE SPIN`): the click sends the `dsl` rpc on the game socket; the answer
  (`dslr`) carries the ticket, which the wheel opens.
- **Progress failure** (the API refuses, or no answer): the `catch {}` in the progress fetch keeps the
  store's default, so the item shows `0/5000`. No "Unavailable", no message.

## Box Office (before this change)

- `client/src/ui/v10_daily_spin.c:215-216` (before this change, the user's report): on the krunker.io backend a click
  called `account_popup` with `kn.dailySpin.playMore` ("Play more to unlock the daily spin") when not
  claimable. This was the user's report. `kn.dailySpin.playMore` is set at
  `ui/i18n/kn/en.json:177`.
- `client/src/ui/v10_daily_spin.c` before this change: a failed progress ask set `failed` (`:125` the
  refusal or no answer, `:143` refused at once, `:176` Box Office's "dsp" with no data), and the item
  showed `kn.dailySpin.unavailable` ("Unavailable", `:67-68`; `ui/i18n/kn/en.json:178`). Both keys are
  gone now; the item keeps `0/5000` on a load failure.
- Box Office's own brain rule is `services/brain/daily_spin.c:68`: `"Play more to unlock the daily spin"`
  for a claim below the score. It is unchanged. A Box Office click sends `dsl`, and the refusal comes back
  as the claim's error.

## Krunker Box Office after this change

- Krunker.io server: a locked click does nothing (`v10_daily_spin.c`, `knDailySpin` handler): no popup,
  no request. A claimable item (FREE SPIN) still shows the "can't be claimed on krunker.io servers yet"
  message, because the claim is the game-socket rpc `dsl`, which the krunker.io account does not send
  (`krunker_net.h`). That is the one message left on this path, and it is not a load failure.
- Krunker.io progress failures show nothing: the item keeps `0/5000`, as v10.
- Box Office: the progress failure shows nothing (as above); the brain's own refusals and the claim
  timeout ("No answer from Box Office") are unchanged.
- The item's click also plays `playSelect()` before the handler, as v10's does.

## Known differences left in place

- **When the progress is asked.** v10.0.3 asks at the sidebar's mount only on the FRVR platform, and at
  the end of each match. Box Office asks at login and then every 60 s while the menu is open
  (`DS_REFRESH_MS`). Matching v10's timing would mean the item shows `0/5000` until a match ends on the
  web build; it is not changed here.
- **The claim on krunker.io.** Not sent (see above).
