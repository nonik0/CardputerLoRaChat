#include <Arduino.h>
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

M5Canvas oldCanvas(&M5Cardputer.Display);

// TODO: extract drawing functionality to class
M5Canvas *canvas;
M5Canvas *canvasSystemBar;
M5Canvas *canvasTabBar;

unsigned short activeTabIndex;
const unsigned short TabCount = 5;

const unsigned long debounceDelay = 200;
unsigned long lastKeyPressMillis = 0;

int w = 240; // M5Cardputer.Display.width();
int h = 135; // M5Cardputer.Display.height();
int m = 3;

int sx = m;
int sy = m;
int sw = w - 2 * m;
int sh = 18;

int tx = m;
int ty = sy + sh + m;
int tw = 18;
int th = h - ty - m;

int wx = tw;
int wy = sy + sh + m;
int ww = w - wx - m;
int wh = h - wy - m;

int batteryPct = M5Cardputer.Power.getBatteryLevel();
int updateDelay = 0;

struct Message
{
  // TODO: timestamp, sender ID, etc.
  // time_t timestamp;
  bool isSender;
  String text;
};

struct Tab
{
  unsigned char channel;
  std::vector<Message> messages;
  String messageBuffer;
  int viewIndex;
};
Tab tabs[5];

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

void drawSystemBar()
{
  canvasSystemBar->fillSprite(BG_COLOR);
  canvasSystemBar->fillRoundRect(0, 0, sw, sh, 3, UX_COLOR_DARK);
  canvasSystemBar->setTextColor(TFT_SILVER, UX_COLOR_DARK);
  canvasSystemBar->setTextDatum(middle_center);
  canvasSystemBar->setTextSize(1);
  canvasSystemBar->drawString("LoRaChat", sw / 2, sh / 2);
  canvasSystemBar->setTextDatum(middle_left);
  canvasSystemBar->drawString("nonik", m, sh / 2);
  draw_battery_indicator(canvasSystemBar, sw - 32, sh / 2, batteryPct);
  canvasSystemBar->pushSprite(sx, sy);
}


void drawTabBar()
{
  canvasTabBar->fillSprite(BG_COLOR);
  int tabh = (th + 4 * m) / 5;
  int tabf = 5;
  // int taby = tabf;

  //unsigned short color[] = {UX_COLOR_DARK, UX_COLOR_MED, UX_COLOR_LIGHT, UX_COLOR_ACCENT, UX_COLOR_ACCENT2};
  for (int i = 4; i >= -1; i--)
  {
    // draw active tab last/on top
    if (i == activeTabIndex)
    {
      continue;
    }
    else if (i < 0) {
      i = activeTabIndex;
    }

    unsigned short color = (i == activeTabIndex) ? UX_COLOR_ACCENT : UX_COLOR_DARK;

    int taby = tabf + i * tabh - i * m;
    // tab shape
    canvasTabBar->fillTriangle(0, taby, tw, taby, tw, taby - tabf, color);
    canvasTabBar->fillRect(0, taby, tw, tabh - 2 * tabf, color);
    canvasTabBar->fillTriangle(0, taby + tabh - 2 * tabf, tw, taby + tabh - 2 * tabf, tw, taby + tabh - tabf, color);
    // label
    canvasTabBar->setTextColor(TFT_SILVER, color);
    canvasTabBar->setTextDatum(middle_center);
    canvasTabBar->drawString(String(i), tw / 2 - 1, taby + tabh / 2 - tabf / 2 - 1);

    if (i == activeTabIndex)
    {
      break;
    }
  }
  canvasTabBar->pushSprite(tx, ty);
}

// splits string at whitespace into lines of max length
std::vector<String> getMessageLines(const String &s, int maxLength)
{
    std::vector<String> result;
    String currentLine;
    String word;

    for (char c : s) {
        if (std::isspace(c)) {
            if (currentLine.length() + word.length() <= maxLength) {
                currentLine += (currentLine.isEmpty() ? "" : " ") + word;
                word.clear();
            } else {
                result.push_back(currentLine);
                currentLine.clear();
                word.clear();
            }
        } else {
            word += c;
        }
    }

    if (!currentLine.isEmpty() || !word.isEmpty()) {
        currentLine += (currentLine.isEmpty() ? "" : " ") + word;
        result.push_back(currentLine);
    }

    return result;
}

void drawWindow()
{
  // TODO: avoid complete redraw each time by scrolling text if possible?

  canvas->fillSprite(BG_COLOR);
  canvas->fillRoundRect(0, 0, ww, wh, 3, UX_COLOR_MED);
  canvas->setTextColor(TFT_SILVER, UX_COLOR_MED);
  canvas->setTextDatum(top_left);

  // TODO: only change when font changes
  int rowCount = (wh - 2 * m) / (canvas->fontHeight() + m) - 1;
  int colCount = (ww - 2 * m) / canvas->fontWidth() - 1;
  int messageBufferHeight = wh - ((canvas->fontHeight() + m) * rowCount) - m; // buffer takes last row plus extra space
  int messageBufferY = wh - messageBufferHeight;

  // draw message buffer
  for (int i = 0; i < m; i++)
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
    int messagesDrawn = 0;

    // draw all messages or until window is full
    for (int i = tabs[activeTabIndex].messages.size() - 1; i >= 0; i--)
    {
      Message message = tabs[activeTabIndex].messages[i];
      std::vector<String> lines = getMessageLines(message.text, colCount);

      // USBSerial.printf("text: %s\n", message.text.c_str());
      // USBSerial.printf("lines:");
      // for (String line : lines)
      // {
      //   USBSerial.printf("[%s]", line.c_str());
      // }
      // USBSerial.println();

      int cursorX;

      if (message.isSender)
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
        String line = lines[j];

        int cursorY = m + (rowCount - messagesDrawn - 1) * (canvas->fontHeight() + m);
        canvas->drawString(line, cursorX, cursorY);

        messagesDrawn++;
        if (messagesDrawn >= rowCount)
        {
          break;
        }
      }

      if (messagesDrawn >= rowCount)
      {
        break;
      }
    }
  }

  canvas->pushSprite(wx, wy);
}

String generateRandomMessage()
{
  std::srand(millis());
  std::vector<String> messages = {
      "Hello, world!",
      "How are you today?",
      "Coding is fun!",
      "Random messages are cool!",
      "Have a great day!",
      "C++ is awesome!",
      "Keep calm and code on!",
      "Make it simple but significant.",
      "The best way to predict the future is to invent it.",
      "Do or do not. There is no try.",
      "Stay hungry, stay foolish.",
      "The only limit is your imagination.",
      "Failure is the opportunity to begin again more intelligently.",
      "In coding we trust.",
      "May the code be with you!",
      "Life is 10% what happens to us and 90% how we react to it.",
      "Dream big, work hard, stay focused.",
      "Success is not final, failure is not fatal: It is the courage to continue that counts.",
      "Believe you can and you're halfway there.",
      "The harder you work for something, the greater you'll feel when you achieve it.",
      "The only way to do great work is to love what you do.",
      "It always seems impossible until it's done.",
      "Success is stumbling from failure to failure with no loss of enthusiasm."};
  int randomIndex = std::rand() % messages.size();
  return messages[randomIndex];
}

int cursorX = 0;
int cursorY = 0;

bool receiveMessage(String message)
{
  // USBSerial.println(message);
  tabs[activeTabIndex].messages.push_back({false, message});
  return true;
}

enum KeyboardCommand
{
  TextInput,
  ChangeTab,
  None
};

KeyboardCommand waitForKeyboardInput()
{
  KeyboardCommand command = KeyboardCommand::None;

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
  {
    unsigned long currentMillis = millis();
    if (currentMillis - lastKeyPressMillis >= debounceDelay)
    {
      lastKeyPressMillis = currentMillis;
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      for (auto i : status.word)
      {
        // TODO: max length
        tabs[activeTabIndex].messageBuffer += i;
        command = KeyboardCommand::TextInput;
      }

      if (status.del && tabs[activeTabIndex].messageBuffer.length() > 0)
      {
        tabs[activeTabIndex].messageBuffer.remove(tabs[activeTabIndex].messageBuffer.length() - 1);
        command = KeyboardCommand::TextInput;
        // TODO: remove last char from display when not redrawing full sprite
      }

      if (status.enter)
      {
        USBSerial.println(tabs[activeTabIndex].messageBuffer);

        tabs[activeTabIndex].messageBuffer.trim();
        tabs[activeTabIndex].messages.push_back({true, tabs[activeTabIndex].messageBuffer});
        tabs[activeTabIndex].messageBuffer.clear();
        command = KeyboardCommand::TextInput;
      }
      else if (status.tab)
      {
        activeTabIndex = (activeTabIndex + 1) % TabCount;
        command = KeyboardCommand::ChangeTab;
      }
    }
  }

  return command;
}

void setup()
{
  USBSerial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  checkForMenuBoot();

  M5Cardputer.Display.init();
  M5Cardputer.Display.setRotation(1);
  // M5Cardputer.Display.setBrightness(100);

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
}

unsigned long delayTimer = -1;
void loop()
{
  M5Cardputer.update();

  KeyboardCommand command = waitForKeyboardInput();
  switch (command)
  {
  case KeyboardCommand::TextInput:
    drawWindow();
    break;
  case KeyboardCommand::ChangeTab:
    drawTabBar();
    drawWindow();
    break;
  case KeyboardCommand::None:
    break;
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

    // TEMP: debug incoming msgs
    if (receiveMessage(generateRandomMessage()))
    {
      drawWindow();
    }
  }
}