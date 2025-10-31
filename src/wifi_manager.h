#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <vector>

// Public API
void wifiManagerInitialize();
void wifiManagerLoop();

bool wifiManagerHasCredentials();
bool wifiManagerIsConnected();
bool wifiManagerIsConnecting();
bool wifiManagerIsAccessPointActive();

String wifiManagerConnectedSSID();
IPAddress wifiManagerLocalIp();
IPAddress wifiManagerApIp();

struct WifiManagerStatus
{
  bool hasCredentials = false;
  bool connected = false;
  bool connecting = false;
  bool accessPointActive = false;
  String connectedSsid;
  IPAddress stationIp;
  IPAddress accessPointIp;
};

WifiManagerStatus wifiManagerGetStatus();
const char *wifiManagerAccessPointSsid();

bool wifiManagerSetCredentials(const String &ssid, const String &password);
bool wifiManagerForgetCredentials();

// Keep AP running unless actively connected in STA.
// Force AP up (no-op if already on).
void wifiManagerEnsureAccessPoint();

// Scan utilities (blocking, short)
struct WifiScanResult
{
  String ssid;
  int32_t rssi;
  bool hidden;
  bool secure;
};
std::vector<WifiScanResult> wifiManagerScanNetworks();
