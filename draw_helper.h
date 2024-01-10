#include <M5Cardputer.h>

// RBG565 colors
const unsigned short COLOR_BLACK = 0x18E3;
const unsigned short COLOR_DARKGRAY = 0x0861;
const unsigned short COLOR_MEDGRAY = 0x2104;
const unsigned short COLOR_LIGHTGRAY = 0x4208;
const unsigned short COLOR_DARKRED = 0x5800;
const unsigned short COLOR_ORANGE = 0xEBC3;
const unsigned short COLOR_TEAL = 0x07CC;
const unsigned short COLOR_BLUEGRAY = 0x0B0C;
const unsigned short COLOR_BLUE = 0x026E;
const unsigned short COLOR_PURPLE = 0x7075;

inline void draw_battery_indicator(M5Canvas* canvas, int x, int y,
                                   int batteryPct) {
  int battw = 24;
  int batth = 11;

  int ya = y - batth / 2;

  // determine battery color and charge width from charge level
  int chgw = (battw - 2) * batteryPct / 100;
  uint16_t batColor = COLOR_TEAL;
  if (batteryPct < 100) {
    int r = ((100 - batteryPct) / 100.0) * 256;
    int g = (batteryPct / 100.0) * 256;
    batColor = canvas->color565(r, g, 0);
  }
  canvas->fillRoundRect(x, ya, battw, batth, 2, TFT_SILVER);
  canvas->fillRect(x - 2, y - 2, 2, 4, TFT_SILVER);
  canvas->fillRect(x + 1, ya + 1, battw - 2 - chgw, batth - 2,
                   COLOR_DARKGRAY);  // 1px margin from outer battery
  canvas->fillRect(x + 1 + battw - 2 - chgw, ya + 1, chgw, batth - 2,
                   batColor);  // 1px margin from outer battery
}