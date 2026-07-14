#include "bluetooth_classic.h"

#include "lora_utils.h"
#include "esp_log.h"
#include "kiss_utils.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <mutex>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

extern bool bluetoothConnected;

namespace BluetoothClassic {
namespace {

constexpr uint8_t RFCOMM_CHANNEL = 1;
std::atomic<bool> running{false};
std::atomic<bool> connected{false};
pthread_t thread{};
int serverFd = -1;
int clientFd = -1;
std::mutex clientMutex;
sdp_session_t *sdpSession = nullptr;
sdp_record_t *sdpRecord = nullptr;
bool kissMode = false;

bool registerSerialPortService(const std::string &name) {
    sdpRecord = sdp_record_alloc();
    if (!sdpRecord) return false;

    uuid_t serviceUuid, rootUuid, l2capUuid, rfcommUuid, profileUuid;
    sdp_uuid16_create(&serviceUuid, SERIAL_PORT_SVCLASS_ID);
    sdp_list_t *serviceClasses = sdp_list_append(nullptr, &serviceUuid);
    sdp_set_service_classes(sdpRecord, serviceClasses);

    sdp_uuid16_create(&rootUuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t *root = sdp_list_append(nullptr, &rootUuid);
    sdp_set_browse_groups(sdpRecord, root);

    sdp_uuid16_create(&l2capUuid, L2CAP_UUID);
    sdp_list_t *l2cap = sdp_list_append(nullptr, &l2capUuid);
    sdp_uuid16_create(&rfcommUuid, RFCOMM_UUID);
    sdp_list_t *rfcomm = sdp_list_append(nullptr, &rfcommUuid);
    uint8_t channel = RFCOMM_CHANNEL;
    sdp_data_t *channelData = sdp_data_alloc(SDP_UINT8, &channel);
    sdp_list_append(rfcomm, channelData);
    sdp_list_t *protocol = sdp_list_append(nullptr, l2cap);
    sdp_list_append(protocol, rfcomm);
    sdp_list_t *access = sdp_list_append(nullptr, protocol);
    sdp_set_access_protos(sdpRecord, access);

    sdp_profile_desc_t profile{};
    sdp_uuid16_create(&profileUuid, SERIAL_PORT_PROFILE_ID);
    profile.uuid = profileUuid;
    profile.version = 0x0100;
    sdp_list_t *profiles = sdp_list_append(nullptr, &profile);
    sdp_set_profile_descs(sdpRecord, profiles);
    sdp_set_info_attr(sdpRecord, name.c_str(), "LoRa APRS",
                      kissMode ? "LoRa APRS KISS serial port" : "LoRa APRS TNC2 serial port");

    bdaddr_t anyAddress{{0, 0, 0, 0, 0, 0}};
    bdaddr_t localAddress{{0, 0, 0, 0xff, 0xff, 0xff}};
    sdpSession = sdp_connect(&anyAddress, &localAddress, SDP_RETRY_IF_BUSY);
    const bool ok = sdpSession && sdp_record_register(sdpSession, sdpRecord, 0) == 0;

    sdp_data_free(channelData);
    sdp_list_free(access, nullptr);
    sdp_list_free(protocol, nullptr);
    sdp_list_free(rfcomm, nullptr);
    sdp_list_free(l2cap, nullptr);
    sdp_list_free(root, nullptr);
    sdp_list_free(serviceClasses, nullptr);
    sdp_list_free(profiles, nullptr);
    return ok;
}

void unregisterSerialPortService() {
    ESP_LOGI("BT-SPP", "SDP teardown: unregister");
    if (sdpSession && sdpRecord) sdp_record_unregister(sdpSession, sdpRecord);
    ESP_LOGI("BT-SPP", "SDP teardown: close session");
    if (sdpSession) sdp_close(sdpSession);
    // sdp_record_unregister() consumes the registered record with this BlueZ
    // SDP server. Freeing it again causes a double-free during a live
    // Classic -> BLE switch.
    sdpSession = nullptr;
    sdpRecord = nullptr;
}

void closeClient() {
    std::lock_guard<std::mutex> lock(clientMutex);
    if (clientFd >= 0) {
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        clientFd = -1;
    }
    connected = false;
    bluetoothConnected = false;
}

void processTnc2Line(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return;
    ESP_LOGI("BT-SPP", "RX TNC2: %s", line.c_str());
    LoRa_Utils::sendNewPacket(String(line.c_str()));
}

void processKissBuffer(std::string &buffer) {
    if (buffer.size() > 4096) { buffer.clear(); return; }
    while (true) {
        const size_t start = buffer.find(char(FEND));
        if (start == std::string::npos) { buffer.clear(); return; }
        const size_t end = buffer.find(char(FEND), start + 1);
        if (end == std::string::npos) {
            if (start) buffer.erase(0, start);
            return;
        }
        String frame;
        for (size_t i = start; i <= end; ++i) frame += buffer[i];
        buffer.erase(0, end + 1);
        bool dataFrame = false;
        String decoded = KISS_Utils::decodeKISS(frame, dataFrame);
        if (dataFrame && !decoded.isEmpty()) {
            ESP_LOGI("BT-SPP", "RX KISS: %s", decoded.c_str());
            LoRa_Utils::sendNewPacket(decoded);
        }
    }
}

void *serverThread(void *) {
    serverFd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (serverFd < 0) {
        ESP_LOGE("BT-SPP", "socket: %s", strerror(errno));
        running = false;
        return nullptr;
    }
    sockaddr_rc local{};
    local.rc_family = AF_BLUETOOTH;
    bdaddr_t anyAddress{{0, 0, 0, 0, 0, 0}};
    bacpy(&local.rc_bdaddr, &anyAddress);
    local.rc_channel = RFCOMM_CHANNEL;
    if (bind(serverFd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0 ||
        listen(serverFd, 1) < 0) {
        ESP_LOGE("BT-SPP", "bind/listen channel %u: %s", RFCOMM_CHANNEL, strerror(errno));
        close(serverFd); serverFd = -1; running = false;
        return nullptr;
    }
    ESP_LOGI("BT-SPP", "listening on RFCOMM channel %u (%s)", RFCOMM_CHANNEL,
             kissMode ? "KISS" : "TNC2");

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds); FD_SET(serverFd, &readfds);
        timeval timeout{0, 250000};
        if (select(serverFd + 1, &readfds, nullptr, nullptr, &timeout) <= 0) continue;
        sockaddr_rc remote{}; socklen_t len = sizeof(remote);
        int accepted = accept4(serverFd, reinterpret_cast<sockaddr *>(&remote), &len, SOCK_CLOEXEC);
        if (accepted < 0) continue;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            clientFd = accepted;
        }
        connected = true;
        bluetoothConnected = true;
        char address[19]{}; ba2str(&remote.rc_bdaddr, address);
        ESP_LOGI("BT-SPP", "client connected: %s", address);

        std::string accumulator;
        char buffer[512];
        while (running && connected) {
            int count = recv(accepted, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            accumulator.append(buffer, count);
            if (kissMode) {
                processKissBuffer(accumulator);
                continue;
            }
            size_t end;
            while ((end = accumulator.find_first_of("\r\n")) != std::string::npos) {
                processTnc2Line(accumulator.substr(0, end));
                accumulator.erase(0, end + 1);
                while (!accumulator.empty() &&
                       (accumulator.front() == '\r' || accumulator.front() == '\n'))
                    accumulator.erase(0, 1);
            }
            if (accumulator.size() > 2048) accumulator.clear();
        }
        closeClient();
        ESP_LOGI("BT-SPP", "client disconnected");
    }
    if (serverFd >= 0) { close(serverFd); serverFd = -1; }
    return nullptr;
}

} // namespace

bool start(const std::string &deviceName, bool useKiss) {
    if (running) return true;
    kissMode = useKiss;
    // Advertising/pairing policy belongs to BlueZ; SPP data remains in this process.
    std::string safeName;
    safeName.reserve(deviceName.size());
    for (unsigned char c : deviceName) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_') safeName += char(c);
    }
    if (safeName.empty()) safeName = "LoRaTracker";
    std::string alias = "bluetoothctl system-alias '" + safeName + "' >/dev/null 2>&1";
    std::system(alias.c_str());
    std::system("bluetoothctl pairable on >/dev/null 2>&1");
    std::system("bluetoothctl discoverable on >/dev/null 2>&1");
    if (!registerSerialPortService(deviceName)) {
        ESP_LOGE("BT-SPP", "SDP registration failed");
        unregisterSerialPortService();
        return false;
    }
    running = true;
    if (pthread_create(&thread, nullptr, serverThread, nullptr) != 0) {
        running = false;
        unregisterSerialPortService();
        return false;
    }
    return true;
}

void stop() {
    if (!running) return;
    ESP_LOGI("BT-SPP", "stopping RFCOMM server");
    running = false;
    closeClient();
    if (serverFd >= 0) shutdown(serverFd, SHUT_RDWR);
    pthread_join(thread, nullptr);
    ESP_LOGI("BT-SPP", "RFCOMM thread stopped");
    unregisterSerialPortService();
    std::system("bluetoothctl discoverable off >/dev/null 2>&1");
}

bool isRunning() { return running; }
bool isConnected() { return connected; }

void sendToClient(const std::string &tnc2Frame) {
    if (!connected || tnc2Frame.empty()) return;
    String encoded = kissMode ? KISS_Utils::encodeKISS(String(tnc2Frame.c_str()))
                              : String((tnc2Frame + "\r\n").c_str());
    if (encoded.isEmpty()) return;
    const std::string payload(encoded.c_str(), encoded.length());
    std::lock_guard<std::mutex> lock(clientMutex);
    if (clientFd >= 0 && send(clientFd, payload.data(), payload.size(), MSG_NOSIGNAL) < 0) {
        connected = false;
        bluetoothConnected = false;
    }
}

} // namespace BluetoothClassic
