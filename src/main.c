#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rmt.h"
#include "driver/touch_pad.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "nvs.h"
#include "cJSON.h"
#include "web_assets.h"

#include "mqtt_client.h"
#include "esp_system.h"
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))
static const char *TAG = "panel_main";

/* -------------------------------------------------------------------------- */
/* Hardware mapping                                                           */
/*                                                                            */
/* These values describe the board wiring. Keeping them centralized makes it  */
/* easier to change hardware without chasing pin numbers through the rest of  */
/* the code.                                                                  */
/* -------------------------------------------------------------------------- */
#define ETH_PHY_ADDR 1
#define ETH_PHY_RST_GPIO -1
#define ETH_MDC_GPIO 23
#define ETH_MDIO_GPIO 18
#define ETH_PHY_POWER_GPIO 16

#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_14
#define I2C_SCL GPIO_NUM_5
#define I2C_FREQ_HZ 100000

#define PCF_LCD_ADDR 0x27
#define PCF_BUTTON_ADDR 0x26

#define LCD_PIN_EN 0x04
#define LCD_PIN_RW 0x02
#define LCD_PIN_RS 0x01
#define LCD_PIN_BL 0x08

#define BUTTON_COUNT 6
static const uint8_t k_button_pcf_bits[BUTTON_COUNT] = {5, 4, 3, 2, 1, 0};

static const touch_pad_t k_touch_channels[BUTTON_COUNT] = {
    TOUCH_PAD_NUM0, TOUCH_PAD_NUM3, TOUCH_PAD_NUM2, 
    TOUCH_PAD_NUM5, TOUCH_PAD_NUM9, TOUCH_PAD_NUM8,
};

#define LED_COUNT 6
#define LED_PIN GPIO_NUM_17
#define WS2812_RMT_CHANNEL RMT_CHANNEL_0
#define WS2812_RMT_CLK_DIV 2
#define WS2812_T0H_TICKS 14
#define WS2812_T0L_TICKS 32
#define WS2812_T1H_TICKS 28
#define WS2812_T1L_TICKS 24
#define WS2812_RESET_TICKS 2400

/* -------------------------------------------------------------------------- */
/* Panel configuration model                                                  */
/*                                                                            */
/* The runtime UI is driven by a small in-memory description of the panel:    */
/* banks, buttons, MQTT topics, and LED colors.                               */
/* -------------------------------------------------------------------------- */
#define MAX_BANKS 8
#define MAIN_BTN_COUNT 4
#define BTN_BANK_DOWN 4
#define BTN_BANK_UP 5
#define CONFIG_FORMAT_VERSION 2

typedef enum { BTN_TYPE_TOGGLE, BTN_TYPE_MOMENTARY, BTN_TYPE_RADIO } btn_type_t;

typedef struct {
    char display_name[17];
    char entity_id[64];
    btn_type_t type;
    uint8_t color_on[3];  // RGB
    uint8_t color_off[3]; // RGB
    uint8_t radio_group;  // 0 means no group
} button_config_t;

typedef struct {
    char bank_name[17];
    button_config_t buttons[MAIN_BTN_COUNT];
} bank_config_t;

typedef struct {
    char panel_name[17];
    uint8_t num_banks;
    bank_config_t banks[MAX_BANKS];
} system_config_t;

typedef struct {
    char broker_ip[32];
    char port[8];      // Kept as string for easier HTML form parsing
    char username[32];
    char password[64];
} mqtt_config_t;

typedef struct {
    char ssid[33];           // WiFi SSID (max 32 chars)
    char password[64];       // WiFi password
    bool use_dhcp;           // true = DHCP, false = static IP
    char static_ip[16];      // Static IP address (e.g., "192.168.1.50")
    char static_netmask[16]; // Netmask (e.g., "255.255.255.0")
    char static_gateway[16]; // Gateway (e.g., "192.168.1.1")
} app_wifi_config_t;

/* Default MQTT settings are loaded into RAM first, then overwritten from     */
/* NVS if the user has already saved values from the web UI.                  */
static mqtt_config_t s_mqtt_config = {
    .broker_ip = "192.168.1.100",
    .port = "1883",
    .username = "",
    .password = ""
};

/* Default WiFi settings */
static app_wifi_config_t s_wifi_config = {
    .ssid = "YOUR_SSID",
    .password = "YOUR_PASS",
    .use_dhcp = true,
    .static_ip = "192.168.1.100",
    .static_netmask = "255.255.255.0",
    .static_gateway = "192.168.1.1"
};

/* -------------------------------------------------------------------------- */
/* Runtime state                                                              */
/*                                                                            */
/* These globals are intentionally split between persistent configuration     */
/* and live state. The hardware task only reports changes; the logic task      */
/* decides what those changes mean for the UI and MQTT state.                 */
/* -------------------------------------------------------------------------- */
static system_config_t s_config;
static uint8_t s_current_bank = 0;
static bool s_btn_states[MAX_BANKS][MAIN_BTN_COUNT] = {false}; // Logical states (ON/OFF)
static bool s_in_menu = false;
static int s_menu_index = 0;
static const char *s_menu_items[] = { "Panel", "WiFi", "MQTT", "Save" };
static const int s_menu_items_count = sizeof(s_menu_items) / sizeof(s_menu_items[0]);
static const char *s_menu_button_funcs[] = { "Select", "Action", "Save", "Extra" };
static bool s_mqtt_connected = false;
/* Startup animation keeps running until boot is fully complete.              */
static bool s_startup_complete = false;
static bool s_bank_btns_held[2] = {false, false};

/* Connection flags let the UI decide whether to show online/offline cues     */
/* without needing to query the networking stack repeatedly.                  */
static bool s_eth_connected = false;
static bool s_wifi_connected = false;

/* Input events are funneled through a queue so the tasks stay decoupled.     */
static QueueHandle_t s_btn_event_queue;

typedef enum { EVT_BTN_PRESSED, EVT_BTN_RELEASED, EVT_TOUCH_START, EVT_TOUCH_END, EVT_MQTT_SYNC } event_type_t;
typedef struct {
    event_type_t type;
    uint8_t btn_idx;
} input_event_t;


/* RMT frame buffer used to encode one WS2812 LED update.                     */
static rmt_item32_t s_led_items[(LED_COUNT * 24) + 1];
static bool s_eth_link = false;
static bool s_eth_has_ip = false;

/* Touch sensing is based on a baseline captured at boot.                    */
static uint16_t s_touch_baseline[BUTTON_COUNT] = {0};
static bool s_touch_ready = false;

static uint8_t s_button_state = 0xFF;
static uint8_t s_button_last_state = 0xFF;
static int64_t s_button_last_change_ms[BUTTON_COUNT] = {0};

/* Cache the last LCD output so we can avoid sending unchanged text over I2C. */
static char s_lcd_last_line1[17] = {0};
static char s_lcd_last_line2[17] = {0};
static SemaphoreHandle_t s_i2c_mutex = NULL;
static SemaphoreHandle_t s_lcd_mutex = NULL;

/* Keep configuration within safe bounds even if NVS data is old/corrupt. */
static void sanitize_config(void)
{
    if (s_config.num_banks < 1 || s_config.num_banks > MAX_BANKS) {
        s_config.num_banks = 1;
    }

    s_config.panel_name[sizeof(s_config.panel_name) - 1] = '\0';

    for (int b = 0; b < MAX_BANKS; ++b) {
        s_config.banks[b].bank_name[sizeof(s_config.banks[b].bank_name) - 1] = '\0';
        for (int i = 0; i < MAIN_BTN_COUNT; ++i) {
            button_config_t *btn = &s_config.banks[b].buttons[i];
            btn->display_name[sizeof(btn->display_name) - 1] = '\0';
            btn->entity_id[sizeof(btn->entity_id) - 1] = '\0';
            size_t topic_len = strnlen(btn->entity_id, sizeof(btn->entity_id));
            while (topic_len > 0) {
                unsigned char last = (unsigned char)btn->entity_id[topic_len - 1];
                if (last != '/' && !isspace(last)) {
                    break;
                }
                btn->entity_id[topic_len - 1] = '\0';
                topic_len--;
            }
            if (btn->type < BTN_TYPE_TOGGLE || btn->type > BTN_TYPE_RADIO) {
                btn->type = BTN_TYPE_TOGGLE;
            }
        }
    }

    if (s_current_bank >= s_config.num_banks) {
        s_current_bank = 0;
    }
}

/* Queue-based redraw helper. MQTT callbacks run outside the logic task, so   */
/* they request a refresh indirectly instead of touching UI state directly.   */
static void request_ui_refresh(void)
{
    if (s_btn_event_queue != NULL) {
        input_event_t sync_evt = {
            .type = EVT_MQTT_SYNC,
            .btn_idx = 0xFF,
        };
        xQueueSend(s_btn_event_queue, &sync_evt, 0);
    }
}


/* Keep all timing math in milliseconds for readability.                     */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void build_mqtt_topic(char *dst, size_t dst_size, const char *base_topic, const char *suffix)
{
    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';

    if (!base_topic || !suffix) {
        return;
    }

    size_t base_len = strnlen(base_topic, dst_size - 1);
    while (base_len > 0) {
        unsigned char last = (unsigned char)base_topic[base_len - 1];
        if (last != '/' && !isspace(last)) {
            break;
        }
        base_len--;
    }

    if (base_len == 0) {
        return;
    }

    snprintf(dst, dst_size, "%.*s/%s", (int)base_len, base_topic, suffix);
}

static void build_panel_mqtt_topic(char *dst, size_t dst_size, const char *entity_id, const char *suffix)
{
    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';

    if (!entity_id || !suffix) {
        return;
    }

    char base_topic[96];
    if (strncmp(entity_id, "panels/", 7) == 0) {
        snprintf(base_topic, sizeof(base_topic), "%s", entity_id);
    } else if (strncmp(entity_id, "panels-", 7) == 0) {
        snprintf(base_topic, sizeof(base_topic), "panels/%s", entity_id + 7);
    } else {
        snprintf(base_topic, sizeof(base_topic), "panels/%s", entity_id);
    }

    build_mqtt_topic(dst, dst_size, base_topic, suffix);
}

static void sanitize_discovery_id(char *dst, size_t dst_size, const char *src, const char *fallback)
{
    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    size_t out = 0;

    if (src != NULL) {
        for (size_t i = 0; src[i] != '\0' && out < dst_size - 1; ++i) {
            unsigned char ch = (unsigned char)src[i];
            if (isalnum(ch)) {
                dst[out++] = (char)tolower(ch);
            } else if (ch == '/' || ch == '-' || ch == ' ' || ch == '.') {
                dst[out++] = '_';
            } else if (ch == '_') {
                dst[out++] = '_';
            }
        }
    }

    if (out == 0 && fallback != NULL) {
        for (size_t i = 0; fallback[i] != '\0' && out < dst_size - 1; ++i) {
            unsigned char ch = (unsigned char)fallback[i];
            if (isalnum(ch)) {
                dst[out++] = (char)tolower(ch);
            } else if (ch == '/' || ch == '-' || ch == ' ' || ch == '.') {
                dst[out++] = '_';
            }
        }
    }

    dst[out] = '\0';
}

static void publish_button_discovery(uint8_t bank, uint8_t btn)
{
    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }

    char legacy_device_id[48];
    sanitize_discovery_id(legacy_device_id, sizeof(legacy_device_id), s_config.panel_name, "mqtt_button_panel");
    if (legacy_device_id[0] == '\0') {
        snprintf(legacy_device_id, sizeof(legacy_device_id), "%s", "mqtt_button_panel");
    }

    char legacy_object_id[96];
    char legacy_discovery_topic[256];
    snprintf(legacy_object_id, sizeof(legacy_object_id), "%s_bank_%02u_button_%02u", legacy_device_id, (unsigned)(bank + 1), (unsigned)(btn + 1));
    snprintf(legacy_discovery_topic, sizeof(legacy_discovery_topic), "homeassistant/switch/%s/%s/config", legacy_device_id, legacy_object_id);

    esp_mqtt_client_publish(s_mqtt_client, legacy_discovery_topic, "", 0, 1, 1);

    char topic_id[96];
    char unique_id[192];
    char discovery_topic[256];

    const button_config_t *cfg = &s_config.banks[bank].buttons[btn];
    if (bank >= s_config.num_banks || cfg->entity_id[0] == '\0') {
        return;
    }

    sanitize_discovery_id(topic_id, sizeof(topic_id), cfg->entity_id, "button");
    if (topic_id[0] == '\0') {
        return;
    }

    snprintf(unique_id, sizeof(unique_id), "%s", topic_id);
    snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/switch/%s/config", topic_id);

    char state_topic[96];
    char command_topic[96];
    build_panel_mqtt_topic(state_topic, sizeof(state_topic), cfg->entity_id, "state");
    build_panel_mqtt_topic(command_topic, sizeof(command_topic), cfg->entity_id, "command");

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "name", cfg->display_name[0] ? cfg->display_name : topic_id);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", topic_id);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "payload_press", "ON");
    cJSON_AddStringToObject(root, "payload_release", "OFF");

    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
        ESP_LOGI(TAG, "MQTT DISCOVERY: %s", discovery_topic);
        cJSON_free(payload);
    }

    cJSON_Delete(root);
}

static void clear_button_discovery_by_entity_id(const char *entity_id)
{
    if (!s_mqtt_client || !s_mqtt_connected || entity_id == NULL || entity_id[0] == '\0') {
        return;
    }

    char topic_id[96];
    char discovery_topic[256];
    sanitize_discovery_id(topic_id, sizeof(topic_id), entity_id, "button");
    if (topic_id[0] == '\0') {
        return;
    }

    snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/switch/%s/config", topic_id);
    esp_mqtt_client_publish(s_mqtt_client, discovery_topic, "", 0, 1, 1);
    ESP_LOGI(TAG, "MQTT DISCOVERY CLEAR: %s", discovery_topic);
}

static void publish_mqtt_discovery(void)
{
    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }

    for (uint8_t b = 0; b < MAX_BANKS; ++b) {
        for (uint8_t i = 0; i < MAIN_BTN_COUNT; ++i) {
            publish_button_discovery(b, i);
        }
    }
}

static void subscribe_all_state_topics(const system_config_t *cfg)
{
    if (!s_mqtt_client || cfg == NULL) {
        return;
    }

    for (int b = 0; b < cfg->num_banks; b++) {
        for (int i = 0; i < MAIN_BTN_COUNT; i++) {
            char sub_topic[80];
            build_panel_mqtt_topic(sub_topic, sizeof(sub_topic), cfg->banks[b].buttons[i].entity_id, "state");
            esp_mqtt_client_subscribe(s_mqtt_client, sub_topic, 1);
            ESP_LOGI(TAG, "Subscribed to: %s", sub_topic);
        }
    }
}

static void unsubscribe_all_state_topics(const system_config_t *cfg)
{
    if (!s_mqtt_client || cfg == NULL) {
        return;
    }

    for (int b = 0; b < cfg->num_banks; b++) {
        for (int i = 0; i < MAIN_BTN_COUNT; i++) {
            char sub_topic[80];
            build_panel_mqtt_topic(sub_topic, sizeof(sub_topic), cfg->banks[b].buttons[i].entity_id, "state");
            esp_mqtt_client_unsubscribe(s_mqtt_client, sub_topic);
            ESP_LOGI(TAG, "Unsubscribed from: %s", sub_topic);
        }
    }
}

static esp_err_t i2c_write_u8(uint8_t addr, uint8_t value)
{
    if (s_i2c_mutex == NULL) {
        return i2c_master_write_to_device(I2C_PORT, addr, &value, 1, pdMS_TO_TICKS(20));
    }
    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_write_to_device(I2C_PORT, addr, &value, 1, pdMS_TO_TICKS(20));
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

static esp_err_t i2c_read_u8(uint8_t addr, uint8_t *value)
{
    if (s_i2c_mutex == NULL) {
        return i2c_master_read_from_device(I2C_PORT, addr, value, 1, pdMS_TO_TICKS(20));
    }
    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_read_from_device(I2C_PORT, addr, value, 1, pdMS_TO_TICKS(20));
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

/* Bring-up helper for the shared I2C bus. It is left in the file because it  */
/* is still useful when a peripheral disappears or a cable change is made.    */
static void i2c_scan_log(void)
{
    ESP_LOGI(TAG, "I2C scan started");
    for (uint8_t addr = 1; addr < 127; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        
        // Send a write bit to the address; 'true' expects an ACK
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        // Execute the command
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan done");
}

static esp_err_t lcd_expander_write(uint8_t value)
{
    return i2c_write_u8(PCF_LCD_ADDR, value);
}

/* The LCD is driven in 4-bit mode through the I/O expander, so each nibble   */
/* becomes a small enable pulse while keeping the backlight bit intact.       */
static esp_err_t lcd_write4_raw(uint8_t nibble_with_flags)
{
    esp_err_t err = lcd_expander_write(nibble_with_flags | LCD_PIN_BL);
    if (err != ESP_OK) {
        return err;
    }

    err = lcd_expander_write(nibble_with_flags | LCD_PIN_BL | LCD_PIN_EN);
    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(1);
    err = lcd_expander_write(nibble_with_flags | LCD_PIN_BL);
    esp_rom_delay_us(50);
    return err;
}

/* Send a command or data byte as two LCD nibbles.                           */
static esp_err_t lcd_send(uint8_t value, bool is_data)
{
    uint8_t rs = is_data ? LCD_PIN_RS : 0;
    uint8_t hi = (value & 0xF0) | rs;
    uint8_t lo = ((value << 4) & 0xF0) | rs;

    esp_err_t err = lcd_write4_raw(hi);
    if (err != ESP_OK) {
        return err;
    }
    return lcd_write4_raw(lo);
}

static esp_err_t lcd_set_cursor(uint8_t row, uint8_t col)
{
    static const uint8_t row_addr[] = {0x00, 0x40};
    if (row > 1 || col > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    return lcd_send((uint8_t)(0x80 | (row_addr[row] + col)), false);
}

/* Write one fixed-width LCD line and remember what was last displayed.       */
static esp_err_t lcd_write_line(uint8_t row, const char *line)
{
    char padded[17];
    size_t in_len = line ? strlen(line) : 0;
    if (in_len > 16) {
        in_len = 16;
    }

    memset(padded, ' ', 16);
    if (line && in_len > 0) {
        memcpy(padded, line, in_len);
    }
    padded[16] = '\0';

    esp_err_t err = lcd_set_cursor(row, 0);
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < 16; ++i) {
        err = lcd_send((uint8_t)padded[i], true);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (row == 0) {
        memcpy(s_lcd_last_line1, padded, sizeof(s_lcd_last_line1));
    } else {
        memcpy(s_lcd_last_line2, padded, sizeof(s_lcd_last_line2));
    }

    return ESP_OK;
}

/* Update both lines only when they actually changed.                         */
static esp_err_t lcd_set_lines(const char *line1, const char *line2)
{
    if (s_lcd_mutex != NULL) {
        if (xSemaphoreTake(s_lcd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    char l1[17];
    char l2[17];

    snprintf(l1, sizeof(l1), "%-16.16s", line1 ? line1 : "");
    snprintf(l2, sizeof(l2), "%-16.16s", line2 ? line2 : "");

    if (strncmp(l1, s_lcd_last_line1, 16) != 0) {
        esp_err_t err = lcd_write_line(0, l1);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (strncmp(l2, s_lcd_last_line2, 16) != 0) {
        esp_err_t err = lcd_write_line(1, l2);
        if (err != ESP_OK) {
            if (s_lcd_mutex != NULL) {
                xSemaphoreGive(s_lcd_mutex);
            }
            return err;
        }
    }

    if (s_lcd_mutex != NULL) {
        xSemaphoreGive(s_lcd_mutex);
    }

    return ESP_OK;
}

static esp_err_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_PORT, &conf);
    esp_err_t err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_OK && s_i2c_mutex == NULL) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (s_i2c_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK && s_lcd_mutex == NULL) {
        s_lcd_mutex = xSemaphoreCreateMutex();
        if (s_lcd_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return err;
}

/* Standard HD44780 power-up sequence. The repeated 0x30 writes are deliberate */
/* because they force the controller into a known state even if power timing   */
/* is imperfect.                                                              */
static esp_err_t lcd_init(void)
{
    // 1. Wait for LCD to fully power up (some clones need >50ms)
    vTaskDelay(pdMS_TO_TICKS(100)); 
    lcd_expander_write(0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_expander_write(LCD_PIN_BL);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Force 8-bit mode 3 times to completely reset the state machine
    lcd_write4_raw(0x30);
    esp_rom_delay_us(5000); // Must be > 4.1ms
    lcd_write4_raw(0x30);
    esp_rom_delay_us(200);  // Must be > 100us
    lcd_write4_raw(0x30);
    esp_rom_delay_us(200);

    // 3. Switch to 4-bit mode
    lcd_write4_raw(0x20);
    esp_rom_delay_us(200);

    // 4. Configure 4-bit mode, 2 lines, 5x8 font (0x28)
    if (lcd_send(0x28, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(50);

    // 5. Turn display OFF (0x08)
    if (lcd_send(0x08, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(50);

    // 6. Clear display (0x01)
    if (lcd_send(0x01, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(5000); // Give it a full 5ms to clear!

    // 6b. Home the cursor to make sure the LCD state machine is fully reset.
    if (lcd_send(0x02, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(2000);

    // 7. Set Entry Mode: Increment, no shift (0x06)
    if (lcd_send(0x06, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(50);

    // 8. Turn display ON, Cursor OFF, Blink OFF (0x0C)
    if (lcd_send(0x0C, false) != ESP_OK) return ESP_FAIL;
    esp_rom_delay_us(50);

    memset(s_lcd_last_line1, 0, sizeof(s_lcd_last_line1));
    memset(s_lcd_last_line2, 0, sizeof(s_lcd_last_line2));
    return ESP_OK;
}

static esp_err_t ws2812_init(void)
{
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX(LED_PIN, WS2812_RMT_CHANNEL);
    cfg.clk_div = WS2812_RMT_CLK_DIV;
    cfg.mem_block_num = 1;
    cfg.tx_config.loop_en = false;
    cfg.tx_config.carrier_en = false;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    esp_err_t err = rmt_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    return rmt_driver_install(WS2812_RMT_CHANNEL, 0, 0);
}

static void ws2812_encode_byte(uint8_t byte, size_t *idx)
{
    for (int bit = 7; bit >= 0; --bit) {
        bool one = (byte >> bit) & 0x01;
        s_led_items[*idx].level0 = 1;
        s_led_items[*idx].duration0 = one ? WS2812_T1H_TICKS : WS2812_T0H_TICKS;
        s_led_items[*idx].level1 = 0;
        s_led_items[*idx].duration1 = one ? WS2812_T1L_TICKS : WS2812_T0L_TICKS;
        (*idx)++;
    }
}

/* Encode and transmit one frame to the LED strip. Keeping this separate      */
/* from the UI logic makes it easy to repaint the LEDs from any event.        */
static esp_err_t ws2812_show(const uint8_t rgb[LED_COUNT][3])
{
    size_t idx = 0;

    for (uint8_t i = 0; i < LED_COUNT; ++i) {
        /* WS2812 expects GRB order. */
        ws2812_encode_byte(rgb[i][1], &idx);
        ws2812_encode_byte(rgb[i][0], &idx);
        ws2812_encode_byte(rgb[i][2], &idx);
    }

    s_led_items[idx].level0 = 0;
    s_led_items[idx].duration0 = WS2812_RESET_TICKS;
    s_led_items[idx].level1 = 0;
    s_led_items[idx].duration1 = 0;
    idx++;

    esp_err_t err = rmt_write_items(WS2812_RMT_CHANNEL, s_led_items, idx, true);
    if (err == ESP_OK) {
        err = rmt_wait_tx_done(WS2812_RMT_CHANNEL, pdMS_TO_TICKS(50));
    }
    return err;
}

/* PCF8574 inputs are quasi-bidirectional, so writing 1 leaves the pin high    */
/* and lets a button press pull it low.                                       */
static esp_err_t buttons_init(void)
{
    /* PCF8574 pins are quasi-bidirectional; writing 1 lets pin float high for input mode. */
    return i2c_write_u8(PCF_BUTTON_ADDR, 0xFF);
}

static esp_err_t buttons_read_raw(uint8_t *state)
{
    return i2c_read_u8(PCF_BUTTON_ADDR, state);
}

/* Touch pads are calibrated at boot so the threshold can be relative rather   */
/* than absolute. This makes the touch detection less sensitive to drift.     */
static esp_err_t touch_init_all(void)
{
    esp_err_t err = touch_pad_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < ARRAY_LEN(k_touch_channels); ++i) {
        err = touch_pad_config(k_touch_channels[i], 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = touch_pad_filter_start(10);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < ARRAY_LEN(k_touch_channels); ++i) {
        uint16_t filtered = 0;
        err = touch_pad_read_filtered(k_touch_channels[i], &filtered);
        if (err != ESP_OK) {
            return err;
        }
        s_touch_baseline[i] = filtered;
    }

    s_touch_ready = true;
    return ESP_OK;
}

static void touch_read_all(uint16_t out_values[BUTTON_COUNT])
{
    for (size_t i = 0; i < ARRAY_LEN(k_touch_channels); ++i) {
        uint16_t filtered = 0;
        if (touch_pad_read_filtered(k_touch_channels[i], &filtered) == ESP_OK) {
            out_values[i] = filtered;
        } else {
            out_values[i] = 0;
        }
    }
}

/* Touch is considered active when the filtered reading drops enough below    */
/* the stored baseline.                                                       */
static bool touch_is_active(size_t idx, uint16_t current)
{
    if (!s_touch_ready || idx >= BUTTON_COUNT) {
        return false;
    }

    uint16_t baseline = s_touch_baseline[idx];
    if (baseline < 20) {
        return false;
    }

    /* Touch usually decreases the measured filtered value; 18% drop threshold works well for bring-up. */
    uint16_t threshold = (uint16_t)((baseline * 82U) / 100U);
    return current < threshold;
}


static void publish_mqtt_command(uint8_t bank, uint8_t btn, bool state);

/* MQTT events update the connection flag, subscriptions, and mirrored button  */
/* state. The handler is intentionally thin so the logic task still owns UI    */
/* decisions.                                                                 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    if (event->event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT Connected to Broker!");
        s_mqtt_connected = true;

        // Subscribe only to HA state topics; HA remains the source of truth.
        subscribe_all_state_topics(&s_config);
        publish_mqtt_discovery();
        request_ui_refresh();
    } else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT Disconnected from Broker");
        s_mqtt_connected = false;
        request_ui_refresh();
    } else if (event->event_id == MQTT_EVENT_DATA) {
        // Note: MQTT payloads are NOT null-terminated in the event struct!
        char topic[80];
        int topic_len = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';
        
        char payload[16];
        int payload_len = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
        memcpy(payload, event->data, payload_len);
        payload[payload_len] = '\0';
        
        ESP_LOGI(TAG, "MQTT RECV: %s -> %s", topic, payload);
        
        bool new_state = (strncmp(payload, "ON", 2) == 0);
        
        // Find the matching button and update its state
        for (int b = 0; b < s_config.num_banks; b++) {
            for (int i = 0; i < MAIN_BTN_COUNT; i++) {
                char expected_topic[80];
                build_panel_mqtt_topic(expected_topic, sizeof(expected_topic), s_config.banks[b].buttons[i].entity_id, "state");

                if (strcmp(topic, expected_topic) == 0) {
                    s_btn_states[b][i] = new_state;

                    // Only force a UI update if the changed button is on the currently visible bank
                    if (b == s_current_bank) {
                        input_event_t sync_evt = { 
                            .type = EVT_MQTT_SYNC, 
                            .btn_idx = 0xFF // Dummy index to prevent triggering physical button logic
                        };
                        xQueueSend(s_btn_event_queue, &sync_evt, 0);
                    }
                }
            }
        }
    }
}

/* Recreate the MQTT client whenever the network comes back so we never reuse  */
/* a stale client after a link bounce.                                         */
static void start_mqtt(void) {
    // Prevent starting multiple clients if network bounces
    if (s_mqtt_client != NULL) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    s_mqtt_connected = false;

    // Don't try to connect if the web UI hasn't been configured yet
    if (strlen(s_mqtt_config.broker_ip) == 0) {
        ESP_LOGW(TAG, "No MQTT Broker IP configured. Skipping MQTT init.");
        return;
    }

    char broker_uri[64];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%s", s_mqtt_config.broker_ip, s_mqtt_config.port);
    ESP_LOGI(TAG, "Starting MQTT Client to %s", broker_uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = strlen(s_mqtt_config.username) > 0 ? s_mqtt_config.username : NULL,
        .credentials.authentication.password = strlen(s_mqtt_config.password) > 0 ? s_mqtt_config.password : NULL,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

/* Publish intent to Home Assistant; HA remains the source of truth.         */
static void publish_mqtt_command(uint8_t bank, uint8_t btn, bool state) {
    if (!s_mqtt_client || !s_mqtt_connected) return; // Fail gracefully if not connected
    
    char pub_topic[80];
    build_panel_mqtt_topic(pub_topic, sizeof(pub_topic), s_config.banks[bank].buttons[btn].entity_id, "command");
    const char *payload = state ? "ON" : "OFF";
    
    // Publish with QoS 1
    esp_mqtt_client_publish(s_mqtt_client, pub_topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "MQTT PUB: %s -> %s", pub_topic, payload);
}


// Network Event Handler
/* Ethernet is preferred, Wi-Fi is the fallback. This handler reacts to both   */
/* stacks so the panel can stay reachable when one transport drops.           */
static void net_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "Ethernet Link Up");
                s_eth_connected = true;
                break;
            case ETHERNET_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "Ethernet Link Down - Starting Wi-Fi Fallback");
                s_eth_connected = false;
                if (s_eth_has_ip) {
                    // Ethernet was actively serving traffic; fall back to Wi-Fi now.
                    s_eth_has_ip = false;
                    esp_err_t start_err = esp_wifi_start();
                    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_CONN) {
                        ESP_LOGW(TAG, "esp_wifi_start fallback failed: %s", esp_err_to_name(start_err));
                    }
                    esp_err_t conn_err = esp_wifi_connect();
                    if (conn_err != ESP_OK) {
                        ESP_LOGW(TAG, "esp_wifi_connect fallback failed: %s", esp_err_to_name(conn_err));
                    }
                }
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "ETH Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_eth_has_ip = true;
        s_wifi_connected = false;
        // Prefer Ethernet only after it has a usable IP.
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop after ETH IP failed: %s", esp_err_to_name(stop_err));
        }
        start_mqtt();
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Wi-Fi started, attempting connection...");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Wi-Fi connected to AP");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
                    if (disc != NULL) {
                        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", disc->reason);
                    }
                }
                // Only auto-retry if Ethernet is not actively providing IP.
                if (!s_eth_has_ip) {
                    ESP_LOGI(TAG, "Wi-Fi Disconnected, retrying...");
                    esp_err_t conn_err = esp_wifi_connect();
                    if (conn_err != ESP_OK) {
                        ESP_LOGW(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(conn_err));
                    }
                }
                s_wifi_connected = false;
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "WiFi Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        if (!s_eth_has_ip) {
            // Only start MQTT if Ethernet hasn't already
            start_mqtt();
        }
    }
}

// WT32-ETH01 Init (ESP-IDF v5.x compatible)
/* Bring up WT32-ETH01 networking. The explicit delays are intentional so the */
/* external PHY has time to boot before the MAC starts talking to it.         */
static void init_network_failover(void) {
    // 1. Power up the PHY and WAIT
    ESP_ERROR_CHECK(gpio_set_direction(ETH_PHY_POWER_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(ETH_PHY_POWER_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(50)); // Crucial: Give LAN8720 time to boot up

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    
    // 2. Configure WT32-ETH01 50MHz RMII Clock In
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO; // GPIO 0
    
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    esp_err_t _ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(_ret));
        gpio_set_level(ETH_PHY_POWER_GPIO, 0);
        ESP_LOGW(TAG, "Falling back to Wi-Fi only (Ethernet disabled)");
        goto wifi_only;
    }
    _ret = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    if (_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(_ret));
        esp_eth_driver_uninstall(eth_handle);
        gpio_set_level(ETH_PHY_POWER_GPIO, 0);
        ESP_LOGW(TAG, "Falling back to Wi-Fi only (Ethernet disabled)");
        goto wifi_only;
    }

wifi_only:
    // Initialize Wi-Fi (Fallback)
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    
    // Use loaded WiFi config from NVS
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)sta_config.sta.ssid, s_wifi_config.ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char*)sta_config.sta.password, s_wifi_config.password, sizeof(sta_config.sta.password) - 1);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    // Keep Wi-Fi fully awake for stable always-on panel connectivity.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // Apply static IP settings if not using DHCP
    esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi_netif && !s_wifi_config.use_dhcp) {
        esp_netif_dhcpc_stop(wifi_netif);
        esp_netif_ip_info_t ip_info;
        esp_netif_str_to_ip4(s_wifi_config.static_ip, &ip_info.ip);
        esp_netif_str_to_ip4(s_wifi_config.static_netmask, &ip_info.netmask);
        esp_netif_str_to_ip4(s_wifi_config.static_gateway, &ip_info.gw);
        esp_netif_set_ip_info(wifi_netif, &ip_info);
    }
    
    // Start WiFi so it's ready
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Try WiFi connection immediately if SSID is configured (non-empty)
    if (s_wifi_config.ssid[0] != '\0') {
        vTaskDelay(pdMS_TO_TICKS(500)); // Give WiFi stack time to settle
        ESP_LOGI(TAG, "Attempting WiFi connection on boot: %s", s_wifi_config.ssid);
        esp_wifi_connect();
    }

    // Register Handlers
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &net_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &net_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &net_event_handler, NULL);

    _ret = esp_eth_start(eth_handle);
    if (_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(_ret));
        esp_eth_driver_uninstall(eth_handle);
        gpio_set_level(ETH_PHY_POWER_GPIO, 0);
        ESP_LOGW(TAG, "Ethernet start failed, continuing with Wi-Fi only");
    }
}

/* ========================================================================== */
/* Application Logic                                                          */
/* ========================================================================== */

/* Persist the MQTT settings so the web UI survives a reboot.                */
static void save_settings_nvs(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "mqtt_ip", s_mqtt_config.broker_ip);
        nvs_set_str(my_handle, "mqtt_port", s_mqtt_config.port);
        nvs_set_str(my_handle, "mqtt_user", s_mqtt_config.username);
        nvs_set_str(my_handle, "mqtt_pass", s_mqtt_config.password);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Settings saved to NVS");
    }
}

/* Load saved MQTT settings if they exist; otherwise the defaults remain.    */
static void load_settings_nvs(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len;
        
        len = sizeof(s_mqtt_config.broker_ip);
        nvs_get_str(my_handle, "mqtt_ip", s_mqtt_config.broker_ip, &len);
        
        len = sizeof(s_mqtt_config.port);
        nvs_get_str(my_handle, "mqtt_port", s_mqtt_config.port, &len);
        
        len = sizeof(s_mqtt_config.username);
        nvs_get_str(my_handle, "mqtt_user", s_mqtt_config.username, &len);
        
        len = sizeof(s_mqtt_config.password);
        nvs_get_str(my_handle, "mqtt_pass", s_mqtt_config.password, &len);
        
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Settings loaded from NVS");
    }
}

/* Hard-coded demo panel layout used until a full configuration editor exists. */
static void load_default_config(void)
{
    snprintf(s_config.panel_name, sizeof(s_config.panel_name), "Sanctuary Ltg");
    s_config.num_banks = 2;
    
    snprintf(s_config.banks[0].bank_name, sizeof(s_config.banks[0].bank_name), "Worship Set");
    for(int i = 0; i < 4; i++) {
        snprintf(s_config.banks[0].buttons[i].display_name, 17, "Scene %d", i+1);
        
        // ADD THIS: Give the button a default MQTT topic!
        snprintf(s_config.banks[0].buttons[i].entity_id, 64, "sanctuary/worship/scene%d", i+1); 
        
        s_config.banks[0].buttons[i].type = BTN_TYPE_RADIO;
        s_config.banks[0].buttons[i].radio_group = 1;
        
        // ... (Keep your existing color definitions here)
        s_config.banks[0].buttons[i].color_on[0] = 0;   // R
        s_config.banks[0].buttons[i].color_on[1] = 255; // G
        s_config.banks[0].buttons[i].color_on[2] = 0;   // B
        s_config.banks[0].buttons[i].color_off[0] = 255; // R
        s_config.banks[0].buttons[i].color_off[1] = 0;   // G
        s_config.banks[0].buttons[i].color_off[2] = 0;   // B
    }

    snprintf(s_config.banks[1].bank_name, sizeof(s_config.banks[1].bank_name), "House Lights");
    for(int i = 0; i < 4; i++) {
        snprintf(s_config.banks[1].buttons[i].display_name, 17, "Zone %d", i+1);
        
        // ADD THIS: Give the button a default MQTT topic!
        snprintf(s_config.banks[1].buttons[i].entity_id, 64, "sanctuary/house/zone%d", i+1);
        
        s_config.banks[1].buttons[i].type = BTN_TYPE_TOGGLE;
        
        // ... (Keep your existing color definitions here)
        s_config.banks[1].buttons[i].color_on[0] = 0;   // R
        s_config.banks[1].buttons[i].color_on[1] = 255; // G
        s_config.banks[1].buttons[i].color_on[2] = 0;   // B
        s_config.banks[1].buttons[i].color_off[0] = 255; // R
        s_config.banks[1].buttons[i].color_off[1] = 0;   // G
        s_config.banks[1].buttons[i].color_off[2] = 0;   // B
    }
}

/* Build one frame of the orange boot pulse. Only the bank buttons are lit so */
/* the startup state is obvious without pretending the panel is usable yet.   */
static void render_startup_frame(uint8_t phase)
{
    uint8_t rgb[LED_COUNT][3] = {0};
    const uint8_t orange[3] = {255, 96, 0};

    uint8_t brightness = (uint8_t)((phase < 20 ? phase : 39 - phase) * 13);
    
    // Apply orange pulse to all LEDs
    for (int i = 0; i < LED_COUNT; i++) {
        rgb[i][0] = (uint8_t)((orange[0] * brightness) / 255U);
        rgb[i][1] = (uint8_t)((orange[1] * brightness) / 255U);
        rgb[i][2] = (uint8_t)((orange[2] * brightness) / 255U);
    }

    ws2812_show(rgb);
}

/* Startup animation runs in its own task so it can keep pulsing while the    */
/* rest of boot continues. The task deletes itself once boot is complete.     */
static void startup_task(void *arg)
{
    uint8_t phase = 0;

    lcd_set_lines(s_config.panel_name, "Starting...");

    while (!s_startup_complete) {
        render_startup_frame(phase);
        phase = (uint8_t)((phase + 1) % 40);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uint8_t rgb[LED_COUNT][3] = {0};
    ws2812_show(rgb);

    vTaskDelete(NULL);
}

/* Central UI renderer. It combines bank selection, touch preview, MQTT       */
/* connectivity, and held-button feedback into one consistent frame.         */
static void update_system_ui(int touch_preview_idx)
{
    sanitize_config();

    char line1[32];
    char line2[32];

    // --- LCD Updates ---
    if (s_in_menu) {
        snprintf(line1, sizeof(line1), "Menu: %d. %s", s_menu_index + 1, s_menu_items[s_menu_index]);
        if (touch_preview_idx >= 0 && touch_preview_idx < MAIN_BTN_COUNT) {
            snprintf(line2, sizeof(line2), "%s", s_menu_button_funcs[touch_preview_idx]);
        } else {
            line2[0] = '\0';
        }
    } else if (touch_preview_idx >= 0 && touch_preview_idx < MAIN_BTN_COUNT) {
        snprintf(line1, sizeof(line1), "%s", s_config.banks[s_current_bank].bank_name);
        button_config_t *btn = &s_config.banks[s_current_bank].buttons[touch_preview_idx];
        snprintf(line2, sizeof(line2), "%s", btn->display_name);
    } else {
        snprintf(line1, sizeof(line1), "%s", s_config.banks[s_current_bank].bank_name);
        if (s_mqtt_connected) {
            line2[0] = '\0';
        } else {
            snprintf(line2, sizeof(line2), "MQTT OFFLINE");
        }
    }
    lcd_set_lines(line1, line2);

    // --- LED Updates ---
    uint8_t rgb[LED_COUNT][3] = {0};

    if (s_in_menu) {
        // Menu mode: color the four main buttons to indicate actions
        const uint8_t menu_colors[MAIN_BTN_COUNT][3] = {
            {0, 255, 0},    // Button 0: Select (green)
            {255, 255, 0},  // Button 1: Back (yellow)
            {0, 0, 255},    // Button 2: Save/Action (blue)
            {255, 0, 255}   // Button 3: Extra (magenta)
        };

        for (int i = 0; i < MAIN_BTN_COUNT; i++) {
            rgb[i][0] = menu_colors[i][0];
            rgb[i][1] = menu_colors[i][1];
            rgb[i][2] = menu_colors[i][2];
            // Highlight selected item
            if (i == s_menu_index) {
                int r0 = rgb[i][0] * 2;
                int r1 = rgb[i][1] * 2;
                int r2 = rgb[i][2] * 2;
                rgb[i][0] = (uint8_t)(r0 > 255 ? 255 : r0);
                rgb[i][1] = (uint8_t)(r1 > 255 ? 255 : r1);
                rgb[i][2] = (uint8_t)(r2 > 255 ? 255 : r2);
            }
        }

        // Bank buttons: red while held, dim otherwise
        if (s_bank_btns_held[0]) {
            rgb[BTN_BANK_DOWN][0] = 255; rgb[BTN_BANK_DOWN][1] = 0; rgb[BTN_BANK_DOWN][2] = 0;
        } else {
            rgb[BTN_BANK_DOWN][0] = 48; rgb[BTN_BANK_DOWN][1] = 48; rgb[BTN_BANK_DOWN][2] = 48;
        }
        if (s_bank_btns_held[1]) {
            rgb[BTN_BANK_UP][0] = 255; rgb[BTN_BANK_UP][1] = 0; rgb[BTN_BANK_UP][2] = 0;
        } else {
            rgb[BTN_BANK_UP][0] = 48; rgb[BTN_BANK_UP][1] = 48; rgb[BTN_BANK_UP][2] = 48;
        }
    } else {
        if (s_mqtt_connected) {
            for (int i = 0; i < MAIN_BTN_COUNT; i++) {
                button_config_t *btn_cfg = &s_config.banks[s_current_bank].buttons[i];
                bool is_on = s_btn_states[s_current_bank][i];

                if (is_on) {
                    memcpy(rgb[i], btn_cfg->color_on, 3);
                } else {
                    memcpy(rgb[i], btn_cfg->color_off, 3);
                }
            }

            // Bank buttons visual feedback
            if (s_bank_btns_held[0]) {
                rgb[BTN_BANK_DOWN][0] = 255; rgb[BTN_BANK_DOWN][1] = 0; rgb[BTN_BANK_DOWN][2] = 0;
            } else {
                rgb[BTN_BANK_DOWN][0] = 48; rgb[BTN_BANK_DOWN][1] = 48; rgb[BTN_BANK_DOWN][2] = 48;
            }
            if (s_bank_btns_held[1]) {
                rgb[BTN_BANK_UP][0] = 255; rgb[BTN_BANK_UP][1] = 0; rgb[BTN_BANK_UP][2] = 0;
            } else {
                rgb[BTN_BANK_UP][0] = 48; rgb[BTN_BANK_UP][1] = 48; rgb[BTN_BANK_UP][2] = 48;
            }
        } else if (s_bank_btns_held[0] || s_bank_btns_held[1]) {
            // Even if MQTT is offline, reflect the physical press state.
            if (s_bank_btns_held[0]) {
                rgb[BTN_BANK_DOWN][0] = 255; rgb[BTN_BANK_DOWN][1] = 0; rgb[BTN_BANK_DOWN][2] = 0;
            }
            if (s_bank_btns_held[1]) {
                rgb[BTN_BANK_UP][0] = 255; rgb[BTN_BANK_UP][1] = 0; rgb[BTN_BANK_UP][2] = 0;
            }
        }
    }

    ws2812_show(rgb);
}

/* Track all active touches and restore the most recent surviving touch when   */
/* another finger lifts. This keeps the LCD preview stable during multi-touch. */
static void refresh_touch_preview(int *current_touch_preview, bool touch_active[MAIN_BTN_COUNT], uint32_t touch_order[MAIN_BTN_COUNT])
{
    int next_preview = -1;
    uint32_t best_order = 0;

    for (int i = 0; i < MAIN_BTN_COUNT; ++i) {
        if (!touch_active[i]) {
            continue;
        }

        if (next_preview < 0 || touch_order[i] >= best_order) {
            next_preview = i;
            best_order = touch_order[i];
        }
    }

    if (next_preview != *current_touch_preview) {
        *current_touch_preview = next_preview;
        update_system_ui(*current_touch_preview);
    }
}

/* Main state machine. This task receives all input events, mutates logical    */
/* button state, manages bank changes, and triggers UI refreshes.             */
/* forward declaration used by menu select action */
static void save_full_config_nvs(void);

static void logic_task(void *arg)
{
    input_event_t evt;
    int current_touch_preview = -1;
    bool touch_active[MAIN_BTN_COUNT] = {false};
    uint32_t touch_order[MAIN_BTN_COUNT] = {0};
    uint32_t touch_seq = 0;
    (void)0; // placeholder (menu toggles immediately on simultaneous bank press)

    update_system_ui(-1);

    while (1) {
        if (xQueueReceive(s_btn_event_queue, &evt, pdMS_TO_TICKS(50))) {

            if (evt.type == EVT_MQTT_SYNC) {
                update_system_ui(current_touch_preview);
            }

            if (evt.btn_idx == BTN_BANK_DOWN || evt.btn_idx == BTN_BANK_UP) {
                if (evt.type == EVT_BTN_PRESSED) {
                    s_bank_btns_held[evt.btn_idx - BTN_BANK_DOWN] = true;
                    // If both bank buttons are pressed simultaneously, toggle menu immediately
                    if (s_bank_btns_held[0] && s_bank_btns_held[1]) {
                        s_in_menu = !s_in_menu;
                        // Reset menu cursor when opening
                        if (s_in_menu) s_menu_index = 0;
                        update_system_ui(current_touch_preview);
                    } else {
                        // If we're in the menu, single bank presses navigate up/down
                        if (s_in_menu) {
                            if (evt.btn_idx == BTN_BANK_UP) {
                                if (s_menu_index < s_menu_items_count - 1) s_menu_index++;
                            } else if (evt.btn_idx == BTN_BANK_DOWN) {
                                if (s_menu_index > 0) s_menu_index--;
                            }
                        }
                        update_system_ui(current_touch_preview);
                    }
                } else if (evt.type == EVT_BTN_RELEASED) {
                    s_bank_btns_held[evt.btn_idx - BTN_BANK_DOWN] = false;

                    // Only change banks when not in menu mode
                    if (!s_in_menu) {
                        if (evt.btn_idx == BTN_BANK_UP && s_current_bank < s_config.num_banks - 1) s_current_bank++;
                        if (evt.btn_idx == BTN_BANK_DOWN && s_current_bank > 0) s_current_bank--;
                    }

                    update_system_ui(current_touch_preview);
                }
            }

            if (evt.type == EVT_TOUCH_START && evt.btn_idx < MAIN_BTN_COUNT) {
                // Record touch order so the last surviving touch can be restored.
                touch_active[evt.btn_idx] = true;
                touch_order[evt.btn_idx] = ++touch_seq;
            } else if (evt.type == EVT_TOUCH_END && evt.btn_idx < MAIN_BTN_COUNT) {
                touch_active[evt.btn_idx] = false;
            }

            if (evt.type == EVT_TOUCH_START || evt.type == EVT_TOUCH_END) {
                refresh_touch_preview(&current_touch_preview, touch_active, touch_order);
            }

            if (evt.btn_idx < MAIN_BTN_COUNT) {
                if (s_in_menu) {
                    if (evt.type == EVT_BTN_PRESSED) {
                        // In-menu main buttons mapping:
                        // 0 = Select/Enter, 1 = Action (secondary), 2 = Save, 3 = Extra
                        switch (evt.btn_idx) {
                            case 0: // Select/Enter
                                if (strcmp(s_menu_items[s_menu_index], "Save") == 0) {
                                    save_full_config_nvs();
                                } else {
                                    // Placeholder: toggle a simple ack on LCD for non-save items
                                    char ack[17];
                                    snprintf(ack, sizeof(ack), "Selected: %s", s_menu_items[s_menu_index]);
                                    lcd_set_lines("--- MENU ---", ack);
                                    vTaskDelay(pdMS_TO_TICKS(600));
                                }
                                break;
                            case 1: // Secondary action (no-op for now)
                                lcd_set_lines("--- MENU ---", "Action OK");
                                vTaskDelay(pdMS_TO_TICKS(300));
                                break;
                            case 2: // Save
                                save_full_config_nvs();
                                lcd_set_lines("--- MENU ---", "Saved");
                                vTaskDelay(pdMS_TO_TICKS(400));
                                break;
                            case 3: // Extra (reserved)
                                lcd_set_lines("--- MENU ---", "Reserved");
                                vTaskDelay(pdMS_TO_TICKS(300));
                                break;
                        }
                        update_system_ui(current_touch_preview);
                    }
                } else {
                    button_config_t *cfg = &s_config.banks[s_current_bank].buttons[evt.btn_idx];
                    bool *state = &s_btn_states[s_current_bank][evt.btn_idx];

                    if (evt.type == EVT_BTN_PRESSED) {
                        // HA is the source of truth; button presses only emit intent.
                        if (cfg->type == BTN_TYPE_TOGGLE) {
                            // For toggle, publish the desired intent based on the mirrored state.
                            publish_mqtt_command(s_current_bank, evt.btn_idx, !(*state));
                        } else if (cfg->type == BTN_TYPE_MOMENTARY) {
                            // Momentary: send ON on press.
                            publish_mqtt_command(s_current_bank, evt.btn_idx, true);
                        } else if (cfg->type == BTN_TYPE_RADIO) {
                            // Radio: request all peers in the group be turned off,
                            // then request this button be turned on.
                            for (int i = 0; i < MAIN_BTN_COUNT; i++) {
                                if (s_config.banks[s_current_bank].buttons[i].radio_group == cfg->radio_group) {
                                    publish_mqtt_command(s_current_bank, i, false);
                                }
                            }
                            publish_mqtt_command(s_current_bank, evt.btn_idx, true);
                        }
                    } else if (evt.type == EVT_BTN_RELEASED) {
                        if (cfg->type == BTN_TYPE_MOMENTARY) {
                            // Send OFF on release for momentary buttons.
                            publish_mqtt_command(s_current_bank, evt.btn_idx, false);
                        }
                    }
                    update_system_ui(current_touch_preview);
                }
            }
        }

        (void)0;
    }
}

/* Poll the physical inputs, debounce them, and convert changes into events.   */
/* Keeping this task minimal makes the rest of the firmware easier to reason  */
/* about and keeps the UI logic single-threaded.                               */
static void hardware_task(void *arg)
{
    uint16_t touch_values[BUTTON_COUNT] = {0};
    bool last_touch_state[BUTTON_COUNT] = {false};

    while (true) {
        uint8_t raw = 0xFF;
        if (buttons_read_raw(&raw) == ESP_OK) {
            s_button_state = raw;
            for (int i = 0; i < BUTTON_COUNT; ++i) {
                bool is_pressed = ((s_button_state >> k_button_pcf_bits[i]) & 0x01) == 0;
                bool was_pressed = ((s_button_last_state >> k_button_pcf_bits[i]) & 0x01) == 0;

                if (is_pressed != was_pressed) {
                    // Time-based debounce: accept the edge if it has stayed
                    // stable longer than DEBOUNCE_MS, OR accept releases
                    // immediately to avoid missed OFF commands on quick taps.
                    const int DEBOUNCE_MS = 25;
                    int64_t delta = now_ms() - s_button_last_change_ms[i];
                    if (delta > DEBOUNCE_MS || (is_pressed == false)) {
                        input_event_t evt = {
                            .type = is_pressed ? EVT_BTN_PRESSED : EVT_BTN_RELEASED,
                            .btn_idx = i
                        };
                        xQueueSend(s_btn_event_queue, &evt, 0);
                        s_button_last_change_ms[i] = now_ms();
                    }
                }
            }
            s_button_last_state = s_button_state;
        }

        touch_read_all(touch_values);
        for (int i = 0; i < BUTTON_COUNT; ++i) {
            bool is_touched = touch_is_active(i, touch_values[i]);
            if (is_touched != last_touch_state[i]) {
                input_event_t evt = {
                    .type = is_touched ? EVT_TOUCH_START : EVT_TOUCH_END,
                    .btn_idx = i
                };
                xQueueSend(s_btn_event_queue, &evt, 0);
                last_touch_state[i] = is_touched;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

/* --- Web Server Implementation --- */

/* -------------------------------------------------------------------------- */
/* Web configuration UI                                                        */
/*                                                                            */
/* This page is intentionally small. It provides a way to change MQTT access  */
/* without needing a serial terminal or a separate provisioning app.         */
/* -------------------------------------------------------------------------- */
static const char* html_template = 
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:Arial;max-width:400px;margin:20px auto;padding:20px;background:#f4f4f4;}"
    "input{width:100%%;padding:10px;margin:10px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}"
    "button{width:100%%;background:#007BFF;color:white;padding:14px;border:none;border-radius:4px;font-size:16px;}</style></head>"
    "<body><h2>Panel Configuration</h2><form action='/save' method='POST'>"
    "<label>MQTT Broker IP:</label><input type='text' name='mqtt_ip' value='%s'>"
    "<label>MQTT Port:</label><input type='text' name='mqtt_port' value='%s'>"
    "<label>MQTT Username:</label><input type='text' name='mqtt_user' value='%s'>"
    "<label>MQTT Password:</label><input type='password' name='mqtt_pass' value='%s'>"
    "<button type='submit'>Save & Reboot</button></form></body></html>";

/* Serve the configuration page with the current saved values filled in.      */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char response_html[1024];
    snprintf(response_html, sizeof(response_html), html_template, 
             s_mqtt_config.broker_ip, s_mqtt_config.port, 
             s_mqtt_config.username, s_mqtt_config.password);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Save the submitted form, persist it, and reboot so the network stack can   */
/* come back with the new settings cleanly.                                   */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Read the POST body
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse the application/x-www-form-urlencoded data
    httpd_query_key_value(buf, "mqtt_ip", s_mqtt_config.broker_ip, sizeof(s_mqtt_config.broker_ip));
    httpd_query_key_value(buf, "mqtt_port", s_mqtt_config.port, sizeof(s_mqtt_config.port));
    httpd_query_key_value(buf, "mqtt_user", s_mqtt_config.username, sizeof(s_mqtt_config.username));
    httpd_query_key_value(buf, "mqtt_pass", s_mqtt_config.password, sizeof(s_mqtt_config.password));

    // Save to memory and respond
    save_settings_nvs();
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>Settings Saved! Rebooting...</h2>", HTTPD_RESP_USE_STRLEN);
    
    // Reboot the ESP32 after 1 second to apply new networking changes
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/* Forward declarations for functions defined later in this file. */
static void save_full_config_nvs(void);
static void load_full_config_nvs(void);
static void apply_wifi_config(void);
static esp_err_t api_reboot_handler(httpd_req_t *req);

static bool mqtt_config_equal(const mqtt_config_t *a, const mqtt_config_t *b)
{
    return strcmp(a->broker_ip, b->broker_ip) == 0 &&
           strcmp(a->port, b->port) == 0 &&
           strcmp(a->username, b->username) == 0 &&
           strcmp(a->password, b->password) == 0;
}

static bool wifi_config_equal(const app_wifi_config_t *a, const app_wifi_config_t *b)
{
    return strcmp(a->ssid, b->ssid) == 0 &&
           strcmp(a->password, b->password) == 0 &&
           a->use_dhcp == b->use_dhcp &&
           strcmp(a->static_ip, b->static_ip) == 0 &&
           strcmp(a->static_netmask, b->static_netmask) == 0 &&
           strcmp(a->static_gateway, b->static_gateway) == 0;
}

static void migrate_grb_colors_to_rgb(void)
{
    for (int b = 0; b < MAX_BANKS; ++b) {
        for (int i = 0; i < MAIN_BTN_COUNT; ++i) {
            button_config_t *btn = &s_config.banks[b].buttons[i];
            uint8_t tmp = btn->color_on[0];
            btn->color_on[0] = btn->color_on[1];
            btn->color_on[1] = tmp;
            tmp = btn->color_off[0];
            btn->color_off[0] = btn->color_off[1];
            btn->color_off[1] = tmp;
        }
    }
}

/* Convert config RGB storage to API RGB array for web clients. */
static cJSON *make_rgb_array_from_rgb(const uint8_t rgb[3])
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(rgb[0]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(rgb[1]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(rgb[2]));
    return arr;
}

/* Parse API RGB arrays into config RGB storage. */
static void parse_rgb_array_to_rgb(cJSON *rgb_arr, uint8_t rgb_out[3])
{
    if (!cJSON_IsArray(rgb_arr) || cJSON_GetArraySize(rgb_arr) < 3) return;

    cJSON *r = cJSON_GetArrayItem(rgb_arr, 0);
    cJSON *g = cJSON_GetArrayItem(rgb_arr, 1);
    cJSON *b = cJSON_GetArrayItem(rgb_arr, 2);
    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) return;

    rgb_out[0] = (uint8_t)r->valueint;
    rgb_out[1] = (uint8_t)g->valueint;
    rgb_out[2] = (uint8_t)b->valueint;
}


/* JSON API: GET /api/config - returns the current system and mqtt config */
static esp_err_t api_get_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddStringToObject(root, "panel_name", s_config.panel_name);
    cJSON_AddNumberToObject(root, "num_banks", s_config.num_banks);

    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddStringToObject(mqtt, "broker_ip", s_mqtt_config.broker_ip);
    cJSON_AddStringToObject(mqtt, "port", s_mqtt_config.port);
    cJSON_AddStringToObject(mqtt, "username", s_mqtt_config.username);
    cJSON_AddStringToObject(mqtt, "password", s_mqtt_config.password);
    cJSON_AddItemToObject(root, "mqtt", mqtt);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", s_wifi_config.ssid);
    cJSON_AddStringToObject(wifi, "password", s_wifi_config.password);
    cJSON_AddBoolToObject(wifi, "use_dhcp", s_wifi_config.use_dhcp);
    cJSON_AddStringToObject(wifi, "static_ip", s_wifi_config.static_ip);
    cJSON_AddStringToObject(wifi, "static_netmask", s_wifi_config.static_netmask);
    cJSON_AddStringToObject(wifi, "static_gateway", s_wifi_config.static_gateway);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON *banks = cJSON_CreateArray();
    for (int b = 0; b < s_config.num_banks && b < MAX_BANKS; ++b) {
        cJSON *bank = cJSON_CreateObject();
        cJSON_AddStringToObject(bank, "bank_name", s_config.banks[b].bank_name);

        cJSON *buttons = cJSON_CreateArray();
        for (int i = 0; i < MAIN_BTN_COUNT; ++i) {
            cJSON *btn = cJSON_CreateObject();
            cJSON_AddStringToObject(btn, "display_name", s_config.banks[b].buttons[i].display_name);
            cJSON_AddStringToObject(btn, "entity_id", s_config.banks[b].buttons[i].entity_id);
            cJSON_AddStringToObject(btn, "mqtt_topic", s_config.banks[b].buttons[i].entity_id);
            cJSON_AddNumberToObject(btn, "type", s_config.banks[b].buttons[i].type);
            cJSON_AddNumberToObject(btn, "radio_group", s_config.banks[b].buttons[i].radio_group);

            cJSON *on = make_rgb_array_from_rgb(s_config.banks[b].buttons[i].color_on);
            cJSON *off = make_rgb_array_from_rgb(s_config.banks[b].buttons[i].color_off);
            cJSON_AddItemToObject(btn, "color_on", on);
            cJSON_AddItemToObject(btn, "color_off", off);

            cJSON_AddItemToArray(buttons, btn);
        }
        cJSON_AddItemToObject(bank, "buttons", buttons);
        cJSON_AddItemToArray(banks, bank);
    }
    cJSON_AddItemToObject(root, "banks", banks);

    char *out = cJSON_PrintUnformatted(root);
    if (out) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        cJSON_free(out);
    } else {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* JSON API: POST /api/config - accept JSON to update config (partial allowed) */
static esp_err_t api_post_config_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) return ESP_FAIL;

    static system_config_t prev_config;
    mqtt_config_t prev_mqtt = s_mqtt_config;
    app_wifi_config_t prev_wifi = s_wifi_config;
    prev_config = s_config;

    char *buf = malloc(content_len + 1);
    if (!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    cJSON *pn = cJSON_GetObjectItemCaseSensitive(root, "panel_name");
    if (cJSON_IsString(pn) && (pn->valuestring != NULL)) {
        snprintf(s_config.panel_name, sizeof(s_config.panel_name), "%s", pn->valuestring);
    }

    cJSON *nb = cJSON_GetObjectItemCaseSensitive(root, "num_banks");
    if (cJSON_IsNumber(nb)) {
        int v = nb->valueint;
        if (v >= 1 && v <= MAX_BANKS) s_config.num_banks = v;
    }

    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        cJSON *bip = cJSON_GetObjectItemCaseSensitive(mqtt, "broker_ip");
        cJSON *port = cJSON_GetObjectItemCaseSensitive(mqtt, "port");
        cJSON *user = cJSON_GetObjectItemCaseSensitive(mqtt, "username");
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(mqtt, "password");
        if (cJSON_IsString(bip) && bip->valuestring) snprintf(s_mqtt_config.broker_ip, sizeof(s_mqtt_config.broker_ip), "%s", bip->valuestring);
        if (cJSON_IsString(port) && port->valuestring) snprintf(s_mqtt_config.port, sizeof(s_mqtt_config.port), "%s", port->valuestring);
        if (cJSON_IsString(user) && user->valuestring) snprintf(s_mqtt_config.username, sizeof(s_mqtt_config.username), "%s", user->valuestring);
        if (cJSON_IsString(pass) && pass->valuestring) snprintf(s_mqtt_config.password, sizeof(s_mqtt_config.password), "%s", pass->valuestring);
    }

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
        cJSON *wpass = cJSON_GetObjectItemCaseSensitive(wifi, "password");
        cJSON *dhcp = cJSON_GetObjectItemCaseSensitive(wifi, "use_dhcp");
        cJSON *sip = cJSON_GetObjectItemCaseSensitive(wifi, "static_ip");
        cJSON *smask = cJSON_GetObjectItemCaseSensitive(wifi, "static_netmask");
        cJSON *sgw = cJSON_GetObjectItemCaseSensitive(wifi, "static_gateway");
        if (cJSON_IsString(ssid) && ssid->valuestring) snprintf(s_wifi_config.ssid, sizeof(s_wifi_config.ssid), "%s", ssid->valuestring);
        if (cJSON_IsString(wpass) && wpass->valuestring) snprintf(s_wifi_config.password, sizeof(s_wifi_config.password), "%s", wpass->valuestring);
        if (cJSON_IsBool(dhcp)) s_wifi_config.use_dhcp = dhcp->type == cJSON_True;
        if (cJSON_IsString(sip) && sip->valuestring) snprintf(s_wifi_config.static_ip, sizeof(s_wifi_config.static_ip), "%s", sip->valuestring);
        if (cJSON_IsString(smask) && smask->valuestring) snprintf(s_wifi_config.static_netmask, sizeof(s_wifi_config.static_netmask), "%s", smask->valuestring);
        if (cJSON_IsString(sgw) && sgw->valuestring) snprintf(s_wifi_config.static_gateway, sizeof(s_wifi_config.static_gateway), "%s", sgw->valuestring);
    }

    /* Optional: parse banks/buttons if provided */
    cJSON *banks = cJSON_GetObjectItemCaseSensitive(root, "banks");
    if (cJSON_IsArray(banks)) {
        int bidx = 0;
        cJSON *bank;
        cJSON_ArrayForEach(bank, banks) {
            if (bidx >= MAX_BANKS) break;
            cJSON *bname = cJSON_GetObjectItemCaseSensitive(bank, "bank_name");
            if (cJSON_IsString(bname) && bname->valuestring) snprintf(s_config.banks[bidx].bank_name, sizeof(s_config.banks[bidx].bank_name), "%s", bname->valuestring);

            cJSON *buttons = cJSON_GetObjectItemCaseSensitive(bank, "buttons");
            if (cJSON_IsArray(buttons)) {
                int bi = 0;
                cJSON *bitem;
                cJSON_ArrayForEach(bitem, buttons) {
                    if (bi >= MAIN_BTN_COUNT) break;
                    cJSON *d = cJSON_GetObjectItemCaseSensitive(bitem, "display_name");
                    cJSON *mt = cJSON_GetObjectItemCaseSensitive(bitem, "entity_id");
                    if (!cJSON_IsString(mt) || !mt->valuestring) {
                        mt = cJSON_GetObjectItemCaseSensitive(bitem, "mqtt_topic");
                    }
                    cJSON *tp = cJSON_GetObjectItemCaseSensitive(bitem, "type");
                    cJSON *rg = cJSON_GetObjectItemCaseSensitive(bitem, "radio_group");
                    if (cJSON_IsString(d) && d->valuestring) snprintf(s_config.banks[bidx].buttons[bi].display_name, sizeof(s_config.banks[bidx].buttons[bi].display_name), "%s", d->valuestring);
                    if (cJSON_IsString(mt) && mt->valuestring) snprintf(s_config.banks[bidx].buttons[bi].entity_id, sizeof(s_config.banks[bidx].buttons[bi].entity_id), "%s", mt->valuestring);
                    if (cJSON_IsNumber(tp)) s_config.banks[bidx].buttons[bi].type = (btn_type_t)tp->valueint;
                    if (cJSON_IsNumber(rg)) s_config.banks[bidx].buttons[bi].radio_group = (uint8_t)rg->valueint;

                    /* colors */
                    cJSON *co = cJSON_GetObjectItemCaseSensitive(bitem, "color_on");
                    cJSON *cf = cJSON_GetObjectItemCaseSensitive(bitem, "color_off");
                    parse_rgb_array_to_rgb(co, s_config.banks[bidx].buttons[bi].color_on);
                    parse_rgb_array_to_rgb(cf, s_config.banks[bidx].buttons[bi].color_off);

                    bi++;
                }
            }
            bidx++;
        }
    }

    sanitize_config();

    /* Persist changes. Only touch network state if those settings changed. */
    save_full_config_nvs();

    if (s_mqtt_connected) {
        for (int b = 0; b < MAX_BANKS; ++b) {
            for (int i = 0; i < MAIN_BTN_COUNT; ++i) {
                clear_button_discovery_by_entity_id(prev_config.banks[b].buttons[i].entity_id);
            }
        }
    }

    bool mqtt_changed = !mqtt_config_equal(&prev_mqtt, &s_mqtt_config);
    bool wifi_changed = !wifi_config_equal(&prev_wifi, &s_wifi_config);

    if (mqtt_changed || wifi_changed) {
        if (wifi_changed) {
            apply_wifi_config();
        }
        if (mqtt_changed || wifi_changed) {
            start_mqtt();
        }
    }

    if (s_mqtt_connected && !mqtt_changed && !wifi_changed) {
        unsubscribe_all_state_topics(&prev_config);
        subscribe_all_state_topics(&s_config);
        publish_mqtt_discovery();
    }

    request_ui_refresh();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"result\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(root);
    return ESP_OK;
}

/* JSON API: GET /api/diag - returns diagnostic info */
static esp_err_t api_diag_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddBoolToObject(root, "eth_connected", s_eth_connected);
    cJSON_AddBoolToObject(root, "wifi_connected", s_wifi_connected);
    cJSON_AddBoolToObject(root, "mqtt_connected", s_mqtt_connected);
    cJSON_AddNumberToObject(root, "button_raw_state", s_button_state);

    cJSON *tb = cJSON_CreateIntArray((const int*)s_touch_baseline, BUTTON_COUNT);
    cJSON_AddItemToObject(root, "touch_baseline", tb);

    char *out = cJSON_PrintUnformatted(root);
    if (out) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        cJSON_free(out);
    } else {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* Serve embedded web assets helper (file-scope) */
static esp_err_t serve_str(httpd_req_t *req, const char *data, const char *type)
{
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, data, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t serve_dashboard_handler(httpd_req_t *req) { return serve_str(req, web_index_html, "text/html"); }
static esp_err_t serve_config_page_handler(httpd_req_t *req) { return serve_str(req, web_config_html, "text/html"); }
static esp_err_t serve_banks_page_handler(httpd_req_t *req) { return serve_str(req, web_banks_html, "text/html"); }
static esp_err_t serve_diag_page_handler(httpd_req_t *req) { return serve_str(req, web_diag_html, "text/html"); }
static esp_err_t serve_css_handler(httpd_req_t *req) { return serve_str(req, web_styles_css, "text/css"); }
static esp_err_t serve_js_handler(httpd_req_t *req) { return serve_str(req, web_app_js, "application/javascript"); }

/* Start the tiny config server on port 80.                                  */
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 18;  /* Accommodate all endpoints: save, api config/diag/reboot, pages (including banks), static assets */

    if (httpd_start(&server, &config) == ESP_OK) {
        /* Serve root and pages */
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = api_get_config_handler, .user_ctx = NULL };
        /* We'll serve dashboard on /dashboard */
        httpd_uri_t uri_dashboard = { .uri = "/dashboard", .method = HTTP_GET, .handler = NULL, .user_ctx = NULL };
        /* Register simple file handlers below */
        
        /* Serve dashboard HTML */
        httpd_uri_t uri_dash_html = { .uri = "/dashboard", .method = HTTP_GET, .handler = NULL, .user_ctx = NULL };
        /* We'll register dedicated handlers for static files further down */
        
        /* Keep existing config GET handler as /config (UI page) */
        httpd_uri_t uri_config_page = { .uri = "/config", .method = HTTP_GET, .handler = NULL, .user_ctx = NULL };

        httpd_uri_t uri_post = { .uri = "/save", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_post);

        /* Static asset handlers: inline serve from web_assets.h */
        httpd_uri_t uri_static_css = { .uri = "/static/styles.css", .method = HTTP_GET, .handler = NULL, .user_ctx = NULL };
        httpd_uri_t uri_static_js = { .uri = "/static/app.js", .method = HTTP_GET, .handler = NULL, .user_ctx = NULL };

        /* We'll register the API and static handlers after this block using function pointers defined below. */
        
        /* Register JSON API endpoints */
        /* Register JSON API endpoints */
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/api/config", .method = HTTP_GET, .handler = api_get_config_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/api/config", .method = HTTP_POST, .handler = api_post_config_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/api/diag", .method = HTTP_GET, .handler = api_diag_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot_handler, .user_ctx = NULL });

        /* Serve UI pages and static assets using small inline handlers that use embedded strings */
        extern esp_err_t serve_dashboard_handler(httpd_req_t *req);
        extern esp_err_t serve_config_page_handler(httpd_req_t *req);
        extern esp_err_t serve_banks_page_handler(httpd_req_t *req);
        extern esp_err_t serve_diag_page_handler(httpd_req_t *req);
        extern esp_err_t serve_css_handler(httpd_req_t *req);
        extern esp_err_t serve_js_handler(httpd_req_t *req);

        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = serve_dashboard_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/dashboard", .method = HTTP_GET, .handler = serve_dashboard_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/config", .method = HTTP_GET, .handler = serve_config_page_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/banks", .method = HTTP_GET, .handler = serve_banks_page_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/diag", .method = HTTP_GET, .handler = serve_diag_page_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/static/styles.css", .method = HTTP_GET, .handler = serve_css_handler, .user_ctx = NULL });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/static/app.js", .method = HTTP_GET, .handler = serve_js_handler, .user_ctx = NULL });
        
        ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
        return server;
    }
    return NULL;
}


/* -------------------------------------------------------------------------- */
/* Main entry point                                                            */
/*                                                                            */
/* Boot order matters: load settings, create the event queue, initialize      */
/* hardware, start the startup animation, bring up networking, then launch   */
/* the long-lived tasks.                                                      */
/* -------------------------------------------------------------------------- */
void app_main(void)
{
    // 1. Initialize NVS (Required for Wi-Fi and later web settings)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_default_config();
    load_settings_nvs(); // Saved settings override the baked-in defaults.
    load_full_config_nvs(); // Load any saved full system config

    s_btn_event_queue = xQueueCreate(20, sizeof(input_event_t));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Initialize Hardware
    // Bring up peripherals before networking so the splash screen can appear
    // immediately while the transport stack is still starting.
    i2c_init();       
    lcd_init();
    buttons_init();
    touch_init_all();
    ws2812_init();

    s_startup_complete = false;
    // The startup task keeps animating in the background until boot completes.
    xTaskCreate(startup_task, "startup_ui", 3072, NULL, 6, NULL);
    
    // 3. Initialize Network Failover
    init_network_failover();

    // 4. Create Tasks
    xTaskCreate(hardware_task, "hw_poll", 4096, NULL, 5, NULL);
    
    // 5. Start Web Server
    start_webserver();

    s_startup_complete = true;

    xTaskCreate(logic_task, "app_logic", 4096, NULL, 4, NULL);
    
    update_system_ui(-1);

    ESP_LOGI(TAG, "System Boot Complete. Tasks running.");
}

/* Persist the entire system configuration (system + mqtt + wifi) as blobs in NVS. */
static void save_full_config_nvs(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        uint32_t cfg_version = CONFIG_FORMAT_VERSION;
        nvs_set_u32(my_handle, "cfg_ver", cfg_version);
        nvs_set_blob(my_handle, "sys_cfg", &s_config, sizeof(s_config));
        nvs_set_blob(my_handle, "mqtt_cfg", &s_mqtt_config, sizeof(s_mqtt_config));
        nvs_set_blob(my_handle, "wifi_cfg", &s_wifi_config, sizeof(s_wifi_config));
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Full config saved to NVS");
    }
}

static void load_full_config_nvs(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        uint32_t cfg_version = 0;
        if (nvs_get_u32(my_handle, "cfg_ver", &cfg_version) != ESP_OK) {
            cfg_version = 0;
        }

        size_t required_size = 0;
        if (nvs_get_blob(my_handle, "sys_cfg", NULL, &required_size) == ESP_OK && required_size == sizeof(s_config)) {
            nvs_get_blob(my_handle, "sys_cfg", &s_config, &required_size);
            ESP_LOGI(TAG, "Loaded system config from NVS");
        }

        required_size = 0;
        if (nvs_get_blob(my_handle, "mqtt_cfg", NULL, &required_size) == ESP_OK && required_size == sizeof(s_mqtt_config)) {
            nvs_get_blob(my_handle, "mqtt_cfg", &s_mqtt_config, &required_size);
            ESP_LOGI(TAG, "Loaded mqtt config from NVS");
        }

        required_size = 0;
        if (nvs_get_blob(my_handle, "wifi_cfg", NULL, &required_size) == ESP_OK && required_size == sizeof(s_wifi_config)) {
            nvs_get_blob(my_handle, "wifi_cfg", &s_wifi_config, &required_size);
            ESP_LOGI(TAG, "Loaded wifi config from NVS");
        }
        nvs_close(my_handle);

        if (cfg_version < CONFIG_FORMAT_VERSION) {
            migrate_grb_colors_to_rgb();
            save_full_config_nvs();
            ESP_LOGI(TAG, "Migrated LED colors to RGB config format");
        }
    }

    sanitize_config();
}

/* Apply WiFi configuration changes immediately (reconnect with new settings) */
static void apply_wifi_config(void)
{
    // Disconnect first
    esp_wifi_disconnect();
    
    // Create ESP WiFi config struct from app settings
    wifi_config_t esp_wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)esp_wifi_cfg.sta.ssid, s_wifi_config.ssid, sizeof(esp_wifi_cfg.sta.ssid) - 1);
    strncpy((char*)esp_wifi_cfg.sta.password, s_wifi_config.password, sizeof(esp_wifi_cfg.sta.password) - 1);
    
    // Set WiFi config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &esp_wifi_cfg));
    
    // Apply IP settings via netif
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        if (s_wifi_config.use_dhcp) {
            esp_netif_dhcpc_stop(netif);
            esp_netif_dhcpc_start(netif);
            ESP_LOGI(TAG, "WiFi: DHCP enabled");
        } else {
            esp_netif_dhcpc_stop(netif);
            esp_netif_ip_info_t ip_info;
            esp_netif_str_to_ip4(s_wifi_config.static_ip, &ip_info.ip);
            esp_netif_str_to_ip4(s_wifi_config.static_netmask, &ip_info.netmask);
            esp_netif_str_to_ip4(s_wifi_config.static_gateway, &ip_info.gw);
            esp_netif_set_ip_info(netif, &ip_info);
            ESP_LOGI(TAG, "WiFi: Static IP %s set", s_wifi_config.static_ip);
        }
    }
    
    // Reconnect
    esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi reconnecting with new config: %s", s_wifi_config.ssid);
}

/* Reboot the device */
static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reboot requested via web API");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"result\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);
    
    // Schedule reboot in 500ms to ensure response is sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
