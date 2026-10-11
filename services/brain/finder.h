/*
 * services/brain/finder.h - central server finder (roles.h BRAIN_ROLE_FINDER).
 *
 *   POST /servers/heartbeat (alias /servers/submit, and both under /finder)
 *        {v, host, port, query_port, name, community, online_mode, shutdown?}
 *        -> {ok:true, interval}; host "" / absent = the request's IP.
 *   GET  /servers?map=&mode=&tag=&space=1&online_mode=0|1&community=&region=&q=&limit=
 *        -> {ok:true, servers:[...]}
 *
 * Nothing self-reported is listed: the finder polls each server's KNINFO query over
 * UDP (shared/include/server_info.h contract: 0xFF x4 "KNINFO" 0x00 nonce ->
 * 0xFF x4 "KNinfo" 0x00 json) and lists only entries whose last poll answered with the
 * nonce it sent. Entries expire after finder.expire_s without a heartbeat or
 * finder.max_failures failed polls in a row.
 *
 * The pure table lives in finder_table.c (unit tested by tests/test_finder.c); this
 * header is the part main.c needs.
 */
#ifndef KN_BRAIN_FINDER_H
#define KN_BRAIN_FINDER_H

#include "brain.h"

int finder_init(Brain *b);
void finder_shutdown(Brain *b);

#endif
