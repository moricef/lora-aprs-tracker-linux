#pragma once
#include <Arduino.h>
#include <FS.h>
#include <vector>

struct Contact {
    String callsign;
    String name;
    String comment;
};

struct LinkStats {
    uint32_t rxCount;
    uint32_t txCount;
    uint32_t ackCount;
    int      rssiMin;
    int      rssiMax;
    int32_t  rssiTotal;
    float    snrMin;
    float    snrMax;
    float    snrTotal;
};

struct DigiStats {
    String   callsign;
    uint32_t count;
};

struct StationStats {
    String   callsign;
    uint32_t count;
    int      lastRssi;
    float    lastSnr;
    int32_t  rssiTotal;
    float    snrTotal;
    uint32_t lastHeard;
    bool     lastIsDirect;
};

struct DashboardRxEntry {
    String   callsign;
    int      rssi;
    float    snr;
    uint32_t timestamp;
};

namespace STORAGE_Utils {
    void   setup();
    bool   isSDAvailable();

    String getRootPath();
    String getMessagesPath();
    String getInboxPath();
    String getOutboxPath();
    String getContactsPath();
    String getMapsPath();

    bool   fileExists(const String& path);
    File   openFile(const String& path, const char* mode);
    bool   removeFile(const String& path);
    bool   mkdir(const String& path);

    std::vector<String> listFiles(const String& dirPath);
    std::vector<String> listDirs(const String& dirPath);

    String   getStorageType();
    uint64_t getUsedBytes();
    uint64_t getTotalBytes();

    std::vector<Contact> loadContacts();
    bool saveContacts(const std::vector<Contact>& contacts);
    bool addContact(const Contact& contact);
    bool removeContact(const String& callsign);
    bool updateContact(const String& callsign, const Contact& newData);
    Contact* findContact(const String& callsign);
    int  getContactCount();

    bool logRawFrame(const String& frame, int rssi, float snr, bool isDirect);
    void updateStationStats(const String& callsign, int rssi, float snr, bool isDirect);
    const std::vector<String>& getLastFrames(int count);
    void checkFramesLogRotation();
    void loadFramesFromSD();

    void resetStats();
    void updateRxStats(int rssi, float snr);
    void updateTxStats();
    void updateAckStats();
    void updateDigiStats(const String& path);
    LinkStats getStats();
    float getAvgRssi();
    float getAvgSnr();
    const std::vector<DigiStats>&   getDigiStats();
    const std::vector<StationStats>& getStationStats();

    const std::vector<DashboardRxEntry>& getDashboardLastRx();

    const int HISTORY_SIZE = 50;
    const std::vector<int>&   getRssiHistory();
    const std::vector<float>& getSnrHistory();

    void loadStats();
    bool saveStats();
    void checkStatsSave();

    bool isFramesDirty();
    void clearFramesDirty();
    bool isStatsDirty();
    void clearStatsDirty();
}
