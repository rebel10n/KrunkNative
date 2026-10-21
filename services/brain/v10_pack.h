/*
 * services/brain/v10_pack.h - Krunker v10's packed JSON (v10.js "pack" / "unpack", the object
 * at v10.js 27744-27985): the gapi /data/skins and /data/spins bodies. The client reads the
 * response text with responseText and decodes it with unpack, not JSON.parse.
 *
 * Layout: "<strings>^<integers>^<floats>^<tree>", the three dictionaries joined with "|", each
 * string escaped (" " -> "+", "+" "|" "^" "%" -> %2B %7C %5E %25). Integers are base 36 of their
 * dictionary slot (the strings count + the position), floats are decimal slots after both.
 * The tree is "@" (array) or "$" (object: key, value pairs) followed by children joined with "|"
 * and closed by "]"; a leaf is a base-36 slot, or -1 true, -2 false, -3 null, -4 "" (the
 * decoder reads them as base-36 numbers).
 */
#ifndef KN_BRAIN_V10_PACK_H
#define KN_BRAIN_V10_PACK_H

#include "cJSON.h"

/* The packed text of json (a JSON value; arrays and objects are the usual case). Returns a
 * malloc'd NUL-terminated string the caller frees, or NULL when out of memory. */
char *v10_pack_json(const cJSON *json);

#endif
