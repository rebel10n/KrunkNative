# KrunkNative — single entry point for building, running and testing.
#
# CMake does the compiling (vendored GLFW/FreeType/Lexbor/replxx/cJSON, Emscripten
# toolchain); this Makefile drives it so every workflow is one command.
# Configuration presets live in CMakePresets.json (shared with IDEs).
#
#   make                 debug client + server + test binary
#   make run             run the debug client            (ARGS="--offline" MAP=kanji)
#   make web             Emscripten client -> dist/web/  (needs emsdk)
#   make test            shared movement test
#   make ui-test         in-house HTML engine tool  (ARGS="lexbor-probe")
#   make help            everything else
#
# Overridable variables: GENERATOR, JOBS, HTML_ENGINE (ON/OFF; OFF = native client
# without HTML UI), EMSDK, ARGS, MAP, PORT, CMAKE_FLAGS (extra -D flags), KN_GA_ID
# (Google Analytics id for the web pages; off by default, deploy/pages/README.md).

SHELL := bash
.DEFAULT_GOAL := build

# =========================================================================== configuration

CMAKE       ?= cmake
JOBS        ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
GENERATOR   ?= $(if $(shell command -v ninja 2>/dev/null),Ninja,)
GEN_FLAG    := $(if $(GENERATOR),-G "$(GENERATOR)",)
HTML_ENGINE ?= ON
CMAKE_FLAGS ?=
EMSDK       ?= $(HOME)/emsdk
ARGS        ?=
MAP         ?=
PORT        ?= 8080
KN_GA_ID    ?=

DEBUG_DIR     := cmake-build-debug
RELEASE_DIR   := cmake-build-release
STRIKE_DIR    := cmake-build-strike
WEB_DIR       := cmake-build-web
WEB_DEBUG_DIR := cmake-build-web-debug
DIST_WEB      := dist/web
BRIDGE_DIR    := net-bridge

ifeq ($(OS),Windows_NT)
  CLIENT_BIN := client.exe
  SERVER_BIN := server.exe
  TEST_BIN   := proc_input_test.exe
  PYTHON     ?= python
else
  CLIENT_BIN := client_bin
  SERVER_BIN := server_bin
  TEST_BIN   := proc_input_test
  PYTHON     ?= python3
endif

# CMake refuses to change generator inside an existing build dir: wipe it when
# GENERATOR differs from what the cache was configured with.
CHECK_GENERATOR = if [ -n "$(GENERATOR)" ] && [ -f "$(1)/CMakeCache.txt" ] && \
	  ! grep -q '^CMAKE_GENERATOR:INTERNAL=$(GENERATOR)$$' "$(1)/CMakeCache.txt"; then \
	  echo "generator changed, wiping $(1)"; rm -rf "$(1)"; fi

# Configure: preset + generator + HTML engine switch + user flags (the web build
# ignores the switch: its UI is always the browser page driven by web_dom.c).
# It runs only when the build dir has no cache yet or the configure line changed (stamp file):
# re-running `cmake --preset` on every build regenerated the whole build tree each time. Edits to
# a CMakeLists.txt are still picked up: the generator re-runs CMake by itself when one changes.
CONFIGURE = $(call CHECK_GENERATOR,$(2)); \
	kn_line='$(1) $(GEN_FLAG) -DKRUNKNATIVE_HTML_ENGINE=$(HTML_ENGINE) $(CMAKE_FLAGS)'; \
	if [ ! -f "$(2)/CMakeCache.txt" ] || [ "$$(cat "$(2)/.kn-configure" 2>/dev/null)" != "$$kn_line" ]; then \
	  $(CMAKE) --preset $(1) $(GEN_FLAG) -DKRUNKNATIVE_HTML_ENGINE=$(HTML_ENGINE) $(CMAKE_FLAGS) && \
	  printf '%s' "$$kn_line" > "$(2)/.kn-configure"; \
	fi
BUILD     = $(CMAKE) --build $(1) --parallel $(JOBS)

# Emscripten: use emcc from PATH, else activate the emsdk checkout in $(EMSDK).
EMSDK_ACTIVATE = if ! command -v emcc >/dev/null 2>&1; then \
	  [ -f "$(EMSDK)/emsdk_env.sh" ] || { echo "emsdk not found: set EMSDK=/path/to/emsdk (https://emscripten.org/docs/getting_started/)"; exit 1; }; \
	  source "$(EMSDK)/emsdk_env.sh" >/dev/null 2>&1; \
	fi; export EMSDK="$${EMSDK:-$(EMSDK)}"

# Map argument for run targets: MAP=kanji -> assets/maps/kanji.json. The official maps are the v10.0.2 files
# under assets/maps/official/ (NN_<name>.json); a catalogue name whose file is gone (kanji, burg_new, ...) loads
# its catalogue file (map_catalog_load_file, a stderr line says so). The v3.1.1 maps are 311_<name>, its old_* maps 152_<name>.
MAP_ARG = $(if $(MAP),assets/maps/$(MAP).json,)

# --------------------------------------------------------------------------- native

.PHONY: build build-client build-server configure release configure-release all
build: configure ## Build debug client, server and tests (default)
	$(call BUILD,$(DEBUG_DIR))

# The run targets build only what they start (not every test program and tool).
build-client: configure ## Build only the debug client
	$(call BUILD,$(DEBUG_DIR)) --target $(CLIENT_BIN)

build-server: configure ## Build only the debug server
	$(call BUILD,$(DEBUG_DIR)) --target $(SERVER_BIN)

configure: ## Configure the debug build
	@$(call CONFIGURE,debug,$(DEBUG_DIR))

release: configure-release ## Build release client and server
	$(call BUILD,$(RELEASE_DIR))

configure-release: ## Configure the release build
	@$(call CONFIGURE,release,$(RELEASE_DIR))

all: build release web ## Build everything: debug, release and web

# ANGLE (libEGL + libGLESv2) for the OpenGL ES 3.0 backend: Metal on macOS (README.md
# "ANGLE / Metal (macOS)"). The macOS configure picks ANGLE_DIR up automatically
# (KRUNKNATIVE_GL_BACKEND=auto); elsewhere CMAKE_FLAGS=-DKRUNKNATIVE_GL_BACKEND=angle.
ANGLE_DIR ?= $(HOME)/.cache/krunknative/angle
export ANGLE_DIR
.PHONY: angle
angle: ## Build the pinned ANGLE (Metal on macOS; Vulkan/GL on Linux) into ANGLE_DIR (several GB of checkout in ANGLE_DIR.work)
	tools/angle/build-angle.sh "$(ANGLE_DIR)"

.PHONY: run launcher launcher-release run-release server server-release server-dist server-install network
# make run starts the local backend (accounts, matchmaking) when config/config.json exists and
# no brain answers on BRAIN_PORT; BACKEND=0 skips it (the client then plays on the default
# server and shows "Failed to connect" for account actions).
BACKEND    ?= 1
BRAIN_PORT ?= 5050
# the client never reads config/config.json (DB credentials): a non-default brain port reaches
# it as KRUNKNATIVE_BRAIN_URL (5050 is the client's own probed default); an explicit
# KRUNKNATIVE_BRAIN_URL in the environment wins
RUN_BRAIN_ENV = $(if $(KRUNKNATIVE_BRAIN_URL),,$(if $(filter-out 5050,$(BRAIN_PORT)),KRUNKNATIVE_BRAIN_URL=http://127.0.0.1:$(BRAIN_PORT)))
# Runs leave V-Sync and the frame cap to the client's own settings unless asked: VSYNC=1 and/or
# FRAME_CAP=144 force them (env override over the stored setting), VSYNC=0 FRAME_CAP=0 force
# them off. Unset, nothing is passed and the stored settings apply. The shipped defaults are unchanged.
VSYNC ?=
FRAME_CAP ?=
RUN_PERF = $(if $(VSYNC),KRUNKNATIVE_VSYNC=$(VSYNC),) $(if $(FRAME_CAP),KRUNKNATIVE_FRAME_CAP=$(FRAME_CAP),)
run: build-client ## Run the debug client (ARGS="--offline" MAP=<name>; VSYNC=1 FRAME_CAP=144 to force pacing, VSYNC=0 FRAME_CAP=0 to force uncapped; BACKEND=0 skips the backend)
	@if [ "$(BACKEND)" != 0 ] && [ -f config/config.json ] && ! curl -s -m 1 http://127.0.0.1:$(BRAIN_PORT)/mm/status 2>/dev/null | grep -q '"brain"'; then \
	  echo "[run] no brain on 127.0.0.1:$(BRAIN_PORT): starting the local backend (make backend-up; BACKEND=0 skips)"; \
	  $(MAKE) --no-print-directory backend-up BRAIN_PORT=$(BRAIN_PORT) || echo "[run] backend-up failed: accounts are unavailable (see make backend-status)"; \
	fi
	cd $(DEBUG_DIR) && $(RUN_BRAIN_ENV) $(RUN_PERF) ./$(CLIENT_BIN) $(ARGS) $(if $(MAP),../$(MAP_ARG),)

# The launcher (BoxOffice, launcher/, doc/backend.md 9.8): updates check + game content sync, then client_bin.
# The dev build has no kn-install.json, so it never replaces the build; content syncs into ./assets.
launcher: build ## Run the debug launcher (ARGS="--cli --no-update -- --offline"; UPDATES_URL=off skips the server)
	cd $(DEBUG_DIR) && KRUNKNATIVE_ASSETS_DIR=$(CURDIR)/assets/ $(if $(UPDATES_URL),KRUNKNATIVE_UPDATES_URL=$(UPDATES_URL),) ./BoxOffice $(ARGS)

launcher-release: release ## Run the release launcher (ARGS=...)
	cd $(RELEASE_DIR) && KRUNKNATIVE_ASSETS_DIR=$(CURDIR)/assets/ $(if $(UPDATES_URL),KRUNKNATIVE_UPDATES_URL=$(UPDATES_URL),) ./BoxOffice $(ARGS)

run-release: release ## Run the release client
	cd $(RELEASE_DIR) && $(RUN_PERF) ./$(CLIENT_BIN) $(ARGS) $(if $(MAP),../$(MAP_ARG),)

server: build-server ## Run the debug server (ARGS=...)
	cd $(DEBUG_DIR) && ./$(SERVER_BIN) $(ARGS)

server-release: release ## Run the release server
	cd $(RELEASE_DIR) && ./$(SERVER_BIN) $(ARGS)

# --------------------------------------------------------------------------- Krunker Strike (flavor)
# Krunker Strike is a product flavor of Krunker Box Office (doc/v10/krunker-strike.md, flavor.h). The
# flavor is a runtime flag (--flavor strike / KN_FLAVOR); `make strike` also builds a binary whose
# compile-time DEFAULT is Strike, for dedicated Strike packages. Default builds are unchanged.
.PHONY: strike run-strike server-strike
strike: ## Build a Krunker Strike-branded client + server into cmake-build-strike (compile-time flavor)
	@$(call CONFIGURE,strike,$(STRIKE_DIR))
	$(call BUILD,$(STRIKE_DIR))

run-strike: build-client ## Run the debug client as Krunker Strike (runtime --flavor; ARGS=, MAP=)
	cd $(DEBUG_DIR) && $(RUN_BRAIN_ENV) $(RUN_PERF) ./$(CLIENT_BIN) --flavor strike $(ARGS) $(if $(MAP),../$(MAP_ARG),)

server-strike: build-server ## Run the debug server as Krunker Strike (runtime --flavor; ARGS=)
	cd $(DEBUG_DIR) && ./$(SERVER_BIN) --flavor strike $(ARGS)

# strike-dist builds a Release tree whose compile-time DEFAULT flavor is Strike
# (-DKRUNKNATIVE_FLAVOR=strike via the strike-release preset) and packages it with the
# base release tooling (tools/release/package-server.sh, package-client.sh), unchanged.
# The binaries report "Krunker Strike" (flavor_display_name()); every file name, archive
# name and identifier stays krunknative. The base release path is untouched.
STRIKE_RELEASE_DIR := cmake-build-strike-release
STRIKE_DIST        ?= dist/strike
.PHONY: strike-release strike-dist
strike-release: ## Build the Krunker Strike-defaulted Release client + server into cmake-build-strike-release
	@$(call CONFIGURE,strike-release,$(STRIKE_RELEASE_DIR))
	$(CMAKE) --build $(STRIKE_RELEASE_DIR) --parallel $(JOBS) --target server_bin client_bin

strike-dist: strike-release ## Krunker Strike dedicated-server + client packages in dist/strike/ (compile-time flavor)
	tools/release/strike-dist.sh $(STRIKE_RELEASE_DIR) $(STRIKE_DIST)

# The web build carries Krunker Strike as its own installable app at strike/ (platform/web/pwa.py):
# its own manifest, icons and page, which starts the client as Krunker Strike with no flag.
.PHONY: strike-app-test
strike-app-test: ## Both web apps in headless Chromium: manifests, ids / scopes, /strike/ boots Krunker Strike, separate storage (run `make web` first)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	timeout 1200 $(PYTHON) -I client/tests/strike_app_e2e.py $(DIST_WEB)

SERVER_DIST ?= dist/server
PREFIX      ?= /opt/krunknative
server-dist: release ## Dedicated server bundle in dist/server/ (server_bin + maps + server.cfg): copy it anywhere and run ./server_bin
	@rm -rf $(SERVER_DIST) && mkdir -p $(SERVER_DIST)/assets $(SERVER_DIST)/cfg
	cp $(RELEASE_DIR)/$(SERVER_BIN) $(SERVER_DIST)/
	cp -r assets/maps $(SERVER_DIST)/assets/
	cp assets/skins.json $(SERVER_DIST)/assets/ 2>/dev/null || true
	[ -f $(SERVER_DIST)/cfg/server.cfg ] || cp config/server.cfg.example $(SERVER_DIST)/cfg/server.cfg
	cp services/scripts/kn-query.py $(SERVER_DIST)/
	@echo "dedicated server ready in $(SERVER_DIST)/: edit cfg/server.cfg, then cd $(SERVER_DIST) && ./$(SERVER_BIN)"

server-install: server-dist ## Install the dedicated server bundle to PREFIX (default /opt/krunknative; keeps an existing cfg/server.cfg)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/cfg
	install -m 0755 $(SERVER_DIST)/$(SERVER_BIN) $(DESTDIR)$(PREFIX)/bin/
	cp -r $(SERVER_DIST)/assets $(DESTDIR)$(PREFIX)/
	[ -f $(DESTDIR)$(PREFIX)/cfg/server.cfg ] || install -m 0644 config/server.cfg.example $(DESTDIR)$(PREFIX)/cfg/server.cfg
	@echo "installed to $(DESTDIR)$(PREFIX) (systemd: deploy/systemd/kn-gameserver@.service)"

network: build services ## Coordinator + N dedicated servers from one file (NETWORK=services/scripts/network.example.json)
	python3 -I services/scripts/kn-network up $(or $(NETWORK),services/scripts/network.example.json)

.PHONY: test
test: build ## Run the shared movement/collision test (+ the knet transport, official Krunker codec and session translation, content fetch, billboard ad, lag compensation, editor bus, editor model, nav mesh, client update, meshopt, window state and resource pack tests)
	./$(DEBUG_DIR)/$(TEST_BIN)
	./$(DEBUG_DIR)/update_test
	./$(DEBUG_DIR)/asset_fetch_test
	./$(DEBUG_DIR)/backend_online_test
	./$(DEBUG_DIR)/knet_test
	./$(DEBUG_DIR)/server_names_test
	./$(DEBUG_DIR)/krunker_proto_test
	./$(DEBUG_DIR)/v10_translate_test
	./$(DEBUG_DIR)/v10_bridge_test
	./$(DEBUG_DIR)/krunker_link_test
	./$(DEBUG_DIR)/krunker_sync_test
	./$(DEBUG_DIR)/krunker_host_test
	./$(DEBUG_DIR)/krunker_kio_routes_test
	./$(DEBUG_DIR)/menu_sources_test
	./$(DEBUG_DIR)/lagcomp_test
	./$(DEBUG_DIR)/map_v7_test
	./$(DEBUG_DIR)/map_v10_test
	./$(DEBUG_DIR)/map_drop_test
	./$(DEBUG_DIR)/social_hub_test
	./$(DEBUG_DIR)/world_v7_test
	./$(DEBUG_DIR)/ramp_fix_test
	./$(DEBUG_DIR)/app_url_test
	./$(DEBUG_DIR)/vfx_debris_test
	./$(DEBUG_DIR)/market_stats_test
	./$(DEBUG_DIR)/follower_counts_test
	./$(DEBUG_DIR)/ks_game_v1003_test
	./$(DEBUG_DIR)/ai_floor_test
	./$(DEBUG_DIR)/triggers_test
	./$(DEBUG_DIR)/trigger_authority_test
	./$(DEBUG_DIR)/editor_bus_test
	./$(DEBUG_DIR)/editor_serialize_test . --golden doc/ref/krunker-v7.1.4/editor/inspector-edits.json
	./$(DEBUG_DIR)/weapons_v7_test
	./$(DEBUG_DIR)/combat_test
	./$(DEBUG_DIR)/charge_grapple_test
	./$(DEBUG_DIR)/piss_test
	./$(DEBUG_DIR)/nettspend_test
	./$(DEBUG_DIR)/bot_chat_test
	./$(DEBUG_DIR)/ruleset_test
	./$(DEBUG_DIR)/hitboxes_test
	./$(DEBUG_DIR)/v10_gameplay_test
	./$(DEBUG_DIR)/desktop_notify_test
	$(PYTHON) -I tools/strike/gen_rules.py --check
	./$(DEBUG_DIR)/strike_rules_test
	./$(DEBUG_DIR)/strike_assist_test
	./$(DEBUG_DIR)/strike_hud_fx_test
	./$(DEBUG_DIR)/strike_sandbox_test
	$(PYTHON) -I tools/strike/gen_economy.py --check
	./$(DEBUG_DIR)/strike_economy_test
	./$(DEBUG_DIR)/flavor_gameplay_test
	./$(DEBUG_DIR)/flavor_storage_test
	./$(DEBUG_DIR)/strike_maps_test
	./$(DEBUG_DIR)/projectiles_render_test
	./$(DEBUG_DIR)/death_cam_test
	./$(DEBUG_DIR)/third_person_test
	./$(DEBUG_DIR)/billboard_ad_test --headless
	env -u DISPLAY -u WAYLAND_DISPLAY ./$(DEBUG_DIR)/texture_checker_test --headless
	./$(DEBUG_DIR)/zombies_test
	./$(DEBUG_DIR)/weapon_req_test
	./$(DEBUG_DIR)/junk_test
	./$(DEBUG_DIR)/config_test
	./$(DEBUG_DIR)/comp_ranked_test
	./$(DEBUG_DIR)/sandbox_config_test
	./$(DEBUG_DIR)/object_manager_test
	./$(DEBUG_DIR)/weapons_audit_test
	./$(DEBUG_DIR)/ks_storage_test
	./$(DEBUG_DIR)/navgen_test
	./$(DEBUG_DIR)/bot_objective_test
	./$(DEBUG_DIR)/bot_stuck_test
	./$(DEBUG_DIR)/bot_move_test
	./$(DEBUG_DIR)/season_test
	./$(DEBUG_DIR)/target_select_test
	./$(DEBUG_DIR)/meshopt_test .
	./$(DEBUG_DIR)/window_state_test
	./$(DEBUG_DIR)/gl_driver_env_test
	./$(DEBUG_DIR)/win_geom_test
	./$(DEBUG_DIR)/pointer_lock_test
	./$(DEBUG_DIR)/mod_pack_test .
	./$(DEBUG_DIR)/nettspend_audio_test $(CURDIR)
	$(MAKE) --no-print-directory html-engine-test-run

# --------------------------------------------------------------------------- html-engine
# The in-house HTML/CSS engine (client/src/html/engine/README.md, doc/html-ui-engine.md).
# These targets use the debug directory with HTML_ENGINE=ON (the default) so they also
# work after a `make HTML_ENGINE=OFF`.

.PHONY: ui-test ui-import assets-import uidiff uidiff-paint html-engine-test html-engine-test-run
html-engine-test: build ## The engine's unit tests (client/src/html/engine/tests: html_<module>_test, ctest -L html-engine; the GL ones headless)
	$(MAKE) --no-print-directory html-engine-test-run

html-engine-test-run:
	cd $(DEBUG_DIR) && env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_AUDIO_BACKEND=null ctest -L '^html-engine$$' --timeout 300 --output-on-failure

viewer-diff: build ## Viewer render scenarios vs the v7.1.4 viewer captures (doc/ref/krunker-v7.1.4/viewer, headless; EGL_DEVICE=)
	EGL_DEVICE=$(or $(EGL_DEVICE),nvidia) tools/kref/viewer-diff.sh $(DEBUG_DIR)

frame-yield-test: build ## Mid-frame asset loads (spray texture, joining players) never show a frame without the world (web yields emulated, headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I client/tests/frame_yield_e2e.py $(DEBUG_DIR)

offline-dead-test: build ## Offline play: a dead local player gets no tracers / projectiles / player sync until it respawns, like online (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/offline_dead_filter_e2e.py $(DEBUG_DIR)

mobile-touch-test: build ## Phone controls with fingers only (TAP TO PLAY, joystick, look, fire, jump, reload; touch script events; headless, offline)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/mobile_touch_e2e.py $(DEBUG_DIR)

nametag-test: build ## Offline FFA bots: each bot's nametag shows when in view with line of sight, no random flips, the killer's tag clears on respawn (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1500 $(PYTHON) -I client/tests/nametag_visibility_e2e.py $(DEBUG_DIR)

hide-names-test: build ## Client > Hide player names: the numbering unit test, the menu / scoreboard / chat DOM of two scenarios and a live offline scoreboard, on and off (headless; EGL_DEVICE=)
	./$(DEBUG_DIR)/player_names_test
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/hide_names_e2e.py $(DEBUG_DIR)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/hide_names_live_e2e.py $(DEBUG_DIR)

selection-test: build ## Page text selection (dev console): engine checks (ui_test selection) + a headless client drag / Ctrl+C / Ctrl+A run (EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) env -u DISPLAY -u WAYLAND_DISPLAY timeout 120 ./$(DEBUG_DIR)/ui_test --headless selection
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/console_select_e2e.py $(DEBUG_DIR)

slowmo-arms-test: build ## The first-person arms stay locked to the camera through the round-end slow motion and after it (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/slowmo_arms_e2e.py $(DEBUG_DIR)

spawn-camera-test: build ## Every spawn of ours (Play, Play again, third person, a new round) looks along the spawn's yaw, level, with no leftover recoil / zoom / chase (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/spawn_camera_e2e.py $(DEBUG_DIR)

mod-keys-f6-test: build ## F6 opens the editor from the menu and the game page, and does nothing while a text field has focus (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/mod_keys_f6_e2e.py $(DEBUG_DIR)

weapons-client-test: build ## The Slimer streak, Charge Rifle, Grappler and Combat Knife through the real client, as v10 plays them (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/weapons_client_e2e.py $(DEBUG_DIR)

gun-unequip-test: build ## The Physics Gun and the Tool Gun put away and back: the Tools pane tiles, Z, a slot key; the other weapons still fire (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/gun_unequip_e2e.py $(DEBUG_DIR)

piss-hose-test: build ## The Piss's first-person hose is held in the trigger hand and its spray leaves from the hose's tip (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/piss_hose_e2e.py $(DEBUG_DIR)

nettspend-audio-test: build ## The Nettspend clips decode and the Drank reload / Money Spread / laced hooks play them (miniaudio null backend, no GL)
	$(DEBUG_DIR)/nettspend_audio_test $(CURDIR)

nettspend-loadout-test: build ## Nettspend is a class card, its Loadout tab shows Purple Drank + Money Spread, the preview wears them and the Drank reload plays the pour up (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/nettspend_loadout_e2e.py $(DEBUG_DIR)

locked-class-test: build ## The Nettspend's card in the class tab: locked with "Not on official servers" in an official lobby, and kept when the official class list arrives (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/locked_class_label_e2e.py $(DEBUG_DIR)

guest-earned-test: build ## A guest's Unclaimed Rewards box (v10_rewards.c): the end screen's and the menu's, its KR / XP amounts and its glow (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/guest_earned_e2e.py $(DEBUG_DIR)

leaderboard-scroll-test: build ## The capped Leaderboard list of a guest's end card scrolls under the wheel: three clicks move its first visible row (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/leaderboard_scroll_e2e.py $(DEBUG_DIR)

version-menu-e2e: build ## Krunker Box Office shows Krunker's version only: --version and the Outdated box name the build by its commit, the What's New tile shows the newest changelog update on Krunker's line (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/version_menu_e2e.py $(DEBUG_DIR)

guest-ledger-e2e: build ## A guest's Unclaimed Rewards come from kn_brain's guest profile: the brain's 12 KR / 800 XP show, the browser's stale 3 / 3 do not (kn_brain on sqlite, headless)
	timeout 600 $(PYTHON) -I client/tests/guest_ledger_e2e.py $(DEBUG_DIR)

guest-ledger-server-e2e: build ## A guest's round on server_bin credits its balance in kn_brain, the "ger" push follows, a live sign-in zeroes it (kn_brain on sqlite)
	timeout 400 $(PYTHON) -I server/tests/guest_ledger_server_e2e.py $(DEBUG_DIR)

secondary-req-test: build ## The Select Secondary picker applies v10's weapon req: the Grappler's card is not clickable in an official game, and is in a custom one (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/secondary_req_e2e.py $(DEBUG_DIR)

nettspend-vfx-test: build ## Nettspend: the Drank's cup + pour-up, the Money Spread's bills and the laced post-effect, headless frames (EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1800 $(PYTHON) -I client/tests/nettspend_vfx_e2e.py $(DEBUG_DIR)

nettspend-arms-test: build ## Nettspend: the Drank's arms in third and first person, the off hand's rest beside the hip (and the refill's end), the off elbow bends down and back (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/nettspend_arms_e2e.py $(DEBUG_DIR)

minimap-round-test: build ## The in-game minimap is round from its first visible frame after joining, respawning, a map change, spectating and at other sizes / minimapScale (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/minimap_round_e2e.py $(DEBUG_DIR)

preview-hole-test: build ## The menu shade covers the class preview box seamlessly at many window sizes / UI scales (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1800 $(PYTHON) -I client/tests/preview_hole_e2e.py $(DEBUG_DIR)

menu-bleed-test: build ## The menu covers the window: no 3D scene at its right or bottom edge at many sizes / UI scales, at startup and across live resizes (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/menu_bleed_e2e.py $(DEBUG_DIR)

class-preview-test: build ## Every class draws: the menu's class preview model and the first-person view in play (offline Kanji, headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/class_preview_all_e2e.py $(DEBUG_DIR)

preview-hole-test-web: ## The same for the web build in headless Chromium at several sizes / devicePixelRatios and a browser zoom (run `make web` first)
	timeout 1800 $(PYTHON) -I client/tests/preview_hole_e2e.py --web $(DIST_WEB)

.PHONY: v10-market-e2e
v10-market-e2e: build ## v10 Market & Trading from the v10 page: list, buy, quick sell, trade offer, accept (local kn_brain on sqlite, headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/v10_market_e2e.py $(DEBUG_DIR) $(CURDIR)

native-auth-test: build ## Native client accounts e2e: register with email, verify, login, forgot password -> mailed code -> reset (local kn_brain + fake SMTP, headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/native_auth_e2e.py $(DEBUG_DIR)

account-manager-test: build ## Native client Account Manager e2e: register, Add stores a typed login (no account server needed), Use fills the login card, Login, restart resumes, Logout, refused session forgotten, Remove, krunker.io row (local kn_brain + fake SMTP, headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1500 $(PYTHON) -I client/tests/account_manager_e2e.py $(DEBUG_DIR)

resize-early-test: build ## Window resizes during startup / stale resize events reach the viewport and HTML layout (private Xvfb + xfwm4 + xdotool)
	timeout 600 $(PYTHON) -I client/tests/resize_early_e2e.py $(DEBUG_DIR)

window-center-test: build ## The client window maps where it stays: centred on the work area (launch, page windows) or at its kept position (F4 / F5), never moved after (private Xvfb)
	timeout 600 $(PYTHON) -I client/tests/window_center_e2e.py $(DEBUG_DIR)

asset-fetch-test: build ## Native on-demand content fetch: deleted airdrop files are downloaded from stub origins, verified and pop in; mismatch refused, 404 given up once (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/asset_fetch_e2e.py $(DEBUG_DIR)

user-assets-test: build ## A community map's Krunker user assets (texture, OBJ model) fetched from stub hosts (a 403'ing Krunker, the mirror) and drawn; 404 / block page show the placeholder; late models built into the running map; video sidecars play (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/user_assets_e2e.py $(DEBUG_DIR)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/user_assets_late_e2e.py $(DEBUG_DIR)

console-settings-test: build ## Dev console switch survives Settings opens / restarts, console clear / cls, chat /clear (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/console_settings_e2e.py $(DEBUG_DIR)

crash-capture-test: build ## Crash capture: a forced NULL dereference writes a report with a frame list, the next start names it once, a forced 6 s stall writes a freeze dump and the client keeps running (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/crash_capture_e2e.py $(DEBUG_DIR)

raw-mouse-test: build ## Raw mouse motion has one switch (Controls > Raw Mouse Input): on by default, off and on by click, stored Raw Input migrates once (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/raw_mouse_e2e.py $(DEBUG_DIR)

bhop-setting-test: build ## The bhop mode is one setting (Settings > Game > Gameplay > Bhop Mode): the row shows five modes, the movement follows it, the bhop key moves the row, it survives a restart, the v3.1.1 "bhop" key migrates (headless; EGL_DEVICE=)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1200 $(PYTHON) -I client/tests/bhop_setting_e2e.py $(DEBUG_DIR)

fps-uncapped-test: build ## Launch paths run uncapped with V-Sync and Frame Cap off in the stored settings: make -n pacing, no display, windowed (xvfb-run), --flavor strike's store
	timeout 900 $(PYTHON) -I client/tests/fps_uncapped_e2e.py $(DEBUG_DIR)

.PHONY: strike-vsync-test
strike-vsync-test: build ## Krunker Strike's Display tab: V-Sync and Frame Cap are there, change, and reach the client (the fps lines, the krunknative-strike store; headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/strike_vsync_e2e.py $(DEBUG_DIR)

gltf-test: build ## GLTF loader on v7 animated store models via the pet path, pets.js follow AI, skin animations (headless GL)
	env -u DISPLAY -u WAYLAND_DISPLAY ./$(DEBUG_DIR)/gltf_test --headless

ui-test: ## Build ui_test (HTML_ENGINE=ON) and run it: ARGS="lexbor-probe" (default: help)
	@$(MAKE) --no-print-directory configure HTML_ENGINE=ON
	$(call BUILD,$(DEBUG_DIR)) --target ui_test
	./$(DEBUG_DIR)/ui_test $(or $(ARGS),help)

ui-import: ## Regenerate ui/ from the Krunker v10 live site mirror (page, windows, hub / editor, fonts, ligatures; overwrites hand edits: diff first) via tools/ui-import
	$(PYTHON) -I tools/ui-import/import.py --ui ui

assets-import: ## Copy the v3.1.1 content mod.zip lacks (textures/previews/) into assets/ (tools/ui-import/game_assets.py)
	$(PYTHON) -I tools/ui-import/game_assets.py --assets assets

.PHONY: gun-unequip-test mod-keys-f6-test store-import store-check assets-import-v7 assets-check-v7 assets-import-live assets-check-live gltf-test viewer-diff frame-yield-test offline-dead-test resize-early-test window-center-test native-auth-test account-manager-test asset-fetch-test console-settings-test crash-capture-test slowmo-arms-test spawn-camera-test piss-hose-test weapons-client-test selection-test minimap-round-test preview-hole-test preview-hole-test-web menu-bleed-test class-preview-test user-assets-test
assets-import-v7: ## Import the Krunker content set into assets/ (models, textures, sounds, the v10 store table) and regenerate the store tables
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --assets assets
	node tools/ui-import/gen_viewer_store.js

assets-check-v7: ## Drift check of assets/ and the generated store tables against the Krunker content set (exit 1 on any difference)
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --check --assets assets
	GEN_CHECK=1 node tools/ui-import/gen_viewer_store.js
	node tools/ui-import/test_gen_viewer_store.js

store-import: ## The store table: assets/skins.json from Krunker v10.0.2 (tools/ui-import/v10_skins.js), viewer_store_table.c, store_spins_table.c
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --tables --assets assets
	node tools/ui-import/gen_viewer_store.js

store-check: ## Drift check of the v10 store table, its generated C tables and test, and the store items' asset files
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --tables --check --assets assets
	GEN_CHECK=1 node tools/ui-import/gen_viewer_store.js
	node tools/ui-import/test_gen_viewer_store.js
	node tools/ui-import/v10_store_assets.js

assets-import-live: ## Add the files the Krunker content set lacks from the live asset hosts (tools/ui-import/live_assets.lock, then live_assets_v10.lock: the v10 items' files; KN_FETCH_PROXY=, never overwrites)
	$(PYTHON) -I tools/ui-import/fetch_live_assets.py install --assets assets --ui ui
	$(PYTHON) -I tools/ui-import/fetch_live_assets.py install --lock tools/ui-import/live_assets_v10.lock --roots assets --assets assets --ui ui

assets-check-live: ## List both locks' files missing from (exit 1) or differing in assets/ and ui/img
	$(PYTHON) -I tools/ui-import/fetch_live_assets.py check --assets assets --ui ui
	$(PYTHON) -I tools/ui-import/fetch_live_assets.py check --lock tools/ui-import/live_assets_v10.lock --assets assets --ui ui

.PHONY: video-frames
video-frames: ## The native HTML engine's <video> frame sidecars (<file>.knv next to the v10 page's videos in assets/; ffmpeg; ARGS=--check)
	$(PYTHON) -I tools/ui-import/video_frames.py --assets assets $(ARGS)

.PHONY: branding branding-check
branding: ## Fetch krunker.io's app icons (tools/branding/branding.lock, sha256-pinned; KN_FETCH_PROXY=) and write the PWA / window / Windows / Android / iOS icons
	$(PYTHON) -I tools/branding/fetch_branding.py install

branding-check: ## The verbatim krunker.io icons in the tree match tools/branding/branding.lock (exit 1 otherwise)
	$(PYTHON) -I tools/branding/fetch_branding.py check

.PHONY: window-icon-test
window-icon-test: build ## The window icon is the Box Office art (assets/img/icon.png) and the window class is not the title (headless; KRUNKNATIVE_ASSETS_DIR for a worktree)
	timeout 300 $(PYTHON) -I client/tests/window_icon_test.py $(DEBUG_DIR); rc=$$?; [ $$rc -eq 0 ] || [ $$rc -eq 77 ]

.PHONY: webview2 webview2-check
webview2: ## Fetch the WebView2 SDK header and loader for the Windows captcha webview (tools/webview2/webview2.lock, sha256-pinned; KN_FETCH_PROXY=) into ~/.cache/krunknative/webview2
	$(PYTHON) -I tools/webview2/fetch_webview2.py install

webview2-check: ## The staged WebView2 SDK files match tools/webview2/webview2.lock (exit 1 otherwise)
	$(PYTHON) -I tools/webview2/fetch_webview2.py check

# Official Krunker servers: the build constants (AHKEY, clientVersion, buildVersion, packet name
# tables, cheat-check keystream) are extracted statically from Krunker's client bundle into the
# committed shared/include/krunker_build.h + tools/krunker/build.json (tools/krunker/extract_build.py).
# SRC=<path|url> reads another bundle (game.deob.js / game.debug.js / game.min.js / manifest).
.PHONY: krunker-build krunker-build-check krunker-build-test
krunker-build: ## Regenerate shared/include/krunker_build.h from Krunker's bundle (default https://kru.eli.gift/game.deob.js, cached by ETag; SRC=, FORCE=1)
	$(PYTHON) -I tools/krunker/extract_build.py $(SRC) $(if $(FORCE),--force,)

krunker-build-check: ## Regenerate krunker_build.h from the current Krunker bundle into a temp file; exit 1 when the committed header / build.json differ (bundle unreachable: header <-> build.json only, STRICT=1 fails)
	$(PYTHON) -I tools/krunker/extract_build.py $(SRC) --check $(if $(STRICT),--strict,)

krunker-build-test: ## Extractor regression test: every local Krunker bundle (deob / debug / min / v10.js / the current one) incl. the per-player layouts, synthetic broken anchors, the header compiles
	$(PYTHON) -I tools/krunker/test_extract.py

.PHONY: maps-import-v7 maps-check-v7
maps-import-v7: ## Import Krunker v7.1.4's 32 official map thumbnails into ui/img/maps (the map JSON is v10's: assets/maps/official/)
	$(PYTHON) -I tools/maps/import_v7_maps.py

maps-check-v7: ## Drift check of the map thumbnails in ui/img/maps against Krunker v7.1.4
	$(PYTHON) -I tools/maps/import_v7_maps.py --check


# uidiff renders every scenario through the real client and its C UI logic
# (`client_bin --render-scenario`, client/src/ui/ui_scenario.c) on llvmpipe inside one
# Xvfb server (1920x1080, large enough for every scenario size), then pixel-diffs the
# regions (tools/uidiff/README.md). SCENARIOS="main-menu customize-class" limits the run;
# UIDIFF_NO_XVFB=1 uses the current display; UIDIFF_OUT/UIDIFF_FUZZ_SCALE pass through.
# UIDIFF_HEADLESS=1 renders with no window and no X server (EGL, llvmpipe);
# UIDIFF_EGL_DEVICE=nvidia renders headless on the NVIDIA GPU (client/include/headless.h).
SCENARIOS ?=
UIDIFF_XVFB_SCREEN ?= 1920x1080x24
UIDIFF_HEADLESS ?= 0
UIDIFF_EGL_DEVICE ?=

uidiff-paint: ## Pixel-diff the paint pages (tools/uidiff/paint: 3D flip, masks, backdrop-filter, color-mix) against their Chrome captures (ui_test render-page)
	@$(MAKE) --no-print-directory configure HTML_ENGINE=ON
	$(call BUILD,$(DEBUG_DIR)) --target ui_test
	tools/uidiff/paint.sh $(DEBUG_DIR)/ui_test

uidiff: ## Render tools/uidiff scenarios with client_bin --render-scenario (HTML_ENGINE=ON) and pixel-diff them (SCENARIOS=...)
	@$(MAKE) --no-print-directory configure HTML_ENGINE=ON
	$(call BUILD,$(DEBUG_DIR)) --target $(CLIENT_BIN)
	@export UIDIFF_RENDER_CMD="$(CURDIR)/$(DEBUG_DIR)/$(CLIENT_BIN) --render-scenario" \
	        LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
	        UIDIFF_HEADLESS="$(UIDIFF_HEADLESS)" UIDIFF_EGL_DEVICE="$(UIDIFF_EGL_DEVICE)"; \
	if [ "$${UIDIFF_NO_XVFB:-0}" = 1 ] || [ "$(UIDIFF_HEADLESS)" = 1 ] || [ -n "$(UIDIFF_EGL_DEVICE)" ]; then \
	  tools/uidiff/uidiff.sh $(DEBUG_DIR)/$(CLIENT_BIN) $(SCENARIOS); \
	else \
	  command -v xvfb-run >/dev/null || { echo "uidiff: xvfb-run not found (UIDIFF_NO_XVFB=1 uses the current display)"; exit 2; }; \
	  xvfb-run $${UIDIFF_XVFB_ARGS:--a} -s "-screen 0 $(UIDIFF_XVFB_SCREEN)" \
	    tools/uidiff/uidiff.sh $(DEBUG_DIR)/$(CLIENT_BIN) $(SCENARIOS); \
	fi

# --------------------------------------------------------------------------- web

.PHONY: web web-debug web-serve pages-github assets-cdn
web: ## Build the Emscripten client into dist/web/ (release; KN_GA_ID=G-... adds Google Analytics)
	@$(EMSDK_ACTIVATE); $(call CONFIGURE,web,$(WEB_DIR)) && $(call BUILD,$(WEB_DIR))
	@$(PYTHON) -I deploy/pages/ga.py $(DIST_WEB) --id "$(KN_GA_ID)"
	@echo "web client ready in $(DIST_WEB)/  (make web-serve, or: make bridge)"

web-debug: ## Build the Emscripten client with debug info into dist/web/
	@$(EMSDK_ACTIVATE); $(call CONFIGURE,web-debug,$(WEB_DEBUG_DIR)) && $(call BUILD,$(WEB_DEBUG_DIR))
	@$(PYTHON) -I deploy/pages/ga.py $(DIST_WEB) --id "$(KN_GA_ID)"

pages-github: ## Stage dist/web for GitHub Pages into dist/gh-pages, as CI publishes it (deploy/pages/README.md)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	deploy/pages/github.sh

assets-cdn: ## Mirror dl.krunk.io's game content and deploy it to the two Pages projects of the asset CDN (deploy/asset-shards.json; ARGS=--dry-run counts only)
	deploy/pages/assets-cdn.sh $(ARGS)

web-serve: ## Serve dist/web/ for local testing (PORT=8080); use ?offline=1 without a server
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@echo "http://localhost:$(PORT)/?offline=1   (ctrl-c to stop; for the game server bridge use 'make bridge')"
	cd $(DIST_WEB) && $(PYTHON) -m http.server $(PORT)

# --------------------------------------------------------------------------- mobile
# Krunker's mobile apps wrapped the web game; so do these (platform/README.md).
# The web client itself detects phones and shows the touch UI (client/src/ui/ui_mobile.c).
# ANDROID_ARGS passes Gradle properties, e.g. ANDROID_ARGS="-Pkrunknative.wsUrl=ws://192.168.1.10:8080/native".

# ANDROID_APP=strike builds the Krunker Strike app instead (io.krunknative.strike: its own name, icon
# and page, platform/web/pwa.py's strike/), which installs next to Krunker Box Office (io.krunknative.app).
ANDROID_DIR  := platform/android
ANDROID_APP  ?= boxoffice
ANDROID_APK_NAME := $(if $(filter strike,$(ANDROID_APP)),app-strike,app)
ANDROID_APK  := $(ANDROID_DIR)/app/build/outputs/apk/debug/$(ANDROID_APK_NAME)-debug.apk
ANDROID_ARGS ?=

.PHONY: android android-only android-install android-release ios-prepare
android: web android-only ## Build the web client, then the Android debug APK (needs JDK 17+ and the Android SDK; ANDROID_APP=strike: Krunker Strike)

android-only: ## Android debug APK from the existing dist/web (no web rebuild)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@[ -n "$${ANDROID_HOME:-$${ANDROID_SDK_ROOT:-}}" ] || [ -f $(ANDROID_DIR)/local.properties ] || \
	  { echo "Android SDK not found: set ANDROID_HOME (or write sdk.dir=... to $(ANDROID_DIR)/local.properties)"; exit 1; }
	cd $(ANDROID_DIR) && ./gradlew --no-daemon assembleDebug -Pkrunknative.app=$(ANDROID_APP) $(ANDROID_ARGS)
	@ls -l $(ANDROID_APK)

android-install: ## Install the debug APK on a connected device (adb)
	adb install -r $(ANDROID_APK)

# The published APK (client.yml job android): a thin app over ANDROID_SERVER_URL, release-signed
# with the key of tools/release/android-keystore.sh (ANDROID_KEYS = its keystore.env).
ANDROID_KEYS       ?= $(HOME)/.config/kn-secrets/android/keystore.env
ANDROID_SERVER_URL ?= https://krunk.io/$(if $(filter strike,$(ANDROID_APP)),strike/,)
android-release: ## Signed release APK over https://krunk.io/ (ANDROID_APP=strike: Krunker Strike over https://krunk.io/strike/; key: tools/release/android-keystore.sh)
	@[ -f "$(ANDROID_KEYS)" ] || { echo "no $(ANDROID_KEYS): run tools/release/android-keystore.sh"; exit 1; }
	set -a; . "$(ANDROID_KEYS)"; set +a; cd $(ANDROID_DIR) && \
	  ./gradlew --no-daemon assembleRelease -Pkrunknative.app=$(ANDROID_APP) -Pkrunknative.serverUrl=$(ANDROID_SERVER_URL) $(ANDROID_ARGS)
	@ls -l $(ANDROID_DIR)/app/build/outputs/apk/release/$(ANDROID_APK_NAME)-release.apk

ios-prepare: ## Copy dist/web into platform/ios/web (then: cd platform/ios && xcodegen generate, on macOS)
	platform/ios/prepare.sh $(DIST_WEB)

# --------------------------------------------------------------------------- net-bridge

.PHONY: bridge bridge-install bridge-check
bridge-install: ## Install net-bridge dependencies
	cd $(BRIDGE_DIR) && npm install

bridge: $(BRIDGE_DIR)/node_modules ## Host dist/web/ and bridge /native WebSocket <-> native TCP server (port 8080)
	cd $(BRIDGE_DIR) && npm run web

# The v10 client's bridge in C (server/bridge, doc: tools/krunker-local/README.md): the official v10 client's
# WebSocket (msgpack, v10 labels) on ws://V10_BIND:V10_LISTEN/ws, translated to a KrunkerBox game server's TCP
# (V10_NATIVE). Foreground; ctrl-c closes the sessions. ARGS= adds flags (e.g. --lobby=KN:local).
V10_LISTEN ?= 8081
V10_BIND   ?= 127.0.0.1
V10_NATIVE ?= 127.0.0.1:21015
.PHONY: v10-bridge
v10-bridge: build ## Run the v10 client bridge (C): ws://V10_BIND:V10_LISTEN/ws -> native V10_NATIVE (defaults 127.0.0.1:8081 -> 127.0.0.1:21015)
	./$(DEBUG_DIR)/v10_bridge --v10-listen=$(V10_LISTEN) --bind=$(V10_BIND) --native=$(V10_NATIVE) $(ARGS)

bridge-check: $(BRIDGE_DIR)/node_modules ## Typecheck + unit-test net-bridge, and test the WebRTC gateway when Go is installed
	cd $(BRIDGE_DIR) && npm run typecheck && npm test
	@if command -v go >/dev/null; then cd $(BRIDGE_DIR)/rtc && GOTOOLCHAIN=local go test ./...; else echo "go not installed, skipping net-bridge/rtc tests"; fi

# WebTransport relay (net-bridge/wt, doc/transport.md): HTTP/3 on UDP WT_ADDR, datagrams 1:1 to
# the game server's UDP port. Without WT_CERT/WT_KEY it makes a self-signed cert and writes its
# hash to $(DIST_WEB)/wt.json (serverCertificateHashes; the web client fetches it).
WT_BIN  := $(BRIDGE_DIR)/wt/kn-wt
WT_ADDR ?= :8443
.PHONY: wt wt-build
wt-build: ## Build the WebTransport relay (net-bridge/wt/kn-wt, needs Go)
	cd $(BRIDGE_DIR)/wt && go build -trimpath -o kn-wt .

wt: wt-build ## Run the WebTransport <-> UDP relay on UDP $(WT_ADDR) (WT_CERT=/WT_KEY= for a real certificate)
	$(WT_BIN) -addr $(WT_ADDR) -info-file $(DIST_WEB)/wt.json $(if $(WT_CERT),-cert $(WT_CERT) -key $(WT_KEY))

# WebRTC gateway (net-bridge/rtc, doc/transport.md): signaling over HTTP on RTC_HTTP, ICE/DTLS on
# the one UDP port RTC_ICE; DataChannel messages 1:1 to the game server's UDP port. Writes
# $(DIST_WEB)/rtc.json (the gateway URL; the web client fetches it when kn-rtc is empty).
RTC_BIN  := $(BRIDGE_DIR)/rtc/kn-rtc
RTC_HTTP ?= 127.0.0.1:8085
RTC_ICE  ?= :21100
.PHONY: rtc rtc-build
rtc-build: ## Build the WebRTC gateway (net-bridge/rtc/kn-rtc, needs Go)
	cd $(BRIDGE_DIR)/rtc && go build -trimpath -o kn-rtc .

rtc: rtc-build ## Run the WebRTC <-> UDP gateway (signaling http $(RTC_HTTP), ICE udp $(RTC_ICE))
	$(RTC_BIN) -http $(RTC_HTTP) -ice-addr $(RTC_ICE) -info-file $(DIST_WEB)/rtc.json

alt-test: build ## The alt manager's vault: AES-256-GCM vectors, tamper and wrong-key rejection, no plaintext on disk, profile switching, the file keystore and the store (headless, file keystore)
	cd $(DEBUG_DIR) && env -u DISPLAY -u WAYLAND_DISPLAY ctest -R '^alt_vault_test$$' --output-on-failure

test-net: build ## knet transport unit test (loss/dup/reorder), v7 chat/voice channels + sprays (social_test), 2-client UDP end-to-end (headless)
	./$(DEBUG_DIR)/knet_test
	./$(DEBUG_DIR)/social_test
	./$(DEBUG_DIR)/votekick_test
	server/tests/udp_e2e.sh $(DEBUG_DIR)
	KN_UDP_LOSS="15 5 5" server/tests/udp_e2e.sh $(DEBUG_DIR)

.PHONY: krunker-official-e2e
krunker-official-e2e: build ## Official krunker.io mode (krunker_net.h) against a local matchmaker + game server stub: native, and the web build when dist/web has one
	timeout 900 $(PYTHON) -I server/tests/krunker_official_e2e.py $(DEBUG_DIR) $(DIST_WEB)

.PHONY: krunker-account-e2e
krunker-account-e2e: build ## The krunker.io account backend (krunker_account.h) through v10's one account UI: in-game sign-in (Turnstile, 2FA form), krunker.io Quick Join, refresh, accessToken / at=, sign out, against local stubs
	timeout 400 $(PYTHON) -I server/tests/krunker_account_e2e.py $(DEBUG_DIR)

.PHONY: account-unify-e2e
account-unify-e2e: build ## One account UI, two backends (backend_ui.h active_account()): krunk.io DEV + official guest, official signed in, switching back (header, player list, dev actions, alt manager, login modal), kn_brain + krunker.io stubs
	timeout 400 $(PYTHON) -I server/tests/account_unify_e2e.py $(DEBUG_DIR)

.PHONY: kio-store-e2e
kio-store-e2e: build ## The Store as krunker.io's while it is the active backend (kio_store.h): shop/displays + ranked/shop from krunker.io stubs, every purchase / spin / redeem read-only, Box Office's store again after leaving
	timeout 400 $(PYTHON) -I server/tests/kio_store_e2e.py $(DEBUG_DIR)

.PHONY: ranked-krunker-e2e
ranked-krunker-e2e: build ## Ranked on a krunker.io account (krunker_ranked.h): v10's paths with Bearer, the queue socket (QUEUED, MATCHED, COOLDOWN), KrunkNative's brain untouched, krunker.io wins when both are signed in (fake krunker.io, scratch kn_brain, headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I server/tests/ranked_krunker_e2e.py $(DEBUG_DIR)

.PHONY: target-select-e2e
target-select-e2e: build ## Player target selectors on server_bin: god * / ungod * / give * over clients and bots, a guest refused, kick @bots, rcon op / kick *
	timeout 300 $(PYTHON) -I server/tests/target_select_e2e.py $(DEBUG_DIR)

.PHONY: bots-chat-e2e
bots-chat-e2e: build ## Bot chat (shared/src/bot_chat.c): gg lines at match end, AI lines through kn_brain against a STUBBED AI gateway, filters, rate limits, fallbacks (local kn_brain on sqlite)
	timeout 900 $(PYTHON) -I server/tests/bots_chat_e2e.py $(DEBUG_DIR)

.PHONY: command-auth-e2e
command-auth-e2e: build ## Command authorisation (doc/v7/command-auth.md): forged commands / dev packets / tickets / rcon guesses refused + logged, dev / op / rcon still work
	timeout 400 $(PYTHON) -I server/tests/command_auth_e2e.py $(DEBUG_DIR)

.PHONY: live-signin-e2e
live-signin-e2e: build ## Live sign-in: sign in / switch / sign out mid-session upgrades the live connection (DEV rights, Object Manager), forged / throttled / banned refused (local kn_brain on sqlite, headless client)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/live_signin_e2e.py $(DEBUG_DIR)

.PHONY: votekick-e2e
votekick-e2e: build ## The player list's vote kick + K.P.D call: vote / kick / voteKickBan, the call recorded in kpd_calls, an officer's verdict (kn_brain on the local PostgreSQL; KN_TEST_DB_DRIVER=sqlite)
	timeout 300 $(PYTHON) -I server/tests/votekick_e2e.py $(DEBUG_DIR)

.PHONY: playerlist-ui-e2e
playerlist-ui-e2e: build ## The in-game Player List's controls on a headless client: mutes (saved, chat dropped), Votekick, Call K.P.D popup, the officer's suspect row (EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 400 $(PYTHON) -I client/tests/playerlist_e2e.py $(DEBUG_DIR)

.PHONY: row-centre-e2e
row-centre-e2e: build ## The action buttons of the Player List and Account Manager rows sit within 1 px of their row's middle (headless --render-scenario, the box dump)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/row_centre_e2e.py $(DEBUG_DIR)

.PHONY: billboard-e2e
billboard-e2e: build ## server.cfg billboard / billboard_image: packets before INIT for late joiners and map changes, unset and URL configs send none
	timeout 300 $(PYTHON) -I server/tests/billboard_e2e.py $(DEBUG_DIR)

.PHONY: idle-kick-e2e
idle-kick-e2e: build ## Server idle kick: shooting / strafing / looking / chatting / respawning players stay, AFK and menu idlers are kicked
	timeout 120 $(PYTHON) -I server/tests/idle_kick_e2e.py $(DEBUG_DIR)

.PHONY: join-spawn-e2e
join-spawn-e2e: build ## A joiner gets its own dead SPAWN after INIT (the join waits for its ENT), then spawns after the ENT
	timeout 120 $(PYTHON) -I server/tests/join_spawn_e2e.py $(DEBUG_DIR)

.PHONY: nuke-e2e
nuke-e2e: build ## The Nuke on server_bin: a guest's 25 kills, key 3 through a headless client, "n" 0 / 1, every enemy dies (FFA + TDM), flash and kill feed (EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I server/tests/nuke_e2e.py $(DEBUG_DIR)

.PHONY: ruleset-e2e
ruleset-e2e: build ## Krunker v10.0.2 is the only version: server_bin and the offline client play v10 with no choice (INIT, server-info, no --ruleset, no picker)
	timeout 120 $(PYTHON) -I server/tests/ruleset_e2e.py $(DEBUG_DIR)

.PHONY: v10-settings-e2e
v10-settings-e2e: build ## v10 Settings: one setting per tab applied + persisted, search, presets, export, keybind rebinding, sandbox bind, reset (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/v10_settings_e2e.py $(DEBUG_DIR)

.PHONY: settings-wiring-e2e
settings-wiring-e2e: build ## v10 settings that used to do nothing: set each one, observe its effect in game (fpsRate, nametagScale, dmgType, custom hitmarker / damage, endMessage, ...; headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/settings_wiring_e2e.py $(DEBUG_DIR)

.PHONY: pickup-chat-e2e junk-wallet-e2e
pickup-chat-e2e: build ## Pickup feedback on server_bin: Object Manager Junk + G prints "You found [0.01] Junk" to the finder only, a weapon pickup plays pick_0
	timeout 120 $(PYTHON) -I server/tests/pickup_chat_e2e.py $(DEBUG_DIR)

junk-wallet-e2e: build ## The Junk counter on the v10 page (doc/v10/junk.md): signed in with 12.34 Junk it reads 12.34; a game server's credit reads 12.39 through the brain's push (kn_brain on sqlite, headless)
	timeout 400 $(PYTHON) -I server/tests/junk_wallet_e2e.py $(DEBUG_DIR)

.PHONY: v10-rewards-e2e
v10-rewards-e2e: build ## v10 rewards: Get Signup Rewards -> register -> perks once, a challenge done on server_bin, a Battle Pass claim + gift (headless)
	timeout 600 $(PYTHON) -I server/tests/v10_rewards_e2e.py $(DEBUG_DIR)

.PHONY: reconnect-test
reconnect-test: build ## Console reload + rejoin online + offline, F5 relaunch, console reconnect / sandbox / offline <map>, --sandbox (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I server/tests/reconnect_key_e2e.py $(DEBUG_DIR)

.PHONY: mm-token-e2e
mm-token-e2e: build ## Matchmaker token (generate-token -> seek-game -> join ticket), Quickplay seek prefs, official krunker.io lobbies (stub), F4 / F5 (kn_brain + 2 server_bin + headless client; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I server/tests/mm_token_e2e.py $(DEBUG_DIR)

.PHONY: server-tabs-e2e
server-tabs-e2e: build ## The Servers window's krunk.io / krunker.io tabs (each its own list, v10's layout) and the tab's + Host Game: the backend it hosts on, the session's kept (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/server_tabs_e2e.py $(DEBUG_DIR)

.PHONY: menu-sources-e2e
menu-sources-e2e: build ## The menu's Popular Now is Box Office's only (hidden on krunker.io and Both) and its Live Streams come from the brain or api.krunker.io (brain / krunker.io stubs; headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/menu_sources_e2e.py $(DEBUG_DIR)

.PHONY: games-window-e2e
games-window-e2e: build ## The Custom Games window and the social hub's Maps: a Popular Categories tile opens Search on its category (the hub asks the brain for it), the tab row stays inside the window, a Popular card keeps the tab, krunker.io's lists fill (brain / krunker.io stubs; headless; EGL_DEVICE=...; KN_GAMES_WINDOW_SHOTS=dir for the frames)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/games_window_e2e.py $(DEBUG_DIR)

.PHONY: seek-region-e2e
seek-region-e2e: build ## Ping-aware seeks (v10 getRegion): the fastest region's stub /ping wins, cached, an explicit region wins; never Mumbai for a New York player (krunker.io and brain stubs; headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/seek_region_e2e.py $(DEBUG_DIR)

.PHONY: seek-cross-region-e2e
seek-cross-region-e2e: build ## A seek finds its region's node with brain.matchmaking.cross_region off: the official and the native client's v10 ids, a saved KrunkNative code (kn_brain + four edge nodes, sqlite)
	timeout 300 $(PYTHON) -I server/tests/seek_cross_region_e2e.py $(DEBUG_DIR)

.PHONY: quickplay-retry-e2e
quickplay-retry-e2e: build ## Quick Play retries a full lobby until the player is in one (Find Game, Quick Match, Auto Seek on Launch: full, full, then a lobby with room; a stub brain; headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I server/tests/quickplay_retry_e2e.py $(DEBUG_DIR)

.PHONY: seek-servers-migrate-e2e
seek-servers-migrate-e2e: build ## Server Type reset once per profile (knSeekServersMigrated): a saved krunker.io choice from the old popup becomes Box Office once; a later pick is kept (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/seek_servers_migrate_e2e.py $(DEBUG_DIR)

.PHONY: client-proxy-e2e
client-proxy-e2e: build ## Settings > Client > Proxy: krunker.io's lobby list through a stub HTTP CONNECT / SOCKS5 proxy, persisted, live change, HTTP_PROXY, the ACCESS DENIED message (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I server/tests/client_proxy_e2e.py $(DEBUG_DIR)

.PHONY: auto-restart-e2e
auto-restart-e2e: build ## Auto-restart for updates: a server update (004004) restarts the client through the launcher / itself and it rejoins the lobby / server; the web page reloads with its query (WEB_DIST=dist/web; kn_brain + server_bin + headless client; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I server/tests/auto_restart_e2e.py $(DEBUG_DIR) $(WEB_DIST)

.PHONY: connect-ux-e2e
connect-ux-e2e: build ## Server switch / reconnect shows the loading spinner, not a lost connection; the disconnected screen with Retry only after the retry budget (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I server/tests/connect_ux_e2e.py $(DEBUG_DIR)

.PHONY: console-reload-test
console-reload-test: build ## Dev console kept in the window (off-screen save, resize, F11, drag, console_reset) + the console's full reload in place and its restart fallback (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I server/tests/console_reload_e2e.py $(DEBUG_DIR)

.PHONY: console-give-autofill-test
console-give-autofill-test: build ## Dev console give completion: the selector / item / receiver hints of each line, a selector's item list = the no-selector list, Tab on one match (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I client/tests/console_give_autofill_e2e.py $(DEBUG_DIR)

.PHONY: console-autofill-audit-test
console-autofill-audit-test: build ## Dev console autofill audited per command: the hint rows of each argument against the parser that runs it (player targets, toggles, maps, class, bots, free text, lists), Enter replies, Tab (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 2400 $(PYTHON) -I client/tests/console_autofill_audit_e2e.py $(DEBUG_DIR)

.PHONY: console-autofill-nondev-test
console-autofill-nondev-test: build ## Dev console ping hints in a non-dev session on a LAN server (a guest connection from this machine's address; the dev control over loopback) (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 1200 $(PYTHON) -I client/tests/console_autofill_nondev_e2e.py $(DEBUG_DIR)

.PHONY: console-visible-test
console-visible-test: build ## Dev console never hidden while on: menu, windows, view stack, item view GL, game, death, end, F5, resizes, editor page; close + ` and restart (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 900 $(PYTHON) -I client/tests/console_visible_e2e.py $(DEBUG_DIR)

.PHONY: download-screen-test
download-screen-test: build ## The native client offers Download our client on a fresh profile, desktop and phone layout: the More Krunker tile opens the Download window, the hub's Android badge shows, and no window is open at startup (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 300 $(PYTHON) -I client/tests/download_screen_e2e.py $(DEBUG_DIR)

.PHONY: console-strike-test
console-strike-test: build ## Dev console on Krunker Strike's page: opens styled, runs a typed command and reads its output (also the default page) (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I client/tests/console_strike_e2e.py $(DEBUG_DIR)

.PHONY: hitmarker-test
hitmarker-test: build ## The hit marker never stays on the death screen: hit -> die, a hit while dead, respawn (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I server/tests/hitmarker_death_e2e.py $(DEBUG_DIR)

.PHONY: ui-transitions-test
ui-transitions-test: build ## UI state across ESC / death / end screen / spectate / windows / map change / disconnect / offline<->online, Object Manager scroll (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I server/tests/ui_transitions_e2e.py $(DEBUG_DIR)

.PHONY: voice-test
voice-test: build ## Voice chat end to end: a test tone from client A, decoded by B (PTT / toggle / dead / spectator / team / volumes; WEB_DIST=dist/web adds the browser over ws + WebRTC; headless, null audio)
	timeout 900 $(PYTHON) -I server/tests/voice_e2e.py $(DEBUG_DIR) $(if $(WEB_DIST),--web $(WEB_DIST),)

.PHONY: modes-test
modes-test: build ## Objective modes: each rotation mode's HUD, the CTF flag cycle offline + online (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I server/tests/modes_e2e.py $(DEBUG_DIR)

$(BRIDGE_DIR)/node_modules: $(BRIDGE_DIR)/package.json $(BRIDGE_DIR)/package-lock.json
	cd $(BRIDGE_DIR) && npm ci
	@touch $@

# --------------------------------------------------------------------------- backend
# Backend services (services/, doc/backend.md): knsvc libraries, kn_migrate, kn_brain.
# They are part of the normal debug build when OpenSSL (+ libpq for the DB parts) is
# installed; config/config.json (copy config/config.example.json) holds the credentials.
# GAMESERVERS=<n> game servers for backend-up; BACKEND_ARGS go to every service.

GAMESERVERS  ?= 2
BACKEND_ARGS ?=

.PHONY: services brain db-migrate db-status services-test backend-up backend-down backend-status backend-run
services: configure ## Build the backend libraries, tools and brain (services/)
	$(call BUILD,$(DEBUG_DIR)) --target services_all

brain: services ## Build kn_brain (the matchmaker)

db-migrate: services ## Apply services/db/migrations to the configured database (config/config.json)
	./$(DEBUG_DIR)/services/common/kn_migrate $(BACKEND_ARGS)

db-status: services ## List applied/pending database migrations
	./$(DEBUG_DIR)/services/common/kn_migrate --status $(BACKEND_ARGS)

services-test: services ## Run the backend unit tests (ctest -L knsvc; DB tests use their own database)
	cd $(DEBUG_DIR) && ctest -L knsvc --output-on-failure

backend-up: build-server services ## Start the local backend in the background: migrate, brain, GAMESERVERS game servers, net-bridge
	BUILD_DIR=$(DEBUG_DIR) GAMESERVERS=$(GAMESERVERS) BACKEND_ARGS="$(BACKEND_ARGS)" deploy/local/backend.sh up

backend-down: ## Stop what backend-up started
	deploy/local/backend.sh down

backend-status: ## Show the local backend processes, brain status and game list
	deploy/local/backend.sh status

backend-run: build services ## Foreground variant of backend-up (ctrl-c stops all)
	BUILD_DIR=$(DEBUG_DIR) GAMESERVERS=$(GAMESERVERS) services/scripts/backend-up.sh $(BACKEND_ARGS)

# --------------------------------------------------------------------------- checks

# Client updates (launcher/, doc/backend.md "Client updates"): the launcher against a local updates
# store made by tools/release/kn-updates.py, served on a free port; the last step runs the client
# headless (EGL_DEVICE=...; LAUNCHER_E2E_ARGS=--no-client skips it).
LAUNCHER_E2E_ARGS ?=
.PHONY: launcher-e2e
launcher-e2e: build ## Launcher + updates server end to end: executable update, self-update, content diff, deb, corrupt package, the client's Update now, Auto-restart for updates (mid-match defers to the match end, the menu restarts, a refused package restarts once)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I launcher/tests/launcher_e2e.py $(DEBUG_DIR) $(LAUNCHER_E2E_ARGS)

# A launcher-started client is online (launcher/tests/launcher_online_e2e.py): stub finder +
# matchmaker and a server_bin on free ports, the server browser, the finder error, and the
# refused game content dir inside a git checkout.
.PHONY: launcher-online-e2e
launcher-online-e2e: build ## Launcher-started client: finder + matchmaker requests, browser rows, finder errors shown
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) timeout 600 $(PYTHON) -I launcher/tests/launcher_online_e2e.py $(DEBUG_DIR)

.PHONY: smoke smoke-native smoke-ui smoke-web lint check
check: test ks-test editor-mcp-test lint bridge-check deploy-test krunker-build-test ## Everything CI runs that does not need a display

.PHONY: deploy-test
deploy-test: ## The asset CDN split (deploy/asset-shards.json) + the edge game-server render (Box Office + Krunker Strike instances, firewall, net-bridge allowlist; needs ansible-playbook, else skipped); the brain's user-asset proxy from a fake krunkbox .env (ansible, skipped without it)
	$(PYTHON) -I deploy/pages/asset_shards.py self-test
	$(PYTHON) -I deploy/pages/assets_cdn_test.py
	$(PYTHON) -I deploy/ansible/tests/render_test.py
	$(PYTHON) -I deploy/ansible/tests/user_assets_proxy_test.py; rc=$$?; [ $$rc -eq 0 ] || [ $$rc -eq 77 ]

lint: ## Lint workflow YAML, presets and the web glue scripts (ui/js)
	@command -v actionlint >/dev/null && actionlint || echo "actionlint not installed, skipping"
	$(PYTHON) -c "import json; json.load(open('CMakePresets.json'))"
	@for f in ui/js/*.js; do node --check $$f || exit 1; done
	@# GL calls must go through gl.h (glad / GLES); GLFW's system GL header links on Linux only
	@bad=$$(for f in $$(grep -lE '\bgl[A-Z][A-Za-z0-9]+\(' client/src/*.c client/src/*/*.c client/src/*/*/*.c 2>/dev/null); do \
		grep -qE '#include [<"](gl\.h|editor_internal\.h)[>"]' $$f || echo $$f; done); \
		if [ -n "$$bad" ]; then echo "GL calls without #include <gl.h>: $$bad"; exit 1; fi
	@# MinGW's C runtime has no timespec_get/TIME_UTC: use ks_wall_ms() or guard it with _WIN32
	@bad=$$(grep -lE '\btimespec_get\(' $$(git ls-files '*.c') | xargs -r grep -L '_WIN32'); \
		if [ -n "$$bad" ]; then echo "timespec_get without a _WIN32 path: $$bad"; exit 1; fi
	@# nor timegm in what the Windows build compiles: #define timegm _mkgmtime under _WIN32 (account_ui.c)
	@bad=$$(grep -lE '\btimegm\(' $$(git ls-files 'client/*.c' 'shared/*.c' 'server/*.c') | xargs -r grep -L '_WIN32'); \
		if [ -n "$$bad" ]; then echo "timegm without a _WIN32 path: $$bad"; exit 1; fi
	@# The bhop toast has two callers (client/include/bhop_toast.h): ui_bhop.c (bhop) and ui_mod_keys.c
	@# (Ctrl TP, the one exception). Only they show it, only hud_ui.c ticks it, and nothing else names
	@# its element or includes its header
	@bad=$$(grep -lE 'bhop_toast_show\(' $$(git ls-files 'client/*.c' 'shared/*.c' 'server/*.c') | grep -vE '^(client/src/ui/bhop_toast|client/src/ui/ui_bhop|client/src/ui/ui_mod_keys)\.c$$'); \
		bad2=$$(grep -lE 'bhop_toast_tick\(' $$(git ls-files 'client/*.c' 'shared/*.c' 'server/*.c') | grep -vE '^(client/src/ui/bhop_toast|client/src/ui/hud_ui)\.c$$'); \
		bad3=$$(grep -lE 'knBhopToast|bhop_toast\.h' $$(git ls-files 'client/*.c' 'client/*.h' 'shared/*.c' 'shared/*.h' 'server/*.c' 'server/*.h') | grep -vE '^(client/src/ui/bhop_toast\.c|client/include/bhop_toast\.h|client/src/ui/ui_bhop\.c|client/src/ui/ui_mod_keys\.c|client/src/ui/hud_ui\.c)$$'); \
		if [ -n "$$bad$$bad2$$bad3" ]; then echo "bhop toast used outside its callers (ui_bhop.c, ui_mod_keys.c) (bhop_toast_show: $$bad; bhop_toast_tick: $$bad2; knBhopToast/bhop_toast.h: $$bad3)"; exit 1; fi
	@echo "lint ok"

.PHONY: ks-test
ks-test: build ## KrunkScript: front end + runtime tests (ks_front_test, ks_vm_test) and `ksc check` over the corpus and the compiled-JS fixtures
	./$(DEBUG_DIR)/ks_front_test shared/tests/ks
	./$(DEBUG_DIR)/ks_vm_test shared/tests/ks
	./$(DEBUG_DIR)/ksc check --quiet shared/tests/ks/corpus/*.krnk shared/tests/ks/js/*.js

# kn_editor_mcp: the Krunker map editor MCP server (tools/editor-mcp/README.md). The test drives
# it over MCP stdio and covers every tool; renders use the headless client_bin (EGL, no window)
# and are skipped when client_bin is missing. EDITOR_MCP_TEST_ARGS="--no-render" skips them.
EDITOR_MCP_TEST_ARGS ?=
EDITOR_MCP_PREFIX ?= $(HOME)/.local
.PHONY: editor-mcp editor-mcp-test editor-mcp-install
editor-mcp: configure ## Build kn_editor_mcp (debug: cmake-build-debug/tools/editor-mcp/kn_editor_mcp)
	$(call BUILD,$(DEBUG_DIR)) --target kn_editor_mcp

.PHONY: editor-logic-test
editor-logic-test: build ## Editor logic UI / game config / custom GUI vs v7.1.4 (headless client_bin; tools/editor/logic_parity.py)
	$(PYTHON) -I tools/editor/logic_parity.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

editor-mcp-test: build ## kn_editor_mcp test suite over MCP stdio (every tool, round trips, undo, validation, renders)
	$(PYTHON) -I tools/editor-mcp/tests/mcp_test.py --bin $(DEBUG_DIR)/tools/editor-mcp/kn_editor_mcp $(EDITOR_MCP_TEST_ARGS)

.PHONY: editor-inspector-test
editor-inspector-test: build ## Object inspector + Environment/Terrain/Zone/Ocean panels vs v7.1.4: panel dumps and edits (headless; EGL_DEVICE=...)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/inspector_check.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

.PHONY: editor-windows-test
editor-windows-test: build ## Assets / Mods / Textures / Sounds / My Games windows vs v7.1.4 (headless, no brain; tools/editor/windows_check.py)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/windows_check.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

.PHONY: editor-publish-test
editor-publish-test: build ## Editor publishing / My Games / asset windows against a local kn_brain (headless; needs config/config.json DB creds or KN_TEST_DB_DRIVER=sqlite)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/publish_e2e.py $(DEBUG_DIR)

.PHONY: editor-publish-ui-e2e
editor-publish-ui-e2e: build ## Test play, Templates / Welcome / Publishing Guide windows, toolbar menus, asset UV channels vs v10.0.2 (headless, no brain; tools/editor/publish_ui_e2e.py)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/publish_ui_e2e.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

.PHONY: editor-tools-test
editor-tools-test: build ## Tools / Prefabs panels, Optimize Faces and Color Merging vs v7.1.4 (headless; tools/editor/tools_check.py)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/tools_check.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# the editor's KrunkScript window (doc/v7/editor.md WP5): headless client_bin --editor runs
# against v7.1.4's window (doc/ref/krunker-v7.1.4/editor/scripting*.dump.txt), ksc's diagnostics,
# editing, export and test play. Needs the game content (fonts); skipped without it.
.PHONY: editor-script-test
editor-script-test: build ## KrunkScript window e2e (v7 parity dumps, ksc diagnostics, editing, export, test play)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/script_window_test.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# the KrunkScript window against Krunker v10.0.2: the v10 autocomplete and the checker's v10 members (tools/editor/script_e2e.py)
.PHONY: editor-script-e2e
editor-script-e2e: build ## KrunkScript window v10.0.2 parity e2e (v10 autocomplete, v10 API members validate)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/script_e2e.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# the editor's objects (doc/v10/editor-social.md 1.3, 1.5): the object inspector's v10 rows against
# v7.1.4's goldens plus the documented v10 deltas (inspector_check.py), and the BOT_PATH_NODE buttons,
# asset map slots, group copy / paste and Quick Add search (objects_e2e.py), headless.
.PHONY: editor-objects-e2e
editor-objects-e2e: build ## Editor objects e2e: inspector v10 expectation, BOT_PATH_NODE, map slots, group paste, Quick Add (headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/objects_e2e.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/inspector_check.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# the Essential panel's v10 game configuration, the config / Environment round trips (tools/editor/mapsettings_e2e.py)
.PHONY: editor-mapsettings-e2e
editor-mapsettings-e2e: build ## Map settings e2e: Essential v10 rows, config and Environment round trips (headless; tools/editor/mapsettings_e2e.py)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/mapsettings_e2e.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# the File menu's round trips (Import / Export, Save Project, Ctrl+S, backups and Load Backup, drag and
# drop, GLTF / MTL, every map under assets/maps): headless client_bin --editor (tools/editor/io_e2e.py)
.PHONY: editor-io-e2e
editor-io-e2e: build ## Editor file round trips: export/import, save format, Ctrl+S, backups, drop, File menu, GLTF/MTL, all maps (headless)
	$(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) $(PYTHON) -I tools/editor/io_e2e.py --bin $(DEBUG_DIR)/$(CLIENT_BIN)

# its own release build dir: only the kn_editor_mcp target, independent of cmake-build-release
EDITOR_MCP_BUILD := cmake-build-mcp-release
editor-mcp-install: ## Install a release kn_editor_mcp to EDITOR_MCP_PREFIX/bin (default ~/.local/bin; schema embedded)
	@$(call CHECK_GENERATOR,$(EDITOR_MCP_BUILD)); $(CMAKE) --preset release -B $(EDITOR_MCP_BUILD) $(GEN_FLAG) -DKRUNKNATIVE_SERVICES=OFF $(CMAKE_FLAGS) > /dev/null
	$(call BUILD,$(EDITOR_MCP_BUILD)) --target kn_editor_mcp
	install -D -m 0755 $(EDITOR_MCP_BUILD)/tools/editor-mcp/kn_editor_mcp $(EDITOR_MCP_PREFIX)/bin/kn_editor_mcp
	@echo "installed $(EDITOR_MCP_PREFIX)/bin/kn_editor_mcp (register: claude mcp add --scope user krunker-editor -- $(EDITOR_MCP_PREFIX)/bin/kn_editor_mcp --root $(CURDIR))"

smoke: smoke-native smoke-web ## Headless render checks for both clients (screenshots in /tmp)

# The native smoke run is headless (client/include/headless.h): an EGL device context and
# no window on any display. EGL_DEVICE=nvidia|mesa|<index> picks the GPU (default: NVIDIA
# when present, else Mesa). KRUNKNATIVE_SCREENSHOT writes the frame once the menu map has
# rendered, exits the client and keeps the run's settings in a private XDG_CONFIG_HOME.
# Runs from the repo root: assets_path() is ./assets/.
EGL_DEVICE ?=
# UI_DIR=<dir> runs it on another page tree (KRUNKNATIVE_UI_DIR); it then also fails when that
# tree's window templates are missing (ui_windows.c falling back to ui/windows/)
UI_DIR ?=

smoke-native: build ## Run the native client offline headless (no window; EGL_DEVICE=nvidia|mesa, UI_DIR=<dir>) and screenshot the menu (/tmp/smoke-native.png)
	@rm -f /tmp/smoke-native.png
	@env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 $(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) \
	  $(if $(UI_DIR),KRUNKNATIVE_UI_DIR=$(UI_DIR),) \
	  KRUNKNATIVE_SCREENSHOT=/tmp/smoke-native.png KRUNKNATIVE_CAPTURE_FRAMES=60 \
	  timeout 120 stdbuf -oL $(DEBUG_DIR)/$(CLIENT_BIN) --offline > /tmp/smoke-native.log 2>&1 || { tail -20 /tmp/smoke-native.log; exit 1; }
	@grep -q "^\[ui\] setting " /tmp/smoke-native.log && [ -s /tmp/smoke-native.png ] \
	  && ! grep "windows.json missing\|missing window file" /tmp/smoke-native.log \
	  && echo "smoke-native ok (C UI logic set up the $(or $(UI_DIR),ui) page and applied its settings): /tmp/smoke-native.png" \
	  && grep -m1 "GL_RENDERER" /tmp/smoke-native.log || { tail -20 /tmp/smoke-native.log; exit 1; }

# The render-only uidiff scenarios (tools/uidiff/scenarios/*.json without a regions file: menu,
# Settings and Change Controls tabs, windows, store, social ...) rendered headless into
# /tmp/smoke-ui/. Fails on a non-zero exit, an empty frame or a missing window template.
smoke-ui: build ## Render the page's render-only scenarios headless (/tmp/smoke-ui/*.png)
	@rm -rf /tmp/smoke-ui && mkdir -p /tmp/smoke-ui/cfg
	@for f in tools/uidiff/scenarios/*.json; do n=$$(basename $$f .json); [ -f tools/uidiff/regions/$$n.json ] && continue; \
	  env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 $(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) \
	    XDG_CONFIG_HOME=/tmp/smoke-ui/cfg \
	    timeout 120 $(DEBUG_DIR)/$(CLIENT_BIN) --render-scenario $$f /tmp/smoke-ui/$$n.png > /tmp/smoke-ui/$$n.log 2>&1 \
	    && [ -s /tmp/smoke-ui/$$n.png ] && ! grep "windows.json missing\|missing window file" /tmp/smoke-ui/$$n.log \
	    || { echo "smoke-ui: $$n failed"; tail -20 /tmp/smoke-ui/$$n.log; exit 1; }; \
	  echo "smoke-ui: $$n ok"; done

smoke-web: ## Load the web build offline in headless Chromium and screenshot it (/tmp/smoke-web.png)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@chrome=$$(command -v chromium-browser || command -v chromium || command -v google-chrome) || { echo "needs chromium or google-chrome"; exit 1; }; \
	(cd $(DIST_WEB) && $(PYTHON) -m http.server $(PORT) >/dev/null 2>&1) & srv=$$!; sleep 1; \
	timeout 120 $$chrome --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
	  --window-size=1280,720 --virtual-time-budget=20000 --enable-logging=stderr --v=0 \
	  --screenshot=/tmp/smoke-web.png "http://localhost:$(PORT)/?offline=1&map=burg" 2>&1 | grep CONSOLE | cut -c1-200 > /tmp/smoke-web.log; \
	kill $$srv 2>/dev/null; \
	[ -f /tmp/smoke-web.png ] && echo "smoke-web ok: /tmp/smoke-web.png ($$(wc -l < /tmp/smoke-web.log) console lines in /tmp/smoke-web.log)"

.PHONY: web-gl-errors
web-gl-errors: ## Play the web build offline for a minute in headless Chromium; fail on any GL_INVALID_* in the console (MAP=, PLAY_SECONDS=)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	tools/kref/gl-errors-e2e.sh $(DIST_WEB) "offline=1&map=$(or $(MAP),kanji)" $(or $(PLAY_SECONDS),60)

web-session-e2e: ## Web e2e: a stack (kn_brain, server_bin, net-bridge --web, headless Chromium) and the session tests in tools/kref/web-session/ (needs make web, make build)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@[ -d $(BRIDGE_DIR)/node_modules ] || $(MAKE) bridge-install
	KN_WEB_BUILD=$(or $(KN_WEB_BUILD),$(DEBUG_DIR)) KN_WEB_DIST=$(DIST_WEB) tools/kref/web-session/web-session-e2e.sh $(TESTS)

# --------------------------------------------------------------------------- housekeeping

.PHONY: clean distclean help
clean: ## Remove build directories and dist/
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR) $(WEB_DIR) $(WEB_DEBUG_DIR) dist $(ANDROID_DIR)/app/build $(ANDROID_DIR)/.gradle platform/ios/web

distclean: clean ## clean + net-bridge/node_modules
	rm -rf $(BRIDGE_DIR)/node_modules

help: ## Show this help
	@awk 'BEGIN {FS = ":.*## "; printf "\nUsage: make <target> [VAR=value]\n\n"} \
	  /^# -+ [a-z-]+$$/ {gsub(/^# -+ /, ""); printf "\n\033[1m%s\033[0m\n", $$0} \
	  /^[a-zA-Z0-9_-]+:.*## / {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nVariables: GENERATOR=%s JOBS=%s HTML_ENGINE=%s EMSDK=%s PORT=%s\n\n" "$(GENERATOR)" "$(JOBS)" "$(HTML_ENGINE)" "$(EMSDK)" "$(PORT)"
