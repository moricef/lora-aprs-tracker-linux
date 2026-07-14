#include "bluetooth_ble.h"

#include "esp_log.h"
#include "kiss_utils.h"
#include "lora_utils.h"

#include <gio/gio.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

extern bool bluetoothConnected;

namespace BluetoothBLE {
namespace {

constexpr const char *ADAPTER = "/org/bluez/hci0";
constexpr const char *ROOT = "/com/loraaprs";
constexpr const char *SERVICE = "/com/loraaprs/service0";
constexpr const char *TX_CHAR = "/com/loraaprs/service0/tx";
constexpr const char *RX_CHAR = "/com/loraaprs/service0/rx";
constexpr const char *GATT_SERVICE_IFACE = "org.bluez.GattService1";
constexpr const char *GATT_CHAR_IFACE = "org.bluez.GattCharacteristic1";

// Same UUIDs as the original tracker firmware.
constexpr const char *KISS_SERVICE = "00000001-ba2a-46c9-ae49-01b0961f68bb";
constexpr const char *KISS_RX = "00000002-ba2a-46c9-ae49-01b0961f68bb";
constexpr const char *KISS_TX = "00000003-ba2a-46c9-ae49-01b0961f68bb";
constexpr const char *TNC2_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *TNC2_TX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *TNC2_RX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

std::atomic<bool> running{false};
std::atomic<bool> connected{false};
bool useKiss = false;
bool notifying = false;
std::string localName;
std::string serviceUuid;
std::string txUuid;
std::string rxUuid;
std::string kissBuffer;
std::vector<uint8_t> txValue;
std::mutex stateMutex;
std::mutex startupMutex;
std::condition_variable startupCondition;
bool startupDone = false;
bool startupOk = false;
bool managementAdvertisement = false;
pthread_t thread{};
GMainLoop *loop = nullptr;
GDBusConnection *bus = nullptr;
GDBusNodeInfo *introspection = nullptr;
std::vector<guint> registrations;

const char *XML = R"xml(
<node>
  <interface name='org.freedesktop.DBus.ObjectManager'>
    <method name='GetManagedObjects'>
      <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>
    </method>
  </interface>
  <interface name='org.bluez.GattService1'>
    <property name='UUID' type='s' access='read'/>
    <property name='Primary' type='b' access='read'/>
    <property name='Includes' type='ao' access='read'/>
  </interface>
  <interface name='org.bluez.GattCharacteristic1'>
    <method name='ReadValue'><arg name='options' type='a{sv}' direction='in'/><arg name='value' type='ay' direction='out'/></method>
    <method name='WriteValue'><arg name='value' type='ay' direction='in'/><arg name='options' type='a{sv}' direction='in'/></method>
    <method name='StartNotify'/><method name='StopNotify'/>
    <property name='UUID' type='s' access='read'/>
    <property name='Service' type='o' access='read'/>
    <property name='Flags' type='as' access='read'/>
    <property name='Value' type='ay' access='read'/>
    <property name='Notifying' type='b' access='read'/>
  </interface>
</node>)xml";

GVariant *byteArray(const std::vector<uint8_t> &value) {
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.data(), value.size(), 1);
}

void setConnection(bool value) {
    connected = value;
    bluetoothConnected = value;
}

void processTnc2(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    while (!value.empty() && (value.front() == '\r' || value.front() == '\n')) value.erase(0, 1);
    if (value.empty()) return;
    ESP_LOGI("BLE", "RX TNC2: %s", value.c_str());
    LoRa_Utils::sendNewPacket(String(value.c_str()));
}

void processWrite(const uint8_t *data, size_t size) {
    if (!data || !size) return;
    setConnection(true);
    if (!useKiss) {
        processTnc2(std::string(reinterpret_cast<const char *>(data), size));
        return;
    }
    kissBuffer.append(reinterpret_cast<const char *>(data), size);
    if (kissBuffer.size() > 4096) { kissBuffer.clear(); return; }
    while (true) {
        const size_t start = kissBuffer.find(char(FEND));
        if (start == std::string::npos) { kissBuffer.clear(); return; }
        const size_t end = kissBuffer.find(char(FEND), start + 1);
        if (end == std::string::npos) {
            if (start) kissBuffer.erase(0, start);
            return;
        }
        String frame;
        for (size_t i = start; i <= end; ++i) frame += kissBuffer[i];
        kissBuffer.erase(0, end + 1);
        bool dataFrame = false;
        String decoded = KISS_Utils::decodeKISS(frame, dataFrame);
        if (dataFrame && !decoded.isEmpty()) {
            ESP_LOGI("BLE", "RX KISS: %s", decoded.c_str());
            LoRa_Utils::sendNewPacket(decoded);
        }
    }
}

void addServiceObject(GVariantBuilder &objects) {
    GVariantBuilder interfaces;
    GVariantBuilder properties;
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(serviceUuid.c_str()));
    g_variant_builder_add(&properties, "{sv}", "Primary", g_variant_new_boolean(TRUE));
    GVariantBuilder includes;
    g_variant_builder_init(&includes, G_VARIANT_TYPE("ao"));
    g_variant_builder_add(&properties, "{sv}", "Includes", g_variant_builder_end(&includes));
    g_variant_builder_add(&interfaces, "{sa{sv}}", GATT_SERVICE_IFACE, &properties);
    g_variant_builder_add(&objects, "{oa{sa{sv}}}", SERVICE, &interfaces);
}

void addCharacteristicObject(GVariantBuilder &objects, const char *path, bool tx) {
    GVariantBuilder interfaces;
    GVariantBuilder properties;
    GVariantBuilder flags;
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string((tx ? txUuid : rxUuid).c_str()));
    g_variant_builder_add(&properties, "{sv}", "Service", g_variant_new_object_path(SERVICE));
    g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
    if (tx) {
        g_variant_builder_add(&flags, "s", "read");
        g_variant_builder_add(&flags, "s", "notify");
    } else {
        g_variant_builder_add(&flags, "s", "write");
        g_variant_builder_add(&flags, "s", "write-without-response");
    }
    g_variant_builder_add(&properties, "{sv}", "Flags", g_variant_builder_end(&flags));
    if (tx) {
        std::lock_guard<std::mutex> lock(stateMutex);
        g_variant_builder_add(&properties, "{sv}", "Value", byteArray(txValue));
        g_variant_builder_add(&properties, "{sv}", "Notifying", g_variant_new_boolean(notifying));
    }
    g_variant_builder_add(&interfaces, "{sa{sv}}", GATT_CHAR_IFACE, &properties);
    g_variant_builder_add(&objects, "{oa{sa{sv}}}", path, &interfaces);
}

void methodCall(GDBusConnection *, const gchar *, const gchar *path,
                const gchar *interface, const gchar *method, GVariant *parameters,
                GDBusMethodInvocation *invocation, gpointer) {
    if (g_str_equal(interface, "org.freedesktop.DBus.ObjectManager") &&
        g_str_equal(method, "GetManagedObjects")) {
        GVariantBuilder objects;
        g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        addServiceObject(objects);
        addCharacteristicObject(objects, TX_CHAR, true);
        addCharacteristicObject(objects, RX_CHAR, false);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(a{oa{sa{sv}}})", &objects));
        return;
    }
    if (g_str_equal(interface, GATT_CHAR_IFACE)) {
        if (g_str_equal(method, "ReadValue") && g_str_equal(path, TX_CHAR)) {
            std::lock_guard<std::mutex> lock(stateMutex);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", byteArray(txValue)));
            return;
        }
        if (g_str_equal(method, "WriteValue") && g_str_equal(path, RX_CHAR)) {
            GVariant *value = g_variant_get_child_value(parameters, 0);
            gsize size = 0;
            const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(value, &size, 1));
            processWrite(data, size);
            g_variant_unref(value);
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (g_str_equal(method, "StartNotify") && g_str_equal(path, TX_CHAR)) {
            { std::lock_guard<std::mutex> lock(stateMutex); notifying = true; }
            setConnection(true);
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (g_str_equal(method, "StopNotify") && g_str_equal(path, TX_CHAR)) {
            { std::lock_guard<std::mutex> lock(stateMutex); notifying = false; }
            setConnection(false);
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
    }
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Not supported");
}

GVariant *getProperty(GDBusConnection *, const gchar *, const gchar *path,
                      const gchar *interface, const gchar *property, GError **, gpointer) {
    if (g_str_equal(interface, GATT_SERVICE_IFACE)) {
        if (g_str_equal(property, "UUID")) return g_variant_new_string(serviceUuid.c_str());
        if (g_str_equal(property, "Primary")) return g_variant_new_boolean(TRUE);
        if (g_str_equal(property, "Includes")) return g_variant_new_objv(nullptr, 0);
    }
    if (g_str_equal(interface, GATT_CHAR_IFACE)) {
        const bool tx = g_str_equal(path, TX_CHAR);
        if (g_str_equal(property, "UUID")) return g_variant_new_string((tx ? txUuid : rxUuid).c_str());
        if (g_str_equal(property, "Service")) return g_variant_new_object_path(SERVICE);
        if (g_str_equal(property, "Flags")) {
            const char *txFlags[] = {"read", "notify", nullptr};
            const char *rxFlags[] = {"write", "write-without-response", nullptr};
            return g_variant_new_strv(tx ? txFlags : rxFlags, -1);
        }
        if (tx && g_str_equal(property, "Value")) {
            std::lock_guard<std::mutex> lock(stateMutex);
            return byteArray(txValue);
        }
        if (tx && g_str_equal(property, "Notifying")) {
            std::lock_guard<std::mutex> lock(stateMutex);
            return g_variant_new_boolean(notifying);
        }
    }
    return nullptr;
}

const GDBusInterfaceVTable VTABLE = {methodCall, getProperty, nullptr, {nullptr}};

GDBusInterfaceInfo *interfaceInfo(const char *name) {
    return g_dbus_node_info_lookup_interface(introspection, name);
}

bool registerObject(const char *path, const char *interface) {
    GError *error = nullptr;
    const guint id = g_dbus_connection_register_object(bus, path, interfaceInfo(interface),
                                                        &VTABLE, nullptr, nullptr, &error);
    if (!id) {
        ESP_LOGE("BLE", "register %s: %s", path, error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    registrations.push_back(id);
    return true;
}

bool bluezCall(const char *interface, const char *method, GVariant *parameters) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.bluez", ADAPTER, interface, method, parameters, nullptr,
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (!reply) {
        ESP_LOGE("BLE", "%s: %s", method, error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

GVariant *objectWithEmptyOptions(const char *path) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    return g_variant_new("(oa{sv})", path, &options);
}

void finishStartup(bool ok) {
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        startupDone = true;
        startupOk = ok;
    }
    startupCondition.notify_one();
    if (!ok && loop) g_main_loop_quit(loop);
}

void applicationRegistered(GObject *source, GAsyncResult *result, gpointer) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!reply) {
        ESP_LOGE("BLE", "RegisterApplication: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        finishStartup(false);
        return;
    }
    g_variant_unref(reply);
    ESP_LOGI("BLE", "advertising %s (%s, %s) via kernel management",
             localName.c_str(), useKiss ? "KISS" : "TNC2", serviceUuid.c_str());
    finishStartup(true);
}

void cleanup() {
    if (bus) {
        bluezCall("org.bluez.GattManager1", "UnregisterApplication", g_variant_new("(o)", ROOT));
        for (guint id : registrations) g_dbus_connection_unregister_object(bus, id);
    }
    registrations.clear();
    if (managementAdvertisement)
        std::system("timeout 3 script -qec 'sudo -n btmgmt rm-adv 1' /dev/null >/dev/null 2>&1");
    managementAdvertisement = false;
    if (introspection) g_dbus_node_info_unref(introspection);
    introspection = nullptr;
    if (bus) g_object_unref(bus);
    bus = nullptr;
    if (loop) g_main_loop_unref(loop);
    loop = nullptr;
    setConnection(false);
}

void *bleThread(void *) {
    GError *error = nullptr;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    introspection = g_dbus_node_info_new_for_xml(XML, &error);
    loop = g_main_loop_new(nullptr, FALSE);
    const bool ok = bus && introspection && loop &&
              registerObject(ROOT, "org.freedesktop.DBus.ObjectManager") &&
              registerObject(SERVICE, GATT_SERVICE_IFACE) &&
              registerObject(TX_CHAR, GATT_CHAR_IFACE) &&
              registerObject(RX_CHAR, GATT_CHAR_IFACE);
    if (!ok && error) ESP_LOGE("BLE", "startup: %s", error->message);
    g_clear_error(&error);
    if (ok) {
        g_dbus_connection_call(bus, "org.bluez", ADAPTER,
            "org.bluez.GattManager1", "RegisterApplication", objectWithEmptyOptions(ROOT),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, applicationRegistered, nullptr);
        g_main_loop_run(loop);
    } else {
        finishStartup(false);
    }
    cleanup();
    running = false;
    return nullptr;
}

} // namespace

bool start(const std::string &deviceName, bool kiss) {
    if (running) return true;
    localName = deviceName.empty() ? "LoRaTracker" : deviceName.substr(0, 26);
    std::string safeName;
    for (unsigned char c : localName) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_') safeName += char(c);
    }
    if (safeName.empty()) safeName = "LoRaTracker";
    std::system(("bluetoothctl system-alias '" + safeName + "' >/dev/null 2>&1").c_str());
    useKiss = kiss;
    serviceUuid = useKiss ? KISS_SERVICE : TNC2_SERVICE;
    txUuid = useKiss ? KISS_TX : TNC2_TX;
    rxUuid = useKiss ? KISS_RX : TNC2_RX;
    // Add the controller advertisement before registering the GATT
    // application. Concurrent management and GATT registration transactions
    // can block one another on this Pi controller.
    const std::string advertise = "timeout 3 script -qec 'sudo -n btmgmt rm-adv 1' /dev/null >/dev/null 2>&1 || true; "
        "timeout 3 script -qec 'sudo -n btmgmt add-adv -c -g -n -u " + serviceUuid + " 1' /dev/null >/dev/null 2>&1";
    managementAdvertisement = std::system(advertise.c_str()) == 0;
    if (!managementAdvertisement) {
        ESP_LOGE("BLE", "kernel management advertisement failed");
        return false;
    }
    startupDone = startupOk = false;
    running = true;
    if (pthread_create(&thread, nullptr, bleThread, nullptr) != 0) {
        running = false;
        return false;
    }
    std::unique_lock<std::mutex> lock(startupMutex);
    startupCondition.wait_for(lock, std::chrono::seconds(7), [] { return startupDone; });
    if (!startupDone || !startupOk) {
        lock.unlock();
        stop();
        return false;
    }
    return true;
}

void stop() {
    if (!running) return;
    if (loop) g_main_loop_quit(loop);
    pthread_join(thread, nullptr);
    running = false;
}

bool isRunning() { return running; }
bool isConnected() { return connected; }

void sendToClient(const std::string &tnc2Frame) {
    if (!running || !connected || tnc2Frame.empty() || !bus) return;
    String payload = useKiss ? KISS_Utils::encodeKISS(String(tnc2Frame.c_str()))
                             : String((tnc2Frame + "\n").c_str());
    if (payload.isEmpty()) return;
    constexpr size_t CHUNK = 180;
    for (size_t offset = 0; offset < size_t(payload.length()); offset += CHUNK) {
        const size_t count = std::min(CHUNK, size_t(payload.length()) - offset);
        GVariantBuilder changed;
        GVariantBuilder invalidated;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            txValue.assign(reinterpret_cast<const uint8_t *>(payload.c_str()) + offset,
                           reinterpret_cast<const uint8_t *>(payload.c_str()) + offset + count);
            g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&changed, "{sv}", "Value", byteArray(txValue));
        }
        g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
        g_dbus_connection_emit_signal(bus, nullptr, TX_CHAR,
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            g_variant_new("(sa{sv}as)", GATT_CHAR_IFACE, &changed, &invalidated), nullptr);
    }
}

} // namespace BluetoothBLE
