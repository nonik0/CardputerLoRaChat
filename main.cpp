#include <Arduino.h>
#include <EEPROM.h>
#include <M5Cardputer.h>
#include <M5StackUpdater.h>
#include <M5_LoRa_E220_JP.h>

#include "draw_helper.h"

// color palette, eventually configurable
#define BG_COLOR BLACK
#define UX_COLOR_DARK COLOR_DARKGRAY
#define UX_COLOR_MED COLOR_MEDGRAY
#define UX_COLOR_LIGHT COLOR_LIGHTGRAY
#define UX_COLOR_ACCENT COLOR_ORANGE
#define UX_COLOR_ACCENT2 YELLOW

enum KeyboardInput
{
  TextInput,
  ChangeTab,
  None
};

// TODO: refine message format
struct LoRaMessage
{
  uint8_t nonce : 6;
  uint8_t channel : 2;
  char username[6]; // use MAC or something tied to device?
  // unsigned long millis; // used to synchronize with local clock for relative timestamps
  int rssi;
  char text[100]; // make variable?
};

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

volatile bool receivedMessage = false; // signal to redraw window
volatile KeyboardInput keyboardInput = KeyboardInput::None;
bool repeatMode = false;

// TODO: extract drawing functionality to class?
M5Canvas *canvas;
M5Canvas *canvasSystemBar;
M5Canvas *canvasTabBar;

// tab state
unsigned short activeTabIndex;
const unsigned short TabCount = 5;
Tab tabs[TabCount];

// settings
const uint8_t MaxUsernameLength = 6;
char username[MaxUsernameLength] = "nonik";
float chatTextSize = 1.0;

// display layout constants
const int w = 240; // M5Cardputer.Display.width();
const int h = 135; // M5Cardputer.Display.height();
const int m = 3;

const int sx = m;
const int sy = 0;
const int sw = w - 2 * m;
const int sh = 18;

const int tx = m;
const int ty = sy + sh + m;
const int tw = 18;
const int th = h - ty - m;

const int wx = tw;
const int wy = sy + sh + m;
const int ww = w - wx - m;
const int wh = h - wy - m;

int batteryPct = M5Cardputer.Power.getBatteryLevel();
int updateDelay = 0;

// String generateRandomMessage()
// {
//   std::srand(millis());
//   std::vector<String> messages = {
//       "Hello, world!",
//       "How are you today?",
//       "Coding is fun!",
//       "Random messages are cool!",
//       "Have a great day!",
//       "C++ is awesome!",
//       "Keep calm and code on!",
//       "Make it simple but significant.",
//       "The best way to predict the future is to invent it.",
//       "Do or do not. There is no try.",
//       "Stay hungry, stay foolish.",
//       "The only limit is your imagination.",
//       "Failure is the opportunity to begin again more intelligently.",
//       "In coding we trust.",
//       "May the code be with you!",
//       "Life is 10% what happens to us and 90% how we react to it.",
//       "Dream big, work hard, stay focused.",
//       "Success is not final, failure is not fatal: It is the courage to continue that counts.",
//       "Believe you can and you're halfway there.",
//       "The harder you work for something, the greater you'll feel when you achieve it.",
//       "The only way to do great work is to love what you do.",
//       "It always seems impossible until it's done.",
//       "Success is stumbling from failure to failure with no loss of enthusiasm."};
//   int randomIndex = std::rand() % messages.size();
//   return messages[randomIndex];
// }

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

// splits string at whitespace into lines of max length
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

  // USBSerial.printf("text: %s\n", message.text.c_str());
  // USBSerial.printf("lines:");
  // for (String line : lines)
  // {
  //   USBSerial.printf("[%s]", line.c_str());
  // }
  // USBSerial.println();

  return messageLines;
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
  canvasSystemBar->drawString(username, m, sy + sh / 2);
  draw_rssi_indicator(canvasSystemBar, sw - 64, sy + sh / 2, batteryPct);
  draw_battery_indicator(canvasSystemBar, sw - 32, sy + sh / 2, batteryPct);
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
    case 3:
      draw_user_icon(canvasTabBar, tw / 2 - 2, taby + tabh / 2 - tabf / 2 - 3);
      break;
    case 4:
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
      // TODO: username
      std::vector<String> lines = getMessageLines(message.text, colCount);

      int cursorX;

      if (std::strncmp(message.username, username, MaxUsernameLength) == 0)
      {
        cursorX = ww - m;
        canvas->setTextDatum(top_right);
      }
      else
      {
        cursorX = m;
        canvas->setTextDatum(top_left);
      }

      for (int j = lines.size() - 1; j >= 0; j--)
      {
        int cursorY = m + (rowCount - linesDrawn - 1) * (canvas->fontHeight() + m);
        canvas->drawString(lines[j], cursorX, cursorY);
        linesDrawn++;

        if (linesDrawn >= rowCount)
        {
          break;
        }
      }
    }
  }
}

void drawSettingsWindow()
{
  canvas->fillSprite(BG_COLOR);
  canvas->fillRoundRect(0, 0, ww, wh, 3, UX_COLOR_MED);
  canvas->setTextColor(TFT_SILVER, UX_COLOR_MED);
  canvas->setTextDatum(top_left);

  canvas->drawString("Settings", m, m);
  canvas->drawString("Name", m, m + canvas->fontHeight() + m);
  canvas->drawString("Lora Coonfig", m, m + 2 * (canvas->fontHeight() + m));
}

void drawWindow()
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
  case 3:
    drawChatWindow();
    break;
  case 4:
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

void checkForRepeatMode()
{
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isKeyPressed('r'))
  {
    repeatMode = true;
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

  unsigned long timestamp = millis();

  frameData[0] = (loraNonce & 0x3F) | ((tabs[activeTabIndex].channel & 0x03) << 6);
  std::memcpy(frameData + 1, username, MaxUsernameLength);
  // std::memcpy(frameData + 7, static_cast<const void*>(&timestamp), sizeof(unsigned long));
  std::memcpy(frameData + MaxUsernameLength + 1, messageText.c_str(), messageText.length() + 1);
  frameDataLength = MaxUsernameLength + messageText.length() + 2;
}

void loraParseFrame(const uint8_t *frameData, size_t frameDataLength, LoRaMessage &message)
{
  if (frameDataLength < 8) // TODO: max
    return;

  message.nonce = (frameData[0] & 0x3F);
  message.channel = ((frameData[0] >> 6) & 0x03);
  std::memcpy(message.username, frameData + 1, 6);
  // frame.millis = *reinterpret_cast<const unsigned long *>(data + sizeof(unsigned long));
  size_t messageLength = frameDataLength - 7;
  std::memcpy(message.text, frameData + 7, messageLength);
  message.text[messageLength] = '\0';

  USBSerial.printf("|%d|%d|%s|%s|\n", message.nonce, message.channel, message.username, message.text);
}

bool loraSendMessage(const String &messageText, LoRaMessage &sentMessage)
{
  uint8_t frameData[201]; // TODO: correct max size
  size_t frameDataLength;
  loraCreateFrame(messageText, frameData, frameDataLength);

  USBSerial.print("sending frame: ");
  printHexDump(frameData, frameDataLength);

  if (lora.SendFrame(loraConfig, frameData, frameDataLength) == 0)
  {
    USBSerial.println("sent!");
    sentMessage.channel = tabs[activeTabIndex].channel;
    sentMessage.nonce = loraNonce++;
    std::memcpy(sentMessage.username, username, 6);
    std::memcpy(sentMessage.text, tabs[activeTabIndex].messageBuffer.c_str(), tabs[activeTabIndex].messageBuffer.length() + 1);
    sentMessage.rssi = 0;

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

      tabs[message.channel].messages.push_back(message);
      receivedMessage = true;

      if (repeatMode)
      {
        String response = String("message: " + String(message.text) + ", rssi: " + String(loraFrame.rssi));
        LoRaMessage sentMessage;
        if (loraSendMessage(response, sentMessage))
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

void keyboardInputTask(void *pvParameters)
{
  const unsigned long debounceDelay = 200;
  unsigned long lastKeyPressMillis = 0;

  while (1)
  {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
    {
      KeyboardInput input = KeyboardInput::None;
      unsigned long currentMillis = millis();
      if (currentMillis - lastKeyPressMillis >= debounceDelay)
      {
        lastKeyPressMillis = currentMillis;
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        for (auto i : status.word)
        {
          // TODO: max length
          tabs[activeTabIndex].messageBuffer += i;
          input = KeyboardInput::TextInput;
        }

        if (status.del && tabs[activeTabIndex].messageBuffer.length() > 0)
        {
          tabs[activeTabIndex].messageBuffer.remove(tabs[activeTabIndex].messageBuffer.length() - 1);
          input = KeyboardInput::TextInput;
        }

        if (status.enter)
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
            strcpy(sentMessage.text, "send failed");
            tabs[activeTabIndex].messages.push_back(sentMessage);
          }

          tabs[activeTabIndex].messageBuffer.clear();
          input = KeyboardInput::TextInput;
        }
        else if (status.tab)
        {
          activeTabIndex = (activeTabIndex + 1) % TabCount;
          input = KeyboardInput::ChangeTab;
        }
      }

      keyboardInput = input;
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
  checkForRepeatMode();

  M5Cardputer.Display.init();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(repeatMode ? 0 : 70);

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

  drawSystemBar();
  drawTabBar();
  drawWindow();

  loraInit();

  xTaskCreateUniversal(loraReceiveTask, "loraReceiveTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
  xTaskCreateUniversal(keyboardInputTask, "keyboardInputTask", 8192, NULL, 1, NULL, APP_CPU_NUM);
}

void loop()
{
  // primarily handles drawing state changes from input/receive tasks

  switch (keyboardInput)
  {
  case KeyboardInput::None:
    break;
  case KeyboardInput::TextInput:
    drawWindow();
    keyboardInput = KeyboardInput::None;
    break;
  case KeyboardInput::ChangeTab:
    drawTabBar();
    drawWindow();
    keyboardInput = KeyboardInput::None;
    break;
  }

  if (receivedMessage)
  {
    receivedMessage = false;
    drawWindow();
  }

  // redraw occcasionally for battery and bt connection status updates
  if (millis() > updateDelay)
  {
    updateDelay = millis() + 2000;
    int newBatteryPct = M5Cardputer.Power.getBatteryLevel();
    if (newBatteryPct != batteryPct)
    {
      batteryPct = newBatteryPct;
      drawSystemBar();
    }
  }

  delay(100);
}