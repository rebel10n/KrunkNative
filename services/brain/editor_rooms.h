/* services/brain/editor_rooms.h - Krunker v10's multiplayer map editing rooms (editor_rooms.c). */
#pragma once

struct SocialSession;
struct cJSON;

typedef struct EditorRoomRoute {
    const char *label;
    void (*fn)(struct SocialSession *s, const struct cJSON *frame);
} EditorRoomRoute;

/* the mp* labels, NULL-terminated */
extern const EditorRoomRoute k_editor_room_routes[];

/* the route of an mp* label, or NULL */
static inline const EditorRoomRoute *editor_room_route(const char *label) {
    if (!label || label[0] != 'm' || label[1] != 'p') return 0;
    for (const EditorRoomRoute *r = k_editor_room_routes; r->label; r++) {
        const char *a = r->label, *b = label;
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return r;
    }
    return 0;
}

/* the socket closed (or is leaving): out of its room, if any */
void editor_rooms_leave(struct SocialSession *s);
