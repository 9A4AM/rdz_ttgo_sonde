# Makefile — convenience wrappers around PlatformIO for the rdz_ttgo_sonde firmware.
#
# PlatformIO is not on PATH on this machine: the system Python (3.9) is too old for
# the pinned Tasmota espressif32 platform, so pio runs from a Python 3.13 venv.
# Override PIO=... on the command line to use a different PlatformIO.
#
#   make            # compile firmware (default)
#   make upload     # flash firmware over USB
#   make image      # build + merge into a single firmware-image.bin (full 4MB image)
#   make help       # list all targets

# PlatformIO executable (Python 3.13 venv created during setup).
PIO ?= /home/dev-python/.pio-venv/bin/pio

# PlatformIO's managed penv — holds esptool + deps and a `python` on PATH.
# The repo's merge step (scripts/pio-build-extension.py) shells out to `esptool.py`,
# whose `#!/usr/bin/env python` shebang needs this dir on PATH to resolve.
PENV_BIN ?= /home/dev-python/.platformio/penv/bin
MERGE_PATH := PATH=$(PENV_BIN):$$PATH

# Build artifact locations (PlatformIO env is ttgo-lora32, see platformio.ini).
BUILD_DIR := .pio/build/ttgo-lora32
# Assemble OTA artifacts OUTSIDE .pio/build: PlatformIO recreates its build tree,
# which swaps the directory's inode. A Docker bind mount pins the inode at container
# start, so a swapped dir orphans the ota-serve mount (404 until restart). Keeping
# this dir stable and writing files in place keeps the running container in sync.
OTA_DIR   := ota-dist

.DEFAULT_GOAL := build
.PHONY: build upload uploadfs buildfs uploadfonts monitor mock-sondehub image ota ota-version ota-serve ota-stop test clean help

# OTA HTTP server (nginx in Docker, see ota-server/). Serves $(OTA_DIR) on port 80.
OTA_IMAGE     ?= rdz-ota
OTA_CONTAINER ?= rdz-ota
OTA_PORT      ?= 80

build: ## Compile firmware
	$(PIO) run

upload: ## Compile + flash firmware over USB
	$(PIO) run --target upload

uploadfs: ## Build + flash the LittleFS data partition (RX_FSK/data/)
	$(PIO) run --target uploadfs

buildfs: ## Build the LittleFS data image (littlefs.bin) without flashing
	$(PIO) run --target buildfs

uploadfonts: ## Flash the fonts partition (only when a separate fonts partition is enabled)
	$(PIO) run --target uploadfonts

monitor: ## Open the serial monitor (115200 baud)
	$(PIO) run --target monitor

# Local mock SondeHub API + live dashboard (scripts/mock_sondehub.py) for testing
# the offline upload cache. Override ports with SH_PORT / SH_WEB_PORT.
SH_PORT     ?= 8080
SH_WEB_PORT ?= 8081

mock-sondehub: ## Start the mock SondeHub API + live dashboard (test the offline upload cache)
	python3 scripts/mock_sondehub.py --port $(SH_PORT) --web-port $(SH_WEB_PORT)

image: buildfs ## Build the merged full-flash firmware-image.bin into $(OTA_DIR) (for USB re-flash downloads)
	$(MERGE_PATH) $(PIO) run --target firmware
	mkdir -p $(OTA_DIR)
	mv $(BUILD_DIR)/firmware-image.bin $(OTA_DIR)/firmware-image.bin
	@echo "Full flash image: $(OTA_DIR)/firmware-image.bin"

ota-version: ## Stamp version.h: bump FS_MINOR if RX_FSK/data changed + fresh py5ol<timestamp> id
	python3 scripts/ota_version.py fsbump RX_FSK/data
	python3 scripts/ota_version.py bump

# ota-version runs before build so the new version_id is compiled into the binary.
ota: ota-version build ## Build OTA artifacts (update.ino.bin + update.fs.bin + update-info.html)
	mkdir -p $(OTA_DIR)
	mv $(BUILD_DIR)/firmware.bin $(OTA_DIR)/update.ino.bin
	python3 scripts/makefsupdate.py RX_FSK/data > $(OTA_DIR)/update.fs.bin
	python3 scripts/ota_version.py info > $(OTA_DIR)/update-info.html
	@echo "OTA artifacts in $(OTA_DIR):"
	@echo "  update.ino.bin (app)  +  update.fs.bin (data)  +  update-info.html ($$(cat $(OTA_DIR)/update-info.html))"
	@echo "Serve this dir over HTTP (make ota-serve) or deploy it to your OTA server."

# Build the two binaries you flash/upload to test, without cutting a release.
# Three deliberate differences from `ota`:
#   1. No ota-version dependency, so version.h is left exactly as it is. A test build must
#      never consume a version number or rewrite the stored ota_fsdata_sha256 — doing so
#      would make the NEXT real build see unchanged data and skip its FS_MINOR bump.
#      The version increments when you cut the release with `make ota`, not here.
#   2. Only the two .bin files. No update-info.html: since the version is deliberately not
#      stamped, that file would advertise the PREVIOUS release's version alongside these
#      binaries, and a device comparing against it would draw the wrong conclusion.
#   3. Files stay in $(BUILD_DIR) and firmware.bin is COPIED, not moved, so $(OTA_DIR) is
#      untouched (a test run can neither be mistaken for a release nor clobber one) and the
#      binary is still where `pio` left it for `make upload`.
# Note ota-serve intentionally does not serve these: $(BUILD_DIR) is recreated by
# PlatformIO, which swaps the directory inode and would orphan a Docker bind mount.
test: build ## Build just the two OTA binaries into $(BUILD_DIR) for testing (no version bump)
	cp $(BUILD_DIR)/firmware.bin $(BUILD_DIR)/update.ino.bin
	python3 scripts/makefsupdate.py RX_FSK/data > $(BUILD_DIR)/update.fs.bin
	@echo "Test binaries in $(BUILD_DIR):"
	@echo "  update.ino.bin (app)  +  update.fs.bin (data)"

ota-serve: ## Build & start the nginx OTA server (serves $(OTA_DIR) on OTA_PORT, default 80)
	@[ -f $(OTA_DIR)/update.ino.bin ] || { echo "No OTA artifacts in $(OTA_DIR) — run 'make ota' first."; exit 1; }
	docker build -t $(OTA_IMAGE) ota-server
	docker rm -f $(OTA_CONTAINER) 2>/dev/null || true
	docker run -d --name $(OTA_CONTAINER) -p $(OTA_PORT):80 \
		-v "$(CURDIR)/$(OTA_DIR):/usr/share/nginx/html:ro" $(OTA_IMAGE)
	@echo "OTA server up on http://localhost:$(OTA_PORT)/  (serving $(OTA_DIR))"

ota-stop: ## Stop and remove the nginx OTA server container
	docker rm -f $(OTA_CONTAINER) 2>/dev/null || true
	@echo "OTA server stopped."

clean: ## Remove build artifacts
	$(PIO) run --target clean

help: ## List available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
