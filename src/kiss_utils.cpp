#include "kiss_utils.h"

namespace {
constexpr uint8_t DIGIPITED = 0x80;
constexpr uint8_t LAST_ADDRESS = 0x01;

String encodeAddress(String address) {
    const bool repeated = address.indexOf('*') >= 0;
    if (repeated) address = address.substring(0, address.indexOf('*'));
    int dash = address.indexOf('-');
    int ssid = dash >= 0 ? address.substring(dash + 1).toInt() : 0;
    if (dash < 0) dash = address.length();
    String out;
    for (int i = 0; i < 6; ++i) {
        const char c = i < dash ? address[i] : ' ';
        out += char(uint8_t(c) << 1);
    }
    out += char((ssid << 1) | 0x60 | (repeated ? DIGIPITED : 0));
    return out;
}

String decodeAddress(const String &raw, bool &last, bool relay) {
    String out;
    for (int i = 0; i < 6 && i < raw.length(); ++i) {
        const char c = char(uint8_t(raw[i]) >> 1);
        if (c != ' ') out += c;
    }
    const uint8_t flags = uint8_t(raw[6]);
    last = flags & LAST_ADDRESS;
    const int ssid = (flags >> 1) & 0x0f;
    if (ssid) { out += '-'; out += String(ssid); }
    if (relay && (flags & DIGIPITED)) out += '*';
    return out;
}

String encapsulate(const String &ax25) {
    String out;
    out += char(FEND);
    out += char(Data);
    for (int i = 0; i < ax25.length(); ++i) {
        const uint8_t c = uint8_t(ax25[i]);
        if (c == FEND) { out += char(FESC); out += char(TFEND); }
        else if (c == FESC) { out += char(FESC); out += char(TFESC); }
        else out += char(c);
    }
    out += char(FEND);
    return out;
}
}

namespace KISS_Utils {

bool validateTNC2Frame(const String &frame) {
    const int gt = frame.indexOf('>');
    const int colon = frame.indexOf(':');
    return gt > 0 && colon > gt;
}

String encodeKISS(const String &frame) {
    if (!validateTNC2Frame(frame)) return {};
    const int gt = frame.indexOf('>');
    const int colon = frame.indexOf(':');
    String ax25;
    String destinationAndPath = frame.substring(gt + 1, colon);
    int comma = destinationAndPath.indexOf(',');
    String destination = comma < 0 ? destinationAndPath : destinationAndPath.substring(0, comma);
    ax25 += encodeAddress(destination);
    ax25 += encodeAddress(frame.substring(0, gt));
    while (comma >= 0) {
        destinationAndPath = destinationAndPath.substring(comma + 1);
        comma = destinationAndPath.indexOf(',');
        ax25 += encodeAddress(comma < 0 ? destinationAndPath : destinationAndPath.substring(0, comma));
    }
    ax25[ax25.length() - 1] = char(uint8_t(ax25[ax25.length() - 1]) | LAST_ADDRESS);
    ax25 += char(ControlField);
    ax25 += char(InformationField);
    ax25 += frame.substring(colon + 1);
    return encapsulate(ax25);
}

String decodeKISS(const String &frame, bool &dataFrame) {
    dataFrame = false;
    if (frame.length() < 4 || uint8_t(frame[0]) != FEND ||
        uint8_t(frame[frame.length() - 1]) != FEND ||
        (uint8_t(frame[1]) & 0x0f) != Data) return {};
    dataFrame = true;
    String ax25;
    for (int i = 2; i < frame.length() - 1; ++i) {
        uint8_t c = uint8_t(frame[i]);
        if (c == FESC && i + 1 < frame.length() - 1) {
            const uint8_t escaped = uint8_t(frame[++i]);
            c = escaped == TFEND ? FEND : escaped == TFESC ? FESC : escaped;
        }
        ax25 += char(c);
    }
    if (ax25.length() < 16) { dataFrame = false; return {}; }
    bool last = false;
    const String destination = decodeAddress(ax25.substring(0, 7), last, false);
    const String source = decodeAddress(ax25.substring(7, 14), last, false);
    String tnc2 = source + ">" + destination;
    int offset = 14;
    while (!last && offset + 7 <= ax25.length()) {
        tnc2 += ',';
        tnc2 += decodeAddress(ax25.substring(offset, offset + 7), last, true);
        offset += 7;
    }
    if (offset + 2 > ax25.length()) { dataFrame = false; return {}; }
    tnc2 += ':';
    tnc2 += ax25.substring(offset + 2);
    return tnc2;
}

}
