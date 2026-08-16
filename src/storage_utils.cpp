#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <algorithm>
#include <deque>
#include <fstream>
#include <nlohmann/json.hpp>
#include "storage_utils.h"

static const char* TAG = "Storage";

static std::string _rootDir;
static std::string _messagesDir;
static std::string _contactsDir, _contactsFile;
static std::string _statsFile, _framesFile;

static const char* ROOT_DIR      = nullptr;
static const char* MESSAGES_DIR  = nullptr;
static const char* CONTACTS_DIR  = nullptr;
static const char* CONTACTS_FILE = nullptr;
static const char* STATS_FILE    = nullptr;
static const char* FRAMES_FILE   = nullptr;

static void initPaths() {
    const char* env = getenv("TRACKER_DATA");
    _rootDir      = env ? env : "/data/LoRa_Tracker";
    _messagesDir  = _rootDir + "/Messages";
    _contactsDir  = _rootDir + "/Contacts";
    _contactsFile = _rootDir + "/Contacts/contacts.json";
    _statsFile    = _rootDir + "/stats.json";
    _framesFile   = _rootDir + "/frames.log";
    ROOT_DIR      = _rootDir.c_str();
    MESSAGES_DIR  = _messagesDir.c_str();
    CONTACTS_DIR  = _contactsDir.c_str();
    CONTACTS_FILE = _contactsFile.c_str();
    STATS_FILE    = _statsFile.c_str();
    FRAMES_FILE   = _framesFile.c_str();
}

static LinkStats             _stats = {};
// Rolling windows: unix timestamps of RX/TX events in the last 24 h.
// Used to compute "RX in last hour" and "RX in last 24 h" without
// scanning the whole frames history. Reset on process restart.
static std::deque<uint32_t>  _rxTimes;
static std::deque<uint32_t>  _txTimes;
static std::vector<DigiStats>   _digiStats;
static std::deque<uint32_t>     _seenFrameHashes;
static std::vector<StationStats> _stationStats;
static std::vector<DashboardRxEntry> _dashRx;
static std::vector<int>   _rssiHistory;
static std::vector<float> _snrHistory;
static std::vector<String> _lastFrames;
static std::vector<Contact> _contacts;
static bool _contactsLoaded = false;
static bool _framesDirty = false;
static bool _statsDirty  = false;
static bool _messagesDirty = false;
static uint32_t _lastStatsSave = 0;

using json = nlohmann::json;

static void mkdirP(const char* path) { mkdir(path, 0755); }

static bool isGenericDigiAlias(const String &call) {
    static const char *prefixes[] = {"WIDE", "TRACE", "RELAY", "GATE", "ECHO"};
    for (const char *p : prefixes) {
        size_t n = strlen(p);
        if (call.length() < n) continue;
        bool match = true;
        for (size_t i = 0; i < n; i++) {
            if (toupper((unsigned char)call[i]) != p[i]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static uint32_t fnv1a32(const String &s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static bool rememberFrameKey(const String &frameKey) {
    if (frameKey.isEmpty()) return false;
    uint32_t h = fnv1a32(frameKey);
    for (uint32_t seen : _seenFrameHashes) {
        if (seen == h) return true;
    }
    _seenFrameHashes.push_back(h);
    while (_seenFrameHashes.size() > 512) _seenFrameHashes.pop_front();
    return false;
}

namespace STORAGE_Utils {

    void setup() {
        initPaths();
        mkdirP(ROOT_DIR);
        mkdirP(MESSAGES_DIR);
        mkdirP(CONTACTS_DIR);
        char p[256];
        snprintf(p, sizeof(p), "%s/conversations", MESSAGES_DIR);
        mkdirP(p);
        ESP_LOGI(TAG, "Storage: %s", ROOT_DIR);
    }

    bool isSDAvailable() { return true; }

    String getMessagesPath() { return String(MESSAGES_DIR); }

    static std::string resolvePath(const String& path) {
        std::string p = path.c_str();
        if (p.empty()) return std::string(ROOT_DIR);
        if (p[0] != '/') return std::string(ROOT_DIR) + "/" + p;
        if (p.rfind(ROOT_DIR, 0) == 0) return p; // already under root
        if (p.rfind("/data/", 0) == 0) return p;  // SPIFFS-style
        if (p.rfind("/Messages", 0) == 0 || p.rfind("/Contacts", 0) == 0)
            return std::string(ROOT_DIR) + p;
        return std::string(MESSAGES_DIR) + p;
    }
    bool fileExists(const String& path) {
        std::string full = resolvePath(path);
        struct stat st;
        return stat(full.c_str(), &st) == 0;
    }

    File openFile(const String& path, const char* mode) {
        return File(resolvePath(path).c_str(), mode);
    }

    bool removeFile(const String& path) {
        std::string full = resolvePath(path);
        return unlink(full.c_str()) == 0;
    }

    bool mkdir(const String& path) {
        std::string full = resolvePath(path);
        return ::mkdir(full.c_str(), 0755) == 0 || errno == EEXIST;
    }

    std::vector<String> listFiles(const String& dirPath) {
        std::vector<String> result;
        DIR* d = opendir(dirPath.c_str());
        if (!d) return result;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string fp = dirPath.c_str() + std::string("/") + ent->d_name;
            struct stat st; stat(fp.c_str(), &st);
            if (S_ISREG(st.st_mode)) result.push_back(String(fp.c_str()));
        }
        closedir(d);
        return result;
    }

    std::vector<String> listDirs(const String& dirPath) {
        std::vector<String> result;
        DIR* d = opendir(dirPath.c_str());
        if (!d) return result;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string fp = dirPath.c_str() + std::string("/") + ent->d_name;
            struct stat st; stat(fp.c_str(), &st);
            if (S_ISDIR(st.st_mode)) result.push_back(String(fp.c_str()));
        }
        closedir(d);
        return result;
    }

    String   getStorageType()  { return String("Linux"); }
    uint64_t getUsedBytes()    { return 0; }
    uint64_t getTotalBytes()   { return 0; }

    // ─── Contacts ────────────────────────────────────────────────────────────
    std::vector<Contact> loadContacts() {
        if (_contactsLoaded) return _contacts;
        _contacts.clear();
        std::ifstream f(CONTACTS_FILE);
        if (!f.is_open()) { _contactsLoaded = true; return _contacts; }
        json j;
        try { f >> j; } catch (...) { _contactsLoaded = true; return _contacts; }
        for (const auto& c : j) {
            Contact ct;
            ct.callsign = c.value("callsign", "");
            ct.name     = c.value("name",     "");
            ct.comment  = c.value("comment",  "");
            _contacts.push_back(ct);
        }
        _contactsLoaded = true;
        return _contacts;
    }

    bool saveContacts(const std::vector<Contact>& contacts) {
        _contacts = contacts;
        json j = json::array();
        for (const auto& c : contacts) {
            j.push_back({{"callsign", c.callsign.s}, {"name", c.name.s}, {"comment", c.comment.s}});
        }
        std::ofstream f(CONTACTS_FILE);
        if (!f.is_open()) return false;
        f << j.dump(2);
        return true;
    }

    bool addContact(const Contact& c) {
        loadContacts();
        _contacts.push_back(c);
        return saveContacts(_contacts);
    }

    bool removeContact(const String& callsign) {
        loadContacts();
        auto it = std::remove_if(_contacts.begin(), _contacts.end(),
            [&](const Contact& c){ return c.callsign == callsign; });
        if (it == _contacts.end()) return false;
        _contacts.erase(it, _contacts.end());
        return saveContacts(_contacts);
    }

    bool updateContact(const String& callsign, const Contact& newData) {
        loadContacts();
        for (auto& c : _contacts) if (c.callsign == callsign) { c = newData; return saveContacts(_contacts); }
        return false;
    }

    Contact* findContact(const String& callsign) {
        loadContacts();
        for (auto& c : _contacts) if (c.callsign == callsign) return &c;
        return nullptr;
    }

    int getContactCount() { loadContacts(); return (int)_contacts.size(); }

    // ─── Frame logging ───────────────────────────────────────────────────────
    bool logRawFrame(const String& frame, int rssi, float snr, bool isDirect) {
        time_t now = time(nullptr);
        struct tm *tm_info = localtime(&now);

        // Timestamp formaté une fois pour toutes à la réception
        char ts[32];
        snprintf(ts, sizeof(ts), "%02d-%02d %02d:%02d",
                tm_info->tm_mon + 1, tm_info->tm_mday,
                tm_info->tm_hour, tm_info->tm_min);

        FILE* f = fopen(FRAMES_FILE, "a");
        if (!f) return false;
        int snr_int = (int)snr;
        int snr_dec = abs((int)((snr - snr_int) * 10));
        fprintf(f, "%s RSSI:%d SNR:%d.%d %s %s\n",
            ts, rssi, snr_int, snr_dec, isDirect ? "D" : "R", frame.c_str());
        fclose(f);

        // RAM cache
        String entry = String(ts) + " RSSI:" + String(rssi) + " SNR:" + String(snr_int) + "." + String(snr_dec) + " " + frame;
        _lastFrames.push_back(entry);
        if ((int)_lastFrames.size() > HISTORY_SIZE) _lastFrames.erase(_lastFrames.begin());
        _framesDirty = true;
        return true;
    }
    
    void updateStationStats(const String& callsign, int rssi, float snr, bool isDirect,
                            const String& path, const String& dest,
                            char symbolTable, char symbol, char payloadType,
                            bool hasPosition, double lat, double lon) {
        for (auto& s : _stationStats) {
            if (s.callsign == callsign) {
                s.count++;
                if (isDirect) {
                    s.directCount++;
                    s.lastRssi = rssi; s.lastSnr = snr;
                    s.rssiTotal += rssi; s.snrTotal += snr;
                }
                s.lastHeard = (uint32_t)time(nullptr);
                s.lastIsDirect = isDirect;
                s.lastPath = path;
                s.lastDest = dest;
                s.lastSymbolTable = symbolTable;
                s.lastSymbol = symbol;
                s.lastPayloadType = payloadType;
                if (hasPosition) {
                    s.hasPosition = true;
                    s.lastLat = lat;
                    s.lastLon = lon;
                }
                _statsDirty = true;
                return;
            }
        }
        StationStats ss;
        ss.callsign = callsign; ss.count = 1;
        ss.directCount = isDirect ? 1 : 0;
        ss.lastRssi = isDirect ? rssi : 0;
        ss.lastSnr  = isDirect ? snr : 0.0f;
        ss.rssiTotal = isDirect ? rssi : 0;
        ss.snrTotal  = isDirect ? snr : 0.0f;
        ss.lastHeard = (uint32_t)time(nullptr);
        ss.lastIsDirect = isDirect;
        ss.lastPath = path;
        ss.lastDest = dest;
        ss.lastSymbolTable = symbolTable;
        ss.lastSymbol = symbol;
        ss.lastPayloadType = payloadType;
        ss.hasPosition = hasPosition;
        ss.lastLat = hasPosition ? lat : 0.0;
        ss.lastLon = hasPosition ? lon : 0.0;
        _stationStats.push_back(ss);
        _statsDirty = true;
    }

    const std::vector<String>& getLastFrames(int) { return _lastFrames; }
    void appendFrame(const String &f) {
        _lastFrames.push_back(f);
        if (_lastFrames.size() > 500) _lastFrames.erase(_lastFrames.begin());
        _framesDirty = true;
    }
    void checkFramesLogRotation() {}  // TODO: rotate if > 1 MB
    void loadFramesFromSD() {}

    // ─── Link stats ──────────────────────────────────────────────────────────
    void resetStats()    {
        _stats = {}; _digiStats.clear();
        _seenFrameHashes.clear();
        _rxTimes.clear(); _txTimes.clear();
        _statsDirty = true;
    }

    static void trimWindow(std::deque<uint32_t>& q, uint32_t now) {
        const uint32_t cutoff = now - 24*3600;
        while (!q.empty() && q.front() < cutoff) q.pop_front();
    }

    void updateRxStats(int rssi, float snr) {
        uint32_t now = (uint32_t)time(nullptr);
        if (_stats.since == 0) _stats.since = now;
        _rxTimes.push_back(now);
        trimWindow(_rxTimes, now);
        _stats.rxCount++;
        if (_stats.rxCount == 1) { _stats.rssiMin = _stats.rssiMax = rssi; _stats.snrMin = _stats.snrMax = snr; }
        else {
            if (rssi < _stats.rssiMin) _stats.rssiMin = rssi;
            if (rssi > _stats.rssiMax) _stats.rssiMax = rssi;
            if (snr  < _stats.snrMin)  _stats.snrMin  = snr;
            if (snr  > _stats.snrMax)  _stats.snrMax  = snr;
        }
        _stats.rssiTotal += rssi;
        _stats.snrTotal  += snr;
        _rssiHistory.push_back(rssi);
        _snrHistory.push_back(snr);
        if ((int)_rssiHistory.size() > HISTORY_SIZE) _rssiHistory.erase(_rssiHistory.begin());
        if ((int)_snrHistory.size()  > HISTORY_SIZE) _snrHistory.erase(_snrHistory.begin());
        _statsDirty = true;
    }

    void updateTxStats() {
        uint32_t now = (uint32_t)time(nullptr);
        if (_stats.since == 0) _stats.since = now;
        _txTimes.push_back(now);
        trimWindow(_txTimes, now);
        _stats.txCount++; _statsDirty = true;
    }
    void updateAckStats(){ _stats.ackCount++; _statsDirty = true; }

    void updateDigiStats(const String& path, const String& frameKey) {
        bool duplicate = rememberFrameKey(frameKey);
        int start = 0;
        int idx = 0;
        bool changed = false;
        while (true) {
            int comma = path.indexOf(',', start);
            String hop = (comma > 0) ? path.substring(start, comma) : path.substring(start);
            hop.trim();
            bool used = hop.indexOf('*') >= 0;
            if (used) hop.replace("*", "");
            if (idx >= 1 && used && !hop.isEmpty() && !isGenericDigiAlias(hop)) {
                bool found = false;
                uint32_t now = (uint32_t)time(nullptr);
                for (auto& d : _digiStats) {
                    if (d.callsign == hop) {
                        d.count++;
                        d.relayCount++;
                        if (duplicate) d.duplicateCount++;
                        else d.newCount++;
                        d.lastSeen = now;
                        found = true;
                        changed = true;
                        break;
                    }
                }
                if (!found) {
                    DigiStats ds;
                    ds.callsign = hop;
                    ds.count = 1;
                    ds.relayCount = 1;
                    ds.newCount = duplicate ? 0 : 1;
                    ds.duplicateCount = duplicate ? 1 : 0;
                    ds.lastSeen = now;
                    _digiStats.push_back(ds);
                    changed = true;
                }
            }
            if (comma < 0) break;
            start = comma + 1;
            idx++;
        }
        if (changed) _statsDirty = true;
    }

    LinkStats getStats()                                   { return _stats; }
    float getAvgRssi() { return _stats.rxCount ? (float)_stats.rssiTotal / _stats.rxCount : 0; }
    float getAvgSnr()  { return _stats.rxCount ? _stats.snrTotal / _stats.rxCount : 0; }

    static uint32_t countSince(const std::deque<uint32_t>& q, uint32_t cutoff) {
        uint32_t n = 0;
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            if (*it >= cutoff) n++;
            else break;
        }
        return n;
    }
    uint32_t getRxCountLastHour()  { uint32_t now=(uint32_t)time(nullptr); return countSince(_rxTimes, now-3600); }
    uint32_t getRxCountLast24h()   { uint32_t now=(uint32_t)time(nullptr); return countSince(_rxTimes, now-24*3600); }
    uint32_t getTxCountLastHour()  { uint32_t now=(uint32_t)time(nullptr); return countSince(_txTimes, now-3600); }
    uint32_t getTxCountLast24h()   { uint32_t now=(uint32_t)time(nullptr); return countSince(_txTimes, now-24*3600); }
    const std::vector<DigiStats>&    getDigiStats()       { return _digiStats; }
    const std::vector<StationStats>& getStationStats()    { return _stationStats; }
    const std::vector<DashboardRxEntry>& getDashboardLastRx() { return _dashRx; }
    void addRxEntry(const DashboardRxEntry &e) {
        _dashRx.insert(_dashRx.begin(), e);
        if (_dashRx.size() > 8) _dashRx.resize(8);
    }
    const std::vector<int>&   getRssiHistory()            { return _rssiHistory; }
    const std::vector<float>& getSnrHistory()             { return _snrHistory; }

    // ─── Stats persistence ───────────────────────────────────────────────────
    void loadStats() {
        std::ifstream f(STATS_FILE);
        if (!f.is_open()) return;
        json j;
        try { f >> j; } catch (...) { return; }
        if (j.contains("link")) {
            _stats.rxCount   = j["link"].value("rxCount",   0U);
            _stats.txCount   = j["link"].value("txCount",   0U);
            _stats.ackCount  = j["link"].value("ackCount",  0U);
            _stats.rssiMin   = j["link"].value("rssiMin",   0);
            _stats.rssiMax   = j["link"].value("rssiMax",   0);
            _stats.rssiTotal = j["link"].value("rssiTotal", 0);
            _stats.snrMin    = j["link"].value("snrMin",    0.0f);
            _stats.snrMax    = j["link"].value("snrMax",    0.0f);
            _stats.snrTotal  = j["link"].value("snrTotal",  0.0f);
            _stats.since     = j["link"].value("since",     0U);
        }
        if (j.contains("stations") && j["stations"].is_array()) {
            for (const auto& s : j["stations"]) {
                StationStats ss;
                ss.callsign    = s.value("callsign",   "");
                ss.count       = s.value("count",      0U);
                ss.lastRssi    = s.value("lastRssi",   0);
                ss.lastSnr     = s.value("lastSnr",    0.0f);
                ss.rssiTotal   = s.value("rssiTotal",  0);
                ss.snrTotal    = s.value("snrTotal",   0.0f);
                ss.directCount = s.value("directCount", 0U);
                // Avant l'ajout de directCount, les cumuls mélangeaient direct et
                // relayé : les moyennes décrivaient autant le digi que la station.
                // On les repart de zéro plutôt que de traîner ce mélange.
                if (!s.contains("directCount")) {
                    ss.rssiTotal = 0;
                    ss.snrTotal  = 0.0f;
                    ss.lastRssi  = 0;
                    ss.lastSnr   = 0.0f;
                }
                ss.lastHeard   = s.value("lastHeard",  0U);
                ss.lastIsDirect= s.value("lastIsDirect",false);
                ss.lastPath    = s.value("lastPath",   "");
                ss.lastDest    = s.value("lastDest",   "");
                std::string symTable = s.value("lastSymbolTable", "");
                std::string sym      = s.value("lastSymbol",      "");
                std::string payType  = s.value("lastPayloadType", "");
                ss.lastSymbolTable = symTable.empty() ? 0 : symTable[0];
                ss.lastSymbol      = sym.empty()      ? 0 : sym[0];
                ss.lastPayloadType = payType.empty()  ? 0 : payType[0];
                ss.hasPosition = s.value("hasPosition", false);
                ss.lastLat     = s.value("lastLat",      0.0);
                ss.lastLon     = s.value("lastLon",      0.0);
                _stationStats.push_back(ss);
            }
        }
        if (j.contains("digis") && j["digis"].is_array()) {
            for (const auto& d : j["digis"]) {
                DigiStats ds;
                ds.callsign       = d.value("callsign",       "");
                ds.count          = d.value("count",          0U);
                ds.relayCount     = d.value("relayCount",     ds.count);
                ds.newCount       = d.value("newCount",       0U);
                ds.duplicateCount = d.value("duplicateCount", 0U);
                ds.lastSeen       = d.value("lastSeen",       0U);
                if (!ds.callsign.isEmpty()) _digiStats.push_back(ds);
            }
        }
        if (_stats.since > 0) {
            time_t t = (time_t)_stats.since;
            struct tm tm; localtime_r(&t, &tm);
            char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            ESP_LOGI(TAG, "Stats loaded: %u RX, %u TX, %zu stations (since %s)",
                     _stats.rxCount, _stats.txCount, _stationStats.size(), buf);
        } else {
            ESP_LOGI(TAG, "Stats loaded: %u RX, %u TX, %zu stations",
                     _stats.rxCount, _stats.txCount, _stationStats.size());
        }
    }

    bool saveStats() {
        json j;
        j["link"]["rxCount"]   = _stats.rxCount;
        j["link"]["txCount"]   = _stats.txCount;
        j["link"]["ackCount"]  = _stats.ackCount;
        j["link"]["rssiMin"]   = _stats.rssiMin;
        j["link"]["rssiMax"]   = _stats.rssiMax;
        j["link"]["rssiTotal"] = _stats.rssiTotal;
        j["link"]["snrMin"]    = _stats.snrMin;
        j["link"]["snrMax"]    = _stats.snrMax;
        j["link"]["snrTotal"]  = _stats.snrTotal;
        j["link"]["since"]     = _stats.since;
        j["digis"] = json::array();
        int maxDigis = std::min((int)_digiStats.size(), 20);
        for (int i = 0; i < maxDigis; i++) {
            const auto& d = _digiStats[i];
            j["digis"].push_back({
                {"callsign",       d.callsign.s},
                {"count",          d.count},
                {"relayCount",     d.relayCount},
                {"newCount",       d.newCount},
                {"duplicateCount", d.duplicateCount},
                {"lastSeen",       d.lastSeen}
            });
        }
        j["stations"] = json::array();
        // The file keeps 20 stations: pick the RF neighbourhood first, most recently
        // heard inside each group, instead of whichever were discovered first.
        std::vector<size_t> order(_stationStats.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
            bool aDirect = _stationStats[a].directCount > 0;
            bool bDirect = _stationStats[b].directCount > 0;
            if (aDirect != bDirect) return aDirect;
            return _stationStats[a].lastHeard > _stationStats[b].lastHeard;
        });
        int max = std::min((int)order.size(), 20);
        for (int i = 0; i < max; i++) {
            const auto& s = _stationStats[order[i]];
            j["stations"].push_back({
                {"callsign",    s.callsign.s},
                {"count",       s.count},
                {"directCount", s.directCount},
                {"lastRssi",    s.lastRssi},
                {"lastSnr",     s.lastSnr},
                {"rssiTotal",   s.rssiTotal},
                {"snrTotal",    s.snrTotal},
                {"lastHeard",   s.lastHeard},
                {"lastIsDirect",s.lastIsDirect},
                {"lastPath",    s.lastPath.s},
                {"lastDest",    s.lastDest.s},
                {"lastSymbolTable", std::string(s.lastSymbolTable ? 1 : 0, s.lastSymbolTable)},
                {"lastSymbol",      std::string(s.lastSymbol      ? 1 : 0, s.lastSymbol)},
                {"lastPayloadType", std::string(s.lastPayloadType ? 1 : 0, s.lastPayloadType)},
                {"hasPosition", s.hasPosition},
                {"lastLat",     s.lastLat},
                {"lastLon",     s.lastLon}
            });
        }
        std::ofstream f(STATS_FILE);
        if (!f.is_open()) return false;
        f << j.dump(2);
        _statsDirty = false;
        return true;
    }

    void checkStatsSave() {
        if (!_statsDirty) return;
        uint32_t now = millis();
        if (now - _lastStatsSave >= 5 * 60 * 1000) {
            saveStats();
            _lastStatsSave = now;
        }
    }

    bool isFramesDirty()   { return _framesDirty; }
    void clearFramesDirty(){ _framesDirty = false; }
    bool isStatsDirty()    { return _statsDirty; }
    void clearStatsDirty() { _statsDirty = false; }
    bool isMessagesDirty()   { return _messagesDirty; }
    void clearMessagesDirty(){ _messagesDirty = false; }
    void markMessagesDirty() { _messagesDirty = true; }
}
