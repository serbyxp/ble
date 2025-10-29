#pragma once

#include <Arduino.h>
#include <IPAddress.h>

struct WifiManagerStatus
{
  bool hasCredentials;
  bool connected;
  bool accessPointActive;
  String connectedSsid;
  IPAddress stationIp;
  IPAddress accessPointIp;
};

void wifiManagerInitialize();
void wifiManagerLoop();
WifiManagerStatus wifiManagerGetStatus();
const char *wifiManagerAccessPointSsid();
const char *wifiManagerAccessPointPassword();
bool wifiManagerSetCredentials(const String &ssid, const String &password);
bool wifiManagerForgetCredentials();
void wifiManagerEnsureAccessPoint();
