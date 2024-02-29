#include <Arduino.h>

enum RedrawFlags
{
  MainWindow = 0b001,
  SystemBar = 0b010,
  TabBar = 0b100,
  None = 0b000
};

// TODO: refine message format
struct Message
{
  uint8_t nonce : 6;
  uint8_t channel : 2; // 4 channels, 0b01,0b10,0b11 channels, 0b11 reserved for pings
  String username;     // use MAC or something tied to device?
  int rssi;
  String text;
};

// track presence of other users
struct Presence
{
  String username;
  int rssi;
  unsigned long lastSeenMillis;
};
std::vector<Presence> presence;

struct ChatTab
{
  unsigned char channel;
  // TODO: access in thread-safe way
  std::vector<Message> messages;
  String messageBuffer;
  int viewIndex;
};

enum Settings
{
  Username = 0,
  Brightness = 1,
  PingMode = 2,
  RepeatMode = 3,
  EspNowMode = 4,
  WriteConfig = 5,
  LoRaSettings = 6
};

const int SettingsCount = 7;
const String SettingsNames[SettingsCount] = {"Username", "Brightness", "Ping Mode", "Repeat Mode", "ESP-NOW Mode", "App Config", "LoRa Config"};
const String SettingsFilename = "/LoRaChat.conf";