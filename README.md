Simple chat app for M5 Cardputer and the M5 E220 LoRa modules. Doing for my own learning and enjoyment. Work in progress.

## Hardware used:
- [M5 Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3])
- [M5 E220 LoRa module](https://shop.m5stack.com/products/lora-unit-jp-version-with-antenna-e220)

## Overview:
The interface is split into 5 tabs. The first three tabs are separate chat channels, the next tab shows the status of other users seen, and the last tab is for changing settings. Press the tab key to switch between tabs. The system bar shows the current username, the signal strength of the user seen with the best signal strength, and the Cardputer's current battery level. LoRa modules are currently configured using the default settings from the [LoRa modules's library](https://github.com/m5stack/M5-LoRa-E220-JP). Modules must be configured initially to work, can do so in settings.

## Tab Info
Tab|Image|Info
---|---|---
Chat Tab|![chatWindow](https://github.com/nonik0/CardputerLoRaChat/assets/17152317/2f14c060-d6e2-4bbd-a743-855d09410a38)|A, B, and C chat channels. Use keyboard and enter to type and send messages.
Users Seen|![userTab](https://github.com/nonik0/CardputerLoRaChat/assets/17152317/cbe63ba1-48d6-478e-8def-97ffdfe75c00)|Shows users seen in chat and with pings. Shows last received signal strength and when last seen.
Settings|![settingsTab](https://github.com/nonik0/CardputerLoRaChat/assets/17152317/a966e694-fa3e-4055-949d-657cdcda9707)|Use arrow keys and enter to navigate settings. Use keyboard to update username when highlighted.

## Setting Details

Setting|Info
---|---
Username|Min length 2, max length 8, ASCII only.
Brightness|Set display brightness [0-100]. If set very low, the display will automatically brighten when buttons are pressed.
Ping Mode|Send an occassional ping when not sending messages to show presence to other users.
Repeat Mode|Repeat back messages received. Just a testing function for now.
App Config|Writes current settings (username, brightness, ping mode, repeat mode) to SD card, will be reloaded
LoRa Config|Writes register values to LoRa module. M0 and M1 switches must be set to off (config mode).

## TODO
See TODOs in code for now.

**ideas:**
- ACK Mode: know when users receive your message, resend/catch up?
- Mesh Mode: repeat messages from other users to extend range
- message scrolling
- saving chats to SD
