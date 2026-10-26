PIO_DIR := ESP-32-CoastCue/pio
IOS_PROJECT := ios/CoastCue.xcodeproj
IOS_SCHEME := CoastCue
TOOLING_DIR := .tooling
IOS_DERIVED_DATA := $(TOOLING_DIR)/xcode-derived-data
IOS_TEST_DESTINATION ?=
IOS_FLAGS := -project $(IOS_PROJECT) -scheme $(IOS_SCHEME) -derivedDataPath $(IOS_DERIVED_DATA) CODE_SIGNING_ALLOWED=NO \
	SWIFT_STRICT_CONCURRENCY=complete SWIFT_TREAT_WARNINGS_AS_ERRORS=YES GCC_TREAT_WARNINGS_AS_ERRORS=YES
PYTHON ?= python3
VENV_PYTHON := $(TOOLING_DIR)/venv/bin/python
TOOLING_STAMP := $(TOOLING_DIR)/.requirements-installed
PIO_VARS := PLATFORMIO_CORE_DIR=$(CURDIR)/$(TOOLING_DIR)/platformio PLATFORMIO_SETTING_ENABLE_TELEMETRY=false
PIO := $(PIO_VARS) $(VENV_PYTHON) -m platformio
PIO_ENV ?= boat
SERIAL_SECONDS ?= 20
PORT ?=
ALLOW_UNKNOWN_PORT ?= 0
CONFIRM_STORAGE_FORMAT ?=
CONFIRM_US_915_UPLOAD ?=

.DEFAULT_GOAL := help
.PHONY: FORCE help setup tooling local-config ci-test-config preflight build firmware-check \
	host-test python-test check-host check ios-check ios-test-compile ios-test \
	ios-test-if-available devices port-check require-role flash init-shore-storage serial clean-tooling

FORCE:

help:
	@printf "CoastCue\n"
	@printf "  make setup                      Create local config and install pinned tooling\n"
	@printf "  make build-boat | build-shore   Compile one firmware environment\n"
	@printf "  make check                      Product checks (firmware, host, iOS if a Simulator exists)\n"
	@printf "  make preflight                  Validate the private boat ID and LoRa key\n"
	@printf "  make devices                    List serial ports\n"
	@printf "  make flash-boat PORT=...        Build, upload, and capture serial (also flash-shore)\n"
	@printf "  make init-shore-storage PORT=... CONFIRM_STORAGE_FORMAT=ERASE\n"
	@printf "  US uploads also need CONFIRM_US_915_UPLOAD=US_915\n"

setup: local-config tooling

tooling: FORCE $(VENV_PYTHON)
	@$(VENV_PYTHON) -c 'import platformio, serial' >/dev/null 2>&1 || rm -f $(TOOLING_STAMP)
	@$(MAKE) --no-print-directory $(TOOLING_STAMP)
	@mkdir -p $(TOOLING_DIR)/platformio

$(VENV_PYTHON):
	@mkdir -p $(TOOLING_DIR)
	$(PYTHON) -m venv $(TOOLING_DIR)/venv

$(TOOLING_STAMP): requirements-tools.txt $(VENV_PYTHON)
	$(VENV_PYTHON) -m pip install --disable-pip-version-check --requirement requirements-tools.txt
	@touch $(TOOLING_STAMP)

local-config:
	@if test -f $(PIO_DIR)/include/coastcue_config.h; then \
		$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h --check-schema; \
	else \
		cp $(PIO_DIR)/include/coastcue_config.example.h $(PIO_DIR)/include/coastcue_config.h; \
		chmod 600 $(PIO_DIR)/include/coastcue_config.h; \
		printf "Created private config. Replace the boat ID and LoRa key before upload.\n"; \
	fi

# CI only: a non-deployable fixture in a clean checkout.
ci-test-config:
	@test ! -e $(PIO_DIR)/include/coastcue_config.h || (printf "Refusing to overwrite an existing private config.\n" >&2 && exit 1)
	@$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h --template $(PIO_DIR)/include/coastcue_config.example.h --write-ci-fixture

preflight:
	$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h

build: tooling
	$(PIO) run -d $(PIO_DIR) -e $(PIO_ENV)

build-%: FORCE
	@$(MAKE) --no-print-directory build PIO_ENV=$*

firmware-check: tooling
	$(PIO) run -d $(PIO_DIR) -e boat -e shore

host-test:
	@set -eu; \
		dir="$$(mktemp -d "$${TMPDIR:-/tmp}/coastcue-host-tests.XXXXXX")"; \
		trap 'rm -rf "$$dir"' EXIT; \
		run() { name="$$1"; shift; c++ -std=c++17 -I$(PIO_DIR)/include "$$@" -o "$$dir/$$name"; "$$dir/$$name"; }; \
		run shore-logic $(PIO_DIR)/test/host/shore_logic_test.cpp $(PIO_DIR)/src/shore_logic.cpp; \
		run boat-logic $(PIO_DIR)/test/host/boat_logic_test.cpp; \
		run incident-protocol $(PIO_DIR)/test/host/incident_protocol_test.cpp

python-test:
	$(PYTHON) -m unittest discover -s scripts/tests

check-host: host-test python-test firmware-check

check: check-host ios-test-if-available

ios-check:
	xcodebuild build -destination 'generic/platform=iOS Simulator' $(IOS_FLAGS)

ios-test-compile:
	xcodebuild build-for-testing -destination 'generic/platform=iOS Simulator' $(IOS_FLAGS)

ios-test:
	@destination='$(IOS_TEST_DESTINATION)'; \
	test -n "$$destination" || destination="$$($(PYTHON) scripts/ios_simulator_destination.py --project $(IOS_PROJECT) --scheme $(IOS_SCHEME))"; \
	test -n "$$destination" || { printf "No iOS Simulator runtime; use make ios-check.\n" >&2; exit 1; }; \
	xcodebuild test -destination "$$destination" $(IOS_FLAGS)

ios-test-if-available:
	@destination="$$($(PYTHON) scripts/ios_simulator_destination.py --project $(IOS_PROJECT) --scheme $(IOS_SCHEME) 2>/dev/null || true)"; \
	if test -n "$$destination"; then $(MAKE) --no-print-directory ios-test IOS_TEST_DESTINATION="$$destination"; \
	else $(MAKE) --no-print-directory ios-test-compile && printf "No iOS Simulator; tests compiled, not run.\n"; fi

devices: tooling
	$(PIO) device list

port-check: tooling
	@test -n "$(PORT)" || (printf "PORT is required. Run make devices first.\n" >&2 && exit 1)
	$(PIO_VARS) COASTCUE_PLATFORMIO="$(VENV_PYTHON) -m platformio" $(VENV_PYTHON) scripts/port_guard.py --port $(PORT) $(if $(filter 1 true yes,$(ALLOW_UNKNOWN_PORT)),--allow-unknown,)

require-role:
	@case "$(PIO_ENV)" in boat|shore) ;; \
		*) printf "Unknown firmware environment '%s'.\n" "$(PIO_ENV)" >&2; exit 1 ;; esac
	@test "$(CONFIRM_US_915_UPLOAD)" = "US_915" || \
		(printf "Add CONFIRM_US_915_UPLOAD=US_915 only when the connected radio and antenna are 915 MHz.\n" >&2; exit 1)

flash: require-role
	$(MAKE) --no-print-directory build PIO_ENV=$(PIO_ENV)
	$(MAKE) --no-print-directory port-check PORT=$(PORT)
	$(MAKE) --no-print-directory preflight
	$(PIO) run -d $(PIO_DIR) -e $(PIO_ENV) -t upload --upload-port $(PORT)
	$(MAKE) --no-print-directory serial PORT=$(PORT) ROLE=$(PIO_ENV)

flash-%: FORCE
	@$(MAKE) --no-print-directory flash PIO_ENV=$*

# Erases all shore history. Only for a new or intentionally reset shore.
init-shore-storage:
	@test "$(CONFIRM_STORAGE_FORMAT)" = "ERASE" || (printf "This erases all shore history. Re-run with CONFIRM_STORAGE_FORMAT=ERASE.\n" >&2 && exit 1)
	$(MAKE) --no-print-directory require-role PIO_ENV=shore
	$(MAKE) --no-print-directory port-check PORT=$(PORT)
	$(PIO) run -d $(PIO_DIR) -e shore -t uploadfs --upload-port $(PORT)
	$(MAKE) --no-print-directory serial PORT=$(PORT) ROLE=shore

serial: tooling
	@test -n "$(PORT)" || (printf "PORT is required. Run make devices first.\n" >&2 && exit 1)
	$(VENV_PYTHON) scripts/serial_capture.py --port $(PORT) --seconds $(SERIAL_SECONDS) $(if $(ROLE),--smoke-role $(ROLE),)

clean-tooling:
	rm -rf $(TOOLING_DIR)/venv $(TOOLING_DIR)/platformio/.cache $(TOOLING_STAMP)
