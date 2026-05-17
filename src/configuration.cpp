#include "esp_log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "configuration.h"
#include "smartbeacon_utils.h"

static const char* TAG = "Config";
static const char* CONFIG_PATH = "/data/LoRa_Tracker/tracker_conf.json";

using json = nlohmann::json;

static json safeGet(const json& j, const std::string& k) {
    return j.contains(k) ? j[k] : json{};
}

Configuration::Configuration() { setDefaultValues(); }

void Configuration::setDefaultValues() {
    wifiEnabled = true;
    wifiAutoAP.active   = false;
    wifiAutoAP.password = "1234567890";
    wifiAutoAP.timeout  = 10;

    Beacon bcn;
    bcn.callsign           = "NOCALL-7";
    bcn.symbol             = "[";
    bcn.overlay            = "/";
    bcn.micE               = "";
    bcn.comment            = "Linux Tracker";
    bcn.smartBeaconActive  = true;
    bcn.smartBeaconSetting = 0;
    bcn.sbSlowRate = bcn.sbFastRate = bcn.sbMinSpeed = bcn.sbMaxSpeed = 0;
    bcn.sbMinTurnAngle = bcn.sbTurnSlope = bcn.sbMinBeaconTime = 0;
    bcn.gpsEcoMode = false;
    beacons.clear();
    beacons.push_back(bcn);

    display = {true, false, 4, false};
    gpsConfig = {false};

    battery = {false, false, false, false, 2.9f};
    winlink = {""};
    telemetry = {false, false, 0.0f};
    notification = {false,-1,false,-1,false,-1,false,-1,-1,50,false,false,false,false,false,false};
    ptt = {false,-1,0,0,false};
    bluetooth = {false,"LoRaTracker",true,true};

    aprs_is = {false, "euro.aprs2.net", 14580, "-1"};

    LoraType lt;
    lt.frequency       = 433775000L;
    lt.spreadingFactor = 12;
    lt.signalBandwidth = 125000L;
    lt.codingRate4     = 5;
    lt.power           = 20;
    lt.dataRate        = 300;
    loraTypes.clear();
    loraTypes.push_back(lt);

    lora = {true, false};

    simplifiedTrackerMode   = false;
    sendCommentAfterXBeacons= 10;
    path                    = "WIDE2-1";
    email                   = "";
    nonSmartBeaconRate      = 10;
    rememberStationTime     = 30;
    standingUpdateTime      = 15;
    sendAltitude            = true;
    disableGPS              = false;
}

bool Configuration::readFile() {
    std::ifstream f(CONFIG_PATH);
    if (!f.is_open()) {
        ESP_LOGW(TAG, "Config file not found, using defaults");
        return false;
    }
    json data;
    try { f >> data; } catch (...) {
        ESP_LOGW(TAG, "Failed to parse config JSON, using defaults");
        return false;
    }

    // WiFi
    if (data.contains("wifi")) {
        const auto& w = data["wifi"];
        if (w.contains("AP") && w["AP"].is_array()) {
            for (const auto& ap : w["AP"]) {
                WiFi_AP a;
                a.ssid     = ap.value("ssid",     "");
                a.password = ap.value("password", "");
                wifiAPs.push_back(a);
            }
        }
        if (w.contains("autoAP")) {
            wifiAutoAP.active   = w["autoAP"].value("active",   false);
            wifiAutoAP.password = w["autoAP"].value("password", "1234567890");
            wifiAutoAP.timeout  = w["autoAP"].value("timeout",  10);
        }
        wifiEnabled = w.value("enabled", true);
    }

    // Beacons
    if (data.contains("beacons") && data["beacons"].is_array()) {
        beacons.clear();
        for (const auto& b : data["beacons"]) {
            Beacon bcn;
            bcn.callsign           = b.value("callsign",           "NOCALL-7");
            bcn.symbol             = b.value("symbol",             "[");
            bcn.overlay            = b.value("overlay",            "/");
            bcn.micE               = b.value("micE",               "");
            bcn.comment            = b.value("comment",            "");
            bcn.status             = b.value("status",             "");
            bcn.smartBeaconActive  = b.value("smartBeaconActive",  true);
            bcn.smartBeaconSetting = b.value("smartBeaconSetting", 0);
            bcn.sbSlowRate         = b.value("sbSlowRate",         0);
            bcn.sbFastRate         = b.value("sbFastRate",         0);
            bcn.sbMinSpeed         = b.value("sbMinSpeed",         0);
            bcn.sbMaxSpeed         = b.value("sbMaxSpeed",         0);
            bcn.sbMinTurnAngle     = b.value("sbMinTurnAngle",     0);
            bcn.sbTurnSlope        = b.value("sbTurnSlope",        0);
            bcn.sbMinBeaconTime    = b.value("sbMinBeaconTime",    0);
            bcn.gpsEcoMode         = b.value("gpsEcoMode",         false);
            bcn.profileLabel       = b.value("profileLabel",       "");
            // uppercase callsign
            for (char& c : bcn.callsign.s) c = (char)toupper((unsigned char)c);
            beacons.push_back(bcn);
        }
    }

    // Display
    if (data.contains("display")) {
        const auto& d = data["display"];
        display.ecoMode    = d.value("ecoMode",    false);
        display.timeout    = d.value("timeout",    4);
        display.turn180    = d.value("turn180",    false);
        display.showSymbol = d.value("showSymbol", true);
    }

    // GPSConfig
    if (data.contains("gpsConfig"))
        gpsConfig.strict3DFix = data["gpsConfig"].value("strict3DFix", false);

    // Bluetooth
    if (data.contains("bluetooth")) {
        const auto& bt = data["bluetooth"];
        bluetooth.active     = bt.value("active",     false);
        bluetooth.deviceName = bt.value("deviceName", "LoRaTracker");
        bluetooth.useBLE     = bt.value("useBLE",     true);
        bluetooth.useKISS    = bt.value("useKISS",    true);
    }

    // APRS-IS
    if (data.contains("aprs_is")) {
        const auto& a = data["aprs_is"];
        aprs_is.active   = a.value("active",   false);
        aprs_is.server   = a.value("server",   "euro.aprs2.net");
        aprs_is.port     = a.value("port",     14580);
        aprs_is.passcode = a.value("passcode", "-1");
    }

    // LoRa types
    if (data.contains("lora") && data["lora"].is_array()) {
        loraTypes.clear();
        for (const auto& l : data["lora"]) {
            LoraType lt;
            lt.frequency       = l.value("frequency",       433775000);
            lt.spreadingFactor = l.value("spreadingFactor", 12);
            lt.signalBandwidth = l.value("signalBandwidth", 125000);
            lt.codingRate4     = l.value("codingRate4",     5);
            lt.power           = l.value("power",           20);
            // dataRate: recalculate from SF/CR/BW
            int sf = lt.spreadingFactor, cr = lt.codingRate4;
            long bw = lt.signalBandwidth;
            if (bw == 125000) {
                struct { int sf; int cr; int rate; } presets[] = {
                    {12,5,300},{12,6,244},{12,7,209},{12,8,183},{10,8,610},{9,7,1200}
                };
                lt.dataRate = 300;
                for (auto& p : presets) if (p.sf==sf && p.cr==cr) { lt.dataRate=p.rate; break; }
            } else {
                double rate = (double)sf * ((double)bw/(1<<sf)) * (4.0/cr);
                lt.dataRate = (int)(rate+0.5);
            }
            loraTypes.push_back(lt);
        }
    }

    // Battery
    if (data.contains("battery")) {
        const auto& b = data["battery"];
        battery.sendVoltage        = b.value("sendVoltage",        false);
        battery.voltageAsTelemetry = b.value("voltageAsTelemetry", false);
        battery.sendVoltageAlways  = b.value("sendVoltageAlways",  false);
        battery.monitorVoltage     = b.value("monitorVoltage",     false);
        battery.sleepVoltage       = b.value("sleepVoltage",       2.9f);
    }

    // LoRa config
    if (data.contains("loraConfig")) {
        lora.sendInfo     = data["loraConfig"].value("sendInfo",     true);
        lora.repeaterMode = data["loraConfig"].value("repeaterMode", false);
    }

    // Telemetry
    if (data.contains("telemetry")) {
        const auto& t = data["telemetry"];
        telemetry.active                = t.value("active",                false);
        telemetry.sendTelemetry         = t.value("sendTelemetry",         false);
        telemetry.temperatureCorrection = t.value("temperatureCorrection", 0.0f);
    }

    // Winlink
    if (data.contains("winlink"))
        winlink.password = data["winlink"].value("password", "");

    // Notification
    if (data.contains("notification")) {
        const auto& n = data["notification"];
        notification.ledTx            = n.value("ledTx",            false);
        notification.ledTxPin         = n.value("ledTxPin",         -1);
        notification.ledMessage       = n.value("ledMessage",       false);
        notification.ledMessagePin    = n.value("ledMessagePin",    -1);
        notification.buzzerActive     = n.value("buzzerActive",     false);
        notification.buzzerPinTone    = n.value("buzzerPinTone",    -1);
        notification.buzzerPinVcc     = n.value("buzzerPinVcc",     -1);
        notification.volume           = n.value("volume",           50);
        notification.bootUpBeep       = n.value("bootUpBeep",       false);
        notification.txBeep           = n.value("txBeep",           false);
        notification.messageRxBeep    = n.value("messageRxBeep",    false);
        notification.stationBeep      = n.value("stationBeep",      false);
        notification.lowBatteryBeep   = n.value("lowBatteryBeep",   false);
        notification.shutDownBeep     = n.value("shutDownBeep",     false);
        notification.ledFlashlight    = n.value("ledFlashlight",    false);
        notification.ledFlashlightPin = n.value("ledFlashlightPin", -1);
    }

    // PTT
    if (data.contains("pttTrigger")) {
        const auto& p = data["pttTrigger"];
        ptt.active   = p.value("active",   false);
        ptt.io_pin   = p.value("io_pin",   -1);
        ptt.preDelay = p.value("preDelay", 0);
        ptt.postDelay= p.value("postDelay",0);
        ptt.reverse  = p.value("reverse",  false);
    }

    // Other
    if (data.contains("other")) {
        const auto& o = data["other"];
        simplifiedTrackerMode    = o.value("simplifiedTrackerMode",    false);
        sendCommentAfterXBeacons = o.value("sendCommentAfterXBeacons", 10);
        path                     = o.value("path",                     "WIDE2-1");
        nonSmartBeaconRate       = o.value("nonSmartBeaconRate",       10);
        rememberStationTime      = o.value("rememberStationTime",      30);
        standingUpdateTime       = o.value("standingUpdateTime",       15);
        sendAltitude             = o.value("sendAltitude",             true);
        disableGPS               = o.value("disableGPS",               false);
        email                    = o.value("email",                    "");
    }

    ESP_LOGI(TAG, "Config loaded: %s, freq=%ldHz, path=%s",
             beacons.empty() ? "NOCALL" : beacons[0].callsign.c_str(),
             loraTypes.empty() ? 0L : loraTypes[0].frequency,
             path.c_str());
    return true;
}

bool Configuration::writeFile() {
    json data;

    for (size_t i = 0; i < wifiAPs.size(); i++) {
        data["wifi"]["AP"][i]["ssid"]     = wifiAPs[i].ssid.s;
        data["wifi"]["AP"][i]["password"] = wifiAPs[i].password.s;
    }
    data["wifi"]["autoAP"]["active"]   = wifiAutoAP.active;
    data["wifi"]["autoAP"]["password"] = wifiAutoAP.password.s;
    data["wifi"]["autoAP"]["timeout"]  = wifiAutoAP.timeout;
    data["wifi"]["enabled"]            = wifiEnabled;

    for (size_t i = 0; i < beacons.size(); i++) {
        auto& b = beacons[i];
        data["beacons"][i]["callsign"]           = b.callsign.s;
        data["beacons"][i]["symbol"]             = b.symbol.s;
        data["beacons"][i]["overlay"]            = b.overlay.s;
        data["beacons"][i]["micE"]               = b.micE.s;
        data["beacons"][i]["comment"]            = b.comment.s;
        data["beacons"][i]["smartBeaconActive"]  = b.smartBeaconActive;
        data["beacons"][i]["smartBeaconSetting"] = b.smartBeaconSetting;
        data["beacons"][i]["sbSlowRate"]     = b.sbSlowRate     ? b.sbSlowRate     : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 0);
        data["beacons"][i]["sbFastRate"]     = b.sbFastRate     ? b.sbFastRate     : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 1);
        data["beacons"][i]["sbMinSpeed"]     = b.sbMinSpeed     ? b.sbMinSpeed     : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 2);
        data["beacons"][i]["sbMaxSpeed"]     = b.sbMaxSpeed     ? b.sbMaxSpeed     : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 3);
        data["beacons"][i]["sbMinTurnAngle"] = b.sbMinTurnAngle ? b.sbMinTurnAngle : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 4);
        data["beacons"][i]["sbTurnSlope"]    = b.sbTurnSlope    ? b.sbTurnSlope    : SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 5);
        data["beacons"][i]["sbMinBeaconTime"]= b.sbMinBeaconTime? b.sbMinBeaconTime: SMARTBEACON_Utils::getProfileDefault(b.smartBeaconSetting, 6);
        data["beacons"][i]["gpsEcoMode"]     = b.gpsEcoMode;
        data["beacons"][i]["profileLabel"]   = b.profileLabel.s;
        data["beacons"][i]["status"]         = b.status.s;
    }

    data["display"]["ecoMode"]    = display.ecoMode;
    data["display"]["timeout"]    = display.timeout;
    data["display"]["turn180"]    = display.turn180;
    data["display"]["showSymbol"] = display.showSymbol;

    data["gpsConfig"]["strict3DFix"] = gpsConfig.strict3DFix;

    data["bluetooth"]["active"]     = bluetooth.active;
    data["bluetooth"]["deviceName"] = bluetooth.deviceName.s;
    data["bluetooth"]["useBLE"]     = bluetooth.useBLE;
    data["bluetooth"]["useKISS"]    = bluetooth.useKISS;

    data["aprs_is"]["active"]   = aprs_is.active;
    data["aprs_is"]["server"]   = aprs_is.server.s;
    data["aprs_is"]["port"]     = aprs_is.port;
    data["aprs_is"]["passcode"] = aprs_is.passcode.s;

    for (size_t i = 0; i < loraTypes.size(); i++) {
        data["lora"][i]["frequency"]       = loraTypes[i].frequency;
        data["lora"][i]["spreadingFactor"] = loraTypes[i].spreadingFactor;
        data["lora"][i]["signalBandwidth"] = loraTypes[i].signalBandwidth;
        data["lora"][i]["codingRate4"]     = loraTypes[i].codingRate4;
        data["lora"][i]["power"]           = loraTypes[i].power;
        data["lora"][i]["dataRate"]        = loraTypes[i].dataRate;
    }

    data["battery"]["sendVoltage"]        = battery.sendVoltage;
    data["battery"]["voltageAsTelemetry"] = battery.voltageAsTelemetry;
    data["battery"]["sendVoltageAlways"]  = battery.sendVoltageAlways;
    data["battery"]["monitorVoltage"]     = battery.monitorVoltage;
    data["battery"]["sleepVoltage"]       = battery.sleepVoltage;

    data["loraConfig"]["sendInfo"]     = lora.sendInfo;
    data["loraConfig"]["repeaterMode"] = lora.repeaterMode;

    data["telemetry"]["active"]                = telemetry.active;
    data["telemetry"]["sendTelemetry"]         = telemetry.sendTelemetry;
    data["telemetry"]["temperatureCorrection"] = telemetry.temperatureCorrection;

    data["winlink"]["password"] = winlink.password.s;

    data["notification"]["ledTx"]            = notification.ledTx;
    data["notification"]["ledTxPin"]         = notification.ledTxPin;
    data["notification"]["ledMessage"]       = notification.ledMessage;
    data["notification"]["ledMessagePin"]    = notification.ledMessagePin;
    data["notification"]["buzzerActive"]     = notification.buzzerActive;
    data["notification"]["volume"]           = notification.volume;
    data["notification"]["txBeep"]           = notification.txBeep;
    data["notification"]["messageRxBeep"]    = notification.messageRxBeep;

    data["pttTrigger"]["active"]    = ptt.active;
    data["pttTrigger"]["io_pin"]    = ptt.io_pin;
    data["pttTrigger"]["preDelay"]  = ptt.preDelay;
    data["pttTrigger"]["postDelay"] = ptt.postDelay;
    data["pttTrigger"]["reverse"]   = ptt.reverse;

    data["other"]["simplifiedTrackerMode"]    = simplifiedTrackerMode;
    data["other"]["sendCommentAfterXBeacons"] = sendCommentAfterXBeacons;
    data["other"]["path"]                     = path.s;
    data["other"]["nonSmartBeaconRate"]       = nonSmartBeaconRate;
    data["other"]["rememberStationTime"]      = rememberStationTime;
    data["other"]["standingUpdateTime"]       = standingUpdateTime;
    data["other"]["sendAltitude"]             = sendAltitude;
    data["other"]["disableGPS"]               = disableGPS;
    data["other"]["email"]                    = email.s;

    std::ofstream f(CONFIG_PATH);
    if (!f.is_open()) {
        ESP_LOGE(TAG, "Cannot write config to %s", CONFIG_PATH);
        return false;
    }
    f << data.dump(2);
    ESP_LOGI(TAG, "Config saved");
    return true;
}

void Configuration::init() {
    setDefaultValues();
    if (!readFile()) {
        writeFile();
    }
}

bool Configuration::reload() {
    return readFile();
}
