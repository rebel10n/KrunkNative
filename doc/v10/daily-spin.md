# Daily Spin (v10.0.3)

The sidebar's Daily Spin item (`#dailySpinDiv`) as Krunker v10.0.3 has it, and what Krunker
Box Office does on each server type. Reference: `~/.cache/kn-v103/deob-10.0.3.js` (the readable
bundle; line numbers and byte offsets are into that file). `game.js` in
`~/jails/kru/src/Krunker_v10.0.3/` is the same code string-encoded (its `$dailySpinState`
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
  (`dslr`) carries the ticket, which the wheel opens. The whole flow is below.
- **Progress failure** (the API refuses, or no answer): the `catch {}` in the progress fetch keeps the
  store's default, so the item shows `0/5000`. No "Unavailable", no message.

## The claim, in full (v10.0.3)

| Step | Where (deob-10.0.3.js) |
|---|---|
| `claiming = true`; the item reads `CLAIMING...` while it is set (`FREE SPIN` otherwise) | 164134, 164193 |
| `socket.rpc(DAILY_SPIN_CLAIM)`: `rpcCounter++`, `send(RPC, id, "dsl")` = the wire frame `["_0", id, "dsl"]` (msgpack + ahNum, like every frame), `rpcHandlers[id] = {resolve, reject}` | 164136; `rpc` 111529-111540; `RPC: "_0"` 111096; `DAILY_SPIN_CLAIM: "dsl"` 111109 |
| the answer: `["_0", id, results...]` `RPC_SUCCESS` resolves the promise with the results, `["_1", id, errors...]` `RPC_ERROR` rejects it; an unknown id is dropped | parser 111438-111452; `RPC_SUCCESS: "_0"` 111117, `RPC_ERROR: "_1"` 111118 |
| on success `store.update(isClaimed: true)`: the item reads `CLAIMED` (`claimable` is false, `claimed` true) | 164137-164140; 164127-164128 |
| on error `catch {}`: nothing shown, no text, no popup | 164141 |
| `finally claiming = false` | 164142 |
| the server's push `["dslr", {ticketId}]` (`DAILY_SPIN_CLAIM_RESPONSE`): `account.addSkin(ticketId)` (an owned entry's `cnt += 1`, else `{ind, cnt: 1, st: 1}`), then `window.prizeWheel(0, ticketId)` | 111361; handler 261748-261756; `addSkin` 129095-129120 |
| `prizeWheel(0, ticketId)`: the ticket's `spinWheelId` picks the wheel (`store.wheels[...]`), the spin screen (`#spinUI`) opens with the Redeem button; the spin itself is the usual `["spin", wheel, ticket, multi]` from the spin button | 254380-254400 |
| the ticket ids: stage 0 Starter 8067, every 3rd Elite 8068, every 7th Heroic 7993 | 164104-164125 |

No sound of its own beyond `playSelect()` at the top of the handler (164132); the wheel's sounds are the
spin screen's. No `ua` / `a` is tied to the claim: the handler changes only `isClaimed`, and the progress
(`0/5000`, `claimed`) is whatever the next `GET /daily-spin/progress` says (the victory flow, 257588, and
the FRVR mount, 164146-164155, ask it; Box Office asks every minute). The `dsl` rpc carries no argument
and the handler ignores the `RPC_SUCCESS` results.

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
  no request. A claimable item (FREE SPIN) is claimed the way v10 does it (the table above):
  `backend_social_send_text("[\"dsl\"]")` -> `kio_held_send` -> `krunker_net_send_account_frame`, which sends
  the rpc `["_0", rpcId, "dsl"]` (`send_rpc`, `krunker_net.c`) and keeps the id in `rpc_pending`. The link
  forwards `_0` / `_1` / `dslr` as account frames (`krunker_link.c` `account_scoped`); `rpc_answer`
  (`krunker_net.c`) names the rpc in the id's place (`["_0", "dsl"]`, `["_1", "dsl", err]`) and drops the
  LOGIN rpc's answer; `account_ui.c` `kio_frame` routes them and `dslr` to `v10_daily_spin_frame`:
  `_0` sets `claimed` (CLAIMED), `_1` clears `claiming` and shows nothing (v10's `catch {}`), `dslr`
  adds the ticket to the krunker.io account's `skins` (`account_add_skin`, v10's `addSkin`) and dispatches
  `prizeWheel(0, ticketId)` (`store_ui.c`: the ticket's wheel, 8067 / 8068 / 7993 in `k_ticket_wheels`).
  The old "can't be claimed on krunker.io servers yet" text (`kn.dailySpin.officialClaim`) is gone.
- Krunker.io, no official game open: v10's socket is connected in a game only, so the click sends nothing
  (`krunker_net_send_account_frame` returns 0: the socket is not open) and says nothing; the item stays
  FREE SPIN. A claim with no answer in 15 s (`DS_CLAIM_TIMEOUT_MS`) returns to FREE SPIN with no text
  (the Box Office "No answer" popup is Box Office's).
- Krunker.io progress failures show nothing: the item keeps `0/5000`, as v10.
- Box Office: the progress failure shows nothing (as above); the brain's own refusals and the claim
  timeout ("No answer from Box Office") are unchanged.
- The item's click also plays `playSelect()` before the handler, as v10's does.

## Known differences left in place

- **When the progress is asked.** v10.0.3 asks at the sidebar's mount only on the FRVR platform, and at
  the end of each match. Box Office asks at login and then every 60 s while the menu is open
  (`DS_REFRESH_MS`). Matching v10's timing would mean the item shows `0/5000` until a match ends on the
  web build; it is not changed here.
- **The ticket's spin on krunker.io.** The claim is sent and the wheel opens, but the spin button's
  `["spin", wheel, ticket, multi]` stays read-only on krunker.io (`store_ui.c` `spin_button_click`,
  `kio_store_read_only`): the ticket sits in the account's inventory until that policy changes.
- **An unanswered claim.** v10's `await` has no timeout (the item would read `CLAIMING...` until the
  answer); Box Office gives up after 15 s (above).

## Tests

`server/tests/kio_spin_battlepass_e2e.py` (`KIO_MODES=claim,claimerr,...`): the stub game server answers
the rpc with `["_0", rpcId]` + `["dslr", {ticketId: 8067}]` (claim) or `["_1", rpcId, text]` (claimerr),
the shapes the parser above takes (111438-111452, 261748-261756); the results of a success and the error's
text are the stub's, since v10 reads neither. The stub cannot say what krunker.io's server actually sends
as the rpc's results or error text, nor exercise the spin after the claim.
