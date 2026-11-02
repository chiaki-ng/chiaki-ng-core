CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3
PORT ?= 8000
BUILD_DIR ?= build-standalone
TEST_BUILD_DIR ?= build-standalone-tests
BUILD_TYPE ?= Release
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
PREFIX ?= /usr/local
CMAKE_ARGS ?=
CTEST_ARGS ?=
TEST_BUILD_ARGS ?= -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=16 -DLIBCHIAKING_TESTS_PER_BINARY=ON
AREA ?=
PLATFORM_BUILD_ROOT ?= build
DEPS_PREFIX ?=
GENERATOR ?= $(if $(shell command -v ninja 2>/dev/null),Ninja,)

SRC_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

.DEFAULT_GOAL := all

PLATFORMS := darwin ios ios-simulator linux switch windows
include $(SRC_DIR)/cmake/platform/darwin.mk $(SRC_DIR)/cmake/platform/ios.mk \
	$(SRC_DIR)/cmake/platform/linux.mk $(SRC_DIR)/cmake/platform/switch.mk \
	$(SRC_DIR)/cmake/platform/windows.mk
TESTED_PLATFORMS := $(foreach platform,$(PLATFORMS),$(if $($(platform)_TESTS),$(platform)))

ifeq ($(OS),Windows_NT)
HOST_PLATFORM := windows
else
HOST_UNAME := $(shell uname -s)
HOST_PLATFORM := $(if $(filter Darwin,$(HOST_UNAME)),darwin,$(if $(filter Linux,$(HOST_UNAME)),linux,$(HOST_UNAME)))
endif

platform_check = $(if $(filter $($(1)_HOST),$(HOST_PLATFORM)),,$(error make $(1) builds on a $($(1)_HOST) host, not $(HOST_PLATFORM)))$(foreach required,$($(1)_REQUIRES),$(if $($(required)),,$(error make $(1) needs $(required))))
generator_flag = $(if $(GENERATOR),-G "$(GENERATOR)")
platform_configure = "$(CMAKE)" -S "$(SRC_DIR)" -B "$(2)" $(generator_flag) \
	-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $($(1)_CMAKE_ARGS) \
	$(if $(DEPS_PREFIX),-DCMAKE_PREFIX_PATH="$(DEPS_PREFIX)" -DOPENSSL_ROOT_DIR="$(DEPS_PREFIX)") $(CMAKE_ARGS)

define platform_rules
$(1):
	$$(call platform_check,$(1))
	$$(call platform_configure,$(1),$$(PLATFORM_BUILD_ROOT)/$(1)) -DBUILD_TESTING=OFF
	"$$(CMAKE)" --build "$$(PLATFORM_BUILD_ROOT)/$(1)" --parallel "$$(JOBS)" --config "$$(BUILD_TYPE)"
endef

define platform_test_rules
$(1)-test:
	$$(call platform_check,$(1))
	$$(call platform_configure,$(1),$$(PLATFORM_BUILD_ROOT)/$(1)-tests) -DBUILD_TESTING=ON $$(TEST_BUILD_ARGS)
	"$$(CMAKE)" --build "$$(PLATFORM_BUILD_ROOT)/$(1)-tests" --parallel "$$(JOBS)" --config "$$(BUILD_TYPE)"
	"$$(CTEST)" --test-dir "$$(PLATFORM_BUILD_ROOT)/$(1)-tests" --build-config "$$(BUILD_TYPE)" \
		--output-on-failure $$(CTEST_ARGS)
endef

$(foreach platform,$(PLATFORMS),$(eval $(call platform_rules,$(platform))))
$(foreach platform,$(TESTED_PLATFORMS),$(eval $(call platform_test_rules,$(platform))))

.NOTPARALLEL:
.PHONY: all configure build install test test-constrained-curl docs docs-serve docs-lint docs-lint-internal clean help \
	$(PLATFORMS) $(addsuffix -test,$(TESTED_PLATFORMS))

all: build

configure:
	"$(CMAKE)" -S "$(SRC_DIR)" -B "$(BUILD_DIR)" $(generator_flag) \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $(CMAKE_ARGS) -DBUILD_TESTING=OFF

build: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --parallel "$(JOBS)" --config "$(BUILD_TYPE)"

install: build
	"$(CMAKE)" --install "$(BUILD_DIR)" --prefix "$(PREFIX)" --config "$(BUILD_TYPE)"

test:
	"$(CMAKE)" -S "$(SRC_DIR)" -B "$(TEST_BUILD_DIR)" $(generator_flag) \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $(CMAKE_ARGS) -DBUILD_TESTING=ON $(TEST_BUILD_ARGS)
	"$(CMAKE)" --build "$(TEST_BUILD_DIR)" --parallel "$(JOBS)" --config "$(BUILD_TYPE)"
	"$(CTEST)" --test-dir "$(TEST_BUILD_DIR)" --build-config "$(BUILD_TYPE)" \
		--output-on-failure $(CTEST_ARGS)

test-constrained-curl:
	@if [ -z "$(CURL_SOURCE)" ]; then echo "set CURL_SOURCE to a curl tarball or source directory"; exit 1; fi
	cd "$(SRC_DIR)" && "$(PYTHON)" tools/verification/constrained_curl.py --curl-source "$(CURL_SOURCE)" --jobs "$(JOBS)"

docs:
	cd "$(SRC_DIR)" && "$(PYTHON)" tools/docs/docs.py build

docs-serve: docs
	cd "$(SRC_DIR)" && "$(PYTHON)" -m http.server "$(PORT)" --bind 127.0.0.1 --directory build/docs/html

docs-lint:
	cd "$(SRC_DIR)" && "$(PYTHON)" tools/docs/docs.py lint

docs-lint-internal:
	cd "$(SRC_DIR)" && "$(PYTHON)" tools/docs/docs.py lint-internal $(if $(AREA),--area "$(AREA)")

clean:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		"$(CMAKE)" --build "$(BUILD_DIR)" --target clean --config "$(BUILD_TYPE)"; \
	fi
	@if [ -f "$(TEST_BUILD_DIR)/CMakeCache.txt" ]; then \
		"$(CMAKE)" --build "$(TEST_BUILD_DIR)" --target clean --config "$(BUILD_TYPE)"; \
	fi
	@for dir in $(foreach platform,$(PLATFORMS),"$(PLATFORM_BUILD_ROOT)/$(platform)" "$(PLATFORM_BUILD_ROOT)/$(platform)-tests"); do \
		if [ -f "$$dir/CMakeCache.txt" ]; then "$(CMAKE)" --build "$$dir" --target clean --config "$(BUILD_TYPE)"; fi; \
	done

help:
	@echo "make             Build libraries in $(BUILD_DIR) ($(BUILD_TYPE), without tests)"
	@echo "make test        Build and run tests in $(TEST_BUILD_DIR)"
	@echo "make darwin|ios|ios-simulator|linux|windows  Build one platform in $(PLATFORM_BUILD_ROOT)/<platform> on its own host"
	@echo "make darwin-test|linux-test|windows-test     Build and run one platform's tests in $(PLATFORM_BUILD_ROOT)/<platform>-tests"
	@echo "                 ios and ios-simulator need DEPS_PREFIX=<iOS dependency prefix>"
	@echo "make test-constrained-curl CURL_SOURCE=...  Run the curl provider tests against a minimal libcurl"
	@echo "make install     Build and install to PREFIX=$(PREFIX)"
	@echo "make docs        Generate Doxygen HTML in build/docs/html"
	@echo "make docs-serve  Generate and serve docs at http://127.0.0.1:$(PORT) (Ctrl-C to stop)"
	@echo "make docs-lint   Check public API documentation"
	@echo "make docs-lint-internal [AREA=users]  Check internal header documentation"
	@echo "make clean       Clean compiled outputs in every configured build directory"
	@echo "Overrides: CMAKE CTEST PYTHON PORT BUILD_DIR TEST_BUILD_DIR BUILD_TYPE JOBS PREFIX CMAKE_ARGS CTEST_ARGS AREA"
	@echo "Platform overrides: PLATFORM_BUILD_ROOT DEPS_PREFIX GENERATOR MACOS_DEPLOYMENT_TARGET IOS_DEPLOYMENT_TARGET IOS_ARCHITECTURES"
