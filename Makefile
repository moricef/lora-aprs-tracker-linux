# LoRa APRS Tracker — Linux port (Odroid C2/C4)
# Modelé sur l'architecture firmware ESP32 : src/, include/, lib/
#
#   make                  → headless (stdout pipe)
#   make WITH_DISPLAY=1   → LVGL fbdev UI + tracker intégré

CXX      = g++
CC       = gcc
CXXFLAGS = -std=gnu++17 -Wall -O2 -g -DLV_CONF_INCLUDE_SIMPLE
CFLAGS   = -O2 -g -DLV_CONF_INCLUDE_SIMPLE

# Hardware pinout. Raspberry Pi 4B is the primary target; use
# `make BOARD=odroid_c2` for the original Odroid wiring.
BOARD ?= rpi4
ifeq ($(BOARD),rpi4)
  CXXFLAGS += -DBOARD_RPI4
else ifeq ($(BOARD),odroid_c2)
  CXXFLAGS += -DBOARD_ODROID_C2
else
  $(error Unsupported BOARD '$(BOARD)'; expected rpi4 or odroid_c2)
endif

# ---- L
# ---- Dependances lib/ -------------------------------------------------------
RADIO    = lib/RadioLib

INC  = -Iinclude -Iinclude/map -Isrc
INC += -Ilib -Ilib/lvgl -Ilib/lvgl/src
INC += -Ilib/gps_math
INC += -Ilib/APRSPacketLib/include -Ilib/APRSPacketLib/src
INC += -Ilib/PMTiles/cpp -Ilib/vtzero/include -Ilib/protozero/include
INC += -I$(RADIO) -I$(RADIO)/modules/SX126x
INC += -I$(RADIO)/utils -I$(RADIO)/protocols/PhysicalLayer
INC += -I/usr/include/libdrm
INC += $(shell pkg-config --cflags gio-2.0)

# ---- Sources headless (toujours compilés) -----------------------------------
SRCS  = src/main.cpp
SRCS += src/arduino_compat.cpp src/linux_hal.cpp src/linux_connectivity.cpp
SRCS += src/bluetooth_classic.cpp src/bluetooth_ble.cpp src/kiss_utils.cpp src/lora_utils.cpp
SRCS += src/gps_utils.cpp src/configuration.cpp src/smartbeacon_utils.cpp
SRCS += src/station_utils.cpp src/storage_utils.cpp src/msg_utils.cpp
SRCS += src/aprs_is_utils.cpp src/webconf_httpd.cpp src/notification_utils.cpp
SRCS += src/gpx_writer.cpp
SRCS += lib/APRSPacketLib/src/APRSPacketLib.cpp lib/gps_math/gps_math.cpp
SRCS += $(RADIO)/Hal.cpp $(RADIO)/Module.cpp
SRCS += $(RADIO)/modules/SX126x/SX126x.cpp
SRCS += $(RADIO)/modules/SX126x/SX1262.cpp
SRCS += $(RADIO)/modules/SX126x/SX126x_LR_FHSS.cpp
SRCS += $(RADIO)/protocols/PhysicalLayer/PhysicalLayer.cpp
SRCS += $(RADIO)/utils/Utils.cpp $(RADIO)/utils/CRC.cpp
SRCS += $(RADIO)/utils/FEC.cpp $(RADIO)/utils/Cryptography.cpp

TARGET  = lora_aprs_tracker
LDFLAGS = -lpthread -lgps -lm -lmicrohttpd -ldrm -lbluetooth $(shell pkg-config --libs gio-2.0)

# WITH_MAPLIBRE (opt-in GPU display path) needs the LVGL UI too.
ifdef WITH_MAPLIBRE
  WITH_DISPLAY := 1
endif

# ── WITH_DISPLAY=1 ─────────────────────────────────────────────────────────
ifdef WITH_DISPLAY
  CXXFLAGS += -DUSE_LVGL_UI
  # main.cpp gère les deux modes (headless + UI) via #ifdef USE_LVGL_UI

  # UI sources
  SRCS += src/ui_dashboard.cpp src/ui_messaging.cpp src/ui_settings.cpp
  SRCS += src/ui_popups.cpp src/thorvg_stubs.cpp
  SRCS += src/map/map_view.cpp src/map/map_vector.cpp
  SRCS += src/map/map_coordinate_math.cpp src/map/map_state.cpp
  SRCS += src/map/map_io.cpp src/map/map_markers.cpp src/map/map_traces.cpp
  SRCS += src/map/map_labels.cpp src/map/map_engine.cpp src/map/map_input.cpp
  SRCS += src/lora_aprs_logo.c
  SRCS += src/lv_font_mono_16.c src/lv_font_mono_18.c src/lv_font_mono_20.c src/lv_font_mono_22.c src/lv_font_mono_24.c src/mouse_cursor_icon.c

  # Vector tiles
  LDFLAGS += -lz

  # FreeType : polices accentuées au runtime (labels carte, comme le firmware OpenSans-Bold)
  INC     += $(shell pkg-config --cflags freetype2)
  LDFLAGS += $(shell pkg-config --libs freetype2)

  # LVGL C (tous sauf ThorVG vector + drivers SDL)
  LVGL_C := $(shell find lib/lvgl/src -name '*.c' 2>/dev/null \
             | grep -v lv_draw_vector \
             | grep -v '/sdl/' \
             | sort)
  SRCS += $(LVGL_C)
endif

# ── WITH_MAPLIBRE=1 (opt-in) ───────────────────────────────────────────────
# GPU display path: MapLibre base map + LVGL overlay on EGL/KMS. The mbgl
# archives are cross-compiled ARM64 (see maplibre/proto). maplibre_display.cpp
# is the only TU that pulls mbgl headers, compiled apart (C++20, -fno-rtti).
ifdef WITH_MAPLIBRE
  CXXFLAGS += -DWITH_MAPLIBRE
  CFLAGS   += -DWITH_MAPLIBRE
  ML      ?= /home/adrasec09/maplibre-native
  MLBUILD ?= $(ML)/build-cross
  INC += -Ilib/lvgl/src/drivers/opengles/glad/include
  ML_INC := -I$(ML)/include -I$(ML)/platform/default/include -I$(ML)/vendor/maplibre-native-base/include
  ML_INC += $(foreach d,$(wildcard $(ML)/vendor/maplibre-native-base/deps/*/include),-I$(d))
  SRCS += src/maplibre_display.cpp
  ML_LIBS := $(MLBUILD)/libmbgl-core.a \
    $(MLBUILD)/libmbgl-vendor-parsedate.a \
    $(MLBUILD)/vendor/maplibre-tile-spec/cpp/libmlt-cpp.a \
    $(MLBUILD)/libmbgl-vendor-csscolorparser.a \
    $(MLBUILD)/libmbgl-harfbuzz.a \
    $(MLBUILD)/libmbgl-freetype.a \
    $(MLBUILD)/libmbgl-vendor-nunicode.a \
    $(MLBUILD)/libmbgl-vendor-sqlite.a
  LDFLAGS += $(ML_LIBS) $(ML_LIBS) -lEGL -lGLESv2 -lgbm -lcurl -ljpeg -lpng -lwebp -luv \
             -licuuc -licui18n -licudata -lsqlite3 -lrt -ldl
endif

OBJS := $(SRCS:.cpp=.o)
OBJS := $(OBJS:.c=.o)
DEPS := $(OBJS:.o=.d)

# ---- Règles -----------------------------------------------------------------
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# -MMD -MP : génère un .d par objet listant ses en-têtes, pour que make
# recompile quand un header change (sans ça, modifier un .h ne déclenche
# aucune recompilation et le binaire garde l'ancienne valeur).
# mbgl headers require C++20 and are built without RTTI; compile this TU apart.
src/maplibre_display.o: src/maplibre_display.cpp
	$(CXX) -std=gnu++20 -Wall -O2 -g -fno-rtti -DWITH_MAPLIBRE -DLV_CONF_INCLUDE_SIMPLE $(INC) $(ML_INC) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	find lib \( -name "*.o" -o -name "*.d" \) -delete

-include $(DEPS)

.PHONY: all clean
