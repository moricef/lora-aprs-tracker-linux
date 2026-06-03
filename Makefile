# LoRa APRS Tracker — Linux port (Odroid C2/C4)
# Modelé sur l'architecture firmware ESP32 : src/, include/, lib/
#
#   make                  → headless (stdout pipe)
#   make WITH_DISPLAY=1   → LVGL fbdev UI + tracker intégré

CXX      = g++
CC       = gcc
CXXFLAGS = -std=gnu++17 -Wall -O2 -g -DLV_CONF_INCLUDE_SIMPLE
CFLAGS   = -O2 -g -DLV_CONF_INCLUDE_SIMPLE

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

# ---- Sources headless (toujours compilés) -----------------------------------
SRCS  = src/main.cpp
SRCS += src/arduino_compat.cpp src/linux_hal.cpp src/lora_utils.cpp
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
LDFLAGS = -lpthread -lgps -lm -lmicrohttpd -ldrm

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

OBJS := $(SRCS:.cpp=.o)
OBJS := $(OBJS:.c=.o)

# ---- Règles -----------------------------------------------------------------
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	find lib -name "*.o" -delete

.PHONY: all clean
