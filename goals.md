# Functional Requirements Specification: Church Automation Button Panel

## 1. Project Overview
This document outlines the functional goals and operational requirements for a network-connected button panel, designed primarily for church lighting and systems automation. The panel serves as an interactive MQTT client, sending commands to Home Assistant, which acts as the central automation hub. Key features include dynamic button mapping via "fader-style" banks, capacitive touch previewing, RGB status indication, seamless network failover, and comprehensive web-based configuration.

## 2. Hardware Overview
* **Processing & Connectivity:** WT32-ETH01 (Provides both Ethernet and Wi-Fi capabilities).
* **Programming Interface:** Custom PCB featuring a CP2102 for USB-C programming and debugging.
* **Display:** 16x2 Character LCD for local feedback and menus.
* **Physical Controls:** * 4x Main Action Buttons.
    * 2x Bank Control Buttons (Up / Down).
* **Sensory Inputs:** 6x capacitive touch rings (one surrounding each physical button).
* **Visual Indicators:** 6x Addressable RGB LEDs located beneath each button.

## 3. Functional Requirements

### 3.1. Bank and Button Management
* **Bank Shifting:** The panel must support multiple "banks" of button layouts, similar to a digital mixing console. The Bank Up and Bank Down buttons shift the logical assignment of the 4 main physical buttons to a new set of functions.
* **Configurable Button Types:** Each logical button (e.g., Button 2 on Bank 3) must be individually configurable to behave as:
    * `Toggle / On-Off`: Alternates between two states per press.
    * `Momentary`: Active only while physically held down.
    * `Radio / Scene`: Pressing the button activates it and deactivates any other buttons assigned to the same "radio group."
* **State Synchronization:** The panel must track the state of each button locally to control its LED, while simultaneously listening for external state changes broadcasted via MQTT (ensuring two-way sync with Home Assistant).

### 3.2. Capacitive Touch "Preview" Mode
* **Touch to Preview:** Touching the capacitive ring around any button *without physically depressing it* must trigger a preview action.
* **Display Feedback:** Upon touch detection, the 16x2 LCD must immediately display the configured name and intended function of that specific button.
* **Auto-Revert:** When the user removes their finger from the capacitive ring, the LCD must automatically revert to its default idle view.

### 3.3. LCD Display Behavior
* **Idle State:** * Top Line: Displays the custom Panel Name or the currently active Bank Name.
    * Bottom Line: Displays network status, active IP address, or a general system summary.
* **Preview State (During Touch):**
    * Top Line: "Button: [Assigned Button Name]"
    * Bottom Line: "Action: [Assigned Function/Current State]"
* **Menu State:** Displays navigable settings when the local menu is activated (see Section 3.7).

### 3.4. LED Status Management
* **State Indication:** The RGB LED beneath each button reflects its current state.
* **Customization:** The colors for "ON" and "OFF" states must be user-configurable per button via the Web UI. The system defaults to Green for ON and Red for OFF.
* **Bank Transition Feedback:** The LEDs under the Bank Up/Down buttons should provide visual feedback (e.g., a brief flash) to confirm a successful bank change, or indicate if the user has reached the highest/lowest available bank.

### 3.5. Network Connectivity and Failover
* **Primary Link:** Hardwired Ethernet.
* **Secondary Link:** Wi-Fi.
* **Automatic Failover:** The system must actively monitor the Ethernet connection. If the physical link drops or the connection to the MQTT broker is lost, the panel must seamlessly and automatically switch to the pre-configured Wi-Fi network to maintain functionality.
* **IP Management:** The panel must support both DHCP (default) and Static IP addressing.

### 3.6. MQTT Integration
* **Standardized Communication:** All actions and state changes are communicated to the Home Assistant broker via MQTT.
* **Configurable Topics:** Base topic structures and individual button labels must be entirely customizable via the Web UI to fit the user's existing Home Assistant naming conventions.
* **Standardized Payloads:** The panel should use standard, easily parseable payloads (e.g., simple strings like "ON", "OFF" or basic JSON).

### 3.7. Local Device Menu
* **Activation:** The user can access a local configuration menu directly on the panel by pressing and holding both the Bank Up and Bank Down buttons simultaneously.
* **Navigation:** The Bank Up/Down buttons are used to scroll through menu items, while specific main buttons act as "Select" and "Back/Exit".
* **Menu Capabilities:**
    * **Network Diagnostics:** View active interface (ETH vs. Wi-Fi), IP Address, and MAC Address.
    * **IP Configuration:** Toggle between DHCP and Static IP modes, and manually alter a static IP address directly from the panel.
    * **System Control:** Trigger a device reboot or initiate a full factory reset.

### 3.8. Web Configuration Interface
* **Accessibility:** The panel must host a local web server accessible via standard web browsers.
* **Global Settings:** Configure Panel Name, Max Number of Banks, and MQTT Broker details (IP, Port, Username, Password).
* **Network Settings:** Input Wi-Fi credentials and define IP configurations.
* **Button Matrix Setup:** A comprehensive interface to define the behavior for every button on every bank. This includes:
    * The Button's Display Name (for the LCD).
    * The specific MQTT label/topic.
    * The Button Type (Toggle, Momentary, Scene Group).
    * Custom RGB HEX codes for ON and OFF states.
* **Persistence:** All settings configured in the web interface must be saved securely to non-volatile memory so they survive reboots and power cycles.
