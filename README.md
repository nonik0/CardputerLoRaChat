Simple chat app for M5 Cardputer and the M5 E220 LoRa modules. Doing for my own learning and enjoyment. Work in progress.

## Hardware used:
- [M5 Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3])
- [M5 E220 LoRa module](https://shop.m5stack.com/products/lora-unit-jp-version-with-antenna-e220)

## Overview:
The interface is split into 5 tabs. The first three tabs are separate chat channels, the next tab shows the status of other users seen, and the last tab is for changing settings. Press the tab key to switch between tabs. Use arrow keys and enter to navigate settings. The system bar shows the username, the signal strength of user with the best signal strength, and the device battery. LoRa modules are currently configured using the default settings from the device's library. Modules must be configured initially to work, can do so in settings (need to put in config mode! i.e. flip M0/M1 switches before writing settings).

Ping Mode: send occassional ping when not sending messages to show presence to other users
Repeat Mode: repeat back message (testing for now)

**ideas:**
ACK Mode: know when users receive your message, resend/catch up?
Mesh Mode: repeat messages from other users to extend range

## UX Pics:
TODO
