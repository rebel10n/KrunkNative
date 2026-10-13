#pragma once
// Player target selectors (KrunkNative): the one resolver behind every player-targeting console /
// chat command (shared/src/chat_commands.c god, kick, give, ...; server/src/cli.c kick, ban, op,
// send). Not in Krunker v3.1.1 / v7.1.4, whose commands take one name or id
// (resolveOnlinePlayerByQuery). A query is, in this order:
//   *  @all  @a           every candidate (humans and bots)
//   @me  @s  !            the caller (needs one: the server console has none)
//   @others               everyone but the caller
//   @bots  @humans        bots / humans only
//   @team  @enemies       the caller's team (the caller included) / everyone else; with no team
//                         (FFA, team 0) @team is the caller alone and @enemies everyone else
//   @alive  @dead         spawned / not spawned
//   a glob with * or ?    case-insensitive over the name, as typed or normalised (Joel*, *bot?)
//   a name or id          normalised (lowercase, [^a-z0-9] dropped): exact name or id, else a
//                         unique prefix (TARGET_EXACT_ONLY: exact only)
// Permissions are not decided here: a command keeps its own gate whatever the query matched.
// Pure logic over a candidate list, no Game access, so any host (players, connections) can use it.
#include <stddef.h>

#define TARGET_SELECT_MAX 256

typedef struct {
    const char *name;        // display name ("" / NULL: matched by id only)
    int id;                  // Player.uid / connection id
    int team;                // 0 = no team (FFA)
    unsigned char bot;
    unsigned char alive;     // Player.active
    unsigned char self;      // the caller (at most one)
} TargetCandidate;

typedef enum {
    TARGET_OK = 0,           // count >= 1 matches in out[]
    TARGET_EMPTY,            // nothing to look for ("" or only punctuation)
    TARGET_NOT_FOUND,        // a name / id matched nobody
    TARGET_AMBIGUOUS,        // a name prefix matched several (no exact match)
    TARGET_NO_MATCH,         // a selector or glob matched nobody
    TARGET_NO_SELF,          // @me / @team / @enemies without a caller
} TargetStatus;

enum {
    TARGET_EXACT_ONLY = 1,   // names / ids: no unique-prefix fallback
};

typedef struct {
    TargetStatus status;
    int count;               // matches (out[] holds the first `max` of them)
    int selector;            // the query was a selector or a glob (several players may match)
} TargetResult;

// 1 when `query` is a selector or a glob (not a plain name / id).
int target_is_selector(const char *query);
// Indices into cands[] of every match, in candidate order. Returns result.count.
int target_select(const TargetCandidate *cands, int n, const char *query, int flags, int *out, int max, TargetResult *result);
// Case-insensitive glob (* any run, ? one character), exposed for the tests.
int target_glob_match(const char *pattern, const char *text);
