#include "webconf_httpd.h"
#include "configuration.h"
#include "lora_utils.h"
#include <microhttpd.h>
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <pthread.h>

extern Configuration Config;

using json = nlohmann::json;

static struct MHD_Daemon *_daemon = nullptr;
static pthread_mutex_t   _cfg_mtx = PTHREAD_MUTEX_INITIALIZER;
static std::string       _assets_dir;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string readFile(const std::string& path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::string s(sz, '\0');
    fread(&s[0], 1, sz, f);
    fclose(f);
    return s;
}

static std::string readJsonConfig() {
    const char* td = getenv("TRACKER_DATA");
    std::string path = (td ? std::string(td) : std::string("/data/LoRa_Tracker")) + "/tracker_conf.json";
    std::string body = readFile(path);
    if (body.empty()) return body;

    try {
        json data = json::parse(body);
        data["loraFreqMin"] = 100000000;
        data["loraFreqMax"] = 1000000000;
        data["bluetooth"]["hasBTClassic"] = true;
        if (data.contains("lora") && data["lora"].is_array()) {
            const char *defaults[] = {"EU/WORLD", "Poland", "UK"};
            for (size_t i = 0; i < data["lora"].size(); i++) {
                if (!data["lora"][i].contains("profileName") ||
                    !data["lora"][i]["profileName"].is_string() ||
                    data["lora"][i]["profileName"].get<std::string>().empty()) {
                    data["lora"][i]["profileName"] = (i < 3) ? defaults[i] : ("PROFILE " + std::to_string(i + 1));
                }
            }
        }
        if (data.contains("wifi") && data["wifi"].contains("AP") && data["wifi"]["AP"].is_array()) {
            json deduped = json::array();
            for (const auto& ap : data["wifi"]["AP"]) {
                bool duplicate = false;
                for (const auto& existing : deduped) {
                    if (existing.value("ssid", "") == ap.value("ssid", "") &&
                        existing.value("password", "") == ap.value("password", "")) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                deduped.push_back(ap);
                if (deduped.size() >= 64) break;
            }
            data["wifi"]["AP"] = deduped;
        }
        return data.dump();
    } catch (...) {
        return body;
    }
}

static std::string trimCopy(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static int clampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

static long clampLong(long value, long minValue, long maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

static std::string upperTrimmed(std::string value) {
    value = trimCopy(value);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::toupper(c);
    });
    return value;
}

// ── MHD connection state ───────────────────────────────────────────────────────

struct ConnState {
    struct MHD_PostProcessor *pp;
    std::map<std::string,std::string> fields;
};

static MHD_Result iteratePost(void *cls, enum MHD_ValueKind,
                               const char *key, const char*,
                               const char*, const char*,
                               const char *data, uint64_t, size_t size)
{
    auto *st = static_cast<ConnState*>(cls);
    if (key && data && size > 0)
        st->fields[key].append(data, size);
    return MHD_YES;
}

// ── apply POST fields to Config and write JSON ────────────────────────────────

static void applyAndSave(const std::map<std::string,std::string>& f) {
    auto str = [&](const std::string& k, const std::string& def="") -> std::string {
        auto it = f.find(k); return it != f.end() ? it->second : def;
    };
    auto has = [&](const std::string& k) { return f.count(k) > 0; };
    auto num = [&](const std::string& k, int def=0) -> int {
        auto it = f.find(k);
        if (it == f.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    };
    auto dbl = [&](const std::string& k, double def=0) -> double {
        auto it = f.find(k);
        if (it == f.end() || it->second.empty()) return def;
        try { return std::stod(it->second); } catch (...) { return def; }
    };

    pthread_mutex_lock(&_cfg_mtx);

    while ((int)Config.beacons.size() < 3) Config.beacons.push_back({});

    for (int i = 0; i < 3 && i < (int)Config.beacons.size(); i++) {
        std::string p = "beacons." + std::to_string(i) + ".";
        auto& b = Config.beacons[i];
        b.callsign           = str(p+"callsign",   b.callsign.c_str());
        b.symbol             = str(p+"symbol",     b.symbol.c_str());
        b.overlay            = str(p+"overlay",    b.overlay.c_str());
        b.micE               = str(p+"micE",       b.micE.c_str());
        b.comment            = str(p+"comment",    b.comment.c_str());
        b.status             = str(p+"status",     b.status.c_str());
        b.profileLabel       = str(p+"profileLabel", b.profileLabel.c_str());
        b.gpsEcoMode         = has(p+"gpsEcoMode");
        b.smartBeaconActive  = has(p+"smartBeaconActive");
        b.smartBeaconSetting = num(p+"smartBeaconSetting", b.smartBeaconSetting);
        b.sbSlowRate         = num(p+"sbSlowRate",  b.sbSlowRate);
        b.sbFastRate         = num(p+"sbFastRate",  b.sbFastRate);
        b.sbMinSpeed         = num(p+"sbMinSpeed",  b.sbMinSpeed);
        b.sbMaxSpeed         = num(p+"sbMaxSpeed",  b.sbMaxSpeed);
        b.sbMinTurnAngle     = num(p+"sbMinTurnAngle", b.sbMinTurnAngle);
        b.sbTurnSlope        = num(p+"sbTurnSlope", b.sbTurnSlope);
        b.sbMinBeaconTime    = num(p+"sbMinBeaconTime", b.sbMinBeaconTime);
    }

    Config.path                      = str("path",                     Config.path.c_str());
    Config.sendCommentAfterXBeacons  = num("sendCommentAfterXBeacons", Config.sendCommentAfterXBeacons);
    Config.nonSmartBeaconRate        = num("nonSmartBeaconRate",       Config.nonSmartBeaconRate);
    Config.rememberStationTime       = num("rememberStationTime",      Config.rememberStationTime);
    Config.standingUpdateTime        = num("standingUpdateTime",       Config.standingUpdateTime);
    Config.sendAltitude              = has("sendAltitude");
    Config.disableGPS                = has("disableGPS");
    Config.simplifiedTrackerMode     = has("simplifiedTrackerMode");
    Config.email                     = str("email",                    Config.email.c_str());

    Config.display.ecoMode           = has("display.ecoMode");
    Config.display.timeout           = num("display.timeout",          Config.display.timeout);
    Config.display.turn180           = has("display.turn180");
    Config.display.showSymbol        = has("display.showSymbol");

    Config.aprs_is.active            = has("aprs_is.active");
    Config.aprs_is.server            = str("aprs_is.server",           Config.aprs_is.server.c_str());
    Config.aprs_is.port              = num("aprs_is.port",             Config.aprs_is.port);
    Config.aprs_is.passcode          = str("aprs_is.passcode",         Config.aprs_is.passcode.c_str());

    if (has("wifi.AP.count")) {
        int wifiAPCount = clampInt(num("wifi.AP.count", (int)Config.wifiAPs.size()), 1, 64);
        std::vector<WiFi_AP> postedWifiAPs;
        postedWifiAPs.reserve(wifiAPCount);
        for (int i = 0; i < wifiAPCount; i++) {
            std::string p = "wifi.AP." + std::to_string(i) + ".";
            WiFi_AP ap;
            ap.ssid = trimCopy(str(p+"ssid", ""));
            ap.password = str(p+"password", "");
            postedWifiAPs.push_back(ap);
        }
        Config.wifiAPs = std::move(postedWifiAPs);
    } else {
        while ((int)Config.wifiAPs.size() < 2) Config.wifiAPs.push_back({});
        Config.wifiAPs[0].ssid       = str("wifi.AP.0.ssid",           Config.wifiAPs[0].ssid.c_str());
        Config.wifiAPs[0].password   = str("wifi.AP.0.password",       Config.wifiAPs[0].password.c_str());
        Config.wifiAPs[1].ssid       = str("wifi.AP.1.ssid",           Config.wifiAPs[1].ssid.c_str());
        Config.wifiAPs[1].password   = str("wifi.AP.1.password",       Config.wifiAPs[1].password.c_str());
    }
    Config.wifiAutoAP.password       = str("wifi.autoAP.password",     Config.wifiAutoAP.password.c_str());

    Config.bluetooth.active          = has("bluetooth.active");
    Config.bluetooth.useBLE          = f.count("bluetooth.transport")
                                          ? str("bluetooth.transport") == "ble"
                                          : has("bluetooth.useBLE");
    Config.bluetooth.useKISS         = f.count("bluetooth.protocol")
                                          ? str("bluetooth.protocol") == "kiss"
                                          : has("bluetooth.useKISS");
    Config.bluetooth.deviceName      = str("bluetooth.deviceName",     Config.bluetooth.deviceName.c_str());

    Config.lora.sendInfo             = has("loraConfig.sendInfo");
    Config.lora.repeaterMode         = has("loraConfig.repeaterMode");
    Config.lora.digipeatAlias        = upperTrimmed(str("loraConfig.digipeatAlias", Config.lora.digipeatAlias.c_str()));
    if (Config.lora.digipeatAlias.isEmpty()) Config.lora.digipeatAlias = "WIDE1-1";

    if (has("lora.count")) {
        int loraProfileCount = clampInt(num("lora.count", (int)Config.loraTypes.size()), 1, 64);
        std::vector<LoraType> postedLoraProfiles;
        postedLoraProfiles.reserve(loraProfileCount);

        for (int i = 0; i < loraProfileCount; i++) {
            std::string p = "lora." + std::to_string(i) + ".";
            LoraType profile;
            std::string name = trimCopy(str(p+"profileName", "PROFILE " + std::to_string(i + 1)));
            if (name.empty()) name = "PROFILE " + std::to_string(i + 1);
            if (name.size() > 16) name.resize(16);
            profile.profileName = name;
            profile.frequency = clampLong((long)dbl(p+"frequency", 433775000), 100000000, 1000000000);
            profile.spreadingFactor = clampInt(num(p+"spreadingFactor", 12), 5, 12);
            profile.codingRate4 = clampInt(num(p+"codingRate4", 5), 5, 8);
            profile.signalBandwidth = num(p+"signalBandwidth", 125000) == 62500 ? 62500 : 125000;
            profile.power = clampInt(num(p+"power", 20), 1, 22);
            profile.dataRate = LoRa_Utils::calculateDataRate(
                profile.spreadingFactor,
                profile.codingRate4,
                profile.signalBandwidth
            );
            postedLoraProfiles.push_back(profile);
        }
        Config.loraTypes = std::move(postedLoraProfiles);
    } else {
        for (int i = 0; i < 3; i++) {
            std::string p = "lora." + std::to_string(i) + ".";
            if (!has(p+"frequency")) continue;
            while ((int)Config.loraTypes.size() <= i) Config.loraTypes.push_back({});
            std::string name = trimCopy(str(p+"profileName", Config.loraTypes[i].profileName.c_str()));
            if (!name.empty()) {
                if (name.size() > 16) name.resize(16);
                Config.loraTypes[i].profileName = name;
            }
            Config.loraTypes[i].frequency       = clampLong((long)dbl(p+"frequency",       Config.loraTypes[i].frequency), 100000000, 1000000000);
            Config.loraTypes[i].spreadingFactor = clampInt(num(p+"spreadingFactor",        Config.loraTypes[i].spreadingFactor), 5, 12);
            Config.loraTypes[i].codingRate4     = clampInt(num(p+"codingRate4",            Config.loraTypes[i].codingRate4), 5, 8);
            Config.loraTypes[i].signalBandwidth = num(p+"signalBandwidth", Config.loraTypes[i].signalBandwidth) == 62500 ? 62500 : 125000;
            Config.loraTypes[i].power           = clampInt(num(p+"power",                  Config.loraTypes[i].power), 1, 22);
            Config.loraTypes[i].dataRate        = LoRa_Utils::calculateDataRate(
                Config.loraTypes[i].spreadingFactor,
                Config.loraTypes[i].codingRate4,
                Config.loraTypes[i].signalBandwidth
            );
        }
    }

    Config.battery.sendVoltage       = has("battery.sendVoltage");
    Config.battery.voltageAsTelemetry= has("battery.voltageAsTelemetry");
    Config.battery.sendVoltageAlways = has("battery.sendVoltageAlways");
    Config.battery.monitorVoltage    = has("battery.monitorVoltage");
    Config.battery.sleepVoltage      = (float)dbl("battery.sleepVoltage", Config.battery.sleepVoltage);

    Config.telemetry.active          = has("telemetry.active");
    Config.telemetry.sendTelemetry   = has("telemetry.sendTelemetry");
    Config.telemetry.temperatureCorrection = (float)dbl("telemetry.temperatureCorrection", Config.telemetry.temperatureCorrection);

    Config.winlink.password          = str("winlink.password",         Config.winlink.password.c_str());

    Config.ptt.active                = has("ptt.active");
    Config.ptt.io_pin                = num("ptt.io_pin",               Config.ptt.io_pin);
    Config.ptt.preDelay              = num("ptt.preDelay",             Config.ptt.preDelay);
    Config.ptt.postDelay             = num("ptt.postDelay",            Config.ptt.postDelay);
    Config.ptt.reverse               = has("ptt.reverse");

    Config.notification.buzzerActive   = has("notification.buzzerActive");
    Config.notification.bootUpBeep     = has("notification.bootUpBeep");
    Config.notification.txBeep         = has("notification.txBeep");
    Config.notification.messageRxBeep  = has("notification.messageRxBeep");
    Config.notification.stationBeep    = has("notification.stationBeep");
    Config.notification.shutDownBeep   = has("notification.shutDownBeep");
    Config.notification.lowBatteryBeep = has("notification.lowBatteryBeep");
    Config.notification.volume         = num("notification.volume",    Config.notification.volume);

    Config.writeFile();
    pthread_mutex_unlock(&_cfg_mtx);

    // Trigger config reload in main loop via SIGHUP handler
    kill(getpid(), SIGHUP);
}

// ── MHD request handler ────────────────────────────────────────────────────────

static MHD_Result sendString(struct MHD_Connection *conn, int code,
                              const char *mime, const std::string& body,
                              bool gzip = false)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(
        body.size(), (void*)body.data(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(r, "Content-Type", mime);
    if (gzip) MHD_add_response_header(r, "Content-Encoding", "gzip");
    MHD_Result ret = MHD_queue_response(conn, code, r);
    MHD_destroy_response(r);
    return ret;
}

static MHD_Result handleRequest(void*, struct MHD_Connection *conn,
                                 const char *url, const char *method,
                                 const char*, const char *upload_data,
                                 size_t *upload_data_size, void **con_cls)
{
    if (strcmp(method, "GET") == 0 && strcmp(url, "/status") == 0)
        return sendString(conn, 200, "text/plain", "OK");

    if (strcmp(method, "GET") == 0 && strcmp(url, "/configuration.json") == 0) {
        std::string json = readJsonConfig();
        if (json.empty()) json = "{}";
        return sendString(conn, 200, "application/json", json);
    }

    if (strcmp(method, "POST") == 0 && strcmp(url, "/configuration.json") == 0) {
        if (*con_cls == nullptr) {
            ConnState *st = new ConnState();
            st->pp = MHD_create_post_processor(conn, 65536, iteratePost, st);
            *con_cls = st;
            return MHD_YES;
        }
        ConnState *st = static_cast<ConnState*>(*con_cls);
        if (*upload_data_size > 0) {
            MHD_post_process(st->pp, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }
        applyAndSave(st->fields);
        return sendString(conn, 200, "application/json", "{\"status\":\"ok\"}");
    }

    if (strcmp(method, "POST") == 0 && strcmp(url, "/action") == 0) {
        const char *type = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "type");
        if (type && strcmp(type, "reboot") == 0) {
            int rc = std::system("sudo -n /usr/bin/systemctl reboot");
            return sendString(conn, rc == 0 ? 200 : 500, "text/plain", rc == 0 ? "Rebooting" : "Reboot failed");
        }
        return sendString(conn, 404, "text/plain", "Not found");
    }

    struct { const char *url; const char *file; const char *mime; } assets[] = {
        { "/",              "index.html",   "text/html"              },
        { "/index.html",    "index.html",   "text/html"              },
        { "/style.css",     "style.css",    "text/css"               },
        { "/script.js",     "script.js",    "application/javascript" },
        { "/bootstrap.css", "bootstrap.css","text/css"               },
        { "/bootstrap.js",  "bootstrap.js", "application/javascript" },
        { "/favicon.png",   "favicon.png",  "image/png"              },
        { "/github-sponsors.png", "github-sponsors.png", "image/png" },
        { "/paypalme.png",  "paypalme.png", "image/png"              },
        { nullptr, nullptr, nullptr }
    };

    for (int i = 0; assets[i].url; i++) {
        if (strcmp(url, assets[i].url) == 0) {
            std::string path = _assets_dir + "/" + assets[i].file + ".gz";
            std::string body = readFile(path);
            if (body.empty()) return sendString(conn, 404, "text/plain", "Not found");
            return sendString(conn, 200, assets[i].mime, body, true);
        }
    }

    return sendString(conn, 404, "text/plain", "Not found");
}

static void cleanupConn(void*, struct MHD_Connection*, void **con_cls,
                         enum MHD_RequestTerminationCode)
{
    ConnState *st = static_cast<ConnState*>(*con_cls);
    if (st) {
        if (st->pp) MHD_destroy_post_processor(st->pp);
        delete st;
        *con_cls = nullptr;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

namespace WEBCONF {

void start(int port) {
    if (_daemon) return;

    // Look for data_embed next to the binary
    char exepath[512] = {};
    ssize_t n = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
    if (n > 0) {
        char *slash = strrchr(exepath, '/');
        if (slash) *slash = '\0';
        _assets_dir = std::string(exepath) + "/data_embed";
    }
    // Fallback: TRACKER_DATA/data_embed, then source tree
    if (_assets_dir.empty() || readFile(_assets_dir + "/index.html.gz").empty()) {
        const char *td = getenv("TRACKER_DATA");
        if (td) _assets_dir = std::string(td) + "/data_embed";
    }

    struct sockaddr_in sa = {};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    _daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        port, nullptr, nullptr,
        handleRequest, nullptr,
        MHD_OPTION_SOCK_ADDR,        (struct sockaddr*)&sa,
        MHD_OPTION_NOTIFY_COMPLETED, cleanupConn, nullptr,
        MHD_OPTION_END);

    fprintf(stderr, "[WEBCONF] port=%d assets=%s daemon=%p\n",
            port, _assets_dir.c_str(), (void*)_daemon);
}

void stop() {
    if (_daemon) {
        MHD_stop_daemon(_daemon);
        _daemon = nullptr;
    }
}

bool isRunning() { return _daemon != nullptr; }

} // namespace WEBCONF
