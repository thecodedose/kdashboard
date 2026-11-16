CXX ?= g++
KINDLE_CXX ?= arm-linux-gnueabi-g++
ZIG ?= zig
ZIG_TARGET ?= arm-linux-musleabi
ZIG_MCPU ?=
ZIG_CPU_FLAG := $(if $(ZIG_MCPU),-mcpu=$(ZIG_MCPU),)
ZIG_LDFLAGS ?= -static

SRC := src/kindle_dashboard.cpp
INFLATER := vendor/miniz/miniz_tinfl.c
WIDGETS := src/dashboard_widgets.inc src/dashboard_vacuum.inc src/vacuum_views.inc src/music_state.inc src/music_views.inc src/music_client.inc src/orders_state.inc src/orders_views.inc src/orders_client.inc src/narwal_client.hpp $(INFLATER)
MINIZ_FLAGS := -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
BUILD_DIR := build
TARGET := $(BUILD_DIR)/kindle-dashboard-local
KINDLE_TARGET := $(BUILD_DIR)/kindle-dashboard-arm
ZIG_TARGET_BINARY := $(BUILD_DIR)/kindle-dashboard-zig
EXTENSION_ROOT := $(BUILD_DIR)/kual-package
EXTENSION_DIR := $(EXTENSION_ROOT)/kindle-dashboard
EXTENSION_ARCHIVE := $(BUILD_DIR)/kindle-dashboard-kual.tar.gz

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2

.PHONY: all local kindle kindle-zig extension extension-zig check check-widgets clean narwal-probe narwal-probe-zig check-narwal check-narwal-client check-narwal-map

all: local

# Standalone LAN diagnostic; does not replace or launch the dashboard.
narwal-probe: $(BUILD_DIR)/narwal-probe-local

$(BUILD_DIR)/narwal-probe-local: src/narwal_probe.cpp src/narwal_client.hpp $(INFLATER)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

narwal-probe-zig: $(BUILD_DIR)/narwal-probe-arm

$(BUILD_DIR)/narwal-probe-arm: src/narwal_probe.cpp src/narwal_client.hpp $(INFLATER)
	mkdir -p $(BUILD_DIR)
	$(ZIG) c++ -target $(ZIG_TARGET) $(ZIG_CPU_FLAG) $(CXXFLAGS) $(ZIG_LDFLAGS) -s $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-narwal: $(BUILD_DIR)/narwal-probe-local
	NARWAL_PROBE_BINARY=$(CURDIR)/$(BUILD_DIR)/narwal-probe-local python3 ../../tests/test_narwal_probe.py

$(BUILD_DIR)/narwal-client-test: ../../tests/narwal-client-driver.cpp src/narwal_client.hpp $(INFLATER)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-narwal-client: $(BUILD_DIR)/narwal-client-test
	NARWAL_CLIENT_BINARY=$(CURDIR)/$(BUILD_DIR)/narwal-client-test NARWAL_PROBE_BINARY=$(CURDIR)/$(BUILD_DIR)/narwal-probe-local python3 ../../tests/test_narwal_client.py

$(BUILD_DIR)/narwal-map-test: ../../tests/native-narwal-map.cpp src/narwal_client.hpp $(INFLATER)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-narwal-map: $(BUILD_DIR)/narwal-map-test
	$(BUILD_DIR)/narwal-map-test

local: $(TARGET)

$(TARGET): $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

kindle: $(KINDLE_TARGET)

$(KINDLE_TARGET): $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	@command -v $(KINDLE_CXX) >/dev/null 2>&1 || { \
		echo "Missing Kindle cross compiler: $(KINDLE_CXX)"; \
		echo "Install a compatible ARM toolchain or rerun with KINDLE_CXX=/path/to/compiler"; \
		exit 1; \
	}
	$(KINDLE_CXX) $(CXXFLAGS) -static $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

kindle-zig: $(ZIG_TARGET_BINARY)

$(ZIG_TARGET_BINARY): $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	@command -v $(ZIG) >/dev/null 2>&1 || { \
		echo "Missing Zig compiler: $(ZIG)"; \
		echo "Install Zig or rerun with ZIG=/path/to/zig"; \
		exit 1; \
	}
	$(ZIG) c++ -target $(ZIG_TARGET) $(ZIG_CPU_FLAG) $(CXXFLAGS) $(ZIG_LDFLAGS) -s $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

extension: $(KINDLE_TARGET)
	rm -rf $(EXTENSION_ROOT)
	mkdir -p $(EXTENSION_DIR)/bin
	mkdir -p $(EXTENSION_DIR)/assets
	cp ../kual/kindle-dashboard/config.xml $(EXTENSION_DIR)/config.xml
	cp ../kual/kindle-dashboard/config.sh.example $(EXTENSION_DIR)/config.sh.example
	cp ../kual/kindle-dashboard/menu.json $(EXTENSION_DIR)/menu.json
	cp ../kual/kindle-dashboard/assets/meal-planner-cover.pgm $(EXTENSION_DIR)/assets/meal-planner-cover.pgm
	cp ../kual/kindle-dashboard/assets/profile-placeholder.pgm $(EXTENSION_DIR)/assets/profile-placeholder.pgm
	cp ../kual/kindle-dashboard/assets/narwal-x10-pro.pgm $(EXTENSION_DIR)/assets/narwal-x10-pro.pgm
	cp ../kual/kindle-dashboard/assets/spotify-logo.pgm $(EXTENSION_DIR)/assets/spotify-logo.pgm
	cp ../kual/kindle-dashboard/assets/orders-parcel.pgm $(EXTENSION_DIR)/assets/orders-parcel.pgm
	mkdir -p $(EXTENSION_DIR)/licenses
	cp vendor/miniz/LICENSE $(EXTENSION_DIR)/licenses/miniz.txt
	if [ -d ../kual/kindle-dashboard/assets/recipes ]; then cp -R ../kual/kindle-dashboard/assets/recipes $(EXTENSION_DIR)/assets/recipes; fi
	cp ../kual/kindle-dashboard/bin/menu-ping.sh $(EXTENSION_DIR)/bin/menu-ping.sh
	cp ../kual/kindle-dashboard/bin/start.sh $(EXTENSION_DIR)/bin/start.sh
	cp ../kual/kindle-dashboard/bin/start-light.sh $(EXTENSION_DIR)/bin/start-light.sh
	cp ../kual/kindle-dashboard/bin/start-dark.sh $(EXTENSION_DIR)/bin/start-dark.sh
	cp ../kual/kindle-dashboard/bin/once.sh $(EXTENSION_DIR)/bin/once.sh
	cp ../kual/kindle-dashboard/bin/once-light.sh $(EXTENSION_DIR)/bin/once-light.sh
	cp ../kual/kindle-dashboard/bin/once-dark.sh $(EXTENSION_DIR)/bin/once-dark.sh
	cp ../kual/kindle-dashboard/bin/proof.sh $(EXTENSION_DIR)/bin/proof.sh
	cp ../kual/kindle-dashboard/bin/stop.sh $(EXTENSION_DIR)/bin/stop.sh
	cp ../kual/kindle-dashboard/bin/dashboard.sh $(EXTENSION_DIR)/bin/dashboard.sh
	cp ../kual/kindle-dashboard/bin/diagnose.sh $(EXTENSION_DIR)/bin/diagnose.sh
	cp $(KINDLE_TARGET) $(EXTENSION_DIR)/bin/kindle-dashboard
	chmod +x $(EXTENSION_DIR)/bin/menu-ping.sh $(EXTENSION_DIR)/bin/start.sh $(EXTENSION_DIR)/bin/start-light.sh $(EXTENSION_DIR)/bin/start-dark.sh $(EXTENSION_DIR)/bin/once.sh $(EXTENSION_DIR)/bin/once-light.sh $(EXTENSION_DIR)/bin/once-dark.sh $(EXTENSION_DIR)/bin/proof.sh $(EXTENSION_DIR)/bin/stop.sh $(EXTENSION_DIR)/bin/dashboard.sh $(EXTENSION_DIR)/bin/diagnose.sh $(EXTENSION_DIR)/bin/kindle-dashboard
	COPYFILE_DISABLE=1 tar -C $(EXTENSION_ROOT) -czf $(EXTENSION_ARCHIVE) kindle-dashboard
	@echo "Packaged $(EXTENSION_ARCHIVE)"

extension-zig: $(ZIG_TARGET_BINARY)
	rm -rf $(EXTENSION_ROOT)
	mkdir -p $(EXTENSION_DIR)/bin
	mkdir -p $(EXTENSION_DIR)/assets
	cp ../kual/kindle-dashboard/config.xml $(EXTENSION_DIR)/config.xml
	cp ../kual/kindle-dashboard/config.sh.example $(EXTENSION_DIR)/config.sh.example
	cp ../kual/kindle-dashboard/menu.json $(EXTENSION_DIR)/menu.json
	cp ../kual/kindle-dashboard/assets/meal-planner-cover.pgm $(EXTENSION_DIR)/assets/meal-planner-cover.pgm
	cp ../kual/kindle-dashboard/assets/profile-placeholder.pgm $(EXTENSION_DIR)/assets/profile-placeholder.pgm
	cp ../kual/kindle-dashboard/assets/narwal-x10-pro.pgm $(EXTENSION_DIR)/assets/narwal-x10-pro.pgm
	cp ../kual/kindle-dashboard/assets/spotify-logo.pgm $(EXTENSION_DIR)/assets/spotify-logo.pgm
	cp ../kual/kindle-dashboard/assets/orders-parcel.pgm $(EXTENSION_DIR)/assets/orders-parcel.pgm
	mkdir -p $(EXTENSION_DIR)/licenses
	cp vendor/miniz/LICENSE $(EXTENSION_DIR)/licenses/miniz.txt
	if [ -d ../kual/kindle-dashboard/assets/recipes ]; then cp -R ../kual/kindle-dashboard/assets/recipes $(EXTENSION_DIR)/assets/recipes; fi
	cp ../kual/kindle-dashboard/bin/menu-ping.sh $(EXTENSION_DIR)/bin/menu-ping.sh
	cp ../kual/kindle-dashboard/bin/start.sh $(EXTENSION_DIR)/bin/start.sh
	cp ../kual/kindle-dashboard/bin/start-light.sh $(EXTENSION_DIR)/bin/start-light.sh
	cp ../kual/kindle-dashboard/bin/start-dark.sh $(EXTENSION_DIR)/bin/start-dark.sh
	cp ../kual/kindle-dashboard/bin/once.sh $(EXTENSION_DIR)/bin/once.sh
	cp ../kual/kindle-dashboard/bin/once-light.sh $(EXTENSION_DIR)/bin/once-light.sh
	cp ../kual/kindle-dashboard/bin/once-dark.sh $(EXTENSION_DIR)/bin/once-dark.sh
	cp ../kual/kindle-dashboard/bin/proof.sh $(EXTENSION_DIR)/bin/proof.sh
	cp ../kual/kindle-dashboard/bin/stop.sh $(EXTENSION_DIR)/bin/stop.sh
	cp ../kual/kindle-dashboard/bin/dashboard.sh $(EXTENSION_DIR)/bin/dashboard.sh
	cp ../kual/kindle-dashboard/bin/diagnose.sh $(EXTENSION_DIR)/bin/diagnose.sh
	cp $(ZIG_TARGET_BINARY) $(EXTENSION_DIR)/bin/kindle-dashboard
	chmod +x $(EXTENSION_DIR)/bin/menu-ping.sh $(EXTENSION_DIR)/bin/start.sh $(EXTENSION_DIR)/bin/start-light.sh $(EXTENSION_DIR)/bin/start-dark.sh $(EXTENSION_DIR)/bin/once.sh $(EXTENSION_DIR)/bin/once-light.sh $(EXTENSION_DIR)/bin/once-dark.sh $(EXTENSION_DIR)/bin/proof.sh $(EXTENSION_DIR)/bin/stop.sh $(EXTENSION_DIR)/bin/dashboard.sh $(EXTENSION_DIR)/bin/diagnose.sh $(EXTENSION_DIR)/bin/kindle-dashboard
	COPYFILE_DISABLE=1 tar -C $(EXTENSION_ROOT) -czf $(EXTENSION_ARCHIVE) kindle-dashboard
	@echo "Packaged $(EXTENSION_ARCHIVE)"

$(BUILD_DIR)/kindle-widgets-test: ../../tests/native-widgets.cpp $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-widgets: $(BUILD_DIR)/kindle-widgets-test
	$(BUILD_DIR)/kindle-widgets-test

$(BUILD_DIR)/kindle-music-test: ../../tests/native-music.cpp $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-music: $(BUILD_DIR)/kindle-music-test
	$(BUILD_DIR)/kindle-music-test

$(BUILD_DIR)/kindle-orders-test: ../../tests/native-orders.cpp $(SRC) $(WIDGETS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_FLAGS) -o $@ $< -x c++ $(INFLATER)

check-orders: $(BUILD_DIR)/kindle-orders-test
	$(BUILD_DIR)/kindle-orders-test

check: $(TARGET) check-widgets check-music check-orders check-narwal check-narwal-client check-narwal-map
	$(TARGET) --render fixtures/dashboard-data.json

clean:
	rm -rf $(BUILD_DIR)
