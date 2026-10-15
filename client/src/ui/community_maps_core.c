// Community maps: the data rules of the Games window and the Host Game map pick (community_maps.h).
// No client state and no HTTP: community_maps.c and client/tests/community_maps_test.c use them.

#include <community_maps.h>

#include <map_catalog.h>
#include <modes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// percent-encodes s (RFC 3986 unreserved characters kept) into out; -1 when it does not fit
static int encode_into(char *out, const size_t size, const char *s) {
    size_t n = 0;
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char) *s;
        const int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (n + (keep ? 1 : 3) >= size) return -1;
        if (keep) out[n++] = (char) c;
        else n += (size_t) snprintf(out + n, size - n, "%%%02X", c);
    }
    out[n] = 0;
    return (int) n;
}

size_t community_search_path(char *out, const size_t size, const char *query, const int category, const int source) {
    if (!out || !size) return 0;
    out[0] = 0;
    char enc[256];
    if (encode_into(enc, sizeof(enc), query ? query : "") < 0) return 0;
    char cat[24] = "";
    if (category >= 0 && category < MAP_CATEGORY_COUNT) snprintf(cat, sizeof(cat), "&cat=%d", category);
    const char *src = source == COMMUNITY_SOURCE_OFFICIAL ? "&source=official" : source == COMMUNITY_SOURCE_COMMUNITY ? "&source=community" : "";
    const int n = snprintf(out, size, "/api/search?type=map&val=%s%s%s", enc, cat, src);
    if (n <= 0 || (size_t) n >= size) {
        out[0] = 0;
        return 0;
    }
    return (size_t) n;
}

int community_source_of(const char *value) {
    if (value && !strcmp(value, "official")) return COMMUNITY_SOURCE_OFFICIAL;
    if (value && !strcmp(value, "community")) return COMMUNITY_SOURCE_COMMUNITY;
    return COMMUNITY_SOURCE_ALL;
}

size_t community_mode_names(char *out, const size_t size, const cJSON *modes) {
    if (!out || !size) return 0;
    out[0] = 0;
    size_t n = 0;
    const cJSON *m, *list = cJSON_IsArray(modes) ? modes : NULL;
    cJSON_ArrayForEach(m, list) {
        if (!cJSON_IsNumber(m) || !mode_config((int) m->valuedouble)) continue;
        const int w = snprintf(out + n, size - n, "%s%s", n ? ", " : "", mode_name((int) m->valuedouble));
        if (w < 0 || (size_t) w >= size - n) {
            out[n] = 0; // the names that fit
            break;
        }
        n += (size_t) w;
    }
    return n;
}

int community_catalogue_index(const char *name, const char *creator) {
    if (!name || !*name) return -1;
    const int want_custom = creator && creator[0] && strcmp(creator, "Krunker") != 0;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if ((e->source == MAP_SOURCE_CUSTOM) == want_custom && !strcmp(e->name, name)) return (int) i;
    }
    for (size_t i = 0; i < map_catalog_count(); i++) {
        if (!strcmp(map_catalog_at(i)->name, name)) return (int) i;
    }
    // a bare name matches like cmd_map (prefixes too); a row's name is exact (a published map
    // may be named like the start of a catalogue one)
    return creator && creator[0] ? -1 : map_catalog_find(name);
}

int community_slot_of(const cJSON ***maps, size_t *count, const cJSON *json) {
    if (!maps || !count || !json) return -1;
    for (size_t i = 0; i < *count; i++) {
        if ((*maps)[i] == json) return (int) i;
    }
    const cJSON **grown = realloc((void *) *maps, (*count + 1) * sizeof(*grown));
    if (!grown) return -1;
    grown[*count] = json;
    *maps = grown;
    return (int) (*count)++;
}
