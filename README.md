# MQTT-Button-Panel

Ground-up hardware bring-up firmware for a WT32-ETH01 + custom button panel.

Current firmware purpose: verify all hardware blocks before implementing final device functionality.

## Tested Hardware Blocks

- Ethernet link and DHCP IP events (WT32-ETH01 LAN PHY)
- I2C bus scan
- PCF8574 at `0x27` for 16x2 HD44780 LCD backpack
- PCF8574 at `0x26` for 6 button inputs
- 6 capacitive touch rings (ESP32 touch channels)
- WS2812 LED chain (6 pixels)

## Pin/Address Assumptions (from old firmware)

- I2C SDA: GPIO14
- I2C SCL: GPIO5
- LCD PCF8574: `0x27`
- Button PCF8574: `0x26`
- WS2812 data: GPIO17
- Touch rings: GPIO4, GPIO15, GPIO2, GPIO12, GPIO32, GPIO33

If your schematic differs, edit constants in `src/main.c`.

## Runtime Behavior

- Serial log prints:
	- I2C scan results
	- Ethernet link/IP events
	- Button raw and debounced states
	- Touch filtered values
- LCD:
	- Row 1: button press summary + Ethernet link (`E:U` up / `E:D` down)
	- Row 2: touch summary + IP status (`IP:Y` or `IP:N`)
- LEDs:
	- Red: idle
	- Green: physical button pressed
	- Blue: capacitive touch active