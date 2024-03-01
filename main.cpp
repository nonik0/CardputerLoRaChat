#include <esp_now.h>
#include <M5Cardputer.h>
#include <M5StackUpdater.h>
#include <M5_LoRa_E220_JP.h>
#include <WiFi.h>

#include "common.h"
#include "draw_helper.h"

#define PING_INTERVAL_MS 1000 * 60        // 1 minute
#define PRESENCE_TIMEOUT_MS 1000 * 60 * 5 // 5 minutes, the time before a msg "expires" for the purposes of tracking a user presence

uint8_t messageNonce = 0;

LoRa_E220_JP lora;
struct LoRaConfigItem_t loraConfig;
struct RecvFrame_t loraFrame;

uint8_t espNowBroadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t espNowBroadcastPeerInfo;

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
const uint8_t ChatTabCount = 3;
const uint8_t TabCount = 5;
ChatTab chatTab[ChatTabCount];

// settings
uint8_t activeSettingIndex;
const uint8_t MinUsernameLength = 2; // TODO
const uint8_t MaxUsernameLength = 8;
const uint8_t MaxMessageLength = 100; // TODO
String username = "anoncy";
uint8_t brightness = 70;
float chatTextSize = 1.0; // TODO: S, M, L?
bool pingMode = true;
bool repeatMode = false;
bool espNowMode = false;
int loraWriteStage = 0;
int sdWriteStage = 0;
bool sdInit = false;
SPIClass SPI2;

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

String getHexString(const void *data, size_t size)
{
  const byte *bytes = (const byte *)(data);
  String hexDump = "";

  for (size_t i = 0; i < size; ++i)
  {
    char hex[4];
    snprintf(hex, sizeof(hex), "%02x ", bytes[i]);
    hexDump += hex;
  }

  return hexDump; // Return the accumulated hex dump string
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
    USBSerial.println("saved screenshot to " + filename + ", " + String(pngLen) + " bytes");
  }
  else
  {
    USBSerial.println("cannot save screenshot to file " + filename);
  }

  free(pngBytes);
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

bool recordPresence(const Message &message)
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

  if (chatTab[activeTabIndex].messageBuffer.length() > 0)
  {
    canvas->setTextDatum(middle_right);
    canvas->drawString(chatTab[activeTabIndex].messageBuffer, ww - 2 * m, messageBufferY + messageBufferHeight / 2);
  }

  // draw message window
  if (chatTab[activeTabIndex].messages.size() > 0)
  {
    int linesDrawn = 0;

    // draw all messages or until window is full
    // TODO: view index, scrolling
    for (int i = chatTab[activeTabIndex].messages.size() - 1; i >= 0; i--)
    {
      Message message = chatTab[activeTabIndex].messages[i];
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
    writeConfigSetting = sdInit ? "Write to SD?" : "No SD found";
    writeConfigSettingColor = sdInit ? 0 : TFT_YELLOW;
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
  settingValues[Settings::PingMode] = String(pingMode ? "On" : "Off");
  settingValues[Settings::RepeatMode] = String(repeatMode ? "On" : "Off");
  settingValues[Settings::EspNowMode] = String(espNowMode ? "On" : "Off");
  settingValues[Settings::WriteConfig] = writeConfigSetting;
  settingValues[Settings::LoRaSettings] = loraSetting;

  int settingColors[SettingsCount];
  settingColors[Settings::Username] = username.length() < MinUsernameLength ? TFT_RED : (activeSettingIndex == Settings::Username ? TFT_GREEN : 0);
  settingColors[Settings::Brightness] = 0;
  settingColors[Settings::PingMode] = pingMode ? TFT_GREEN : TFT_RED;
  settingColors[Settings::RepeatMode] = repeatMode ? TFT_GREEN : TFT_RED;
  settingColors[Settings::EspNowMode] = espNowMode ? TFT_GREEN : TFT_RED;
  ;
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

bool sdCardInit()
{
  uint8_t retries = 3;
  SPI2.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
             M5.getPin(m5::pin_name_t::sd_spi_miso),
             M5.getPin(m5::pin_name_t::sd_spi_mosi),
             M5.getPin(m5::pin_name_t::sd_spi_ss));
  while (!(sdInit = SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI2)) && retries-- > 0)
  {
    delay(100);
  }

  return sdInit;
}

void checkForMenuBoot()
{
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isKeyPressed('a') && sdCardInit())
  {
    updateFromFS(SD, "/menu.bin");
    ESP.restart();
  }
}

void readConfigFromSd()
{
  M5Cardputer.update();

  if (!sdInit && !sdCardInit())
  {
    return;
  }

  File configFile = SD.open(SettingsFilename, FILE_READ);
  if (!configFile)
  {
    log_w("config file not found: %s", SettingsFilename.c_str());
    return;
  }

  log_w("reading config file: %s", SettingsFilename.c_str());

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
      log_w("username: %s", username);
    }
    else if (name == "brightness")
    {
      brightness = value.toInt();
      log_w("brightness: %s", String(brightness));
    }
    else if (name == "pingmode")
    {
      pingMode = (value == "true" || value == "1" || value == "on");
      log_w("pingMode: %s", String(pingMode));
    }
    else if (name == "repeatmode")
    {
      repeatMode = (value == "true" || value == "1" || value == "on");
      log_w("repeatMode: %s", String(repeatMode));
    }
    else if (name == "espnowmode")
    {
      espNowMode = (value == "true" || value == "1" || value == "on");
      log_w("espNowMode: %s", String(repeatMode));
    }
  }

  configFile.close();
}

bool writeConfigToSd()
{
  if (!sdInit && !sdCardInit())
  {
    log_w("cannot initialize SD card");
    return false;
  }

  File configFile = SD.open(SettingsFilename, FILE_WRITE);
  if (!configFile)
  {
    return false;
  }

  log_w("writing config file: %s", SettingsFilename.c_str());

  configFile.println("username=" + username);
  configFile.println("brightness=" + String(brightness));
  configFile.println("pingMode=" + pingMode ? "on" : "off");
  configFile.println("repeatMode=" + repeatMode ? "on" : "off");
  configFile.println("espNowMode=" + repeatMode ? "on" : "off");

  configFile.flush();
  configFile.close();
  return true;
}

void createFrame(int channel, const String &messageText, uint8_t *frameData, size_t &frameDataLength)
{
  // Ensure the data array has enough space
  // if (length < sizeof(message) + strlen(message.text) + 1) {
  //   std::cerr << "Error: Insufficient space to create the message." << std::endl;
  //   return;
  // }
  log_w("creating frame: |%d|%d|%s|%s|", channel, messageNonce, username, messageText);
  frameDataLength = 0;

  frameData[0] = (messageNonce & 0x3F) | ((channel & 0x03) << 6);
  frameDataLength += 1;

  size_t usernameByteLength = min(username.length() + 1, (unsigned int)MaxUsernameLength + 1);
  std::memcpy(frameData + frameDataLength, username.c_str(), usernameByteLength);
  frameDataLength += usernameByteLength;

  size_t messageTextByteLength = min(messageText.length() + 1, (unsigned int)MaxMessageLength + 1);
  if (messageTextByteLength > 1)
  {
    std::memcpy(frameData + frameDataLength, messageText.c_str(), messageTextByteLength);
    frameDataLength += messageTextByteLength;
  }
}

void parseFrame(const uint8_t *frameData, size_t frameDataLength, Message &message)
{
  if (frameDataLength < (1 + MinUsernameLength + 1) || frameDataLength > (1 + MaxUsernameLength + 1 + MaxMessageLength + 1)) // TODO: test
    return;

  size_t frameBytesRead = 0;

  message.nonce = (frameData[0] & 0x3F);
  message.channel = ((frameData[0] >> 6) & 0x03);
  frameBytesRead += 1;

  message.username = String((const char *)(frameData + frameBytesRead), MaxUsernameLength).c_str();
  frameBytesRead += message.username.length() + 1;

  size_t messageLength = frameDataLength - frameBytesRead;
  message.text = String((const char *)(frameData + frameBytesRead), messageLength).c_str();

  log_w("parsed frame: |%d|%d|%s|%s|", message.channel, message.nonce, message.username, message.text.c_str());
}

bool sendMessage(int channel, const String &messageText, Message &sentMessage)
{
  uint8_t frameData[201]; // TODO: correct max size
  size_t frameDataLength;
  createFrame(channel, messageText, frameData, frameDataLength);

  log_w("sending frame: %s", getHexString(frameData, frameDataLength).c_str());

  int result;
  if (!espNowMode && (result = lora.SendFrame(loraConfig, frameData, frameDataLength)) == 0 ||
      espNowMode && (result = esp_now_send(espNowBroadcastAddress, frameData, frameDataLength)) == ESP_OK)
  {
    sentMessage.channel = channel;
    sentMessage.nonce = messageNonce++;
    sentMessage.username = "";
    sentMessage.text = chatTab[activeTabIndex].messageBuffer;
    sentMessage.rssi = 0;

    lastTx = millis();
    updateDelay = 0;

    return true;
  }
  else
  {
    if (espNowMode)
      log_e("error sending esp-now frame: %s", esp_err_to_name(result));
    else
      log_e("error sending LoRa frame: %d", result);
  }

  return false;
}

void receiveMessage(const uint8_t *frameData, size_t frameDataLength, int rssi)
{
  log_w("received frame: %s", getHexString(frameData, frameDataLength).c_str());

  Message message;
  parseFrame(frameData, frameDataLength, message);
  message.rssi = rssi;
  lastRx = millis();
  updateDelay = 0;

  // TODO: check nonce, replay for basic meshing

  // send an immediate ping to announce self if new presence (new user or been a while for existing one)
  if (recordPresence(message) && !repeatMode)
  {
    Message sentMessage;
    sendMessage(0b11, "", sentMessage);
  }

  if (message.text.isEmpty())
    return;

  chatTab[message.channel].messages.push_back(message);
  receivedMessage = true;

  if (repeatMode)
  {
    String response = String("name: " + String(message.username) + ", msg: " + String(message.text) + ", rssi: " + String(loraFrame.rssi));
    Message sentMessage;
    if (sendMessage(message.channel, response, sentMessage))
    {
      chatTab[activeTabIndex].messages.push_back(sentMessage);
    }
  }
}

void pingTask(void *pvParameters)
{
  // send out a ping every so often during inactivity to keep presence for other users
  Message sentMessage;
  sendMessage(0b11, "", sentMessage);

  while (1)
  {
    if (pingMode && millis() - lastTx > PING_INTERVAL_MS)
    {
      sendMessage(0b11, "", sentMessage);
    }

    delay(1000);
  }
}

void espNowOnReceive(const uint8_t *mac, const uint8_t *data, int dataLength)
{
  receiveMessage(data, dataLength, 0);
}

void espNowDeinit()
{
  if (!espNowMode)
  {
    log_w("esp-now already disabled");
    return;
  }

  log_w("disabling esp-now");
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  espNowMode = false;
}

void espNowInit()
{
  if (espNowMode)
  {
    log_w("esp-now already enabled");
    return;
  }

  log_w("enabling esp-now");

  WiFi.mode(WIFI_STA);

  esp_err_t result;
  if ((result = esp_now_init()) != ESP_OK)
  {
    log_e("error initializing ESP-NOW: %s", esp_err_to_name(result));
    return;
  }

  memcpy(espNowBroadcastPeerInfo.peer_addr, espNowBroadcastAddress, 6);
  espNowBroadcastPeerInfo.channel = 0;
  espNowBroadcastPeerInfo.encrypt = false;

  if (esp_now_add_peer(&espNowBroadcastPeerInfo) != ESP_OK)
  {
    log_e("Failed to add peer");
    return;
  }

  esp_now_register_recv_cb(espNowOnReceive);
  espNowMode = true;
}

void loraInit()
{
  lora.Init(&Serial2, 9600, SERIAL_8N1, 1, 2);
  lora.SetDefaultConfigValue(loraConfig);
  lora.InitLoRaSetting(loraConfig);
}

void loraReceiveTask(void *pvParameters)
{
  while (1)
  {
    if (lora.RecieveFrame(&loraFrame) == 0)
      receiveMessage(loraFrame.recv_data, loraFrame.recv_data_len, loraFrame.rssi);

    delay(1);
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
      log_e("max length reached: [%s]\n + %c", str.c_str(), i);
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
  if (updateStringFromInput(keyState, chatTab[activeTabIndex].messageBuffer))
  {
    redrawFlags |= RedrawFlags::MainWindow;
  }

  if (keyState.enter)
  {
    //log_w(chatTab[activeTabIndex].messageBuffer.c_str());

    chatTab[activeTabIndex].messageBuffer.trim();

    // empty message reserved for pings
    if (chatTab[activeTabIndex].messageBuffer.isEmpty())
    {
      return;
    }

    Message sentMessage;
    if (sendMessage(activeTabIndex, chatTab[activeTabIndex].messageBuffer, sentMessage))
    {
      chatTab[activeTabIndex].messages.push_back(sentMessage);
    }
    else
    {
      sentMessage.text = "send failed";
      chatTab[activeTabIndex].messages.push_back(sentMessage);
    }

    chatTab[activeTabIndex].messageBuffer.clear();
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
  case Settings::EspNowMode:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
      {
        if (!espNowMode)
        {
          espNowInit();
        }
        else
        {
          espNowDeinit();
          loraInit();
        }

        lastRx = lastTx = 0;
        redrawFlags |= RedrawFlags::MainWindow;
      }
    }
    break;
  case Settings::WriteConfig:
    if (keyState.enter)
    {
      if (!sdInit)
        break;

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
      if (millis() - lastKeyPressMillis >= debounceDelay)
      {
        lastKeyPressMillis = millis();

        // need to see again with display off
        if (brightness <= 30 && !M5Cardputer.Keyboard.isKeyPressed(','))
        {
          brightness = 50;
          M5Cardputer.Display.setBrightness(brightness);
          redrawFlags |= RedrawFlags::MainWindow;
        }

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

  chatTab[0] = {0, {}, "", 0};
  chatTab[1] = {1, {}, "", 0};
  chatTab[2] = {2, {}, "", 0};
  activeTabIndex = 0;
  activeSettingIndex = 0;

  drawSystemBar();
  drawTabBar();
  drawMainWindow();

  if (espNowMode)
  {
    espNowInit();
  }
  else
  {
    loraInit();
  }

  xTaskCreateUniversal(loraReceiveTask, "loraReceiveTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
  xTaskCreateUniversal(pingTask, "pingTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
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