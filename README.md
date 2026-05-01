# MQTT-Button-Panel

A panel of four, customizable, RGB-backlit buttons designed for use with Home Assistant and communication over MQTT via WiFi and Ethernet.

<img width="4624" height="3472" alt="PXL_20260501_205123144 RAW-01 MP COVER" src="https://github.com/user-attachments/assets/71067a1c-6f09-4ba7-a5da-018eb472e060" />

## Hardware

This design is centered around a WT32-ETH01 development board. This is an ESP32 with a LAN8720 Ethernet transceiver allowing for both WiFi and Ethernet connectivity.
On the circuit board are placed the 4 main buttons and two bank/menu buttons. Each button has a WS2812b addressable RGB led placed next to it. The front side of the board also holds the 16x2 character LCD.
The back side of the PCB holds the WT32-ETH01. To program the ESP32, the board is populated with a CP2102 USB to UART converter IC. This allows us to easily program the ESP32 with the USB C connection.
To interface with the character LCD and buttons, two PCF8574 I2C IO expanders are placed on the board. One of which is used for the character LCD while the other is used to read input from the 6 buttons.

## 3D Printed Case

Click [here](/cad/README.md) for detailed 3D printing instructions.

