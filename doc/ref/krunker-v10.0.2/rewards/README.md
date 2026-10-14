# Krunker v10.0.2 rewards references

Captured from the offline v10 sandbox (`tools/uidiff/capture-v10-rewards.js`: the v10.0.2 client
served from the local mirror of krunker.io, no network, no WebSocket, no login), 1280x720.
The signed-in states use `tools/uidiff/season_fixture.json`'s account and Season 7 pass, handed to
v10's own handlers. The uidiff scenarios are `tools/uidiff/scenarios/rewards-*.json` with their
regions in `tools/uidiff/regions/`:

    UIDIFF_HEADLESS=1 tools/uidiff/uidiff.sh cmake-build-debug/client_bin \
      rewards-signup rewards-register rewards-chall-guest rewards-chall-home rewards-chall-map \
      rewards-chall-weapon rewards-bp-guest rewards-bp-home rewards-bp-vip rewards-bp-chal rewards-menu-account

| file | state |
|---|---|
| `signup-rewards.png` | guest: Get Signup Rewards (`showSignupRewards`, the signupRewardsPop) |
| `register-rewards.png` | "Welcome to the Family!" (the `prr` push: 14 cosmetics + 24 h premium) |
| `chall-guest.png` | guest: `showWindow(33)` (LOGIN REQUIRED) |
| `chall-home.png`, `chall-map.png`, `chall-weapon.png` | signed in: the Challenges window, Burg, Assault Rifle |
| `bp-guest.png` | guest: `showWindow(6)`; the offline client also left its login popup over it |
| `bp-home.png`, `bp-vip.png`, `bp-chal.png` | signed in: the Battle Pass (free tier, VIP, Challenges tab) |
| `menu-account.png` | signed in: the main menu with the sidebar's Battle Pass item |

The mirror lacks most `textures/previews/` images, so some skin cards are empty in the references;
the regions leave the card images out.
