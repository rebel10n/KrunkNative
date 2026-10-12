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
CONFIGURE = $(call CHECK_GENERATOR,$(2)); $(CMAKE) --preset $(1) $(GEN_FLAG) -DKRUNKNATIVE_HTML_ENGINE=$(HTML_ENGINE) $(CMAKE_FLAGS)
BUILD     = $(CMAKE) --build $(1) --parallel $(JOBS)

# Emscripten: use emcc from PATH, else activate the emsdk checkout in $(EMSDK).
EMSDK_ACTIVATE = if ! command -v emcc >/dev/null 2>&1; then \
	  [ -f "$(EMSDK)/emsdk_env.sh" ] || { echo "emsdk not found: set EMSDK=/path/to/emsdk (https://emscripten.org/docs/getting_started/)"; exit 1; }; \
	  source "$(EMSDK)/emsdk_env.sh" >/dev/null 2>&1; \
	fi; export EMSDK="$${EMSDK:-$(EMSDK)}"

# Map argument for run targets: MAP=kanji -> assets/maps/kanji.json
MAP_ARG = $(if $(MAP),assets/maps/$(MAP).json,)

# --------------------------------------------------------------------------- native

.PHONY: build configure release configure-release all
build: configure ## Build debug client, server and tests (default)
	$(call BUILD,$(DEBUG_DIR))

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

.PHONY: run run-release server server-release server-dist server-install network
# make run starts the local backend (accounts, matchmaking) when config/config.json exists and
# no brain answers on BRAIN_PORT; BACKEND=0 skips it (the client then plays on the default
# server and shows "Failed to connect" for account actions).
BACKEND    ?= 1
BRAIN_PORT ?= 5050
# the client never reads config/config.json (DB credentials): a non-default brain port reaches
# it as KRUNKNATIVE_BRAIN_URL (5050 is the client's own probed default); an explicit
# KRUNKNATIVE_BRAIN_URL in the environment wins
RUN_BRAIN_ENV = $(if $(KRUNKNATIVE_BRAIN_URL),,$(if $(filter-out 5050,$(BRAIN_PORT)),KRUNKNATIVE_BRAIN_URL=http://127.0.0.1:$(BRAIN_PORT)))
run: build ## Run the debug client (ARGS="--offline" MAP=<name>; starts the local backend unless BACKEND=0)
	@if [ "$(BACKEND)" != 0 ] && [ -f config/config.json ] && ! curl -s -m 1 http://127.0.0.1:$(BRAIN_PORT)/mm/status 2>/dev/null | grep -q '"brain"'; then \
	  echo "[run] no brain on 127.0.0.1:$(BRAIN_PORT): starting the local backend (make backend-up; BACKEND=0 skips)"; \
	  $(MAKE) --no-print-directory backend-up BRAIN_PORT=$(BRAIN_PORT) || echo "[run] backend-up failed: accounts are unavailable (see make backend-status)"; \
	fi
	cd $(DEBUG_DIR) && $(RUN_BRAIN_ENV) ./$(CLIENT_BIN) $(ARGS) $(if $(MAP),../$(MAP_ARG),)

run-release: release ## Run the release client
	cd $(RELEASE_DIR) && ./$(CLIENT_BIN) $(ARGS) $(if $(MAP),../$(MAP_ARG),)

server: build ## Run the debug server (ARGS=...)
	cd $(DEBUG_DIR) && ./$(SERVER_BIN) $(ARGS)

server-release: release ## Run the release server
	cd $(RELEASE_DIR) && ./$(SERVER_BIN) $(ARGS)

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
test: build ## Run the shared movement/collision test (+ the knet transport, lag compensation and editor bus tests)
	./$(DEBUG_DIR)/$(TEST_BIN)
	./$(DEBUG_DIR)/knet_test
	./$(DEBUG_DIR)/lagcomp_test
	./$(DEBUG_DIR)/editor_bus_test

# --------------------------------------------------------------------------- html-engine
# The in-house HTML/CSS engine (client/src/html/engine/README.md, doc/html-ui-engine.md).
# These targets use the debug directory with HTML_ENGINE=ON (the default) so they also
# work after a `make HTML_ENGINE=OFF`.

.PHONY: ui-test ui-import assets-import uidiff
ui-test: ## Build ui_test (HTML_ENGINE=ON) and run it: ARGS="lexbor-probe" (default: help)
	@$(MAKE) --no-print-directory configure HTML_ENGINE=ON
	$(call BUILD,$(DEBUG_DIR)) --target ui_test
	./$(DEBUG_DIR)/ui_test $(or $(ARGS),help)

ui-import: ## Regenerate ui/ from the Krunker reference (fonts, windows, icon ligatures) via tools/ui-import
	$(PYTHON) -I tools/ui-import/import.py --ui ui

assets-import: ## Copy the v3.1.1 content mod.zip lacks (textures/previews/) into assets/ (tools/ui-import/game_assets.py)
	$(PYTHON) -I tools/ui-import/game_assets.py --assets assets

.PHONY: assets-import-v7 assets-check-v7
assets-import-v7: ## Import Krunker v7.1.4 content into assets/ (models, textures, sounds, skins.json...) and regenerate client/src/cosmetics_table.c
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --assets assets
	$(PYTHON) -I tools/ui-import/gen_cosmetics.py

assets-check-v7: ## Drift check of assets/ and cosmetics_table.c against Krunker v7.1.4 (exit 1 on any difference)
	$(PYTHON) -I tools/ui-import/game_assets.py --v7 --check --assets assets
	$(PYTHON) -I tools/ui-import/gen_cosmetics.py --check


# uidiff renders every scenario through the real client and its C UI logic
# (`client_bin --render-scenario`, client/src/ui/ui_scenario.c) on llvmpipe inside one
# Xvfb server (1920x1080, large enough for every scenario size), then pixel-diffs the
# regions (tools/uidiff/README.md). SCENARIOS="menu-1280 win-classes" limits the run;
# UIDIFF_NO_XVFB=1 uses the current display; UIDIFF_OUT/UIDIFF_FUZZ_SCALE pass through.
# UIDIFF_HEADLESS=1 renders with no window and no X server (EGL, llvmpipe);
# UIDIFF_EGL_DEVICE=nvidia renders headless on the NVIDIA GPU (client/include/headless.h).
SCENARIOS ?=
UIDIFF_XVFB_SCREEN ?= 1920x1080x24
UIDIFF_HEADLESS ?= 0
UIDIFF_EGL_DEVICE ?=

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

.PHONY: web web-debug web-serve pages
web: ## Build the Emscripten client into dist/web/ (release; KN_GA_ID=G-... adds Google Analytics)
	@$(EMSDK_ACTIVATE); $(call CONFIGURE,web,$(WEB_DIR)) && $(call BUILD,$(WEB_DIR))
	@$(PYTHON) -I deploy/pages/ga.py $(DIST_WEB) --id "$(KN_GA_ID)"
	@echo "web client ready in $(DIST_WEB)/  (make web-serve, or: make bridge)"

web-debug: ## Build the Emscripten client with debug info into dist/web/
	@$(EMSDK_ACTIVATE); $(call CONFIGURE,web-debug,$(WEB_DEBUG_DIR)) && $(call BUILD,$(WEB_DEBUG_DIR))
	@$(PYTHON) -I deploy/pages/ga.py $(DIST_WEB) --id "$(KN_GA_ID)"

pages: web ## Build and deploy the web client to Cloudflare Pages (krunk.io, deploy/pages/README.md)
	deploy/pages/deploy.sh

web-serve: ## Serve dist/web/ for local testing (PORT=8080); use ?offline=1 without a server
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@echo "http://localhost:$(PORT)/?offline=1   (ctrl-c to stop; for the game server bridge use 'make bridge')"
	cd $(DIST_WEB) && $(PYTHON) -m http.server $(PORT)

# --------------------------------------------------------------------------- mobile
# Krunker's mobile apps wrapped the web game; so do these (platform/README.md).
# The web client itself detects phones and shows the touch UI (client/src/ui/ui_mobile.c).
# ANDROID_ARGS passes Gradle properties, e.g. ANDROID_ARGS="-Pkrunknative.wsUrl=ws://192.168.1.10:8080/native".

ANDROID_DIR  := platform/android
ANDROID_APK  := $(ANDROID_DIR)/app/build/outputs/apk/debug/app-debug.apk
ANDROID_ARGS ?=

.PHONY: android android-only android-install ios-prepare
android: web android-only ## Build the web client, then the Android debug APK (needs JDK 17+ and the Android SDK)

android-only: ## Android debug APK from the existing dist/web (no web rebuild)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@[ -n "$${ANDROID_HOME:-$${ANDROID_SDK_ROOT:-}}" ] || [ -f $(ANDROID_DIR)/local.properties ] || \
	  { echo "Android SDK not found: set ANDROID_HOME (or write sdk.dir=... to $(ANDROID_DIR)/local.properties)"; exit 1; }
	cd $(ANDROID_DIR) && ./gradlew --no-daemon assembleDebug $(ANDROID_ARGS)
	@ls -l $(ANDROID_APK)

android-install: ## Install the debug APK on a connected device (adb)
	adb install -r $(ANDROID_APK)

ios-prepare: ## Copy dist/web into platform/ios/web (then: cd platform/ios && xcodegen generate, on macOS)
	platform/ios/prepare.sh $(DIST_WEB)

# --------------------------------------------------------------------------- net-bridge

.PHONY: bridge bridge-install bridge-check
bridge-install: ## Install net-bridge dependencies
	cd $(BRIDGE_DIR) && npm install

bridge: $(BRIDGE_DIR)/node_modules ## Host dist/web/ and bridge /native WebSocket <-> native TCP server (port 8080)
	cd $(BRIDGE_DIR) && npm run web

bridge-check: $(BRIDGE_DIR)/node_modules ## Typecheck net-bridge
	cd $(BRIDGE_DIR) && npm run typecheck

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

test-net: build ## knet transport unit test (loss/dup/reorder) + 2-client UDP end-to-end (headless)
	./$(DEBUG_DIR)/knet_test
	server/tests/udp_e2e.sh $(DEBUG_DIR)
	KN_UDP_LOSS="15 5 5" server/tests/udp_e2e.sh $(DEBUG_DIR)

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

backend-up: build services ## Start the local backend in the background: migrate, brain, GAMESERVERS game servers, net-bridge
	BUILD_DIR=$(DEBUG_DIR) GAMESERVERS=$(GAMESERVERS) BACKEND_ARGS="$(BACKEND_ARGS)" deploy/local/backend.sh up

backend-down: ## Stop what backend-up started
	deploy/local/backend.sh down

backend-status: ## Show the local backend processes, brain status and game list
	deploy/local/backend.sh status

backend-run: build services ## Foreground variant of backend-up (ctrl-c stops all)
	BUILD_DIR=$(DEBUG_DIR) GAMESERVERS=$(GAMESERVERS) services/scripts/backend-up.sh $(BACKEND_ARGS)

# --------------------------------------------------------------------------- checks

.PHONY: smoke smoke-native smoke-web lint check
check: test ks-test editor-mcp-test lint bridge-check ## Everything CI runs that does not need a display

lint: ## Lint workflow YAML, presets and the web glue scripts (ui/js)
	@command -v actionlint >/dev/null && actionlint || echo "actionlint not installed, skipping"
	$(PYTHON) -c "import json; json.load(open('CMakePresets.json'))"
	@for f in ui/js/*.js; do node --check $$f || exit 1; done
	@echo "lint ok"

.PHONY: ks-test
ks-test: build ## KrunkScript: front end + runtime tests (ks_front_test, ks_vm_test) and `ksc check` over the corpus
	./$(DEBUG_DIR)/ks_front_test shared/tests/ks
	./$(DEBUG_DIR)/ks_vm_test shared/tests/ks
	./$(DEBUG_DIR)/ksc check --quiet shared/tests/ks/corpus/*.krnk

# kn_editor_mcp: the Krunker map editor MCP server (tools/editor-mcp/README.md). The test drives
# it over MCP stdio and covers every tool; renders use the headless client_bin (EGL, no window)
# and are skipped when client_bin is missing. EDITOR_MCP_TEST_ARGS="--no-render" skips them.
EDITOR_MCP_TEST_ARGS ?=
EDITOR_MCP_PREFIX ?= $(HOME)/.local
.PHONY: editor-mcp editor-mcp-test editor-mcp-install
editor-mcp: configure ## Build kn_editor_mcp (debug: cmake-build-debug/tools/editor-mcp/kn_editor_mcp)
	$(call BUILD,$(DEBUG_DIR)) --target kn_editor_mcp

editor-mcp-test: build ## kn_editor_mcp test suite over MCP stdio (every tool, round trips, undo, validation, renders)
	$(PYTHON) -I tools/editor-mcp/tests/mcp_test.py --bin $(DEBUG_DIR)/tools/editor-mcp/kn_editor_mcp $(EDITOR_MCP_TEST_ARGS)

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

smoke-native: build ## Run the native client offline headless (no window; EGL_DEVICE=nvidia|mesa) and screenshot the menu (/tmp/smoke-native.png)
	@rm -f /tmp/smoke-native.png
	@env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 $(if $(EGL_DEVICE),KRUNKNATIVE_EGL_DEVICE=$(EGL_DEVICE),) \
	  KRUNKNATIVE_SCREENSHOT=/tmp/smoke-native.png KRUNKNATIVE_CAPTURE_FRAMES=60 \
	  timeout 120 stdbuf -oL $(DEBUG_DIR)/$(CLIENT_BIN) --offline > /tmp/smoke-native.log 2>&1 || { tail -20 /tmp/smoke-native.log; exit 1; }
	@grep -q "^\[ui\] setting " /tmp/smoke-native.log && [ -s /tmp/smoke-native.png ] \
	  && echo "smoke-native ok (C UI logic set up the page and applied its settings): /tmp/smoke-native.png" \
	  && grep -m1 "GL_RENDERER" /tmp/smoke-native.log || { tail -20 /tmp/smoke-native.log; exit 1; }

smoke-web: ## Load the web build offline in headless Chromium and screenshot it (/tmp/smoke-web.png)
	@[ -f $(DIST_WEB)/client.wasm ] || { echo "no web build yet: run 'make web' first"; exit 1; }
	@chrome=$$(command -v chromium-browser || command -v chromium || command -v google-chrome) || { echo "needs chromium or google-chrome"; exit 1; }; \
	(cd $(DIST_WEB) && $(PYTHON) -m http.server $(PORT) >/dev/null 2>&1) & srv=$$!; sleep 1; \
	timeout 120 $$chrome --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
	  --window-size=1280,720 --virtual-time-budget=20000 --enable-logging=stderr --v=0 \
	  --screenshot=/tmp/smoke-web.png "http://localhost:$(PORT)/?offline=1" 2>&1 | grep CONSOLE | cut -c1-200 > /tmp/smoke-web.log; \
	kill $$srv 2>/dev/null; \
	[ -f /tmp/smoke-web.png ] && echo "smoke-web ok: /tmp/smoke-web.png ($$(wc -l < /tmp/smoke-web.log) console lines in /tmp/smoke-web.log)"

# --------------------------------------------------------------------------- housekeeping

.PHONY: clean distclean help
clean: ## Remove build directories and dist/
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR) $(WEB_DIR) $(WEB_DEBUG_DIR) dist $(ANDROID_DIR)/app/build $(ANDROID_DIR)/.gradle platform/ios/web

distclean: clean ## clean + net-bridge/node_modules
	rm -rf $(BRIDGE_DIR)/node_modules

help: ## Show this help
	@awk 'BEGIN {FS = ":.*## "; printf "\nUsage: make <target> [VAR=value]\n\n"} \
	  /^# -+ [a-z-]+$$/ {gsub(/^# -+ /, ""); printf "\n\033[1m%s\033[0m\n", $$0} \
	  /^[a-zA-Z_-]+:.*## / {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nVariables: GENERATOR=%s JOBS=%s HTML_ENGINE=%s EMSDK=%s PORT=%s\n\n" "$(GENERATOR)" "$(JOBS)" "$(HTML_ENGINE)" "$(EMSDK)" "$(PORT)"
