# Checkout in the system browser (10.0.4)

Question: can the native client run krunker.io's real-money purchases with the payment provider's hosted
page in the player's system browser, and how does it learn the outcome? No embedded webview and no
krunker.io page or game JS in the client.

Refs: `deob-10.0.4.js` = `/home/user/jails/kru/src/Krunker_v10.0.4/deob/game.deob.js`. `frvr-sdk.deob.js`
= the FRVR SDK (`frvr-sdk.min.js`, 2026-10-28) decoded under `~/.cache/kn-official/pay/`.

## 1. Xsolla (card; KR packs, catalogue, bundles)

Bundle flow:
- Selector: `xsollaPayB` "Credit/Debit Card" (:212382); `XsollaPopup` (:212333) and `iïiïïíi` (:212391) send
  C2S `xst` for KR packs (:212348), `xpurchase` for catalogue products (:212396). Bundles `xstb` (:212927),
  `xstbp` (:212959, :216413), `xstbpl` (:213007, :216338).
- Server answer S2C `xst` (`LOAD_XSOLLA`, :98500; S2C table :226151) calls `iíìííìî(token, sandbox)` (:212041).
  Only two values are used: the access token and the sandbox flag (:212041-212050, the 2-arg call at :212043).
- The widget (`XPayStationWidget`, :211996-212033) is built with `access_token`, a 740x630 lightbox and
  `sandbox` (:211997-212004). Its host is `secure.xsolla.com` (`sandbox-secure.xsolla.com` for sandbox), path
  `/paystation2/?<params>` (`xsolla-widget.min.js`, `getPaystationUrl`).
- Widget events: `STATUS` sets `paymentInfo.status` (:212022-212024, :212030-212032); `STATUS_DONE` sends
  C2S `xsin` with `paymentInfo.invoice` (:212025-212028); `CLOSE` runs `purchaseError` if the status is
  "troubled", else `purchaseCancel` (:212013-212016).
- Result: S2C `pur` (`PURCHASE_RESPONSE`, :98334; handler `iïíîííï` :212158) carries error, funds and the
  purchase index. On success it sets the wallet's funds (:212170-212171) and shows `purchase.kr.success`
  (:212165). `purchaseError` / `purchaseCancel` are the widget-driven failure texts.

Answer to 1:
- The only thing the bundle needs from the provider page is `paymentInfo.invoice` in `STATUS_DONE`. Nothing
  else supplies it: the `xst` answer is only [token, sandbox] (:212041), and `paymentInfo` appears only in
  the three widget handlers (:212023, :212028, :212031).
- The bundle has no client-side poll for Xsolla. `getIAPTransaction` polls only the FRVR SDK (section 2).
- Whether the server learns of a completed invoice without the page (an Xsolla webhook, then a `pur` push)
  cannot be confirmed here. The krunker server source is not in the jail: `krunkbox` is the proxy and update
  tool, and a grep of it finds no `xsin`, `xsolla` or webhook handling.
- So a browser-only Xsolla flow has no invoice on the client side. It needs a server endpoint or push keyed
  by the transaction that the client can wait on. That is a server change, not something the bundle
  provides.

## 2. Stripe (the "Stripe" button; FRVR)

Bundle flow:
- The button shows when `canShowStripe` (:212399; index.html: not MS PWA, Steam or Epic) and `FRVR.iap`.
- `iïíîíìï` (:212420-212470): `iap.checkout(body)` (`/iap/checkout`, :26073-26078; bodies :212430-212446) gives
  `data.payment`. Then `FRVR['iap']['purchase'](data.payment)` (:212450), which is the SDK's card popup. Then
  `iap.claim(channelId || "stripe", productId, purchaseId)` (`/iap/claim`, :26079-26088; call :212405).
- Recovery on login: `getUnconsumedPurchases()` claims every "stripe" purchase (:213757-213761). A payment
  finished in the browser is recovered at the next login even if the window was closed.

SDK (`frvr-sdk.deob.js`):
- `purchase(payment)` (:4430-4444) and `purchaseWith` (:4448-4480) call the provider's purchase. The errors
  "Purchase cancelled by user" (:4484-4486) and "Purchase window blocked by the browser" (:4488-4491) show it
  opens a window, so the hosted page is a URL that the SDK opens.
- The Economy purchase (:5330-5440) gets `paymentUrl` and `iapTransactionId` from `createShopIntent`
  (`POST {iapHost}/shop/intent/{gameId}`, :4993-5000; `payment_url` passed at :5419).
- Completion is a poll, not the page: `GET {apiHost}/games/{gameId}/purchases/iap/{iapTransactionId}`
  (:5069-5072), every 300 ms for 30 s by default (:5366-5401, :5397). `status === "success"` with a `result`
  finishes it (:5370-5376). "rejected" and "refunded" are errors (:5383-5388).
- No `checkout.stripe.com`, `js.stripe.com` or client-secret string appears in the SDK. The hosted URL is
  server-provided.

Answer to 2: the browser path works for Stripe if the server's answer carries a hosted URL (the bundle's
`data.payment`, or the SDK's `paymentUrl`). This is server-side data, and no sample response is in the
jail, so it is unconfirmed. Completion is polled with the IAP transaction id, as the SDK does.

## 3. Return path

- The bundle sets no `return_url`, `success_url`, `redirect_url`, `returnUrl` or `successUrl` for any payment.
  A grep finds only UI redirect flags and the Twitch OAuth URL (:226371).
- The SDK takes `returnUrl` only with the `checkout` option (:5014-5017, :5349-5350), and the bundle never
  passes that option.
- Xsolla's PayStation sends its status to the widget, never to a URL the client sees.
- So after payment the provider page stays on its own success page. Nothing redirects to the client, and the
  client cannot see the browser close. Detection must come from the server.

## Recommendation: how the native client detects completion

Open the provider's hosted URL in the system browser, then do not treat the browser or its close as a
signal. Wait on the server's purchase status, polled at the SDK's rhythm (300 ms, 30 s), and apply the
answer through the existing `pur` handling (funds, success text).

- Stripe: poll `GET /games/{gameId}/purchases/iap/{iapTransactionId}` (:5069-5072, :5366-5401). The
  transaction id comes from the shop-intent answer.
- Xsolla: the bundle has no status poll. The server must expose the invoice's status or push `pur` when the
  invoice is paid. Without that, the native client cannot tell a finished payment from an abandoned one.
  This is the blocker and needs a server change.

Unverified: the `/iap/checkout` answer shape (hosted URL and transaction id), the `pur` push on webhook, and
whether the server fulfils Xsolla payments without the client's `xsin`. None is in the bundle or the jail.
