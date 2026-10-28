# Stripe checkout (krunker.io's "Stripe" purchase button)

How Krunker v10.0.4 pays with Stripe, what of it a native client can do, and what it needs. Findings
only; the purchase flow and the Xsolla side are doc/v10/checkout-browser.md. Design (user, 2026-10-28):
the purchase runs in C, no embedded webview, no krunker.io page or JS in the client; a provider's hosted
payment page opens in the system browser (`ui_open_browser`, client/include/ui_logic.h).

## Sources

- `deob-10.0.4.js` = /home/user/jails/kru/src/Krunker_v10.0.4/deob/game.deob.js.
- The FRVR SDK: `https://krunker.io/libs/frvr-sdk.min.js?build=EkJjw` (sha256 `af4d7153…2d779`) and the web
  channel `https://krunker.io/libs/frvr-channel-web.min.js?build=EkJjw` (sha256 `16a001eb…286f1`), both loaded
  by krunker.io's index.html. Decoded with the repo's webcrack pass
  (`Krunker_v10.0.3/deob/tools/deob.cjs`, node:vm sandbox) into `sdk.deob.js` (sha256 `c4eae261…7d1e31`) and
  `channel.deob.js` (sha256 `70f321b0…3db25a`); `sdk:<line>` / `channel:<line>` below are lines of those
  outputs (regenerate: `node deob.cjs frvr-sdk.min.js sdk.deob.js`).

## When the button shows

`stripePayB` shows when `canShowStripe && !steam && FRVR.iap && stripeEnabled` (deob-10.0.4.js:212399,
:212383-212384). krunker.io sets `canShowStripe=!isMsPwa&&!isSteamClient&&!isEpicClient` (index.html), and
`stripeEnabled` is `!0x0` (deob-10.0.4.js:16476, config :20017): on for every web player. PayPal is off
(`canShowPaypal=!1`).

## The flow

1. **Krunker's checkout** (deob-10.0.4.js:212420-212446, iïíîíìï): `POST https://api.krunker.io/iap/checkout`
   (the data client, `API_URI` :16438, `iap.checkout` :26073-26078), `Authorization: Bearer <FRVR access token>`
   (the client's interceptor, :27719-27724: `FRVR.auth.getFreshAccessToken()`). Body, one of:
   `{pack}` / `{bundle, giftPlayerName}` / `{premium, giftPlayerName}` /
   `{productId, display, quantity, giftPlayerName}`. Answer `{data, error}` (:25929-25958); `data.payment` is
   the server's payment.
2. **`payment`** is a server payment for FRVR's wallet: `{paymentId, reference?, provider?}`.
   `FRVR.iap.purchase(payment)` (sdk:4430-4442) rejects one without `paymentId` ("A server payment needs its
   paymentId") and runs `purchaseWith(reference ?? paymentId, {payment_id: paymentId}, {provider: provider ??
   "stripe"})` (sdk:4448-4458): the Stripe provider unless the payment names another.
3. **The Stripe provider** (`stripe`, channel:2426-2460; registered for the web channel's `stripe-checkout`
   provider with `processor: "stripe"` and a `publishableKey`, channel:3047-3076) asks FRVR's wallet for a
   **Stripe Checkout session** (channel:2600-2627):
   `POST https://crucible.frvr.com/v1/iap/wallet/payment-intent/GPEFWO1` (sdk:4779, :4861-4866; `GPEFWO1` is
   krunker.io's `FRVR.config.gameId`, index.html), `Authorization: Bearer <FRVR access token>` (sdk:3866-3887),
   body `{paymentId, checkout, returnUrl}` (plus `source` when the SDK has a purchase source, sdk:4883-4889).
   A 409 whose message starts "payment is SUCCESS" / "payment was already paid" means it is already paid
   (channel:2601-2621). Answer: `{transactionId, payment: {checkout: {mode, url?, clientSecret?, sessionId},
   publishableKey?, paymentId}, order?: {reference, name}}`.
4. **`checkout` is the client's choice** (channel:2534-2560, prepareCheckout): `"embedded"` when the page is
   top level, `"hosted"` when framed by another origin. `returnUrl` is the page's URL without `uuid` /
   `frvr_checkout_session` (channel:1887-1893).
   - `mode: "embedded"`: `clientSecret` for stripe.js (`https://js.stripe.com/dahlia/stripe.js`,
     `initEmbeddedCheckout`) mounted in FRVR's own popup (`.frvr-iap-card-popup`, channel:2228-2235,
     :2749-2800). Needs a page we would host: **not usable natively**.
   - `mode: "hosted"`: **`checkout.url`, a page hosted by Stripe** (Stripe Checkout). runHosted
     (channel:2834-2904) opens it in a popup window (`frvr-stripe-checkout`, 520x760, channel:2396-2411) and
     does not use the page for anything else. **This is the native path: open `checkout.url` in the system
     browser.**
5. **Completion signal: the wallet, not the page.** runHosted polls `GET
   https://crucible.frvr.com/v1/iap/wallet/transactions/<transactionId>` (sdk:4867-4871) every 2 s
   (channel:2887-2906): `status` `"SUCCESS"` = settled, `"REFUNDED"` = failed, anything else pending
   (channel:1876-1884); a poll error is "still waiting" unless its code is `PAYMENT_REJECTED` /
   `PAYMENT_REFUNDED` (channel:2870-2884). When the player closes the payment window it polls once more for
   up to 10 s (channel:2894-2900); the whole wait gives up after 30 min (channel:2394, 1 800 000 ms). The
   redirect to `returnUrl` is never read. Result: `{channelId: "stripe", productId: order.reference ?? sku,
   purchaseId: transactionId, transactionId, transactionReceipt: {transactionId, paymentId: sessionId}}`
   (channel:2669-2682).
6. **Krunker's claim** (deob-10.0.4.js:212404-212418, iiíïïïi): `POST https://api.krunker.io/iap/claim`
   `{channelId: channelId || "stripe", productId, purchaseId}` (:26079-26088), same token. `data` is
   `{funds, pack, price}` (KR) / `{bundle, …}` / `{premium}` / `{productId: "username:…", productName}` /
   `{giftName, productName}` / `{productName}`; an `error` or no `data` is "purchase.error".
7. **Consume** (deob-10.0.4.js:212410, `FRVR.iap.consumePurchase`, sdk:4544-4560 -> channel:2922-2931):
   `POST https://crucible.frvr.com/v1/iap/wallet/transactions/<transactionId>/consume` body `{}`, errors
   ignored. An unconsumed SUCCESS transaction is re-claimed at the next start
   (`GET …/wallet/transactions?consumed=false&status=SUCCESS`, deob-10.0.4.js:213757, sdk:4872-4876).

## What a native client needs

Everything above is plain HTTPS with the krunker.io account's FRVR access token, which the client already
holds (krunker_account.h: the token kio_store_write sends), plus one browser hand-off:

- api.krunker.io `/iap/checkout` and `/iap/claim`: `KRUNKER_SVC_DATA` already reaches api.krunker.io.
- crucible.frvr.com `/v1/iap/wallet/payment-intent/GPEFWO1` (with `checkout: "hosted"`), `…/wallet/transactions/<id>`
  and `…/consume`: **no KrunkerService for crucible's iap yet** (krunker_account.h has crucible only for
  auth). That is the one missing piece.
- `ui_open_browser(checkout.url)`, then the C poll above (2 s, 30 min), the claim, the consume.
- `returnUrl`: the hosted session needs one; krunker.io's page URL is what the bundle sends. The player sees
  it in their browser after paying (a krunker.io tab), which is harmless: completion comes from the poll.
  Which return URLs the payment service accepts is unverified.

Unverified (nothing was sent to crucible or api.krunker.io): the payment-intent answer's exact field set,
whether the service honours `checkout: "hosted"` for a client that is not framed, and the `returnUrl` it accepts.

## State in this repo

`client/src/ui/kio_checkout_stripe.c` is a stub: `kio_checkout_stripe_available()` returns 0, so the selector
offers Xsolla's "Credit/Debit Card" only. Wiring it is the list above: a crucible-iap service in
krunker_account, then the poll / claim / consume in kio_checkout_stripe.c behind the existing header.
