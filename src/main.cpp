#include <Arduino.h>
#include <BleCombo.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <strings.h>
#include <vector>

namespace
{
constexpr size_t JSON_DOC_CAPACITY = 512;
constexpr size_t INPUT_BUFFER_LIMIT = 512;
constexpr size_t MAX_KEY_COMBO = 8;
constexpr uint8_t MOUSE_ALL_BUTTONS = MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE | MOUSE_BACK | MOUSE_FORWARD;
constexpr uint16_t DEFAULT_CHAR_DELAY_MS = 6;
constexpr const char *NVS_NAMESPACE_WIFI = "wifi";
constexpr const char *NVS_KEY_SSID = "ssid";
constexpr const char *NVS_KEY_PASS = "password";
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long WIFI_RETRY_DELAY_MS = 500;
constexpr const char *CONFIG_AP_SSID = "uhid-setup";
constexpr const char *CONFIG_AP_PASSWORD = "uhid1234";

struct NamedCode
{
  const char *name;
  uint8_t code;
};

const NamedCode KEY_NAME_MAP[] = {
    {"KEY_RETURN", KEY_RETURN},
    {"RETURN", KEY_RETURN},
    {"ENTER", KEY_RETURN},
    {"KEY_ESC", KEY_ESC},
    {"ESC", KEY_ESC},
    {"ESCAPE", KEY_ESC},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"BACKSPACE", KEY_BACKSPACE},
    {"KEY_TAB", KEY_TAB},
    {"TAB", KEY_TAB},
    {"KEY_DELETE", KEY_DELETE},
    {"DELETE", KEY_DELETE},
    {"DEL", KEY_DELETE},
    {"KEY_INSERT", KEY_INSERT},
    {"INSERT", KEY_INSERT},
    {"INS", KEY_INSERT},
    {"KEY_PAGE_UP", KEY_PAGE_UP},
    {"PAGE_UP", KEY_PAGE_UP},
    {"PGUP", KEY_PAGE_UP},
    {"KEY_PAGE_DOWN", KEY_PAGE_DOWN},
    {"PAGE_DOWN", KEY_PAGE_DOWN},
    {"PGDN", KEY_PAGE_DOWN},
    {"KEY_HOME", KEY_HOME},
    {"HOME", KEY_HOME},
    {"KEY_END", KEY_END},
    {"END", KEY_END},
    {"KEY_RIGHT_ARROW", KEY_RIGHT_ARROW},
    {"RIGHT", KEY_RIGHT_ARROW},
    {"KEY_LEFT_ARROW", KEY_LEFT_ARROW},
    {"LEFT", KEY_LEFT_ARROW},
    {"KEY_UP_ARROW", KEY_UP_ARROW},
    {"UP", KEY_UP_ARROW},
    {"KEY_DOWN_ARROW", KEY_DOWN_ARROW},
    {"DOWN", KEY_DOWN_ARROW},
    {"KEY_CAPS_LOCK", KEY_CAPS_LOCK},
    {"CAPS_LOCK", KEY_CAPS_LOCK},
    {"CAPSLOCK", KEY_CAPS_LOCK},
    {"KEY_LEFT_CTRL", KEY_LEFT_CTRL},
    {"LEFT_CTRL", KEY_LEFT_CTRL},
    {"CTRL", KEY_LEFT_CTRL},
    {"CONTROL", KEY_LEFT_CTRL},
    {"KEY_RIGHT_CTRL", KEY_RIGHT_CTRL},
    {"RIGHT_CTRL", KEY_RIGHT_CTRL},
    {"KEY_LEFT_SHIFT", KEY_LEFT_SHIFT},
    {"LEFT_SHIFT", KEY_LEFT_SHIFT},
    {"SHIFT", KEY_LEFT_SHIFT},
    {"KEY_RIGHT_SHIFT", KEY_RIGHT_SHIFT},
    {"RIGHT_SHIFT", KEY_RIGHT_SHIFT},
    {"KEY_LEFT_ALT", KEY_LEFT_ALT},
    {"LEFT_ALT", KEY_LEFT_ALT},
    {"ALT", KEY_LEFT_ALT},
    {"KEY_RIGHT_ALT", KEY_RIGHT_ALT},
    {"RIGHT_ALT", KEY_RIGHT_ALT},
    {"ALTGR", KEY_RIGHT_ALT},
    {"KEY_LEFT_GUI", KEY_LEFT_GUI},
    {"LEFT_GUI", KEY_LEFT_GUI},
    {"LGUI", KEY_LEFT_GUI},
    {"GUI", KEY_LEFT_GUI},
    {"WIN", KEY_LEFT_GUI},
    {"WINDOWS", KEY_LEFT_GUI},
    {"COMMAND", KEY_LEFT_GUI},
    {"KEY_RIGHT_GUI", KEY_RIGHT_GUI},
    {"RIGHT_GUI", KEY_RIGHT_GUI},
    {"RGUI", KEY_RIGHT_GUI},
    {"SPACE", ' '},
    {"SPACEBAR", ' '}};

const size_t KEY_NAME_MAP_SIZE = sizeof(KEY_NAME_MAP) / sizeof(KEY_NAME_MAP[0]);

struct ButtonEntry
{
  const char *name;
  uint8_t mask;
};

const ButtonEntry MOUSE_BUTTON_MAP[] = {
    {"LEFT", MOUSE_LEFT},
    {"MOUSE_LEFT", MOUSE_LEFT},
    {"BUTTON1", MOUSE_LEFT},
    {"RIGHT", MOUSE_RIGHT},
    {"MOUSE_RIGHT", MOUSE_RIGHT},
    {"BUTTON2", MOUSE_RIGHT},
    {"MIDDLE", MOUSE_MIDDLE},
    {"SCROLL", MOUSE_MIDDLE},
    {"WHEEL", MOUSE_MIDDLE},
    {"BUTTON3", MOUSE_MIDDLE},
    {"BACK", MOUSE_BACK},
    {"BUTTON4", MOUSE_BACK},
    {"FORWARD", MOUSE_FORWARD},
    {"BUTTON5", MOUSE_FORWARD}};

const size_t MOUSE_BUTTON_MAP_SIZE = sizeof(MOUSE_BUTTON_MAP) / sizeof(MOUSE_BUTTON_MAP[0]);

struct ConsumerEntry
{
  const char *name;
  const MediaKeyReport *report;
};

const ConsumerEntry CONSUMER_KEY_MAP[] = {
    {"KEY_MEDIA_PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"MEDIA_PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"KEY_MEDIA_STOP", &KEY_MEDIA_STOP},
    {"MEDIA_STOP", &KEY_MEDIA_STOP},
    {"KEY_MEDIA_NEXT_TRACK", &KEY_MEDIA_NEXT_TRACK},
    {"MEDIA_NEXT", &KEY_MEDIA_NEXT_TRACK},
    {"NEXT_TRACK", &KEY_MEDIA_NEXT_TRACK},
    {"KEY_MEDIA_PREVIOUS_TRACK", &KEY_MEDIA_PREVIOUS_TRACK},
    {"MEDIA_PREVIOUS", &KEY_MEDIA_PREVIOUS_TRACK},
    {"MEDIA_PREV", &KEY_MEDIA_PREVIOUS_TRACK},
    {"PREVIOUS_TRACK", &KEY_MEDIA_PREVIOUS_TRACK},
    {"KEY_MEDIA_VOLUME_UP", &KEY_MEDIA_VOLUME_UP},
    {"VOLUME_UP", &KEY_MEDIA_VOLUME_UP},
    {"KEY_MEDIA_VOLUME_DOWN", &KEY_MEDIA_VOLUME_DOWN},
    {"VOLUME_DOWN", &KEY_MEDIA_VOLUME_DOWN},
    {"KEY_MEDIA_MUTE", &KEY_MEDIA_MUTE},
    {"MUTE", &KEY_MEDIA_MUTE},
    {"KEY_MEDIA_WWW_HOME", &KEY_MEDIA_WWW_HOME},
    {"WWW_HOME", &KEY_MEDIA_WWW_HOME},
    {"KEY_MEDIA_EMAIL_READER", &KEY_MEDIA_EMAIL_READER},
    {"EMAIL", &KEY_MEDIA_EMAIL_READER},
    {"KEY_MEDIA_CALCULATOR", &KEY_MEDIA_CALCULATOR},
    {"CALCULATOR", &KEY_MEDIA_CALCULATOR},
    {"KEY_MEDIA_WWW_SEARCH", &KEY_MEDIA_WWW_SEARCH},
    {"WWW_SEARCH", &KEY_MEDIA_WWW_SEARCH},
    {"KEY_MEDIA_WWW_STOP", &KEY_MEDIA_WWW_STOP},
    {"WWW_STOP", &KEY_MEDIA_WWW_STOP},
    {"KEY_MEDIA_WWW_BACK", &KEY_MEDIA_WWW_BACK},
    {"WWW_BACK", &KEY_MEDIA_WWW_BACK},
    {"KEY_MEDIA_WWW_BOOKMARKS", &KEY_MEDIA_WWW_BOOKMARKS},
    {"WWW_BOOKMARKS", &KEY_MEDIA_WWW_BOOKMARKS}};

const size_t CONSUMER_KEY_MAP_SIZE = sizeof(CONSUMER_KEY_MAP) / sizeof(CONSUMER_KEY_MAP[0]);

constexpr size_t MAX_CONSUMER_KEYS = 8;

String inputBuffer;
bool lastBleConnectionState = false;
bool configurationMode = false;

bool initializeNvs()
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    if (nvs_flash_erase() != ESP_OK)
    {
      return false;
    }
    err = nvs_flash_init();
  }
  return err == ESP_OK;
}

bool loadWifiCredentials(String &ssid, String &password)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &handle);
  if (err != ESP_OK)
  {
    return false;
  }

  size_t ssidLength = 0;
  err = nvs_get_str(handle, NVS_KEY_SSID, nullptr, &ssidLength);
  if (err != ESP_OK || ssidLength <= 1)
  {
    nvs_close(handle);
    return false;
  }

  std::vector<char> ssidBuffer(ssidLength);
  err = nvs_get_str(handle, NVS_KEY_SSID, ssidBuffer.data(), &ssidLength);
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  size_t passwordLength = 0;
  err = nvs_get_str(handle, NVS_KEY_PASS, nullptr, &passwordLength);
  if (err == ESP_ERR_NVS_NOT_FOUND)
  {
    passwordLength = 1;
  }
  else if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  std::vector<char> passwordBuffer(passwordLength);
  if (passwordLength > 1)
  {
    err = nvs_get_str(handle, NVS_KEY_PASS, passwordBuffer.data(), &passwordLength);
    if (err != ESP_OK)
    {
      nvs_close(handle);
      return false;
    }
  }
  else
  {
    passwordBuffer[0] = '\0';
  }

  nvs_close(handle);

  ssid = String(ssidBuffer.data());
  password = String(passwordBuffer.data());
  return true;
}

bool saveWifiCredentials(const String &ssid, const String &password)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle);
  if (err != ESP_OK)
  {
    return false;
  }

  err = nvs_set_str(handle, NVS_KEY_SSID, ssid.c_str());
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  err = nvs_set_str(handle, NVS_KEY_PASS, password.c_str());
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

void stopAccessPoint()
{
  wifi_mode_t mode = WiFi.getMode();
  if (mode & WIFI_MODE_AP)
  {
    WiFi.softAPdisconnect(true);
    if (mode == WIFI_MODE_AP)
    {
      WiFi.mode(WIFI_OFF);
    }
    else if (mode == WIFI_MODE_APSTA)
    {
      WiFi.mode(WIFI_MODE_STA);
    }
    configurationMode = false;
  }
}

void startAccessPoint()
{
  WiFi.mode(WIFI_MODE_APSTA);
  IPAddress localIp(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, gateway, subnet);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  configurationMode = true;
}

bool connectToStation(const String &ssid, const String &password)
{
  if (ssid.isEmpty())
  {
    return false;
  }

  WiFi.mode(WIFI_MODE_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startAttempt = millis();
  while ((millis() - startAttempt) < WIFI_CONNECT_TIMEOUT_MS)
  {
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED)
    {
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST || status == WL_DISCONNECTED)
    {
      WiFi.reconnect();
    }
    delay(WIFI_RETRY_DELAY_MS);
  }

  WiFi.disconnect();
  return false;
}

void sendStatusOk()
{
  Serial.println(F("{\"status\":\"ok\"}"));
}

void sendStatusError(const char *message)
{
  Serial.print(F("{\"status\":\"error\",\"message\":\""));
  Serial.print(message);
  Serial.println(F("\"}"));
}

void sendEvent(const char *name, const char *detail = nullptr)
{
  Serial.print(F("{\"event\":\""));
  Serial.print(name);
  if (detail)
  {
    Serial.print(F("\",\"detail\":\""));
    Serial.print(detail);
    Serial.println(F("\"}"));
  }
  else
  {
    Serial.println(F("\"}"));
  }
}

bool connectStationAndPersist(const String &ssid, const String &password)
{
  if (!connectToStation(ssid, password))
  {
    return false;
  }

  if (!saveWifiCredentials(ssid, password))
  {
    sendStatusError("Failed to save WiFi credentials");
  }
  stopAccessPoint();
  configurationMode = false;
  sendEvent("wifi_sta_connected", ssid.c_str());
  return true;
}

bool lookupKeyCode(const String &token, uint8_t &code)
{
  for (size_t i = 0; i < KEY_NAME_MAP_SIZE; ++i)
  {
    if (token.equals(KEY_NAME_MAP[i].name))
    {
      code = KEY_NAME_MAP[i].code;
      return true;
    }
  }
  return false;
}

bool lookupConsumerKey(const String &token, const MediaKeyReport *&report)
{
  for (size_t i = 0; i < CONSUMER_KEY_MAP_SIZE; ++i)
  {
    if (token.equals(CONSUMER_KEY_MAP[i].name))
    {
      report = CONSUMER_KEY_MAP[i].report;
      return true;
    }
  }
  return false;
}

void reportInvalidKey(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown key: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else if (value.is<int>())
  {
    String message = F("Invalid key code: ");
    message += String(value.as<int>());
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid key entry");
  }
}

bool parseKeyCode(JsonVariantConst value, uint8_t &code)
{
  if (value.is<int>())
  {
    int raw = value.as<int>();
    if (raw < 0 || raw > 255)
    {
      return false;
    }
    code = static_cast<uint8_t>(raw);
    return true;
  }

  if (value.is<const char *>())
  {
    String token = value.as<const char *>();
    token.trim();
    if (!token.length())
    {
      return false;
    }

    if (token.length() == 1)
    {
      code = static_cast<uint8_t>(token[0]);
      return true;
    }

    if (token.startsWith("0x") || token.startsWith("0X"))
    {
      long parsed = strtol(token.c_str(), nullptr, 16);
      if (parsed >= 0 && parsed <= 255)
      {
        code = static_cast<uint8_t>(parsed);
        return true;
      }
    }

    String upper = token;
    upper.toUpperCase();

    if (lookupKeyCode(upper, code))
    {
      return true;
    }

    if (upper.startsWith("KEY_"))
    {
      String shortened = upper.substring(4);
      if (lookupKeyCode(shortened, code))
      {
        return true;
      }
    }

    if (upper.startsWith("F"))
    {
      long fn = upper.substring(1).toInt();
      if (fn >= 1 && fn <= 24)
      {
        code = KEY_F1 + static_cast<uint8_t>(fn - 1);
        return true;
      }
    }

    if (upper.startsWith("KEY_F"))
    {
      long fn = upper.substring(5).toInt();
      if (fn >= 1 && fn <= 24)
      {
        code = KEY_F1 + static_cast<uint8_t>(fn - 1);
        return true;
      }
    }
  }

  return false;
}

bool collectKeyCodes(JsonVariantConst source, uint8_t *codes, size_t &count, size_t maxCount)
{
  count = 0;
  if (source.is<JsonArrayConst>())
  {
    JsonArrayConst arr = source.as<JsonArrayConst>();
    for (JsonVariantConst key : arr)
    {
      if (count >= maxCount)
      {
        sendStatusError("Too many keys in combo");
        return false;
      }
      if (!parseKeyCode(key, codes[count]))
      {
        reportInvalidKey(key);
        return false;
      }
      ++count;
    }
    return count > 0;
  }

  if (!source.isNull())
  {
    if (!parseKeyCode(source, codes[0]))
    {
      reportInvalidKey(source);
      return false;
    }
    count = 1;
    return true;
  }

  return false;
}

bool extractKeyCodes(JsonVariantConst command, uint8_t *codes, size_t &count, size_t maxCount = MAX_KEY_COMBO)
{
  JsonVariantConst keys = command["keys"];
  if (!keys.isNull())
  {
    return collectKeyCodes(keys, codes, count, maxCount);
  }

  JsonVariantConst key = command["key"];
  if (!key.isNull())
  {
    return collectKeyCodes(key, codes, count, maxCount);
  }

  JsonVariantConst code = command["code"];
  if (!code.isNull())
  {
    return collectKeyCodes(code, codes, count, maxCount);
  }

  sendStatusError("keyboard action requires key(s) or code");
  return false;
}

void reportInvalidButton(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown mouse button: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else if (value.is<int>())
  {
    String message = F("Invalid mouse button mask: ");
    message += String(value.as<int>());
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid mouse button entry");
  }
}

bool addButtonByName(const String &token, uint8_t &mask)
{
  for (size_t i = 0; i < MOUSE_BUTTON_MAP_SIZE; ++i)
  {
    if (token.equals(MOUSE_BUTTON_MAP[i].name))
    {
      mask |= MOUSE_BUTTON_MAP[i].mask;
      return true;
    }
  }
  return false;
}

bool parseButtonMask(JsonVariantConst value, uint8_t &mask)
{
  mask = 0;
  if (value.is<JsonArrayConst>())
  {
    JsonArrayConst arr = value.as<JsonArrayConst>();
    for (JsonVariantConst button : arr)
    {
      if (button.is<const char *>())
      {
        String token = button.as<const char *>();
        token.trim();
        token.toUpperCase();
        if (!addButtonByName(token, mask))
        {
          reportInvalidButton(button);
          return false;
        }
      }
      else if (button.is<int>())
      {
        mask |= static_cast<uint8_t>(button.as<int>());
      }
      else
      {
        reportInvalidButton(button);
        return false;
      }
    }
    return mask != 0;
  }

  if (value.is<const char *>())
  {
    String token = value.as<const char *>();
    token.trim();
    token.toUpperCase();
    if (addButtonByName(token, mask))
    {
      return true;
    }
    reportInvalidButton(value);
    return false;
  }

  if (value.is<int>())
  {
    int raw = value.as<int>();
    if (raw < 0 || raw > 255)
    {
      reportInvalidButton(value);
      return false;
    }
    mask = static_cast<uint8_t>(raw);
    return true;
  }

  reportInvalidButton(value);
  return false;
}

uint16_t clampRepeat(JsonVariantConst value)
{
  if (!value.is<int>())
  {
    return 1;
  }
  int repeat = value.as<int>();
  if (repeat < 1)
  {
    repeat = 1;
  }
  if (repeat > 100)
  {
    repeat = 100;
  }
  return static_cast<uint16_t>(repeat);
}

uint16_t clampDuration(JsonVariantConst value, uint16_t defaultValue, uint16_t minValue = 0, uint16_t maxValue = 1000)
{
  if (!value.is<int>())
  {
    return defaultValue;
  }

  int raw = value.as<int>();
  if (raw < static_cast<int>(minValue))
  {
    return minValue;
  }
  if (raw > static_cast<int>(maxValue))
  {
    return maxValue;
  }
  return static_cast<uint16_t>(raw);
}

int getOptionalInt(JsonVariantConst object, const char *primaryKey, const char *secondaryKey, int defaultValue)
{
  JsonVariantConst primary = object[primaryKey];
  if (!primary.isNull())
  {
    return primary.as<int>();
  }

  if (secondaryKey != nullptr)
  {
    JsonVariantConst secondary = object[secondaryKey];
    if (!secondary.isNull())
    {
      return secondary.as<int>();
    }
  }

  return defaultValue;
}

void reportInvalidConsumerKey(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown consumer key: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid consumer key entry");
  }
}

bool collectConsumerReports(JsonVariantConst source, const MediaKeyReport **reports, size_t &count, size_t maxCount)
{
  count = 0;
  if (source.is<JsonArrayConst>())
  {
    JsonArrayConst arr = source.as<JsonArrayConst>();
    for (JsonVariantConst entry : arr)
    {
      if (!entry.is<const char *>())
      {
        reportInvalidConsumerKey(entry);
        return false;
      }
      if (count >= maxCount)
      {
        sendStatusError("Too many consumer keys");
        return false;
      }
      String token = entry.as<const char *>();
      token.trim();
      token.toUpperCase();
      if (!lookupConsumerKey(token, reports[count]))
      {
        reportInvalidConsumerKey(entry);
        return false;
      }
      ++count;
    }
    return count > 0;
  }

  if (source.is<const char *>())
  {
    String token = source.as<const char *>();
    token.trim();
    token.toUpperCase();
    if (lookupConsumerKey(token, reports[0]))
    {
      count = 1;
      return true;
    }
    reportInvalidConsumerKey(source);
    return false;
  }

  reportInvalidConsumerKey(source);
  return false;
}

void handleKeyboard(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE keyboard not connected");
    return;
  }

  const char *action = command["action"] | "press";
  uint8_t codes[MAX_KEY_COMBO];
  size_t keyCount = 0;

  if (strcmp(action, "write") == 0 || strcmp(action, "print") == 0 || strcmp(action, "println") == 0)
  {
    const char *text = command["text"];
    uint16_t repeat = clampRepeat(command["repeat"]);
    bool addNewLine = strcmp(action, "println") == 0 || command["newline"].as<bool>();
    size_t textLength = text ? strlen(text) : 0;

    uint16_t charDelay = DEFAULT_CHAR_DELAY_MS;
    bool charDelaySpecified = false;
    auto updateCharDelay = [&](JsonVariantConst value) {
      if (!value.isNull())
      {
        charDelay = clampDuration(value, charDelay, 0, 1000);
        charDelaySpecified = true;
      }
    };
    updateCharDelay(command["charDelayMs"]);
    updateCharDelay(command["char_delay_ms"]);
    updateCharDelay(command["interKeyDelayMs"]);
    updateCharDelay(command["inter_key_delay_ms"]);
    if (!charDelaySpecified)
    {
      updateCharDelay(command["delayMs"]);
    }
    if (!charDelaySpecified)
    {
      updateCharDelay(command["delay_ms"]);
    }

    bool newlineCarriage = command["newlineCarriage"].isNull() ? true : command["newlineCarriage"].as<bool>();

    if (text)
    {
      for (uint16_t i = 0; i < repeat; ++i)
      {
        for (size_t idx = 0; idx < textLength; ++idx)
        {
          Keyboard.write(static_cast<uint8_t>(text[idx]));
          if (charDelay)
          {
            delay(charDelay);
          }
        }
        if (addNewLine)
        {
          if (newlineCarriage)
          {
            Keyboard.write('\r');
            if (charDelay)
            {
              delay(charDelay);
            }
          }
          Keyboard.write('\n');
          if (charDelay)
          {
            delay(charDelay);
          }
        }
      }
      sendStatusOk();
      return;
    }

    if (addNewLine)
    {
      for (uint16_t i = 0; i < repeat; ++i)
      {
        if (newlineCarriage)
        {
          Keyboard.write('\r');
          if (charDelay)
          {
            delay(charDelay);
          }
        }
        Keyboard.write('\n');
        if (charDelay)
        {
          delay(charDelay);
        }
      }
      sendStatusOk();
      return;
    }

    if (!extractKeyCodes(command, codes, keyCount))
    {
      return;
    }

    for (uint16_t i = 0; i < repeat; ++i)
    {
      for (size_t idx = 0; idx < keyCount; ++idx)
      {
        Keyboard.write(codes[idx]);
      }
      if (addNewLine)
      {
        Keyboard.println();
      }
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "releaseAll") == 0 || strcmp(action, "release_all") == 0)
  {
    Keyboard.releaseAll();
    sendStatusOk();
    return;
  }

  if (!extractKeyCodes(command, codes, keyCount))
  {
    return;
  }

  if (strcmp(action, "press") == 0)
  {
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.press(codes[idx]);
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "release") == 0)
  {
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.release(codes[idx]);
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "tap") == 0 || strcmp(action, "click") == 0)
  {
    int holdValue = command["holdMs"] | 20;
    if (!command["hold_ms"].isNull())
    {
      holdValue = command["hold_ms"].as<int>();
    }
    if (holdValue < 0)
    {
      holdValue = 0;
    }
    if (holdValue > 1000)
    {
      holdValue = 1000;
    }
    uint16_t holdMs = static_cast<uint16_t>(holdValue);
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.press(codes[idx]);
    }
    delay(holdMs);
    for (size_t idx = keyCount; idx > 0; --idx)
    {
      Keyboard.release(codes[idx - 1]);
    }
    sendStatusOk();
    return;
  }

  String message = F("Unknown keyboard action: ");
  message += action;
  sendStatusError(message.c_str());
}

void handleMouse(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE connection not established");
    return;
  }

  const char *action = command["action"] | "move";

  if (strcmp(action, "move") == 0)
  {
    int dx = getOptionalInt(command, "x", "dx", 0);
    int dy = getOptionalInt(command, "y", "dy", 0);
    int wheel = getOptionalInt(command, "wheel", "scroll", 0);
    int pan = getOptionalInt(command, "pan", nullptr, 0);
    Mouse.move(dx, dy, wheel, pan);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "releaseAll") == 0 || strcmp(action, "release_all") == 0)
  {
    Mouse.release(MOUSE_ALL_BUTTONS);
    sendStatusOk();
    return;
  }

  uint8_t mask = 0;
  bool hasButtons = false;
  JsonVariantConst buttonsVariant = command["buttons"];
  JsonVariantConst buttonVariant = command["button"];

  if (!buttonsVariant.isNull())
  {
    hasButtons = parseButtonMask(buttonsVariant, mask);
    if (!hasButtons)
    {
      return;
    }
  }
  else if (!buttonVariant.isNull())
  {
    hasButtons = parseButtonMask(buttonVariant, mask);
    if (!hasButtons)
    {
      return;
    }
  }
  else
  {
    if (strcmp(action, "click") == 0 || strcmp(action, "press") == 0 || strcmp(action, "release") == 0)
    {
      mask = MOUSE_LEFT;
      hasButtons = true;
    }
  }

  if (strcmp(action, "click") == 0)
  {
    Mouse.click(mask);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "press") == 0)
  {
    if (!hasButtons)
    {
      sendStatusError("mouse press requires button(s)");
      return;
    }
    Mouse.press(mask);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "release") == 0)
  {
    if (!hasButtons)
    {
      sendStatusError("mouse release requires button(s)");
      return;
    }
    Mouse.release(mask);
    sendStatusOk();
    return;
  }

  String message = F("Unknown mouse action: ");
  message += action;
  sendStatusError(message.c_str());
}

void handleConsumer(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE keyboard not connected");
    return;
  }

  const MediaKeyReport *reports[MAX_CONSUMER_KEYS];
  size_t count = 0;

  JsonVariantConst keysVariant = command["keys"];
  if (!keysVariant.isNull())
  {
    if (!collectConsumerReports(keysVariant, reports, count, MAX_CONSUMER_KEYS))
    {
      return;
    }
  }
  else
  {
    JsonVariantConst keyVariant = command["key"];
    if (!keyVariant.isNull())
    {
      if (!collectConsumerReports(keyVariant, reports, count, MAX_CONSUMER_KEYS))
      {
        return;
      }
    }
    else
    {
      JsonVariantConst codeVariant = command["code"];
      if (!codeVariant.isNull())
      {
        if (!collectConsumerReports(codeVariant, reports, count, MAX_CONSUMER_KEYS))
        {
          return;
        }
      }
      else
      {
        sendStatusError("consumer action requires key");
        return;
      }
    }
  }

  if (count == 0)
  {
    sendStatusError("consumer action requires key");
    return;
  }

  uint16_t repeat = clampRepeat(command["repeat"]);
  int gapValue = command["gapMs"] | 5;
  if (!command["gap_ms"].isNull())
  {
    gapValue = command["gap_ms"].as<int>();
  }
  if (gapValue < 0)
  {
    gapValue = 0;
  }
  if (gapValue > 1000)
  {
    gapValue = 1000;
  }
  uint16_t gapMs = static_cast<uint16_t>(gapValue);

  for (uint16_t r = 0; r < repeat; ++r)
  {
    for (size_t idx = 0; idx < count; ++idx)
    {
      Keyboard.write(*reports[idx]);
      if (gapMs > 0)
      {
        delay(gapMs);
      }
    }
  }

  sendStatusOk();
}

void processCommand(const String &payload)
{
  if (payload.length() == 0)
  {
    return;
  }

  if (payload.length() > JSON_DOC_CAPACITY)
  {
    sendStatusError("JSON payload too large");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error)
  {
    String message = F("JSON parse error: ");
    message += error.c_str();
    sendStatusError(message.c_str());
    return;
  }

  const char *device = doc["device"];
  if (!device)
  {
    device = doc["type"];
  }

  if (!device)
  {
    sendStatusError("Command missing device/type field");
    return;
  }

  if (strcasecmp(device, "keyboard") == 0)
  {
    handleKeyboard(doc.as<JsonVariantConst>());
  }
  else if (strcasecmp(device, "mouse") == 0)
  {
    handleMouse(doc.as<JsonVariantConst>());
  }
  else if (strcasecmp(device, "consumer") == 0 || strcasecmp(device, "media") == 0)
  {
    handleConsumer(doc.as<JsonVariantConst>());
  }
  else
  {
    String message = F("Unknown device type: ");
    message += device;
    sendStatusError(message.c_str());
  }
}

void flushInputBuffer()
{
  if (inputBuffer.length() > 0)
  {
    processCommand(inputBuffer);
    inputBuffer = "";
  }
}
} // namespace

void setup()
{
  Serial.begin(115200);
  inputBuffer.reserve(INPUT_BUFFER_LIMIT);
  Keyboard.begin();
  Mouse.begin();

  if (!initializeNvs())
  {
    sendStatusError("Failed to initialize NVS");
  }

  String savedSsid;
  String savedPassword;
  bool staConnected = false;
  if (loadWifiCredentials(savedSsid, savedPassword))
  {
    if (connectStationAndPersist(savedSsid, savedPassword))
    {
      staConnected = true;
    }
  }

  if (!staConnected)
  {
    startAccessPoint();
    if (configurationMode)
    {
      sendEvent("wifi_config_mode");
    }
  }

  sendEvent("ready");
}

void loop()
{
  while (Serial.available())
  {
    char c = static_cast<char>(Serial.read());

    if (c == '\r')
    {
      continue;
    }

    if (c == '\n')
    {
      flushInputBuffer();
      continue;
    }

    if (inputBuffer.length() >= INPUT_BUFFER_LIMIT)
    {
      sendStatusError("Input too long");
      inputBuffer = "";
      continue;
    }

    inputBuffer += c;
  }

  bool connected = Keyboard.isConnected();
  if (connected != lastBleConnectionState)
  {
    lastBleConnectionState = connected;
    sendEvent(connected ? "ble_connected" : "ble_disconnected");
  }

  delay(2);
}
