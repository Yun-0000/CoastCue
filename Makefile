PIO_DIR := ESP-32-CoastCue/pio
IOS_DIR := ios
IOS_PROJECT := $(IOS_DIR)/CoastCue.xcodeproj
IOS_SCHEME := CoastCue
IOS_DERIVED_DATA := .tooling/xcode-derived-data
IOS_TEST_DESTINATION ?=
IOS_STRICT_FLAGS := SWIFT_STRICT_CONCURRENCY=complete SWIFT_TREAT_WARNINGS_AS_ERRORS=YES GCC_TREAT_WARNINGS_AS_ERRORS=YES
PYTHON ?= python3
TOOLING_DIR := .tooling
TOOLING_VENV := $(TOOLING_DIR)/venv
TOOLING_PYTHON := $(TOOLING_VENV)/bin/python
TOOLING_STAMP := $(TOOLING_DIR)/.requirements-installed
TOOLING_REQUIREMENTS := requirements-tools.txt
PIO_ENV ?= boat
MONITOR_BAUD ?= 115200
SERIAL_SECONDS ?= 20
SMOKE_ROLE ?=
PORT ?=
ALLOW_UNKNOWN_PORT ?= 0
CONFIRM_STORAGE_FORMAT ?=
CONFIRM_US_915_UPLOAD ?=
PIO_CORE := .tooling/platformio
PIO_VARS := PLATFORMIO_CORE_DIR=$(CURDIR)/$(PIO_CORE) PLATFORMIO_SETTING_ENABLE_TELEMETRY=false
export PLATFORMIO_SETTING_ENABLE_TELEMETRY ?= false

UNKNOWN_PORT_FLAG := $(if $(filter 1 true yes,$(ALLOW_UNKNOWN_PORT)),--allow-unknown,)
SMOKE_ROLE_FLAG := $(if $(SMOKE_ROLE),--smoke-role $(SMOKE_ROLE),)

.DEFAULT_GOAL := help
.PHONY: FORCE help setup tooling-setup local-config ci-test-config deployment-preflight firmware-default-check firmware-compatibility-check firmware-check firmware-full-check firmware-host-test python-test project-check-host project-check project-full-check ios-check ios-test-compile ios-test ios-test-if-available platformio-core pio-build devices port-check require-role upload init-shore-storage serial board-loop clean-tooling-cache

FORCE:

help:
	@printf "CoastCue development and field workflow\n"
	@printf "  make setup                              Create or validate local config and install pinned tooling\n"
	@printf "  make tooling-setup                      Install pinned PlatformIO/PySerial in .tooling\n"
	@printf "  make pio-build-boat              Compile the locked 433 MHz boat OLED firmware\n"
	@printf "  make pio-build-boat-cn           Compile the headless engineering boat profile\n"
	@printf "  make pio-build-boat-cn-oled      Compile the explicit 433 MHz boat OLED profile\n"
	@printf "  make pio-build-*-fault-*         Compile a bench-only storage fault profile\n"
	@printf "  make pio-build-shore             Compile the default shore firmware\n"
	@printf "  make firmware-default-check            Stable alias for the default firmware gate\n"
	@printf "  make firmware-compatibility-check      Compile headless/diagnostic/fault/US/AXP192 profiles\n"
	@printf "  make project-check                     Product checks; iOS tests run when a Simulator is available\n"
	@printf "  make project-check-host                CI host checks without the separate iOS job\n"
	@printf "  make project-full-check                Product checks plus compatibility builds\n"
	@printf "  make deployment-preflight              Validate the private firmware identity and LoRa key\n"
	@printf "  make clean-tooling-cache               Remove only reproducible Python/PlatformIO caches\n"
	@printf "  make devices                     List serial devices before selecting a port\n"
	@printf "  make board-loop-boat PORT=...    Build/upload boat and capture bounded serial logs\n"
	@printf "  make board-loop-shore PORT=...   Build/upload shore and capture bounded serial logs\n"
	@printf "  fault uploads also require CONFIRM_FAULT_INJECTION=BENCH_ONLY\n"
	@printf "  US 915 uploads also require CONFIRM_US_915_UPLOAD=US_915\n"
	@printf "  make init-shore-storage PORT=... CONFIRM_STORAGE_FORMAT=ERASE\n"

setup: local-config tooling-setup

tooling-setup: FORCE $(TOOLING_PYTHON)
	@$(TOOLING_PYTHON) -c 'import platformio, serial' >/dev/null 2>&1 || rm -f $(TOOLING_STAMP)
	@$(MAKE) --no-print-directory $(TOOLING_STAMP)

$(TOOLING_PYTHON):
	@mkdir -p $(TOOLING_DIR)
	$(PYTHON) -m venv $(TOOLING_VENV)

$(TOOLING_STAMP): $(TOOLING_REQUIREMENTS) $(TOOLING_PYTHON)
	$(TOOLING_PYTHON) -m pip install --disable-pip-version-check --requirement $(TOOLING_REQUIREMENTS)
	@touch $(TOOLING_STAMP)

local-config:
	@if test -f $(PIO_DIR)/include/coastcue_config.h; then \
		$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h --check-schema || { \
			printf "Existing private config was not changed. Update it using %s/include/coastcue_config.example.h, then rerun make deployment-preflight.\n" $(PIO_DIR) >&2; exit 1; \
		}; \
	else \
		cp $(PIO_DIR)/include/coastcue_config.example.h $(PIO_DIR)/include/coastcue_config.h; \
		chmod 600 $(PIO_DIR)/include/coastcue_config.h; \
		printf "Created private config template. Replace both deployment placeholders before upload.\n"; \
	fi

# CI uses this non-secret fixture only in a clean checkout. It refuses to
# replace a developer's private config and deployment-preflight still rejects
# the public template placeholders.
ci-test-config:
	@test ! -e $(PIO_DIR)/include/coastcue_config.h || (printf "Refusing to overwrite an existing private config.\n" >&2 && exit 1)
	@$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h --template $(PIO_DIR)/include/coastcue_config.example.h --write-ci-fixture
	@chmod 600 $(PIO_DIR)/include/coastcue_config.h

deployment-preflight:
	$(PYTHON) scripts/coastcue_deploy_config_check.py --config $(PIO_DIR)/include/coastcue_config.h

firmware-compatibility-check: platformio-core
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio run -d $(PIO_DIR) \
		-e boat-cn -e boat-cn-gps-diagnostic -e boat-cn-fault-nvs -e shore-cn-fault-littlefs \
		-e boat-us -e shore-us -e boat-cn-axp192 -e boat-us-axp192

# Canonical default firmware gate. One PlatformIO process builds both roles;
# the documented external aliases below preserve their stable entry points.
firmware-check: platformio-core
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio run -d $(PIO_DIR) -e boat -e shore
	@printf "Default boat and shore firmware compiled successfully.\n"

firmware-default-check: firmware-check

firmware-full-check: firmware-default-check firmware-compatibility-check
	@printf "All supported firmware profiles compiled successfully.\n"

firmware-host-test:
	@set -eu; \
		test_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/coastcue-host-tests.XXXXXX")"; \
		trap 'rm -rf "$$test_dir"' EXIT; \
		run_test() { name="$$1"; shift; c++ -std=c++17 -I$(PIO_DIR)/include "$$@" -o "$$test_dir/$$name"; "$$test_dir/$$name"; }; \
		run_test shore-logic $(PIO_DIR)/test/host/shore_logic_test.cpp $(PIO_DIR)/src/shore_logic.cpp; \
		run_test boat-report-scheduler $(PIO_DIR)/test/host/boat_report_scheduler_test.cpp; \
		run_test incident-protocol $(PIO_DIR)/test/host/incident_protocol_test.cpp; \
		run_test gps-diagnostics $(PIO_DIR)/test/host/gps_diagnostics_test.cpp

python-test:
	$(PYTHON) -m unittest discover -s scripts/tests -v

ios-check:
	xcodebuild build -project $(IOS_PROJECT) -scheme $(IOS_SCHEME) -destination 'generic/platform=iOS Simulator' -derivedDataPath $(IOS_DERIVED_DATA) CODE_SIGNING_ALLOWED=NO $(IOS_STRICT_FLAGS)

ios-test-compile:
	xcodebuild build-for-testing -project $(IOS_PROJECT) -scheme $(IOS_SCHEME) -destination 'generic/platform=iOS Simulator' -derivedDataPath $(IOS_DERIVED_DATA) CODE_SIGNING_ALLOWED=NO $(IOS_STRICT_FLAGS)

ios-test:
	@test -d $(IOS_PROJECT) || (printf "Missing %s. Add the native iOS project before running this check.\n" $(IOS_PROJECT) >&2 && exit 1)
	@destination='$(IOS_TEST_DESTINATION)'; \
	if test -z "$$destination"; then destination="$$($(PYTHON) scripts/ios_simulator_destination.py --project $(IOS_PROJECT) --scheme $(IOS_SCHEME))"; fi; \
	test -n "$$destination" || { printf "No available iOS Simulator runtime; use make ios-check or provide IOS_TEST_DESTINATION.\n" >&2; exit 1; }; \
	xcodebuild test -project $(IOS_PROJECT) -scheme $(IOS_SCHEME) -destination "$$destination" -derivedDataPath $(IOS_DERIVED_DATA) CODE_SIGNING_ALLOWED=NO $(IOS_STRICT_FLAGS)

ios-test-if-available:
	@destination="$$($(PYTHON) scripts/ios_simulator_destination.py --project $(IOS_PROJECT) --scheme $(IOS_SCHEME) 2>/dev/null || true)"; \
	if test -n "$$destination"; then \
		$(MAKE) --no-print-directory ios-test IOS_TEST_DESTINATION="$$destination"; \
	else \
		$(MAKE) --no-print-directory ios-test-compile && \
		printf "No eligible iOS Simulator runtime; CoastCueTests compiled without execution.\n"; \
	fi

# Single source for the non-iOS checks shared by CI. The workflow keeps iOS in
# its macOS job, while local project-check adds simulator execution/compilation.
project-check-host: firmware-host-test python-test firmware-check
	@printf "CoastCue host project checks passed.\n"

project-check: project-check-host ios-test-if-available
	@printf "CoastCue project check passed.\n"

project-full-check: project-check-host firmware-compatibility-check ios-test-if-available
	@printf "CoastCue full compatibility check passed.\n"

platformio-core: tooling-setup
	@mkdir -p $(PIO_CORE)/.cache

pio-build: platformio-core
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio run -d $(PIO_DIR) -e $(PIO_ENV)

pio-build-%: FORCE
	$(MAKE) --no-print-directory pio-build PIO_ENV=$*

devices: platformio-core
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio device list

port-check: platformio-core
	@test -n "$(PORT)" || (printf "PORT is required. Run make devices, then pass PORT=/dev/cu....\n" >&2 && exit 1)
	$(PIO_VARS) COASTCUE_PLATFORMIO="$(TOOLING_PYTHON) -m platformio" $(TOOLING_PYTHON) scripts/port_guard.py --port $(PORT) $(UNKNOWN_PORT_FLAG)

require-role:
	@test "$(origin PIO_ENV)" = "command line" || (printf "PIO_ENV must be explicit for uploads. Use make board-loop-boat PORT=... or make board-loop-shore PORT=....\n" >&2 && exit 1)
	@case "$(PIO_ENV)" in \
		boat|shore|boat-cn|boat-cn-oled|boat-cn-gps-diagnostic|boat-cn-fault-nvs|shore-cn|shore-cn-fault-littlefs|boat-us|shore-us|boat-cn-axp192|boat-us-axp192) ;; \
		*) printf "PIO_ENV must be a declared boat or shore role for uploads, got '%s'.\n" "$(PIO_ENV)" >&2; exit 1 ;; \
	esac
	@case "$(PIO_ENV)" in \
		*-fault-*) test "$(CONFIRM_FAULT_INJECTION)" = "BENCH_ONLY" || \
			(printf "Fault-injection profiles intentionally corrupt runtime persistence behavior. Re-run with CONFIRM_FAULT_INJECTION=BENCH_ONLY only on a dedicated bench unit, then restore the normal profile.\n" >&2; exit 1) ;; \
		*) ;; \
	esac
	@case "$(PIO_ENV)" in \
		*-us|*-us-*) test "$(CONFIRM_US_915_UPLOAD)" = "US_915" || \
			(printf "US 915 profiles are compatibility-only. Re-run with CONFIRM_US_915_UPLOAD=US_915 only when the connected radio and antenna are intended for 915 MHz.\n" >&2; exit 1) ;; \
		*) ;; \
	esac

upload:
	@printf "Direct upload is disabled. Use make board-loop-boat PORT=... or make board-loop-shore PORT=... so upload is followed by bounded serial smoke validation.\n" >&2
	@exit 2

upload-%: FORCE
	$(MAKE) --no-print-directory upload PIO_ENV=$*

init-shore-storage:
	@test "$(CONFIRM_STORAGE_FORMAT)" = "ERASE" || (printf "Shore storage initialization erases all LittleFS history. Re-run with CONFIRM_STORAGE_FORMAT=ERASE only for a new or intentionally reset shore gateway.\n" >&2 && exit 1)
	$(MAKE) --no-print-directory port-check PORT=$(PORT)
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio run -d $(PIO_DIR) -e shore -t uploadfs --upload-port $(PORT)
	$(MAKE) --no-print-directory serial PORT=$(PORT) SMOKE_ROLE=shore SERIAL_SECONDS=$(SERIAL_SECONDS)

serial: tooling-setup
	@test -n "$(PORT)" || (printf "PORT is required. Run make devices, then pass PORT=/dev/cu....\n" >&2 && exit 1)
	$(TOOLING_PYTHON) scripts/serial_capture.py --port $(PORT) --baud $(MONITOR_BAUD) --seconds $(SERIAL_SECONDS) $(SMOKE_ROLE_FLAG)

board-loop: require-role
	$(MAKE) --no-print-directory pio-build PIO_ENV=$(PIO_ENV)
	$(MAKE) --no-print-directory port-check PORT=$(PORT)
	$(MAKE) --no-print-directory deployment-preflight
	$(PIO_VARS) $(TOOLING_PYTHON) -m platformio run -d $(PIO_DIR) -e $(PIO_ENV) -t upload --upload-port $(PORT)
	$(MAKE) --no-print-directory serial PORT=$(PORT) SMOKE_ROLE=$(if $(filter boat%,$(PIO_ENV)),boat,shore) SERIAL_SECONDS=$(SERIAL_SECONDS)

board-loop-%: FORCE
	$(MAKE) --no-print-directory board-loop PIO_ENV=$*

clean-tooling-cache:
	rm -rf $(TOOLING_VENV) $(PIO_CORE)/.cache $(TOOLING_STAMP)
	@printf "Removed only reproducible Python tooling and PlatformIO download caches.\n"
