#include <Arduino.h>
#include <EEPROM.h>
#include <M5Cardputer.h>
#include <M5StackUpdater.h>
#include <M5_LoRa_E220_JP.h>

#include "draw_helper.h"

#define PING_INTERVAL_MS 1000 * 60        // 1 minute
#define PRESENCE_TIMEOUT_MS 1000 * 60 * 5 // 5 minutes, the time before a msg "expires" for the purposes of tracking a user presence

enum Redraw
{
  Window,
  SystemBar,
  TabBar,
  None
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
uint8_t loraNonce = 0; // todo: save??
unsigned long lastSentMillis = 0;
volatile bool receivedMessage = false; // signal to redraw window

// TODO: extract drawing functionality to class?
M5Canvas *canvas;
M5Canvas *canvasSystemBar;
M5Canvas *canvasTabBar;
volatile Redraw keyboardInput = Redraw::None; // tracks what to redraw with kb input

// tab state
uint8_t activeTabIndex;
const uint8_t UserInfoTabIndex = 3;
const uint8_t SettingsTabIndex = 4;
const uint8_t TabCount = 5;
Tab tabs[TabCount];

// settings
enum Settings
{
  Username = 0,
  Brightness = 1,
  TextSize = 2,
  PingMode = 3,
  RepeatMode = 4,
  LoRaSettings = 5
};
const int SettingsCount = 6;
uint8_t activeSettingIndex;
const uint8_t MinUsernameLength = 2; // TODO
const uint8_t MaxUsernameLength = 6;
String username = "nick";
short brightness = 70;
float chatTextSize = 1.0; // TODO: S, M, L?
bool pingMode = true;
bool repeatMode = false;

// display layout constants
const uint8_t w = 240; // M5Cardputer.Display.width();
const uint8_t h = 135; // M5Cardputer.Display.height();
const uint8_t m = 3;

const uint8_t sx = m;
const uint8_t sy = 0;
const uint8_t sw = w - 2 * m;
const uint8_t sh = 18;

const uint8_t tx = m;
const uint8_t ty = sy + sh + m;
const uint8_t tw = 18;
const uint8_t th = h - ty - m;

const uint8_t wx = tw;
const uint8_t wy = sy + sh + m;
const uint8_t ww = w - wx - m;
const uint8_t wh = h - wy - m;

// track state of system bar elements for redraws
uint8_t batteryPct = M5Cardputer.Power.getBatteryLevel();
int maxRssi = -1000;
int updateDelay = 0;

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

  if (maxRssiAllUsers == -1000)
    USBSerial.println("no presence found");
  return maxRssiAllUsers;
}

void recordPresence(const LoRaMessage &message)
{
  for (int i = 0; i < presence.size(); i++)
  {
    if (presence[i].username == message.username)
    {
      presence[i].rssi = message.rssi;
      presence[i].lastSeenMillis = millis();
      return;
    }
  }

  presence.push_back({message.username, message.rssi, millis()});
}

void drawSystemBar()
{
  canvasSystemBar->fillSprite(BG_COLOR);
  canvasSystemBar->fillRoundRect(sx, sy, sw, sh, 3, UX_COLOR_DARK);
  canvasSystemBar->setTextColor(TFT_SILVER, UX_COLOR_DARK);
  canvasSystemBar->setTextDatum(middle_center);
  canvasSystemBar->setTextSize(1);
  canvasSystemBar->drawString("LoRaChat", sw / 2, sy + sh / 2);
  canvasSystemBar->setTextDatum(middle_left);
  canvasSystemBar->drawString(username, sx + m, sy + sh / 2);
  draw_rssi_indicator(canvasSystemBar, sw - 60, sy + sh / 2, maxRssi);
  draw_battery_indicator(canvasSystemBar, sw - 30, sy + sh / 2, batteryPct);
  canvasSystemBar->pushSprite(sx, sy);
}

void drawTabBar()
{
  canvasTabBar->fillSprite(BG_COLOR);
  int tabh = (th + 4 * m) / 5;
  int tabf = 5;

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
    int taby = tabf + i * tabh - i * m;

    // tab shape
    canvasTabBar->fillTriangle(0, taby, tw, taby, tw, taby - tabf, color);
    canvasTabBar->fillRect(0, taby, tw, tabh - 2 * tabf, color);
    canvasTabBar->fillTriangle(0, taby + tabh - 2 * tabf, tw, taby + tabh - 2 * tabf, tw, taby + tabh - tabf, color);

    // label/icon
    switch (i)
    {
    case 0:
    case 1:
    case 2:
      canvasTabBar->setTextColor(TFT_SILVER, color);
      canvasTabBar->setTextDatum(middle_center);
      canvasTabBar->drawString(String(char('A' + i)), tw / 2 - 1, taby + tabh / 2 - tabf / 2 - 1);
      break;
    case UserInfoTabIndex:
      draw_user_icon(canvasTabBar, tw / 2 - 2, taby + tabh / 2 - tabf / 2 - 3);
      break;
    case SettingsTabIndex:
      draw_wrench_icon(canvasTabBar, tw / 2 - 2, taby + tabh / 2 - tabf / 2 - 3);
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
  int rowCount = (wh - 2 * m) / (canvas->fontHeight() + m) - 1;
  int colCount = (ww - 2 * m) / canvas->fontWidth() - 1;
  int messageWidth = (colCount * 3) / 4;
  int messageBufferHeight = wh - ((canvas->fontHeight() + m) * rowCount) - m; // buffer takes last row plus extra space
  int messageBufferY = wh - messageBufferHeight;

  // draw message buffer
  for (int i = -1; i <= 1; i++)
  {
    canvas->drawLine(0, messageBufferY + i, ww, messageBufferY + i, UX_COLOR_DARK);
  }
  if (tabs[activeTabIndex].messageBuffer.length() > 0)
  {
    canvas->setTextDatum(middle_right);
    canvas->drawString(tabs[activeTabIndex].messageBuffer, ww - m, messageBufferY + messageBufferHeight / 2);
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
        cursorX = ww - m;
        canvas->setTextDatum(top_right);
      }
      else
      {
        cursorX = m + 1;
        canvas->setTextDatum(top_left);
        message.text = String(message.username.c_str()) + " " + message.text; // TODO fix this hack?
      }

      std::vector<String> lines = getMessageLines(message.text, messageWidth);
      for (int j = lines.size() - 1; j >= 0; j--)
      {
        int cursorY = m + (rowCount - linesDrawn - 1) * (canvas->fontHeight() + m);
        // canvas->setTextColor(TFT_SILVER);
        if (j == 0 && !isOwnMessage)
        {
          int usernameWidth = canvas->fontWidth() * message.username.length();

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
  int rowCount = (wh - 2 * m) / (canvas->fontHeight() + m) - 1;
  int colCount = (ww - 2 * m) / canvas->fontWidth() - 1;
  int linesDrawn = 0;
  int cursorX = m + 1;
  int entryOffset = 20;

  canvas->setTextColor(TFT_SILVER);
  canvas->setTextDatum(top_center);
  canvas->drawString("Users Seen", ww / 2, 2 * m);

  canvas->setTextDatum(top_left);
  for (int i = 0; i < presence.size(); i++)
  {
    int cursorY = entryOffset + i * (m + canvas->fontHeight() + m);
    int lastSeenMinutes = (millis() - presence[i].lastSeenMillis) / 1000 / 60;
    String userPresenceString = String(presence[i].username.c_str()) + " RSSI: " + String(presence[i].rssi) + ", last seen: " + String(lastSeenMinutes) + "m";

    // canvas->drawString(userPresenceString, cursorX, cursorY);

    int usernameWidth = canvas->fontWidth() * presence[i].username.length();

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
  canvas->setTextDatum(top_center);

  int settingX = ww / 2;
  int settingOffset = 25;

  String settingLines[SettingsCount];
  settingLines[Settings::Username] = "Username: " + username;
  settingLines[Settings::Brightness] = "Brightness: " + String(brightness);
  settingLines[Settings::TextSize] = "Text Size: TODO"; //+ String(chatTextSize);
  settingLines[Settings::PingMode] = "Ping Mode: " + String(pingMode ? "On" : "Off");
  settingLines[Settings::RepeatMode] = "Repeat Mode: " + String(repeatMode ? "On" : "Off");
  settingLines[Settings::LoRaSettings] = "LoRa Settings: TBD"; 

  canvas->drawString("Settings", settingX, 2 * m);

  for (int i = 0; i < SettingsCount; i++)
  {
    int settingY = settingOffset + i * (m + canvas->fontHeight() + m);

    canvas->setTextColor(i == activeSettingIndex ? COLOR_ORANGE : TFT_SILVER);
    canvas->drawString(settingLines[i], settingX, settingY);
  }
}

void drawMainWindow()
{
  // TODO: avoid complete redraw each time by scrolling text if possible?

  canvas->fillSprite(BG_COLOR);
  canvas->fillRoundRect(0, 0, ww, wh, 3, UX_COLOR_MED);
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

void loraCreateFrame(const String &messageText, uint8_t *frameData, size_t &frameDataLength)
{
  // Ensure the data array has enough space
  // if (length < sizeof(message) + strlen(message.text) + 1) {
  //   std::cerr << "Error: Insufficient space to create the message." << std::endl;
  //   return;
  // }

  frameData[0] = (loraNonce & 0x3F) | ((tabs[activeTabIndex].channel & 0x03) << 6);
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
  message.username = String((const char *)(frameData + 1), MaxUsernameLength);
  size_t messageLength = frameDataLength - 7;
  message.text = String((const char *)(frameData + 7), messageLength - 1);

  // USBSerial.print("username: ");
  // for (int i = 0; i < MaxUsernameLength; i++)
  // {
  //   USBSerial.printf("%02x ", message.username[i]);
  // }
  // USBSerial.print("\nmessage: ");
  // for (int i = 0; i < messageLength; i++)
  // {
  //   USBSerial.printf("%02x ", message.text[i]);
  // }
  // USBSerial.println();
  USBSerial.printf("|%d|%d|%s|%s|\n", message.nonce, message.channel, message.username, message.text.c_str());
}

bool loraSendMessage(const String &messageText, LoRaMessage &sentMessage, int channel = -1)
{
  uint8_t frameData[201]; // TODO: correct max size
  size_t frameDataLength;
  loraCreateFrame(messageText, frameData, frameDataLength);

  USBSerial.print("sending frame: ");
  printHexDump(frameData, frameDataLength);

  if (lora.SendFrame(loraConfig, frameData, frameDataLength) == 0)
  {
    USBSerial.println("sent!");
    sentMessage.channel = channel < 0 ? tabs[activeTabIndex].channel : channel;
    sentMessage.nonce = loraNonce++;
    sentMessage.username = "";
    sentMessage.text = tabs[activeTabIndex].messageBuffer;
    sentMessage.rssi = 0;

    lastSentMillis = millis();

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
      // TODO: check nonce/channel, etc

      recordPresence(message);
      receivedMessage = true;

      if (message.text.isEmpty())
        continue;

      tabs[message.channel].messages.push_back(message);

      if (repeatMode)
      {
        String response = String("name: " + String(message.username) + ", msg: " + String(message.text) + ", rssi: " + String(loraFrame.rssi));
        LoRaMessage sentMessage;
        if (loraSendMessage(response, sentMessage, message.channel))
        {
          tabs[activeTabIndex].messages.push_back(sentMessage);
        }
        // TODO:
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

  while (1)
  {
    if (pingMode && millis() - lastSentMillis > PING_INTERVAL_MS)
    {
      LoRaMessage sentMessage;
      // TODO: for now ping is empty message
      if (loraSendMessage("", sentMessage, 0))
      {
        tabs[activeTabIndex].messages.push_back(sentMessage);
      }
    }

    delay(PING_INTERVAL_MS);
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
      USBSerial.printf("max length reached\n");
    }
  }

  if (keyState.del && str.length() > 0)
  {
    str.remove(str.length() - 1);
    updated = true;
  }

  return updated;
}

void handleChatTabInput(Keyboard_Class::KeysState keyState, Redraw &input)
{
  if (updateStringFromInput(keyState, tabs[activeTabIndex].messageBuffer))
  {
    input = Redraw::Window;
  }
  // for (auto i : keyState.word)
  // {
  //   // TODO: max length
  //   tabs[activeTabIndex].messageBuffer += i;
  //   input = Redraw::Window;
  // }

  // if (keyState.del && tabs[activeTabIndex].messageBuffer.length() > 0)
  // {
  //   tabs[activeTabIndex].messageBuffer.remove(tabs[activeTabIndex].messageBuffer.length() - 1);
  //   input = Redraw::Window;
  // }

  if (keyState.enter)
  {
    USBSerial.println(tabs[activeTabIndex].messageBuffer);

    tabs[activeTabIndex].messageBuffer.trim();

    LoRaMessage sentMessage;
    if (loraSendMessage(tabs[activeTabIndex].messageBuffer, sentMessage))
    {
      tabs[activeTabIndex].messages.push_back(sentMessage);
    }
    else
    {
      sentMessage.text = "send failed";
      tabs[activeTabIndex].messages.push_back(sentMessage);
    }

    tabs[activeTabIndex].messageBuffer.clear();
    input = Redraw::Window;
  }
}

void handleSettingsTabInput(Keyboard_Class::KeysState keyState, Redraw &redraw)
{
  if (M5Cardputer.Keyboard.isKeyPressed(';'))
  {
    activeSettingIndex = (activeSettingIndex == 0)
                             ? SettingsCount - 1
                             : activeSettingIndex - 1;
    redraw = Redraw::Window;
  }
  if (M5Cardputer.Keyboard.isKeyPressed('.'))
  {
    activeSettingIndex = (activeSettingIndex + 1) % SettingsCount;
    redraw = Redraw::Window;
  }

  switch (activeSettingIndex)
  {
  case Settings::Username:
    // dedupe kb input reading into string

    // TODO: need to rename all messages from this user, or just user empty for self messages.
    if (updateStringFromInput(keyState, username, MaxUsernameLength, true))
    {
      redraw = Redraw::SystemBar;
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
        redraw = Redraw::Window;
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
    // break;
  case Settings::PingMode:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
        pingMode = !pingMode;
      redraw = Redraw::Window;
      break;
    }
    break;
  case Settings::RepeatMode:
    for (auto c : keyState.word)
    {
      if (c == ',' || c == '/')
        repeatMode = !repeatMode;
      redraw = Redraw::Window;
      break;
    }
    break;
  }
}

void keyboardInputTask(void *pvParameters)
{
  const unsigned long debounceDelay = 100;
  unsigned long lastKeyPressMillis = 0;

  while (1)
  {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
    {
      Redraw redraw = Redraw::None;
      unsigned long currentMillis = millis();
      if (currentMillis - lastKeyPressMillis >= debounceDelay)
      {
        // need to see again with display off
        if (brightness <= 0) {
          brightness = 50;
          M5Cardputer.Display.setBrightness(brightness);
          redraw = Redraw::Window;
        }

        lastKeyPressMillis = currentMillis;
        Keyboard_Class::KeysState keyState = M5Cardputer.Keyboard.keysState();

        if (activeTabIndex == SettingsTabIndex)
        {
          handleSettingsTabInput(keyState, redraw);
        }
        else if (activeTabIndex == UserInfoTabIndex)
        {
          // TODO?
        }
        else
        {
          handleChatTabInput(keyState, redraw);
        }

        if (keyState.tab)
        {
          activeTabIndex = (activeTabIndex + 1) % TabCount;
          redraw = Redraw::TabBar;
        }
      }

      keyboardInput = redraw;
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
  switch (keyboardInput)
  {
  case Redraw::None:
    break;
  case Redraw::Window:
    drawMainWindow();
    keyboardInput = Redraw::None;
    break;
  case Redraw::TabBar:
    drawTabBar();
    drawMainWindow();
    keyboardInput = Redraw::None;
  case Redraw::SystemBar:
    drawSystemBar();
    drawMainWindow();
    keyboardInput = Redraw::None;
    break;
  }

  if (receivedMessage)
  {
    receivedMessage = false;
    drawMainWindow();
  }

  // redraw occcasionally for battery and rssi updates
  if (millis() > updateDelay)
  {
    bool redraw = false;
    updateDelay = millis() + 2000;

    int newBatteryPct = M5Cardputer.Power.getBatteryLevel();
    if (newBatteryPct != batteryPct)
    {
      batteryPct = newBatteryPct;
      redraw = true;
    }

    int newRssi = getPresenceRssi();
    if (newRssi != maxRssi)
    {
      maxRssi = newRssi;
      redraw = true;
    }

    if (redraw)
    {
      drawSystemBar();
    }
  }

  delay(100);
}