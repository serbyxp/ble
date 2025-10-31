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

void wifiManagerSetCredentials(const String &ssid, const String &password);
void wifiManagerForgetCredentials();

// Keep AP running unless actively connected in STA.
// Force AP up (no-op if already on).
void wifiManagerEnsureAccessPoint();

// Scan utilities (blocking, short)
struct ScanResult
{
  String ssid;
  int32_t rssi;
  wifi_auth_mode_t authmode;
  uint8_t *bssid; // optional (can be nullptr)
  int32_t channel;
  bool hidden;
};
std::vector<ScanResult> wifiManagerScanNetworks();
