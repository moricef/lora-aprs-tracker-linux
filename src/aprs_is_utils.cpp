#include "esp_log.h"
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "aprs_is_utils.h"
#include "configuration.h"

static const char* TAG = "APRS_IS";

extern Configuration Config;
extern Beacon*       currentBeacon;

static int    _sock           = -1;
static bool   _connected      = false;
static bool   _passcodeValid  = false;
static uint32_t _lastTry      = 0;

static String _versionStr = "1.0";

static void closeSocket() {
    if (_sock >= 0) { close(_sock); _sock = -1; }
    _connected = _passcodeValid = false;
}

namespace APRS_IS_Utils {

    void setup() {}

    void connect() {
        if (!Config.aprs_is.active) return;

        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portStr[8];
        snprintf(portStr, sizeof(portStr), "%d", Config.aprs_is.port);

        if (getaddrinfo(Config.aprs_is.server.c_str(), portStr, &hints, &res) != 0 || !res) {
            ESP_LOGW(TAG, "DNS failed for %s", Config.aprs_is.server.c_str());
            _lastTry = millis();
            return;
        }

        _sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (_sock < 0) { freeaddrinfo(res); _lastTry = millis(); return; }

        // Set 3-second connect timeout via SO_SNDTIMEO
        struct timeval tv = {3, 0};
        setsockopt(_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (::connect(_sock, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGW(TAG, "Connect failed: %s", strerror(errno));
            freeaddrinfo(res); closeSocket(); _lastTry = millis(); return;
        }
        freeaddrinfo(res);
        ESP_LOGI(TAG, "Connected to %s:%d", Config.aprs_is.server.c_str(), Config.aprs_is.port);

        // Auth
        String auth = "user " + currentBeacon->callsign + " pass " + Config.aprs_is.passcode +
                      " vers LinuxTracker " + _versionStr;
        auth += "\r\n";
        send(_sock, auth.c_str(), auth.length(), 0);

        // Wait for server response (up to 10 s)
        char buf[512]; uint32_t start = millis();
        while (millis() - start < 10000) {
            int n = recv(_sock, buf, sizeof(buf)-1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            String resp(buf);
            if (resp.indexOf("verified") >= 0 && resp.indexOf("unverified") < 0) {
                _passcodeValid = true;
                _connected = true;
                ESP_LOGI(TAG, "Passcode verified");
                break;
            } else if (resp.indexOf("unverified") >= 0) {
                _connected = true;
                ESP_LOGW(TAG, "Passcode invalid — read-only");
                break;
            }
        }
        if (!_connected) { ESP_LOGW(TAG, "No logresp in 10s"); closeSocket(); }
        _lastTry = millis();
    }

    void upload(const String& packet) {
        if (!_connected || _sock < 0 || !_passcodeValid) return;
        String line = packet + "\r\n";
        if (send(_sock, line.c_str(), line.length(), 0) < 0) {
            ESP_LOGW(TAG, "Send failed, disconnecting");
            closeSocket();
            return;
        }
        ESP_LOGI(TAG, "Uploaded: %s", packet.c_str());
    }

    bool isConnected() {
        if (!_connected || _sock < 0) return false;
        // Quick liveness check: peek
        char c; int r = recv(_sock, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) { closeSocket(); return false; }
        return true;
    }

    void checkConnection() {
        if (!Config.aprs_is.active) { if (_connected) closeSocket(); return; }
        if (_connected && _sock >= 0) {
            char c; int r = recv(_sock, &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) { ESP_LOGI(TAG, "Connection lost"); closeSocket(); }
        }
        if (!_connected && (millis() - _lastTry > 30000)) connect();
    }

    void disconnect() { closeSocket(); }
}
