#pragma once
// The server_bin command line the Host Game flow ends at (client/src/ui/host_flow_ui.c). One line a
// player copies into a terminal to run the game they set up on a dedicated server. Every flag it
// writes exists in server/src/main.c (parse_args). The game settings use the dot-path form
// `--set gameserver.game.<var>=<value>` (the serverConfig keys of shared/src/game_config.c, the
// same keys as server.cfg's `game.<var>` lines); the class / weapon / effect overrides go through
// `--set gameserver.game.classes|weps|cls|prks=<json>`.
#include <stddef.h>
#include <cJSON.h>

// gameserver.host_type names (server/src/config.c): the Host Game chooser's host types.
#define HOST_COMMAND_HOST_TYPES 5

#define HOST_COMMAND_WORD_MAX 8192  // one word of the line, before quoting
#define HOST_COMMAND_GAME_BYTES 3900 // the game settings' text: the server keeps 4096 bytes of them

#define HOST_COMMAND_ERR_SIZE (-1)  // `out` too small, or a word does not fit
#define HOST_COMMAND_ERR_GAME (-2)  // the game settings are longer than the server's game request keeps

typedef struct {
    const char *program;     // the binary the line starts with (NULL: "server_bin")
    const char *map_file;    // positional map JSON (a community map not in the catalogue); NULL: none
    const char *map_key;     // --set gameserver.maps=<key>, a catalogue key (kanji, 311_burg); NULL: none
    int host_type;           // gameserver.host_type 0 custom, 1 comp, 2 rent, 3 zombies, 4 raid; -1: none
    int port;                // --port (0 or the default 21015: omitted)
    const char *hostname;    // --hostname TEXT (NULL / "": omitted)
    const char *password;    // --password TEXT (NULL / "": omitted)
    const char *public_host; // --public-host HOST (NULL / "": omitted)
    int public_port;         // --public-port PORT (0: omitted)
    int headless;            // --headless (1: no interactive console)
} HostCommand;

// Writes the line (NUL-terminated) and returns its length, or HOST_COMMAND_ERR_SIZE /
// HOST_COMMAND_ERR_GAME. `request` is the Host Game config (ui_host_config_collect's object:
// "modes", "classes", "weps" / "cls" / "prks" and every serverConfig row; NULL for none). Only the
// settings that differ from their defaults are written; the server's own defaults apply to the rest.
int host_command_build(const HostCommand *spec, const cJSON *request, char *out, size_t size);

// Shell quoting of one word: plain words stay bare, anything else goes in 'single quotes' with '
// written as '\''. Returns the length written (without the NUL), or HOST_COMMAND_ERR_SIZE.
int host_command_quote(const char *value, char *out, size_t size);
