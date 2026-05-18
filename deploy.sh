#!/bin/bash
set -e
ODROID=fab2@192.168.1.168
REMOTE=/home/fab2/Developpement/LoRa_APRS/linux_tracker
LOCAL=/home/fab2/Developpement/LoRa_APRS/linux_tracker

rsync -av --exclude='*.o' --exclude='lora_aprs_tracker' \
    "$LOCAL/" "$ODROID:$REMOTE/"

ssh "$ODROID" "cd $REMOTE && make"
