#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include "storage_utils.h"

static const char* TAG = "Storage";

static std::string _rootDir;
static std::string _messagesDir, _inboxDir, _outboxDir;
static std::string _contactsDir, _contactsFile, _mapsDir;
static std::string _statsFile, _framesFile;

static const char* ROOT_DIR      = nullptr;
static const char* MESSAGES_DIR  = nullptr;
static const char* INBOX_DIR     = nullptr;
static const char* OUTBOX_DIR    = nullptr;
static const char* CONTACTS_DIR  = nullptr;
static const char* CONTACTS_FILE = nullptr;
static const char* MAPS_DIR      = nullptr;
static const char* STATS_FILE    = nullptr;
static const char* FRAMES_FILE   = nullptr;

static void initPaths() {
    const char* env = getenv("TRACKER_DATA");
    _rootDir      = env ? env : "/data/LoRa_Tracker";
    _messagesDir  = _rootDir + "/Messages";
    _inboxDir     = _rootDir + "/Messages/inbox";
    _outboxDir    = _rootDir + "/Messages/outbox";
    _contactsDir  = _rootDir + "/Contacts";
    _contactsFile = _rootDir + "/Contacts/contacts.json";
    _mapsDir      = _rootDir + "/Maps";
    _statsFile    = _rootDir + "/stats.json";
    _framesFile   = _rootDir + "/frames.log";
    ROOT_DIR      = _rootDir.c_str();
    MESSAGES_DIR  = _messagesDir.c_str();
    INBOX_DIR     = _inboxDir.c_str();
    OUTBOX_DIR    = _outboxDir.c_str();
    CONTACTS_DIR  = _contactsDir.c_str();
    CONTACTS_FILE = _contactsFile.c_str();
    MAPS_DIR      = _mapsDir.c_str();
    STATS_FILE    = _statsFile.c_str();
    FRAMES_FILE   = _framesFile.c_str();
}

static LinkStats             _stats = {};
static std::vector<DigiStats>   _digiStats;
static std::vector<StationStats> _stationStats;
static std::vector<DashboardRxEntry> _dashRx;
static std::vector<int>   _rssiHistory;
static std::vector<float> _snrHistory;
static std::vector<String> _lastFrames;
static std::vector<Contact> _contacts;
static bool _contactsLoaded = false;
static bool _framesDirty = false;
static bool _statsDirty  = false;
static uint32_t _lastStatsSave = 0;

using json = nlohmann::json;

static void mkdirP(const char* path) { mkdir(path, 0755); }

namespace STORAGE_Utils {

    void setup() {
        initPaths();
        mkdirP(ROOT_DIR);
        mkdirP(MESSAGES_DIR);
        mkdirP(INBOX_DIR);
        mkdirP(OUTBOX_DIR);
        mkdirP(CONTACTS_DIR);
        mkdirP(MAPS_DIR);
        // conversations dir
        char p[256];
        snprintf(p, sizeof(p), "%s/conversations", ROOT_DIR);
        mkdirP(p);
        ESP_LOGI(TAG, "Storage: %s", ROOT_DIR);
    }

    bool isSDAvailable() { return true; }

    String getRootPath()     { return String(ROOT_DIR); }
    String getMessagesPath() { return String(MESSAGES_DIR); }
    String getInboxPath()    { return String(INBOX_DIR); }
    String getOutboxPath()   { return String(OUTBOX_DIR); }
    String getContactsPath() { return String(CONTACTS_DIR); }
    String getMapsPath()     { return String(MAPS_DIR); }

    static std::string resolvePath(const String& path) {
        std::string p = path.c_str();
        if (p[0] != '/') return std::string(ROOT_DIR) + "/" + p;
        if (p.rfind(ROOT_DIR, 0) == 0) return p; // already under root
        if (p.rfind("/data/", 0) == 0) return p;  // SPIFFS-style
        return std::string(ROOT_DIR) + p;
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
        std::string full;
        if (path.c_str()[0] != '/' || strncmp(path.c_str(), "/data/", 6) != 0)
            full = std::string(ROOT_DIR) + path.c_str();
        else full = path.c_str();
        return unlink(full.c_str()) == 0;
    }

    bool mkdir(const String& path) {
        std::string full;
        if (path.c_str()[0] != '/' || strncmp(path.c_str(), "/data/", 6) != 0)
            full = std::string(ROOT_DIR) + path.c_str();
        else full = path.c_str();
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
        char buf[16]; time_t now = time(nullptr);
        FILE* f = fopen(FRAMES_FILE, "a");
        if (!f) return false;
        fprintf(f, "%ld RSSI:%d SNR:%.1f %s %s\n",
                (long)now, rssi, snr, isDirect ? "D" : "R", frame.c_str());
        fclose(f);
        // RAM cache (last 50)
        String entry = String((long)now) + " RSSI:" + String(rssi) + " SNR:" + String(snr,1) + " " + frame;
        _lastFrames.push_back(entry);
        if ((int)_lastFrames.size() > HISTORY_SIZE) _lastFrames.erase(_lastFrames.begin());
        _framesDirty = true;
        return true;
    }

    void updateStationStats(const String& callsign, int rssi, float snr, bool isDirect) {
        for (auto& s : _stationStats) {
            if (s.callsign == callsign) {
                s.count++;
                s.lastRssi = rssi; s.lastSnr = snr;
                s.rssiTotal += rssi; s.snrTotal += snr;
                s.lastHeard = (uint32_t)time(nullptr);
                s.lastIsDirect = isDirect;
                _statsDirty = true;
                return;
            }
        }
        StationStats ss;
        ss.callsign = callsign; ss.count = 1;
        ss.lastRssi = rssi; ss.lastSnr = snr;
        ss.rssiTotal = rssi; ss.snrTotal = snr;
        ss.lastHeard = (uint32_t)time(nullptr);
        ss.lastIsDirect = isDirect;
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
    void resetStats()    { _stats = {}; _digiStats.clear(); _statsDirty = true; }

    void updateRxStats(int rssi, float snr) {
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

    void updateTxStats() { _stats.txCount++; _statsDirty = true; }
    void updateAckStats(){ _stats.ackCount++; _statsDirty = true; }

    void updateDigiStats(const String& path) {
        int start = 0;
        while (true) {
            int comma = path.indexOf(',', start);
            String hop = (comma > 0) ? path.substring(start, comma) : path.substring(start);
            hop.trim();
            if (!hop.isEmpty()) {
                bool found = false;
                for (auto& d : _digiStats) if (d.callsign == hop) { d.count++; found = true; break; }
                if (!found) _digiStats.push_back({hop, 1});
            }
            if (comma < 0) break;
            start = comma + 1;
        }
        _statsDirty = true;
    }

    LinkStats getStats()                                   { return _stats; }
    float getAvgRssi() { return _stats.rxCount ? (float)_stats.rssiTotal / _stats.rxCount : 0; }
    float getAvgSnr()  { return _stats.rxCount ? _stats.snrTotal / _stats.rxCount : 0; }
    const std::vector<DigiStats>&    getDigiStats()       { return _digiStats; }
    const std::vector<StationStats>& getStationStats()    { return _stationStats; }
    const std::vector<DashboardRxEntry>& getDashboardLastRx() { return _dashRx; }
    void addRxEntry(const DashboardRxEntry &e) {
        _dashRx.insert(_dashRx.begin(), e);
        if (_dashRx.size() > 4) _dashRx.resize(4);
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
                ss.lastHeard   = s.value("lastHeard",  0U);
                ss.lastIsDirect= s.value("lastIsDirect",false);
                _stationStats.push_back(ss);
            }
        }
        ESP_LOGI(TAG, "Stats loaded: %u RX, %u TX, %zu stations",
                 _stats.rxCount, _stats.txCount, _stationStats.size());
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
        j["stations"] = json::array();
        int max = std::min((int)_stationStats.size(), 20);
        for (int i = 0; i < max; i++) {
            const auto& s = _stationStats[i];
            j["stations"].push_back({
                {"callsign",    s.callsign.s},
                {"count",       s.count},
                {"lastRssi",    s.lastRssi},
                {"lastSnr",     s.lastSnr},
                {"rssiTotal",   s.rssiTotal},
                {"snrTotal",    s.snrTotal},
                {"lastHeard",   s.lastHeard},
                {"lastIsDirect",s.lastIsDirect}
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
}
