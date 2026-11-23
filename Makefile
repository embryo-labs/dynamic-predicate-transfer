BUILD_DIR := build/release
JOBS ?= 4
NATIVE_ARCH ?= ON
CLANG_FORMAT ?= clang-format

.PHONY: release configure test format
release: configure
	cmake --build "$(BUILD_DIR)" --target duckdb rpt_loadable_extension --parallel $(JOBS)

configure:
	cmake -S duckdb -B "$(BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DNATIVE_ARCH=$(NATIVE_ARCH) -DBUILD_UNITTESTS=ON \
		-DDUCKDB_EXTENSION_CONFIGS="$(CURDIR)/extension_config.cmake" \
		-DDUCKDB_EXPLICIT_VERSION=v2.0.0

test: release
	cmake --build "$(BUILD_DIR)" --target unittest --parallel $(JOBS)
	"$(BUILD_DIR)/test/unittest" '*rpt_port.test'
	python3 test/check_dynamic_abandon.py --duckdb "$(BUILD_DIR)/duckdb" \
		--extension "$(BUILD_DIR)/extension/rpt/rpt.duckdb_extension" --output "$(BUILD_DIR)/replay-check"

format:
	find src -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec $(CLANG_FORMAT) -style=file -i {} +
