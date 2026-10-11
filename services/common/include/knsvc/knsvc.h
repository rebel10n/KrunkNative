/*
 * knsvc/knsvc.h - umbrella header for the KrunkNative services common library.
 * Library `knsvc` (no libpq): buf, log, config, json, loop, http, ws, crypto, msgpack.
 * Library `knsvc_db` (libpq): db, migrate. Include knsvc/db.h / knsvc/migrate.h separately.
 * Docs: doc/backend.md, section "common".
 */
#ifndef KNSVC_KNSVC_H
#define KNSVC_KNSVC_H

#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/msgpack.h"
#include "knsvc/ws.h"

#endif
