#include "esp_log.h"
#include "notification_utils.h"
#include <APRSPacketLib.h>
#include <algorithm>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <ctime>
#include "storage_utils.h"
#include "configuration.h"
#include "lora_utils.h"
#include "msg_utils.h"
#include "station_utils.h"
#include "gps_utils.h"
#include "linux_stubs.h"

extern Beacon*        currentBeacon;
extern Configuration  Config;
extern bool           digipeaterActive;
extern int            ackRequestNumber;
extern uint32_t       lastTxTime;
extern APRSPacket     lastReceivedPacket;

static const char* TAG = "MSG";

static std::string lastMessageSaved;
static int  numAPRSMessages = 0;
static int  numWLNKMessages = 0;
static bool noAPRSMsgWarning = false;
static bool noWLNKMsgWarning = false;
static String lastHeardTracker = "NONE";

struct RecentMessage { String sender; String content; uint32_t timestamp; };
static std::vector<RecentMessage>   recentMessagesBuffer;
static const uint32_t MSG_DEDUP_WINDOW_MS = 30000;

static std::vector<String> loadedAPRSMessages;
static std::vector<String> loadedWLNKMails;
static std::vector<String> outputMessagesBuffer;
static std::vector<String> outputAckRequestBuffer;

static std::vector<Packet15SegBuffer> packet15SegBuffer;

int      ackRequestNumber = random(1, 999);
bool     ackRequestState  = false;
String   ackCallsignRequest;
String   ackNumberRequest;
uint32_t lastMsgRxTime    = 0;
uint32_t lastRetryTime    = 0;
bool     messageLed       = false;
uint32_t messageLedTime   = 0;

namespace MSG_Utils {

    static void cleanRecentMessagesBuffer() {
        uint32_t now = millis();
        while (!recentMessagesBuffer.empty() && (now - recentMessagesBuffer[0].timestamp) > MSG_DEDUP_WINDOW_MS)
            recentMessagesBuffer.erase(recentMessagesBuffer.begin());
    }

    static bool isDuplicateMessage(const String& sender, const String& content) {
        cleanRecentMessagesBuffer();
        for (const auto& m : recentMessagesBuffer)
            if (m.sender == sender && m.content == content) return true;
        recentMessagesBuffer.push_back({sender, content, millis()});
        return false;
    }

    bool warnNoAPRSMessages() { return noAPRSMsgWarning; }
    bool warnNoWLNKMails()    { return noWLNKMsgWarning; }
    const String getLastHeardTracker() { return lastHeardTracker; }
    int getNumAPRSMessages() { return numAPRSMessages; }
    int getNumWLNKMails()    { return numWLNKMessages; }
    std::vector<String>& getLoadedAPRSMessages() { return loadedAPRSMessages; }
    std::vector<String>& getLoadedWLNKMails()    { return loadedWLNKMails; }

    void saveToConversation(const String& callsign, const String& message, bool outgoing) {
        String convDir = STORAGE_Utils::getMessagesPath() + "/conversations";
        if (!STORAGE_Utils::fileExists(convDir)) STORAGE_Utils::mkdir(convDir);

        String filename = "/Messages/conversations/" + callsign + ".txt";
        if (STORAGE_Utils::fileExists(filename)) {
            File rf = STORAGE_Utils::openFile(filename, "r");
            if (rf) {
                String lastLine;
                while (rf.available()) { String l = rf.readStringUntil('\n'); l.trim(); if (!l.isEmpty()) lastLine = l; }
                rf.close();
                if (!lastLine.isEmpty()) {
                    int fc = lastLine.indexOf(','), sc = lastLine.indexOf(',', fc+1);
                    if (fc > 0 && sc > fc) {
                        String lastDir = lastLine.substring(fc+1, sc);
                        String lastMsg = lastLine.substring(sc+1);
                        String dir = outgoing ? "OUT" : "IN";
                        if (lastDir == dir && lastMsg == message) return;
                    }
                }
            }
        }

        uint32_t ts = (uint32_t)time(nullptr);
        String dir = outgoing ? "OUT" : "IN";
        String line = String(ts) + "," + dir + "," + message;
        File wf = STORAGE_Utils::openFile(filename, "a");
        if (wf) { wf.println(line); wf.close(); }
        ESP_LOGI(TAG, "Saved %s message to %s", dir.c_str(), filename.c_str());
    }

    std::vector<String> getConversationsList() {
        std::vector<String> callsigns;
        String convDir = STORAGE_Utils::getMessagesPath() + "/conversations";
        if (!STORAGE_Utils::fileExists(convDir)) return callsigns;
        File dir = File(convDir.c_str(), "r");
        if (!dir || !dir.isDirectory()) return callsigns;

        std::vector<std::pair<String, time_t>> convWithTime;
        File entry = dir.openNextFile();
        while (entry) {
            String fname = String(entry.name());
            int ls = fname.lastIndexOf('/');
            if (ls >= 0) fname = fname.substring(ls+1);
            if (fname.endsWith(".txt")) {
                String cs = fname.substring(0, fname.length()-4);
                convWithTime.push_back({cs, entry.getLastWrite()});
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
        std::sort(convWithTime.begin(), convWithTime.end(),
            [](const std::pair<String,time_t>& a, const std::pair<String,time_t>& b){ return a.second > b.second; });
        for (const auto& c : convWithTime) callsigns.push_back(c.first);
        return callsigns;
    }

    std::vector<String> getMessagesForContact(const String& callsign) {
        std::vector<String> result;
        String filename = "/Messages/conversations/" + callsign + ".txt";
        if (!STORAGE_Utils::fileExists(filename)) return result;
        File f = STORAGE_Utils::openFile(filename, "r");
        if (!f) return result;
        while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (!l.isEmpty()) result.push_back(l); }
        f.close();
        return result;
    }

    void loadNumMessages() {
        File fa = STORAGE_Utils::openFile("/aprsMessages.txt", "r");
        if (!fa) { numAPRSMessages = 0; }
        else {
            std::vector<String> v;
            while (fa.available()) v.push_back(fa.readStringUntil('\n'));
            fa.close();
            numAPRSMessages = (int)v.size();
        }
        File fw = STORAGE_Utils::openFile("/winlinkMails.txt", "r");
        if (!fw) { numWLNKMessages = 0; }
        else {
            std::vector<String> v;
            while (fw.available()) v.push_back(fw.readStringUntil('\n'));
            fw.close();
            numWLNKMessages = (int)v.size();
        }
        ESP_LOGI(TAG, "APRS:%d  Winlink:%d", numAPRSMessages, numWLNKMessages);
    }

    void loadMessagesFromMemory(uint8_t typeOfMessage) {
        if (typeOfMessage == 0) {
            noAPRSMsgWarning = (numAPRSMessages == 0);
            if (!noAPRSMsgWarning) {
                loadedAPRSMessages.clear();
                File f = STORAGE_Utils::openFile("/aprsMessages.txt", "r");
                if (f) { while (f.available()) loadedAPRSMessages.push_back(f.readStringUntil('\n')); f.close(); }
            }
        } else if (typeOfMessage == 1) {
            noWLNKMsgWarning = (numWLNKMessages == 0);
            if (!noWLNKMsgWarning) {
                loadedWLNKMails.clear();
                File f = STORAGE_Utils::openFile("/winlinkMails.txt", "r");
                if (f) { while (f.available()) loadedWLNKMails.push_back(f.readStringUntil('\n')); f.close(); }
            }
        }
    }

    void ledNotification() { /* no LED on Linux */ }

    void deleteFile(uint8_t typeOfFile) {
        if (typeOfFile == 0) {
            STORAGE_Utils::removeFile("/aprsMessages.txt");
            String convDir = STORAGE_Utils::getMessagesPath() + "/conversations";
            if (STORAGE_Utils::fileExists(convDir)) {
                File dir = File(convDir.c_str(), "r");
                if (dir && dir.isDirectory()) {
                    File f = dir.openNextFile();
                    while (f) {
                        String path = convDir + "/" + String(f.name()).substring(String(f.name()).lastIndexOf('/')+1);
                        f.close();
                        STORAGE_Utils::removeFile(path);
                        f = dir.openNextFile();
                    }
                    dir.close();
                }
            }
            numAPRSMessages = 0;
            loadedAPRSMessages.clear();
        } else if (typeOfFile == 1) {
            STORAGE_Utils::removeFile("/winlinkMails.txt");
            numWLNKMessages = 0;
            loadedWLNKMails.clear();
        }
    }

    bool deleteMessageByIndex(uint8_t typeOfMessage, int index) {
        loadMessagesFromMemory(typeOfMessage);
        std::vector<String>* msgs = (typeOfMessage == 0) ? &loadedAPRSMessages : &loadedWLNKMails;
        int* cnt = (typeOfMessage == 0) ? &numAPRSMessages : &numWLNKMessages;
        const char* fname = (typeOfMessage == 0) ? "/aprsMessages.txt" : "/winlinkMails.txt";
        if (index < 0 || index >= (int)msgs->size()) return false;
        msgs->erase(msgs->begin() + index);
        (*cnt)--;
        STORAGE_Utils::removeFile(fname);
        if (!msgs->empty()) {
            File f = STORAGE_Utils::openFile(fname, FILE_WRITE);
            if (f) { for (const String& m : *msgs) f.println(m); f.close(); }
        }
        return true;
    }

    bool deleteMessageFromConversation(const String& callsign, int index) {
        String filename = "/Messages/conversations/" + callsign + ".txt";
        if (!STORAGE_Utils::fileExists(filename)) return false;
        std::vector<String> msgs = getMessagesForContact(callsign);
        if (index < 0 || index >= (int)msgs.size()) return false;
        msgs.erase(msgs.begin() + index);
        STORAGE_Utils::removeFile(filename);
        if (!msgs.empty()) {
            File f = STORAGE_Utils::openFile(filename, FILE_WRITE);
            if (f) { for (const String& m : msgs) f.println(m); f.close(); }
        }
        return true;
    }

    void saveNewMessage(uint8_t typeMessage, const String& station, const String& newMessage) {
        String message = newMessage;
        message.trim();
        if (isDuplicateMessage(station, message)) return;

        if (typeMessage == 0) {
            File f = STORAGE_Utils::openFile("/aprsMessages.txt", FILE_APPEND);
            if (!f) { ESP_LOGE(TAG, "Cannot open aprsMessages.txt"); return; }
            f.println(station + "," + message);
            numAPRSMessages++;
            f.close();
            saveToConversation(station, message, false);
            messageLed = true;
            ESP_LOGI(TAG, "APRS msg from %s saved", station.c_str());
        } else if (typeMessage == 1) {
            File f = STORAGE_Utils::openFile("/winlinkMails.txt", FILE_APPEND);
            if (!f) return;
            f.println(station + "," + message);
            numWLNKMessages++;
            f.close();
        }
    }

    void sendMessage(const String& station, const String& text) {
        String packet = APRSPacketLib::generateMessagePacket(
            currentBeacon->callsign, "APLRT1", Config.path, station, text);
        LoRa_Utils::sendNewPacket(packet);
        saveToConversation(station, text, true);
        ESP_LOGI(TAG, "Sent MSG to %s: %s", station.c_str(), text.c_str());
    }

    void addToOutputBuffer(uint8_t typeOfMessage, const String& station, const String& text) {
        (void)typeOfMessage;
        outputMessagesBuffer.push_back(station + ":" + text);
    }

    void processOutputBuffer() {
        if (outputMessagesBuffer.empty()) return;
        String entry = outputMessagesBuffer.front();
        outputMessagesBuffer.erase(outputMessagesBuffer.begin());
        int sep = entry.indexOf(':');
        if (sep < 0) return;
        String station = entry.substring(0, sep);
        String text    = entry.substring(sep + 1);
        sendMessage(station, text);
    }

    void clean15SegBuffer() {
        uint32_t now = millis();
        packet15SegBuffer.erase(
            std::remove_if(packet15SegBuffer.begin(), packet15SegBuffer.end(),
                [&](const Packet15SegBuffer& p){ return now - p.receivedTime > 15000; }),
            packet15SegBuffer.end());
    }

    bool check15SegBuffer(const String& station, const String& payload) {
        for (const auto& p : packet15SegBuffer)
            if (p.station == station && p.payload == payload) return true;
        packet15SegBuffer.push_back({millis(), station, payload});
        return false;
    }

    void checkReceivedMessage(ReceivedLoRaPacket packetReceived) {
        if (packetReceived.text.isEmpty()) return;

        // Strip the 3-byte LoRa preamble (0x3c 0xff 0x01)
        String raw = packetReceived.text.substring(3);

        APRSPacket aprs = APRSPacketLib::processReceivedPacket(
            raw, packetReceived.rssi, packetReceived.snr, packetReceived.freqError);

        if (aprs.sender.isEmpty()) return;

        lastHeardTracker = aprs.sender;
        lastReceivedPacket = aprs;

        // Stations sur la map : uniquement position (0), Mic-E (4), objet (5)
        // Les messages (1), telemetry (2), weather (3) n'ont pas de coordonnées utiles
        if ((aprs.type == 0 || aprs.type == 4 || aprs.type == 5) &&
            (aprs.latitude != 0.0f || aprs.longitude != 0.0f)) {
            STATION_Utils::addMapStation(aprs.sender, aprs.latitude, aprs.longitude,
                                         aprs.symbol, aprs.overlay, packetReceived.rssi);
            GPS_Utils::calculateDistanceCourse(aprs.sender, aprs.latitude, aprs.longitude);
            NOTIFICATION_Utils::stationHeardBeep();
        }

        // Handle APRS messages addressed to us
        if (aprs.type == 5 && !aprs.addressee.isEmpty()) {
            String myCall = currentBeacon->callsign;
            myCall.trim();
            String addr = aprs.addressee;
            addr.trim();
            if (addr == myCall) {
                if (check15SegBuffer(aprs.sender, aprs.payload)) return;

                // Check for ACK
                if (aprs.payload.startsWith("ack")) {
                    String ackNum = aprs.payload.substring(3);
                    if (ackRequestState && ackNum == ackNumberRequest) {
                        ackRequestState = false;
                        STORAGE_Utils::updateAckStats();
                        ESP_LOGI(TAG, "ACK received from %s #%s", aprs.sender.c_str(), ackNum.c_str());
                    }
                    return;
                }

                // Extract message number and send ACK
                String message = aprs.payload;
                String msgNum;
                int braceOpen = aprs.payload.lastIndexOf('{');
                if (braceOpen >= 0) {
                    msgNum  = aprs.payload.substring(braceOpen+1);
                    message = aprs.payload.substring(0, braceOpen);
                    int braceClose = msgNum.indexOf('}');
                    if (braceClose >= 0) msgNum = msgNum.substring(0, braceClose);

                    // Send ACK
                    String ackPkt = APRSPacketLib::generateMessagePacket(
                        myCall, "APLRT1", Config.path, aprs.sender, "ack" + msgNum);
                    LoRa_Utils::sendNewPacket(ackPkt);
                    ESP_LOGI(TAG, "ACK sent to %s #%s", aprs.sender.c_str(), msgNum.c_str());
                }

                message.trim();
                saveNewMessage(0, aprs.sender, message);
                NOTIFICATION_Utils::messageBeep();
                printf("MSG:%s:%s\n", aprs.sender.c_str(), message.c_str()); fflush(stdout);
            }
        }
    }
}
