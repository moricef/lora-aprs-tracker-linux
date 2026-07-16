#include "bluetooth_agent.h"

#include "esp_log.h"

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace BluetoothAgent {
namespace {

constexpr const char *BLUEZ = "org.bluez";
constexpr const char *MANAGER_PATH = "/org/bluez";
constexpr const char *MANAGER_IFACE = "org.bluez.AgentManager1";
constexpr const char *AGENT_IFACE = "org.bluez.Agent1";
constexpr const char *AGENT_PATH = "/com/loraaprs/agent";

const char *XML = R"xml(
<node>
  <interface name='org.bluez.Agent1'>
    <method name='Release'/>
    <method name='RequestPinCode'>
      <arg name='device' type='o' direction='in'/>
      <arg name='pincode' type='s' direction='out'/>
    </method>
    <method name='DisplayPinCode'>
      <arg name='device' type='o' direction='in'/>
      <arg name='pincode' type='s' direction='in'/>
    </method>
    <method name='RequestPasskey'>
      <arg name='device' type='o' direction='in'/>
      <arg name='passkey' type='u' direction='out'/>
    </method>
    <method name='DisplayPasskey'>
      <arg name='device' type='o' direction='in'/>
      <arg name='passkey' type='u' direction='in'/>
      <arg name='entered' type='q' direction='in'/>
    </method>
    <method name='RequestConfirmation'>
      <arg name='device' type='o' direction='in'/>
      <arg name='passkey' type='u' direction='in'/>
    </method>
    <method name='RequestAuthorization'>
      <arg name='device' type='o' direction='in'/>
    </method>
    <method name='AuthorizeService'>
      <arg name='device' type='o' direction='in'/>
      <arg name='uuid' type='s' direction='in'/>
    </method>
    <method name='Cancel'/>
  </interface>
</node>)xml";

std::atomic<bool> running{false};
pthread_t thread{};
GMainContext *context = nullptr;
GMainLoop *loop = nullptr;
GDBusConnection *bus = nullptr;
GDBusNodeInfo *introspection = nullptr;
guint registration = 0;
std::mutex startupMutex;
std::condition_variable startupCondition;
bool startupDone = false;
bool startupOk = false;

void finishStartup(bool ok) {
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        startupDone = true;
        startupOk = ok;
    }
    startupCondition.notify_one();
}

void methodCall(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                const gchar *method, GVariant *parameters,
                GDBusMethodInvocation *invocation, gpointer) {
    if (g_str_equal(method, "RequestPinCode")) {
        ESP_LOGI("BT-Agent", "auto-accept PIN request");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "0000"));
        return;
    }
    if (g_str_equal(method, "RequestPasskey")) {
        ESP_LOGI("BT-Agent", "auto-accept passkey request");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0u));
        return;
    }
    if (g_str_equal(method, "RequestConfirmation")) {
        const gchar *device = nullptr;
        guint32 passkey = 0;
        g_variant_get(parameters, "(&ou)", &device, &passkey);
        ESP_LOGI("BT-Agent", "auto-confirm %06u for %s", passkey, device);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_str_equal(method, "RequestAuthorization")) {
        const gchar *device = nullptr;
        g_variant_get(parameters, "(&o)", &device);
        ESP_LOGI("BT-Agent", "auto-authorize %s", device);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_str_equal(method, "AuthorizeService")) {
        const gchar *device = nullptr;
        const gchar *uuid = nullptr;
        g_variant_get(parameters, "(&o&s)", &device, &uuid);
        ESP_LOGI("BT-Agent", "auto-authorize %s for %s", uuid, device);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_str_equal(method, "Release")) {
        ESP_LOGI("BT-Agent", "released by BlueZ");
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_str_equal(method, "DisplayPinCode") ||
        g_str_equal(method, "DisplayPasskey") ||
        g_str_equal(method, "Cancel")) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.NotSupported", "Not supported");
}

const GDBusInterfaceVTable VTABLE = {methodCall, nullptr, nullptr, {nullptr}};

bool managerCall(const char *method, GVariant *parameters) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, BLUEZ, MANAGER_PATH, MANAGER_IFACE, method, parameters, nullptr,
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (!reply) {
        ESP_LOGE("BT-Agent", "%s: %s", method, error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

void cleanup() {
    if (bus && registration) {
        managerCall("UnregisterAgent", g_variant_new("(o)", AGENT_PATH));
        g_dbus_connection_unregister_object(bus, registration);
    }
    registration = 0;
    if (introspection) g_dbus_node_info_unref(introspection);
    introspection = nullptr;
    if (bus) g_object_unref(bus);
    bus = nullptr;
    if (loop) g_main_loop_unref(loop);
    loop = nullptr;
    if (context) {
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
    context = nullptr;
}

void *agentThread(void *) {
    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);

    GError *error = nullptr;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    introspection = g_dbus_node_info_new_for_xml(XML, &error);
    if (bus && introspection) {
        registration = g_dbus_connection_register_object(
            bus, AGENT_PATH,
            g_dbus_node_info_lookup_interface(introspection, AGENT_IFACE),
            &VTABLE, nullptr, nullptr, &error);
    }
    const bool ok = registration &&
        managerCall("RegisterAgent", g_variant_new("(os)", AGENT_PATH, "DisplayYesNo")) &&
        managerCall("RequestDefaultAgent", g_variant_new("(o)", AGENT_PATH));
    if (!ok) {
        ESP_LOGE("BT-Agent", "startup: %s", error ? error->message : "registration failed");
        g_clear_error(&error);
        finishStartup(false);
        cleanup();
        running = false;
        return nullptr;
    }
    g_clear_error(&error);
    ESP_LOGI("BT-Agent", "default auto-confirm agent registered");
    finishStartup(true);
    g_main_loop_run(loop);
    cleanup();
    running = false;
    return nullptr;
}

} // namespace

bool start() {
    if (running) return true;
    startupDone = startupOk = false;
    running = true;
    if (pthread_create(&thread, nullptr, agentThread, nullptr) != 0) {
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

} // namespace BluetoothAgent
