#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rmt.h"
#include "driver/touch_pad.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

static const char *TAG = "hw_test";

/* WT32-ETH01 ethernet defaults. */
#define ETH_PHY_ADDR 1
#define ETH_PHY_RST_GPIO -1
#define ETH_MDC_GPIO 23
#define ETH_MDIO_GPIO 18
#define ETH_PHY_POWER_GPIO 16

/* Shared I2C bus for both PCF8574 chips. */
#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_14
#define I2C_SCL GPIO_NUM_5
#define I2C_FREQ_HZ 100000

#define PCF_LCD_ADDR 0x27
#define PCF_BUTTON_ADDR 0x26

/* LCD backpack bit mapping for common PCF8574 boards. */
#define LCD_PIN_EN 0x04
#define LCD_PIN_RW 0x02
#define LCD_PIN_RS 0x01
#define LCD_PIN_BL 0x08

/* Old firmware used 6 buttons on PCF8574 pins below (active low). */
#define BUTTON_COUNT 6
static const uint8_t k_button_pcf_bits[BUTTON_COUNT] = {5, 4, 3, 2, 1, 0};

/* Capacitive rings from old firmware mapping. */
static const touch_pad_t k_touch_channels[BUTTON_COUNT] = {
    TOUCH_PAD_NUM0, /* GPIO4  */
    TOUCH_PAD_NUM3, /* GPIO15 */
    TOUCH_PAD_NUM2, /* GPIO2  */
    TOUCH_PAD_NUM5, /* GPIO12 */
    TOUCH_PAD_NUM9, /* GPIO32 */
    TOUCH_PAD_NUM8, /* GPIO33 */
};

/* WS2812 chain from old firmware. */
#define LED_COUNT 6
#define LED_PIN GPIO_NUM_17
#define WS2812_RMT_CHANNEL RMT_CHANNEL_0
#define WS2812_RMT_CLK_DIV 2
#define WS2812_T0H_TICKS 14
#define WS2812_T0L_TICKS 32
#define WS2812_T1H_TICKS 28
#define WS2812_T1L_TICKS 24
#define WS2812_RESET_TICKS 2400

static rmt_item32_t s_led_items[(LED_COUNT * 24) + 1];
static bool s_eth_link = false;
static bool s_eth_has_ip = false;

static uint16_t s_touch_baseline[BUTTON_COUNT] = {0};
static bool s_touch_ready = false;

static uint8_t s_button_state = 0xFF;
static uint8_t s_button_last_state = 0xFF;
static int64_t s_button_last_change_ms[BUTTON_COUNT] = {0};

static char s_lcd_last_line1[17] = {0};
static char s_lcd_last_line2[17] = {0};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t i2c_write_u8(uint8_t addr, uint8_t value)
{
    return i2c_master_write_to_device(I2C_PORT, addr, &value, 1, pdMS_TO_TICKS(20));
}

static esp_err_t i2c_read_u8(uint8_t addr, uint8_t *value)
{
    return i2c_master_read_from_device(I2C_PORT, addr, value, 1, pdMS_TO_TICKS(20));
}

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

static esp_err_t lcd_set_lines(const char *line1, const char *line2)
{
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
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t lcd_init(void)
{
    // 1. Wait for LCD to fully power up (some clones need >50ms)
    vTaskDelay(pdMS_TO_TICKS(100)); 
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

static esp_err_t buttons_init(void)
{
    /* PCF8574 pins are quasi-bidirectional; writing 1 lets pin float high for input mode. */
    return i2c_write_u8(PCF_BUTTON_ADDR, 0xFF);
}

static esp_err_t buttons_read_raw(uint8_t *state)
{
    return i2c_read_u8(PCF_BUTTON_ADDR, state);
}

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

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            s_eth_link = true;
            ESP_LOGI(TAG, "Ethernet link UP");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            s_eth_link = false;
            s_eth_has_ip = false;
            ESP_LOGW(TAG, "Ethernet link DOWN");
            break;
        default:
            break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_eth_has_ip = true;
        ESP_LOGI(TAG, "Ethernet IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

static esp_err_t ethernet_init(void)
{
    ESP_ERROR_CHECK(gpio_set_direction(ETH_PHY_POWER_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(ETH_PHY_POWER_GPIO, 1));

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (!mac) {
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (!phy) {
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif_eth = esp_netif_new(&netif_cfg);
    if (!netif_eth) {
        return ESP_FAIL;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(netif_eth, glue));

    return esp_eth_start(eth_handle);
}

static void update_lcd_status(const uint8_t debounced_pressed[BUTTON_COUNT], const bool touch_active[BUTTON_COUNT])
{
    char line1[17];
    char line2[17];

    snprintf(line1, sizeof(line1), "B:%c%c%c%c%c%c E:%c", 
             debounced_pressed[0] ? '1' : '-',
             debounced_pressed[1] ? '2' : '-',
             debounced_pressed[2] ? '3' : '-',
             debounced_pressed[3] ? '4' : '-',
             debounced_pressed[4] ? '5' : '-',
             debounced_pressed[5] ? '6' : '-',
             s_eth_link ? 'U' : 'D');

    snprintf(line2, sizeof(line2), "T:%c%c%c%c%c%c IP:%c", 
             touch_active[0] ? '1' : '-',
             touch_active[1] ? '2' : '-',
             touch_active[2] ? '3' : '-',
             touch_active[3] ? '4' : '-',
             touch_active[4] ? '5' : '-',
             touch_active[5] ? '6' : '-',
             s_eth_has_ip ? 'Y' : 'N');

    if (lcd_set_lines(line1, line2) != ESP_OK) {
        ESP_LOGW(TAG, "LCD update failed");
    }
}

static void update_led_status(const uint8_t debounced_pressed[BUTTON_COUNT], const bool touch_active[BUTTON_COUNT])
{
    uint8_t rgb[LED_COUNT][3] = {0};

    for (int i = 0; i < LED_COUNT; ++i) {
        if (debounced_pressed[i]) {
            rgb[i][0] = 0;
            rgb[i][1] = 64;
            rgb[i][2] = 0;
        } else {
            rgb[i][0] = 32;
            rgb[i][1] = 0;
            rgb[i][2] = 0;
        }

        if (touch_active[i]) {
            rgb[i][0] = 0;
            rgb[i][1] = 16;
            rgb[i][2] = 64;
        }
    }

    if (ws2812_show(rgb) != ESP_OK) {
        ESP_LOGW(TAG, "LED update failed");
    }
}

static void hardware_test_task(void *arg)
{
    (void)arg;

    uint8_t debounced_pressed[BUTTON_COUNT] = {0};
    uint16_t touch_values[BUTTON_COUNT] = {0};
    bool touch_active[BUTTON_COUNT] = {false};

    while (true) {
        uint8_t raw = 0xFF;
        if (buttons_read_raw(&raw) != ESP_OK) {
            ESP_LOGW(TAG, "Button expander read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        s_button_state = raw;

        for (int i = 0; i < BUTTON_COUNT; ++i) {
            bool is_pressed_now = ((s_button_state >> k_button_pcf_bits[i]) & 0x01) == 0;
            bool was_pressed_last = ((s_button_last_state >> k_button_pcf_bits[i]) & 0x01) == 0;

            if (is_pressed_now != was_pressed_last) {
                s_button_last_change_ms[i] = now_ms();
            }

            if ((now_ms() - s_button_last_change_ms[i]) > 25) {
                debounced_pressed[i] = is_pressed_now ? 1 : 0;
            }
        }

        s_button_last_state = s_button_state;

        touch_read_all(touch_values);
        for (int i = 0; i < BUTTON_COUNT; ++i) {
            touch_active[i] = touch_is_active((size_t)i, touch_values[i]);
        }

        update_led_status(debounced_pressed, touch_active);
        update_lcd_status(debounced_pressed, touch_active);

        ESP_LOGI(TAG,
                 "BTN raw=0x%02X deb=%d%d%d%d%d%d  TOUCH=%u,%u,%u,%u,%u,%u",
                 raw,
                 debounced_pressed[0], debounced_pressed[1], debounced_pressed[2],
                 debounced_pressed[3], debounced_pressed[4], debounced_pressed[5],
                 (unsigned)touch_values[0], (unsigned)touch_values[1], (unsigned)touch_values[2],
                 (unsigned)touch_values[3], (unsigned)touch_values[4], (unsigned)touch_values[5]);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_event_handler, NULL));

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, i2c_cfg.mode, 0, 0, 0));

    i2c_scan_log();

    if (lcd_init() != ESP_OK) {
        ESP_LOGW(TAG, "LCD init failed at 0x%02X", PCF_LCD_ADDR);
    } else {
        lcd_set_lines("WT32 HW TEST", "Init OK");
    }

    if (buttons_init() != ESP_OK) {
        ESP_LOGE(TAG, "Button PCF8574 init failed at 0x%02X", PCF_BUTTON_ADDR);
    }

    if (touch_init_all() != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (rings disabled)");
    } else {
        ESP_LOGI(TAG, "Touch baseline: %u,%u,%u,%u,%u,%u",
                 (unsigned)s_touch_baseline[0], (unsigned)s_touch_baseline[1], (unsigned)s_touch_baseline[2],
                 (unsigned)s_touch_baseline[3], (unsigned)s_touch_baseline[4], (unsigned)s_touch_baseline[5]);
    }

    if (ws2812_init() != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed on GPIO %d", (int)LED_PIN);
    }

    if (ethernet_init() != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet init failed");
    }

    xTaskCreate(hardware_test_task, "hardware_test_task", 4096, NULL, 5, NULL);
}
