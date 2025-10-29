#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

struct WifiManagerStatus
{
  bool hasCredentials;
  bool connected;
  bool connecting;
  bool accessPointActive;
  String connectedSsid;
  IPAddress stationIp;
  IPAddress accessPointIp;
};

struct WifiScanResult
{
  String ssid;
  int32_t rssi;
  bool secure;
  bool hidden;
};

void wifiManagerInitialize();
void wifiManagerLoop();
WifiManagerStatus wifiManagerGetStatus();
bool wifiManagerIsConnecting();
const char *wifiManagerAccessPointSsid();
const char *wifiManagerAccessPointPassword();
bool wifiManagerSetCredentials(const String &ssid, const String &password);
bool wifiManagerForgetCredentials();
void wifiManagerEnsureAccessPoint();
std::vector<WifiScanResult> wifiManagerScanNetworks();
