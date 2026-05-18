# LoRa APRS Tracker - Linux port
# Build on Odroid C2 (aarch64) or dev machine (x86_64)

CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2 -g

# RadioLib base path (local copy)
RADIO = lib/RadioLib

INC = -Iinclude \
      -Ilib/APRSPacketLib/include \
      -Ilib/gps_math \
      -I$(RADIO) \
      -I$(RADIO)/modules/SX126x \
      -I$(RADIO)/utils \
      -I$(RADIO)/protocols/PhysicalLayer

# ── Tracker sources ─────────────────────────────────────────────────────────
SRCS  = src/main.cpp
SRCS += src/arduino_compat.cpp
SRCS += src/linux_hal.cpp
SRCS += src/lora_utils.cpp
SRCS += src/gps_utils.cpp
SRCS += src/configuration.cpp
SRCS += src/smartbeacon_utils.cpp
SRCS += src/station_utils.cpp
SRCS += src/storage_utils.cpp
SRCS += src/msg_utils.cpp
SRCS += src/aprs_is_utils.cpp
SRCS += src/webconf_httpd.cpp
SRCS += src/notification_utils.cpp
SRCS += lib/APRSPacketLib/src/APRSPacketLib.cpp
SRCS += lib/gps_math/gps_math.cpp

# ── RadioLib sources (minimal for SX1262) ───────────────────────────────────
SRCS += $(RADIO)/Hal.cpp
SRCS += $(RADIO)/Module.cpp
SRCS += $(RADIO)/modules/SX126x/SX126x.cpp
SRCS += $(RADIO)/modules/SX126x/SX1262.cpp
SRCS += $(RADIO)/modules/SX126x/SX126x_LR_FHSS.cpp
SRCS += $(RADIO)/protocols/PhysicalLayer/PhysicalLayer.cpp
SRCS += $(RADIO)/utils/Utils.cpp
SRCS += $(RADIO)/utils/CRC.cpp
SRCS += $(RADIO)/utils/FEC.cpp
SRCS += $(RADIO)/utils/Cryptography.cpp

OBJS   = $(SRCS:.cpp=.o)
TARGET = lora_aprs_tracker

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ -lpthread -lgps -lm -lmicrohttpd

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
