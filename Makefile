# LoRa APRS Tracker - Linux port
# Build on Odroid C2 (aarch64) or dev machine (x86_64)

CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2

# RadioLib base path (local copy)
RADIO = lib/RadioLib

INC = -Iinclude -I$(RADIO) -I$(RADIO)/modules/SX126x -I$(RADIO)/utils -I$(RADIO)/protocols/PhysicalLayer

# Our sources
SRCS = src/linux_hal.cpp src/main.cpp

# RadioLib sources (minimal for SX1262)
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

OBJS = $(SRCS:.cpp=.o)
TARGET = lora_rx

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ -lpthread -lgps

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
