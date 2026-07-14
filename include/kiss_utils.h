#pragma once

#include <Arduino.h>

enum KissChar { FEND = 0xc0, FESC = 0xdb, TFEND = 0xdc, TFESC = 0xdd };
enum KissCmd { Data = 0x00 };
enum AX25Char { ControlField = 0x03, InformationField = 0xf0 };

namespace KISS_Utils {
bool validateTNC2Frame(const String &frame);
String encodeKISS(const String &frame);
String decodeKISS(const String &frame, bool &dataFrame);
}
