#pragma once
#include <Arduino.h>
#include <vector>

class WiFi_AP {
public:
    String ssid;
    String password;
};

class WiFi_Auto_AP {
public:
    bool   active;
    String password;
    int    timeout;
};

class Beacon {
public:
    String callsign;
    String symbol;
    String overlay;
    String micE;
    String comment;
    bool   smartBeaconActive;
    byte   smartBeaconSetting;
    int    sbSlowRate;
    int    sbFastRate;
    int    sbMinSpeed;
    int    sbMaxSpeed;
    int    sbMinTurnAngle;
    int    sbTurnSlope;
    int    sbMinBeaconTime;
    bool   gpsEcoMode;
    String profileLabel;
    String status;
};

class Display {
public:
    bool showSymbol;
    bool ecoMode;
    int  timeout;
    bool turn180;
};

class GPSConfig {
public:
    bool strict3DFix;
};

class Battery {
public:
    bool  sendVoltage;
    bool  voltageAsTelemetry;
    bool  sendVoltageAlways;
    bool  monitorVoltage;
    float sleepVoltage;
};

class Winlink {
public:
    String password;
};

class Telemetry {
public:
    bool  active;
    bool  sendTelemetry;
    float temperatureCorrection;
};

class Notification {
public:
    bool ledTx;
    int  ledTxPin;
    bool ledMessage;
    int  ledMessagePin;
    bool ledFlashlight;
    int  ledFlashlightPin;
    bool buzzerActive;
    int  buzzerPinTone;
    int  buzzerPinVcc;
    int  volume;
    bool bootUpBeep;
    bool txBeep;
    bool messageRxBeep;
    bool stationBeep;
    bool lowBatteryBeep;
    bool shutDownBeep;
};

class LoraType {
public:
    String profileName;
    long frequency;
    int  spreadingFactor;
    long signalBandwidth;
    int  codingRate4;
    int  power;
    int  dataRate;
};

class Lora {
public:
    bool sendInfo;
    bool repeaterMode;
    // Alias consumed when repeating others, kept apart from the beacon path so
    // enabling the digipeater cannot shorten our own beacons.
    String digipeatAlias;
};

class PTT {
public:
    bool active;
    int  io_pin;
    int  preDelay;
    int  postDelay;
    bool reverse;
};

class BLUETOOTH {
public:
    bool   active;
    String deviceName;
    bool   useBLE;
    bool   useKISS;
};

class APRS_IS {
public:
    bool   active;
    String server;
    int    port;
    String passcode;
};

class Configuration {
public:
    std::vector<WiFi_AP>  wifiAPs;
    WiFi_Auto_AP          wifiAutoAP;
    bool                  wifiEnabled;
    std::vector<Beacon>   beacons;
    Display               display;
    Battery               battery;
    Winlink               winlink;
    Telemetry             telemetry;
    GPSConfig             gpsConfig;
    Notification          notification;
    std::vector<LoraType> loraTypes;
    Lora                  lora;
    PTT                   ptt;
    BLUETOOTH             bluetooth;
    APRS_IS               aprs_is;

    bool   simplifiedTrackerMode;
    int    sendCommentAfterXBeacons;
    String path;
    String email;
    int    nonSmartBeaconRate;
    int    rememberStationTime;
    int    standingUpdateTime;
    bool   sendAltitude;
    bool   disableGPS;

    void setDefaultValues();
    bool writeFile();
    void init();
    bool reload();
    Configuration();

private:
    bool readFile();
};
