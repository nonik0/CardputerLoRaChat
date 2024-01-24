#include <Arduino.h>
#include <EEPROM.h>
#include <M5Cardputer.h>
#include <M5StackUpdater.h>
#include <M5_LoRa_E220_JP.h>

#include "draw_helper.h"

#define PING_INTERVAL_MS 1000 * 60        // 1 minute
#define PRESENCE_TIMEOUT_MS 1000 * 60 * 5 // 5 minutes, the time before a msg "expires" for the purposes of tracking a user presence

enum RedrawFlags
{
  MainWindow = 0b001,
  SystemBar = 0b010,
  TabBar = 0b100,
  None = 0b000
};

// TODO: refine message format
struct LoRaMessage
{
  uint8_t nonce : 6;
  uint8_t channel : 2; // 4 channels, 0b00->ping,0b01,0b10,0b11 are 3 channels
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

// TODO: rename Channel, ChatTab?
struct Tab
{
  unsigned char channel;
  // TODO: access in thread-safe way
  std::vector<LoRaMessage> messages;
  String messageBuffer;
  int viewIndex;
};

LoRa_E220_JP lora;
struct LoRaConfigItem_t loraConfig;
struct RecvFrame_t loraFrame;
uint8_t loraNonce = 0;

M5Canvas *canvas;
M5Canvas *canvasSystemBar;
M5Canvas *canvasTabBar;

// used by draw loop to trigger redraws
volatile uint8_t keyboardRedrawFlags = RedrawFlags::None;
volatile bool receivedMessage = false; // signal to redraw window
volatile int updateDelay = 0;
volatile unsigned long lastRx = false;
volatile unsigned long lastTx = false;
const int RxTxShowDelay = 1000; // ms

// tab state
uint8_t activeTabIndex;
const uint8_t UserInfoTabIndex = 3;
const uint8_t SettingsTabIndex = 4;
const uint8_t TabCount = 5;
Tab tabs[TabCount]; // TODO only need for chat tabs

// settings
enum Settings
{
  Username = 0,
  Brightness = 1,
  TextSize = 2,
  PingMode = 3,
  RepeatMode = 4,
  WriteConfig = 5,
  LoRaSettings = 6
};
const int SettingsCount = 7;
const String SettingsNames[SettingsCount] = {"Username", "Brightness", "Text Size", "Ping Mode", "Repeat Mode", "App Config", "LoRa Config"};
const String SettingsFilename = "/LoRaChat.conf";
uint8_t activeSettingIndex;
const uint8_t MinUsernameLength = 2; // TODO
const uint8_t MaxUsernameLength = 6;
String username = "anoncy";
short brightness = 70;
float chatTextSize = 1.0; // TODO: S, M, L?
bool pingMode = true;
bool repeatMode = false;
int loraWriteStage = 0;
int sdWriteStage = 0;

// display layout constants
const uint8_t w = 240; // M5Cardputer.Display.width();
const uint8_t h = 135; // M5Cardputer.Display.height();
const uint8_t m = 2;
// system bar
const uint8_t sx = 0;
const uint8_t sy = 0;
const uint8_t sw = w;
const uint8_t sh = 20;
// tab bar
const uint8_t tx = 0;
const uint8_t ty = sy + sh;
const uint8_t tw = 16;
const uint8_t th = h - ty;
// main window
const uint8_t wx = tw;
const uint8_t wy = sy + sh;
const uint8_t ww = w - wx;
const uint8_t wh = h - wy;

// track state of system bar elements for redraws
uint8_t batteryPct = M5Cardputer.Power.getBatteryLevel();
int maxRssi = -1000;

void printHexDump(const void *data, size_t size)
{
  const unsigned char *bytes = static_cast<const unsigned char *>(data);

  for (size_t i = 0; i < size; ++i)
  {
    USBSerial.printf("%02x ", bytes[i]);
    if ((i + 1) % 16 == 0)
    {
      printf("\n");
    }
  }
  USBSerial.print("\n");
}

void saveScreenshot()
{
  size_t pngLen;
  uint8_t *pngBytes = (uint8_t *)M5Cardputer.Display.createPng(&pngLen, 0, 0, 240, 135);

  int i = 0;
  String filename;
  do
  {
    filename = "/screenshot." + String(i++) + ".png";
  } while (SD.exists(filename));

  File file = SD.open(filename, FILE_WRITE);
  if (file)
  {
    file.write(pngBytes, pngLen);
    file.flush();
    file.close();
    USBSerial.println("saved screenshot to " + filename);
  }
  else
  {
    USBSerial.println("cannot save screenshot to file " + filename);
  }
}

std::vector<String> getMessageLines(const String &message, int lineWidth)
{
  std::vector<String> messageLines;
  String currentLine;
  String word;

  for (char c : message)
  {
    if (std::isspace(c))
    {
      if (currentLine.length() + word.length() <= lineWidth)
      {
        currentLine += (currentLine.isEmpty() ? "" : " ") + word;
        word.clear();
      }
      else
      {
        messageLines.push_back(currentLine);
        currentLine.clear();

        currentLine += word; // TODO: word too long, hyphenate
        word.clear();
      }
    }
    else
    {
      word += c;
    }
  }

  if (!currentLine.isEmpty() || !word.isEmpty())
  {
    currentLine += (currentLine.isEmpty() ? "" : " ") + word;
    messageLines.push_back(currentLine);
  }

  return messageLines;
}

int getPresenceRssi()
{
  int maxRssiAllUsers = -1000;

  for (auto presence : presence)
  {
    // only consider presences that have been seen recently
    if (presence.rssi > maxRssiAllUsers && millis() - presence.lastSeenMillis < PRESENCE_TIMEOUT_MS)
    {
      maxRssiAllUsers = presence.rssi;
    }
  }

  return maxRssiAllUsers;
}

bool recordPresence(const LoRaMessage &message)
{
  for (int i = 0; i < presence.size(); i++)
  {
    if (presence[i].username == message.username)
    {
      presence[i].rssi = message.rssi;

      bool beenAWhile = millis() - presence[i].lastSeenMillis > PRESENCE_TIMEOUT_MS;
      presence[i].lastSeenMillis = millis();
      return beenAWhile;
    }
  }

  presence.push_back({message.username, message.rssi, millis()});
  return true;
}

void drawSystemBar()
{
  canvasSystemBar->fillSprite(BG_COLOR);
  canvasSystemBar->fillRoundRect(sx + m, sy, sw - 2 * m, sh - m, 3, UX_COLOR_DARK);
  canvasSystemBar->fillRect(sx + m, sy, sw - 2 * m, 3, UX_COLOR_DARK); // fill round edges on top
  canvasSystemBar->setTextColor(TFT_SILVER, UX_COLOR_DARK);
  canvasSystemBar->setTextSize(1);
  canvasSystemBar->setTextDatum(middle_left);
  canvasSystemBar->drawString(username, sx + 3 * m, sy + sh / 2);
  canvasSystemBar->setTextDatum(middle_center);
  canvasSystemBar->drawString("LoRaChat", sw / 2, sy + sh / 2);
  if (millis() - lastTx < RxTxShowDelay)
    draw_tx_indicator(canvasSystemBar, sw - 71, sy + 1 * (sh / 3) - 1);
  if (millis() - lastRx < RxTxShowDelay)
    draw_rx_indicator(canvasSystemBar, sw - 71, sy + 2 * (sh / 3) - 1);
  draw_rssi_indicator(canvasSystemBar, sw - 60, sy + sh / 2 - 1, maxRssi);
  draw_battery_indicator(canvasSystemBar, sw - 30, sy + sh / 2 - 1, batteryPct);
  canvasSystemBar->pushSprite(sx, sy);
}

void drawTabBar()
{
  canvasTabBar->fillSprite(BG_COLOR);
  int tabx = m;
  int tabw = tw;
  int tabh = (th + 3 * m) / 5;
  int tabm = 5;

  for (int i = 4;; i--)
  {
    // draw active tab last/on top
    if (i == activeTabIndex)
    {
      continue;
    }
    else if (i < 0)
    {
      i = activeTabIndex;
    }

    unsigned short color = (i == activeTabIndex) ? UX_COLOR_ACCENT : UX_COLOR_DARK;
    int taby = tabm + i * tabh - i * m;

    // tab shape
    canvasTabBar->fillTriangle(tabx, taby, tabw, taby, tabw, taby - tabm, color);
    canvasTabBar->fillRect(tabx, taby, tabw, tabh - 2 * tabm, color);
    canvasTabBar->fillTriangle(tabx, taby + tabh - 2 * tabm, tabw, taby + tabh - 2 * tabm, tabw, taby + tabh - tabm, color);

    // label/icon
    switch (i)
    {
    case 0:
    case 1:
    case 2:
      canvasTabBar->setTextColor(TFT_SILVER, color);
      canvasTabBar->setTextDatum(middle_center);
      canvasTabBar->drawString(String(char('A' + i)), tabx + tw / 2 - 1, taby + tabh / 2 - tabm / 2 - 2);
      break;
    case UserInfoTabIndex:
      draw_user_icon(canvasTabBar, tabx + tw / 2 - 1, taby + tabh / 2 - tabm / 2 - 3);
      break;
    case SettingsTabIndex:
      draw_wrench_icon(canvasTabBar, tabx + tw / 2 - 1, taby + tabh / 2 - tabm / 2 - 3);
      break;
    }

    if (i == activeTabIndex)
    {
      break;
    }
  }
  canvasTabBar->pushSprite(tx, ty);
}

void drawChatWindow()
{
  // TODO: only change when font changes
  int rowCount = (wh - 3 * m) / (canvas->fontHeight() + m) - 1;
  int colCount = (ww - 4 * m) / canvas->fontWidth() - 1;
  int messageWidth = (colCount * 3) / 4;
  int messageBufferHeight = wh - ((canvas->fontHeight() + m) * rowCount) - m; // buffer takes last row plus extra space
  int messageBufferY = wh - messageBufferHeight + 2 * m;

  // draw message buffer
  for (int i = 0; i <= 1; i++)
  {
    canvas->drawLine(10, messageBufferY + i, ww - 10, messageBufferY + i, UX_COLOR_LIGHT);
  }

  if (tabs[activeTabIndex].messageBuffer.length() > 0)
  {
    canvas->setTextDatum(middle_right);
    canvas->drawString(tabs[activeTabIndex].messageBuffer, ww - 2 * m, messageBufferY + messageBufferHeight / 2);
  }

  // draw message window
  if (tabs[activeTabIndex].messages.size() > 0)
  {
    int linesDrawn = 0;

    // draw all messages or until window is full
    // TODO: view index, scrolling
    for (int i = tabs[activeTabIndex].messages.size() - 1; i >= 0; i--)
    {
      LoRaMessage message = tabs[activeTabIndex].messages[i];
      bool isOwnMessage = message.username.isEmpty();

      int cursorX;
      if (isOwnMessage)
      {
        cursorX = ww - 2 * m;
        canvas->setTextDatum(top_right);
      }
      else
      {
        cursorX = 2 * m;
        canvas->setTextDatum(top_left);
        message.text = message.username + message.text;
      }

      std::vector<String> lines = getMessageLines(message.text, messageWidth);
      for (int j = lines.size() - 1; j >= 0; j--)
      {
        int cursorY = 2 * m + (rowCount - linesDrawn - 1) * (canvas->fontHeight() + m);
        // canvas->setTextColor(TFT_SILVER);
        if (j == 0 && !isOwnMessage)
        {
          int usernameWidth = canvas->fontWidth() * (message.username.length() + 1);

          canvas->setTextColor(UX_COLOR_ACCENT2);
          canvas->drawString(lines[j].substring(0, message.username.length()), cursorX, cursorY);
          canvas->drawRoundRect(cursorX - 2, cursorY - 2, usernameWidth - 3, canvas->fontHeight() + 4, 2, UX_COLOR_ACCENT);

          canvas->setTextColor(TFT_SILVER);
          canvas->drawString(lines[j].substring(message.username.length()), cursorX + usernameWidth, cursorY);
        }
        else
        {
          canvas->setTextColor(TFT_SILVER);
          canvas->drawString(lines[j], cursorX, cursorY);
        }

        linesDrawn++;

        if (linesDrawn >= rowCount)
        {
          break;
        }
      }
    }
  }
}

void drawUserPresenceWindow()
{
  // TODO: order by last seen
  int entryYOffset = 20;
  int rowHeight = m + canvas->fontHeight() + m;
  int rowCount = (wh - entryYOffset) / rowHeight;
  int linesDrawn = 0;
  int cursorX = 2 * m;

  canvas->setTextColor(TFT_SILVER);
  canvas->setTextDatum(top_center);
  canvas->drawString("Users Seen", ww / 2, 2 * m);
  for (int i = 0; i <= 1; i++)
  {
    canvas->drawLine(10, 3 * m + canvas->fontHeight() + i, ww - 10, 3 * m + canvas->fontHeight() + i, UX_COLOR_LIGHT);
  }

  canvas->setTextDatum(top_left);
  for (int i = 0; i < presence.size(); i++)
  {
    int cursorY = entryYOffset + i * rowHeight;
    int lastSeenSecs = (millis() - presence[i].lastSeenMillis) / 1000;

    String lastSeenString = String(lastSeenSecs) + "s"; // show seconds by default
    if (lastSeenSecs > 60 * 60 * 3)                     // show hours after 3h
    {
      lastSeenString = String(lastSeenSecs / (60 * 60)) + "h";
    }
    else if (lastSeenSecs > PRESENCE_TIMEOUT_MS / 1000) // show minutes after 5m
    {
      lastSeenString = String(lastSeenSecs / 60) + "m";
    }

    String userPresenceString = String(presence[i].username.c_str()) + "RSSI: " + String(presence[i].rssi) + ", last seen: " + lastSeenString;
    int usernameWidth = canvas->fontWidth() * (presence[i].username.length() + 1);

    canvas->setTextColor(UX_COLOR_ACCENT2);
    canvas->drawString(userPresenceString.substring(0, presence[i].username.length()), cursorX, cursorY);
    canvas->drawRoundRect(cursorX - 2, cursorY - 2, usernameWidth - 3, canvas->fontHeight() + 4, 2, UX_COLOR_ACCENT);

    canvas->setTextColor(TFT_SILVER);
    canvas->drawString(userPresenceString.substring(presence[i].username.length()), cursorX + usernameWidth, cursorY);

    linesDrawn++;

    if (linesDrawn >= rowCount)
    {
      break;
    }
  }
}

void drawSettingsWindow()
{
  int settingX = ww / 2 - 16;
  int settingXGap = 10;
  int settingYOffset = 20;

  String loraSetting;
  int loraSettingColor = 0;
  switch (loraWriteStage)
  {
  case 0:
    loraSetting = "Write to module?";
    break;
  case 1:
    loraSetting = "M0, M1 off?";
    loraSettingColor = TFT_YELLOW;
    break;
  case 2:
    loraSetting = "OK!";
    loraSettingColor = TFT_GREEN;
    break;
  case 3:
    loraSetting = "Error!";
    loraSettingColor = TFT_RED;
    break;
  }

  String writeConfigSetting;
  int writeConfigSettingColor = 0;
  switch (sdWriteStage)
  {
  case 0:
    writeConfigSetting = "Write to SD?";
    break;
  case 1:
    writeConfigSetting = "Overwrite?";
    writeConfigSettingColor = TFT_YELLOW;
    break;
  case 2:
    writeConfigSetting = "OK!";
    writeConfigSettingColor = TFT_GREEN;
    break;
  case 3:
    writeConfigSetting = "Error!";
    writeConfigSettingColor = TFT_RED;
    break;
  }

  String settingValues[SettingsCount];
  settingValues[Settings::Username] = username;
  settingValues[Settings::Brightness] = String(brightness);
  settingValues[Settings::TextSize] = "TODO"; // String(chatTextSize);
  settingValues[Settings::PingMode] = String(pingMode ? "On" : "Off");
  settingValues[Settings::RepeatMode] = String(repeatMode ? "On" : "Off");
  settingValues[Settings::WriteConfig] = writeConfigSetting;
  settingValues[Settings::LoRaSettings] = loraSetting;

  int settingColors[SettingsCount];
  settingColors[Settings::Username] = username.length() < MinUsernameLength ? TFT_RED : (activeSettingIndex == Settings::Username ? TFT_GREEN : 0);
  settingColors[Settings::Brightness] = 0;
  settingColors[Settings::TextSize] = 0;
  settingColors[Settings::PingMode] = pingMode ? TFT_GREEN : TFT_RED;
  settingColors[Settings::RepeatMode] = repeatMode ? TFT_GREEN : TFT_RED;
  settingColors[Settings::WriteConfig] = writeConfigSettingColor;
  settingColors[Settings::LoRaSettings] = loraSettingColor;

  canvas->setTextColor(TFT_SILVER);
  canvas->setTextDatum(top_center);
  canvas->drawString("Settings", ww / 2, 2 * m);

  for (int i = 0; i <= 1; i++)
  {
    canvas->drawLine(10, 3 * m + canvas->fontHeight() + i, ww - 10, 3 * m + canvas->fontHeight() + i, UX_COLOR_LIGHT);
  }

  for (int i = 0; i < SettingsCount; i++)
  {
    int settingY = settingYOffset + i * (m + canvas->fontHeight() + m);
    int settingColor = i == activeSettingIndex ? COLOR_ORANGE : TFT_SILVER;

    canvas->setTextColor(settingColor);
    canvas->setTextDatum(top_right);
    canvas->drawString(SettingsNames[i] + ':', settingX, settingY);

    canvas->setTextDatum(top_left);
    canvas->setTextColor(settingColors[i] == 0 ? settingColor : settingColors[i]);
    canvas->drawString(settingValues[i], settingX + settingXGap, settingY);
  }
}

void drawMainWindow()
{
  canvas->fillSprite(BG_COLOR);
  canvas->fillRoundRect(0, 0, ww - m, wh - m, 3, UX_COLOR_MED);
  canvas->fillRect(0, 0, 3, wh - m, UX_COLOR_MED); // removes rounded edges on left side for tabs
  canvas->setTextColor(TFT_SILVER, UX_COLOR_MED);
  canvas->setTextDatum(top_left);

  switch (activeTabIndex)
  {
  case 0:
  case 1:
  case 2:
    drawChatWindow();
    break;
  case 3:
    drawUserPresenceWindow();
    break;
  case SettingsTabIndex:
    drawSettingsWindow();
    break;
  }

  canvas->pushSprite(wx, wy);
}

SPIClass SPI2;
void checkForMenuBoot()
{
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isKeyPressed('a'))
  {
    SPI2.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
               M5.getPin(m5::pin_name_t::sd_spi_miso),
               M5.getPin(m5::pin_name_t::sd_spi_mosi),
               M5.getPin(m5::pin_name_t::sd_spi_ss));
    while (!SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI2))
    {
      delay(500);
    }

    updateFromFS(SD, "/menu.bin");
    ESP.restart();
  }
}

void readConfigFromSd()
{
  M5Cardputer.update();

  uint8_t retries = 3;
  SPI2.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
             M5.getPin(m5::pin_name_t::sd_spi_miso),
             M5.getPin(m5::pin_name_t::sd_spi_mosi),
             M5.getPin(m5::pin_name_t::sd_spi_ss));
  while (!SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI2) && retries-- > 0)
  {
    delay(100);
  }

  if (retries == 0)
  {
    return;
  }

  File configFile = SD.open(SettingsFilename, FILE_READ);
  if (!configFile)
  {
    USBSerial.println("config file " + SettingsFilename + " not found");
    return;
  }

  USBSerial.println("reading config file: " + SettingsFilename);

  while (configFile.available())
  {
    String line = configFile.readStringUntil('\n');
    String name = line.substring(0, line.indexOf('='));
    String value = line.substring(line.indexOf('=') + 1);

    name.trim();
    name.toLowerCase();
    value.trim();
    value.toLowerCase();

    if (name == "username")
    {
      username = value.substring(0, MaxUsernameLength);
      USBSerial.println("username: " + username);
    }
    else if (name == "brightness")
    {
      brightness = value.toInt();
      USBSerial.println("brightness: " + String(brightness));
    }
    else if (name == "pingmode")
    {
      pingMode = (value == "true" || value == "1" || value == "on");
      USBSerial.println("pingMode: " + String(pingMode));
    }
    else if (name == "repeatmode")
    {
      repeatMode = (value == "true" || value == "1" || value == "on");
      USBSerial.println("repeatMode: " + String(repeatMode));
    }
  }

  configFile.close();
}

bool writeConfigToSd()
{
  File configFile = SD.open(SettingsFilename, FILE_WRITE);
  if (!configFile)
  {
    return false;
  }

  USBSerial.println("writing config file: " + SettingsFilename);

  configFile.println("username=" + username);
  configFile.println("brightness=" + String(brightness));
  configFile.println("pingMode=" + pingMode ? "on" : "off");
  configFile.println("repeatMode=" + repeatMode ? "on" : "off");

  configFile.flush();
  configFile.close();
  return true;
}

void loraInit()
{
  lora.Init(&Serial2, 9600, SERIAL_8N1, 1, 2);
  lora.SetDefaultConfigValue(loraConfig);

  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isKeyPressed('c'))
  {
    USBSerial.println("M0, M1 switches should be set to 1 to write new config register values to LoRa module");

    // these are all default values
    // loraConfig.own_address = 0x0000;
    // loraConfig.baud_rate = BAUD_9600;
    // loraConfig.air_data_rate = BW125K_SF9;
    // loraConfig.subpacket_size = SUBPACKET_200_BYTE;
    // loraConfig.rssi_ambient_noise_flag = RSSI_AMBIENT_NOISE_ENABLE;
    // loraConfig.transmitting_power = TX_POWER_13dBm;
    // loraConfig.own_channel = 0x00;
    // loraConfig.rssi_byte_flag = RSSI_BYTE_ENABLE;
    // loraConfig.transmission_method_type = UART_P2P_MODE;
    // loraConfig.lbt_flag = LBT_DISABLE;
    // loraConfig.wor_cycle = WOR_2000MS;
    // loraConfig.encryption_key = 0x1031;
    // loraConfig.target_address = 0x0000;
    // loraConfig.target_channel = 0x00;

    while (lora.InitLoRaSetting(loraConfig) != 0)
      ;
  }
  else
  {
    lora.InitLoRaSetting(loraConfig);
  }
}

void loraCreateFrame(int channel, const String &messageText, uint8_t *frameData, size_t &frameDataLength)
{
  // Ensure the data array has enough space
  // if (length < sizeof(message) + strlen(message.text) + 1) {
  //   std::cerr << "Error: Insufficient space to create the message." << std::endl;
  //   return;
  // }
  USBSerial.printf("creating frame: |%d|%d|%s|%s|\n", loraNonce, channel, username, messageText);

  frameData[0] = (loraNonce & 0x3F) | ((channel & 0x03) << 6);
  std::memcpy(frameData + 1, username.c_str(), min(username.length() + 1, (unsigned int)MaxUsernameLength));
  std::memcpy(frameData + MaxUsernameLength + 1, messageText.c_str(), messageText.length() + 1);
  frameDataLength = MaxUsernameLength + messageText.length() + 2;
}

void loraParseFrame(const uint8_t *frameData, size_t frameDataLength, LoRaMessage &message)
{
  if (frameDataLength < 8) // TODO: max
    return;

  message.nonce = (frameData[0] & 0x3F);
  message.channel = ((frameData[0] >> 6) & 0x03);
  message.username = String((const char *)(frameData + 1), MaxUsernameLength).c_str();
  size_t messageLength = frameDataLength - 7;
  message.text = String((const char *)(frameData + 7), messageLength - 1).c_str();

  USBSerial.printf("parsed frame: |%d|%d|%s|%s|\n", message.nonce, message.channel, message.username, message.text.c_str());
}

bool loraSendMessage(int channel, const String &messageText, LoRaMessage &sentMessage)
{
  uint8_t frameData[201]; // TODO: correct max size
  size_t frameDataLength;
  loraCreateFrame(channel, messageText, frameData, frameDataLength);

  USBSerial.print("sending frame:");
  printHexDump(frameData, frameDataLength);

  if (lora.SendFrame(loraConfig, frameData, frameDataLength) == 0)
  {
    USBSerial.println();

    sentMessage.channel = channel;
    sentMessage.nonce = loraNonce++;
    sentMessage.username = "";
    sentMessage.text = tabs[activeTabIndex].messageBuffer;
    sentMessage.rssi = 0;

    lastTx = millis();
    updateDelay = 0;

    return true;
  }
  else
  {
    USBSerial.println("failed!");
  }

  return false;
}

void loraReceiveTask(void *pvParameters)
{
  while (1)
  {
    if (lora.ReceiveFrame(&loraFrame) == 0)
    {
      USBSerial.print("received frame: ");
      printHexDump(loraFrame.recv_data, loraFrame.recv_data_len);

      LoRaMessage message;
      loraParseFrame(loraFrame.recv_data, loraFrame.recv_data_len, message);
      message.rssi = loraFrame.rssi;
      receivedMessage = true;
      lastRx = millis();
      updateDelay = 0;

      // TODO: check nonce, replay for basic meshing

      // send an immediate ping to announce self if new presence (new user or been a while for existing one)
      if (recordPresence(message) && !repeatMode)
      {
        LoRaMessage sentMessage;
        loraSendMessage(0b11, "", sentMessage);
      }

      if (message.text.isEmpty())
        continue;

      tabs[message.channel].messages.push_back(message);

      if (repeatMode)
      {
        String response = String("name: " + String(message.username) + ", msg: " + String(message.text) + ", rssi: " + String(loraFrame.rssi));
        LoRaMessage sentMessage;
        if (loraSendMessage(message.channel, response, sentMessage))
        {
          tabs[activeTabIndex].messages.push_back(sentMessage);
        }
      }
    }
    else
    {
      // TODO log
    }

    delay(1);
  }
}

void loraPingTask(void *pvParameters)
{
  // send out a ping every so often during inactivity to keep presence for other users
  LoRaMessage sentMessage;
  loraSendMessage(0b11, "", sentMessage);

  while (1)
  {
    if (pingMode && millis() - lastTx > PING_INTERVAL_MS)
    {
      loraSendMessage(0b11, "", sentMessage);
    }

    delay(1000);
  }
}

bool updateStringFromInput(Keyboard_Class::KeysState keyState, String &str, int maxLength = 255, bool alphaNumericOnly = false)
{
  bool updated = false;

  for (auto i : keyState.word)
  {
    if (str.length() < maxLength && (!alphaNumericOnly || std::isalnum(i)))
    {
      str += i;
      updated = true;
    }
    else
    {
      USBSerial.printf("max length reached: [%s]\n + %c", str.c_str(), i);
    }
  }

  if (keyState.del && str.length() > 0)
  {
    str.remove(str.length() - 1);
    updated = true;
  }

  return updated;
}

void handleChatTabInput(Keyboard_Class::KeysState keyState, uint8_t &redrawFlags)
{
  if (updateStringFromInput(keyState, tabs[activeTabIndex].messageBuffer))
  {
    redrawFlags |= RedrawFlags::MainWindow;
  }

  if (keyState.enter)
  {
    USBSerial.println(tabs[activeTabIndex].messageBuffer);

    tabs[activeTabIndex].messageBuffer.trim();

    // empty message reserved for pings
    if (tabs[activeTabIndex].messageBuffer.isEmpty())
    {
      return;
    }

    LoRaMessage sentMessage;
    if (loraSendMessage(activeTabIndex, tabs[activeTabIndex].messageBuffer, sentMessage))
    {
      tabs[activeTabIndex].messages.push_back(sentMessage);
    }
    else
    {
      sentMessage.text = "send failed";
      tabs[activeTabIndex].messages.push_back(sentMessage);
    }

    tabs[activeTabIndex].messageBuffer.clear();
    redrawFlags |= RedrawFlags::MainWindow;
  }
}

void handleSettingsTabInput(Keyboard_Class::KeysState keyState, uint8_t &redrawFlags)
{
  if (M5Cardputer.Keyboard.isKeyPressed(';'))
  {
    activeSettingIndex = (activeSettingIndex == 0)
                             ? SettingsCount - 1
                             : activeSettingIndex - 1;
    redrawFlags |= RedrawFlags::MainWindow;
  }
  if (M5Cardputer.Keyboard.isKeyPressed('.'))
  {
    activeSettingIndex = (activeSettingIndex + 1) % SettingsCount;
    redrawFlags |= RedrawFlags::MainWindow;
  }

  switch (activeSettingIndex)
  {
  case Settings::Username:
    if (updateStringFromInput(keyState, username, MaxUsernameLength, true))
    {
      redrawFlags |= RedrawFlags::SystemBar | RedrawFlags::MainWindow;
    }
    break;
  case Settings::Brightness:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
      {
        brightness = (c == ',')
                         ? max(0, brightness - 10)
                         : min(100, brightness + 10);
        M5Cardputer.Display.setBrightness(brightness);
        redrawFlags |= RedrawFlags::MainWindow;
        break;
      }
    }
    break;
  case Settings::TextSize:
    // for (auto c : keyState.word)
    // {
    //   if (c == ',' || c == '/')
    //   {
    //     chatTextSize = (chatTextSize + 0.5) > 2.0 ? 0.5 : chatTextSize + 0.5;
    //     input = Redraw::Window;
    //   }
    // }
    break;
  case Settings::PingMode:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
      {
        pingMode = !pingMode;
        redrawFlags |= RedrawFlags::MainWindow;
      }
    }
    if (keyState.enter)
    {
      pingMode = !pingMode;
      redrawFlags |= RedrawFlags::MainWindow;
    }
    break;
  case Settings::RepeatMode:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
      {
        repeatMode = !repeatMode;
        redrawFlags |= RedrawFlags::MainWindow;
      }
      break;
    }
    if (keyState.enter)
    {
      repeatMode = !repeatMode;
      redrawFlags |= RedrawFlags::MainWindow;
    }
    break;
  case Settings::WriteConfig:
    if (keyState.enter)
    {
      switch (sdWriteStage)
      {
      case 0:
        sdWriteStage++;
        if (SD.exists(SettingsFilename))
          break;
      case 1:
        sdWriteStage = (writeConfigToSd())
                           ? sdWriteStage + 1
                           : sdWriteStage + 2;
        break;
      default:
        sdWriteStage = 0;
        break;
      }

      redrawFlags |= RedrawFlags::MainWindow;
    }
    break;
  case Settings::LoRaSettings:
    if (keyState.enter)
    {
      switch (loraWriteStage)
      {
      case 0:
        // show switch reminder
        loraWriteStage++;
        break;
      case 1:
        // try to write
        // debug
        loraWriteStage = (lora.InitLoRaSetting(loraConfig) == 0)
                             ? loraWriteStage + 1
                             : loraWriteStage + 2;
        break;
      default:
        loraWriteStage = 0;
        break;
      }

      redrawFlags |= RedrawFlags::MainWindow;
    }
    break;
  }

  if (activeSettingIndex != Settings::WriteConfig)
  {
    sdWriteStage = 0;
  }

  if (activeSettingIndex != Settings::LoRaSettings)
  {
    loraWriteStage = 0;
  }
}

void keyboardInputTask(void *pvParameters)
{
  const unsigned long debounceDelay = 200;
  unsigned long lastKeyPressMillis = 0;

  while (1)
  {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
    {
      uint8_t redrawFlags = RedrawFlags::None;
      unsigned long currentMillis = millis();
      if (currentMillis - lastKeyPressMillis >= debounceDelay)
      {
        // need to see again with display off
        if (brightness <= 30 && !M5Cardputer.Keyboard.isKeyPressed(','))
        {
          brightness = 50;
          M5Cardputer.Display.setBrightness(brightness);
          redrawFlags |= RedrawFlags::MainWindow;
        }

        lastKeyPressMillis = currentMillis;
        Keyboard_Class::KeysState keyState = M5Cardputer.Keyboard.keysState();

        if (activeTabIndex == SettingsTabIndex)
        {
          handleSettingsTabInput(keyState, redrawFlags);
        }
        else if (activeTabIndex == UserInfoTabIndex)
        {
          // TODO? what sort of input would be useful here?
        }
        else
        {
          handleChatTabInput(keyState, redrawFlags);
        }

        if (keyState.tab)
        {
          activeTabIndex = (activeTabIndex + 1) % TabCount;
          updateDelay = 0;
          redrawFlags |= RedrawFlags::TabBar | RedrawFlags::MainWindow;
        }
      }

      keyboardRedrawFlags = redrawFlags;
    }

    if (M5Cardputer.BtnA.isPressed())
    {
      saveScreenshot();
    }
  }
}

void setup()
{
  USBSerial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  delay(1000);
  USBSerial.println("setup");

  checkForMenuBoot();
  readConfigFromSd();

  M5Cardputer.Display.init();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(brightness);

  canvas = new M5Canvas(&M5Cardputer.Display);
  canvas->createSprite(ww, wh);
  canvasSystemBar = new M5Canvas(&M5Cardputer.Display);
  canvasSystemBar->createSprite(sw, sh);
  canvasTabBar = new M5Canvas(&M5Cardputer.Display);
  canvasTabBar->createSprite(tw, th);

  tabs[0] = {0, {}, "", 0};
  tabs[1] = {1, {}, "", 0};
  tabs[2] = {2, {}, "", 0};
  tabs[3] = {3, {}, "", 0};
  tabs[4] = {4, {}, "", 0};
  activeTabIndex = 0;
  activeSettingIndex = 0;

  drawSystemBar();
  drawTabBar();
  drawMainWindow();

  loraInit();

  xTaskCreateUniversal(loraReceiveTask, "loraReceiveTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
  xTaskCreateUniversal(loraPingTask, "loraPingTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
  xTaskCreateUniversal(keyboardInputTask, "keyboardInputTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
}

void loop()
{
  uint8_t redrawFlags = RedrawFlags::None;

  if (keyboardRedrawFlags)
  {
    redrawFlags |= keyboardRedrawFlags;
    keyboardRedrawFlags = RedrawFlags::None;
  }

  if (receivedMessage)
  {
    redrawFlags |= RedrawFlags::MainWindow;
    receivedMessage = false;
  }

  // redraw occasionally for system bar updates and user info tab
  if (millis() > updateDelay)
  {
    bool redraw = false;
    updateDelay = millis() + 5000;

    int newBatteryPct = M5Cardputer.Power.getBatteryLevel();
    if (newBatteryPct != batteryPct)
    {
      batteryPct = newBatteryPct;
      redrawFlags |= RedrawFlags::SystemBar;
    }

    int newRssi = getPresenceRssi();
    if (newRssi != maxRssi)
    {
      maxRssi = newRssi;
      redrawFlags |= RedrawFlags::SystemBar;
    }

    if (millis() - lastRx < RxTxShowDelay * 2 || millis() - lastTx < RxTxShowDelay * 2)
    {
      updateDelay = millis() + RxTxShowDelay / 2;
      redrawFlags |= RedrawFlags::SystemBar;
    }

    // redraw every second to update last seen times
    if (activeTabIndex == UserInfoTabIndex)
    {
      updateDelay = millis() + 1000;
      redrawFlags |= RedrawFlags::MainWindow;
    }
  }

  if (redrawFlags & RedrawFlags::TabBar)
    drawTabBar();
  if (redrawFlags & RedrawFlags::SystemBar)
    drawSystemBar();
  if (redrawFlags & RedrawFlags::MainWindow)
    drawMainWindow();

  delay(10);
}