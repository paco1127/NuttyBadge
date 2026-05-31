#include "uno_common.h"

#include "esp_log.h"

#define TOP_CARD_LED_GPIO GPIO_NUM_2
#define SELECTED_CARD_LED_GPIO GPIO_NUM_4
#define CUSTOM_LED_GPIO GPIO_NUM_5

static const char *TAG = "UNOCommon";

typedef struct {
    uint8_t top_color;
    uint8_t selected_color;
    uint8_t custom_mode;
    bool top_state;
    bool selected_state;
    bool custom_state;
    int64_t top_last_toggle;
    int64_t selected_last_toggle;
    int64_t custom_last_toggle;
} uno_led_state_t;

static uno_led_state_t led_state = {
    .top_color = UNO_COLOR_NONE,
    .selected_color = UNO_COLOR_NONE,
    .custom_mode = UNO_LED_MODE_OFF,
    .top_state = false,
    .selected_state = false,
    .custom_state = false,
    .top_last_toggle = 0,
    .selected_last_toggle = 0,
    .custom_last_toggle = 0
};

static uint32_t uno_color_blink_period_ms(uint8_t color) {
    switch (color) {
        case UNO_COLOR_RED:
            return 0;
        case UNO_COLOR_GREEN:
            return 700;
        case UNO_COLOR_BLUE:
            return 350;
        case UNO_COLOR_YELLOW:
            return 900;
        case UNO_COLOR_WILD:
            return 150;
        default:
            return 0;
    }
}

static void uno_update_color_led(gpio_num_t gpio, uint8_t color, bool *state, int64_t *last_toggle, bool *force_off) {
    if (color == UNO_COLOR_NONE || color > UNO_COLOR_WILD) {
        gpio_set_level(gpio, 0);
        *force_off = true;
        return;
    }

    uint32_t period_ms = uno_color_blink_period_ms(color);
    if (period_ms == 0) {
        gpio_set_level(gpio, 1);
        *force_off = false;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - *last_toggle >= (int64_t)period_ms * 1000) {
        *state = !(*state);
        *last_toggle = now;
        gpio_set_level(gpio, *state ? 1 : 0);
    }
    *force_off = false;
}

void uno_led_init(void) {
    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << TOP_CARD_LED_GPIO) | (1ULL << SELECTED_CARD_LED_GPIO) | (1ULL << CUSTOM_LED_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED GPIO init failed: %d", err);
    }
    gpio_set_level(TOP_CARD_LED_GPIO, 0);
    gpio_set_level(SELECTED_CARD_LED_GPIO, 0);
    gpio_set_level(CUSTOM_LED_GPIO, 0);
}

void led_set_top_card(card_t card) {
    if (card == UNO_CARD_NONE) {
        led_state.top_color = UNO_COLOR_NONE;
    } else {
        led_state.top_color = UNO_CARD_COLOR(card);
    }
    led_state.top_state = true;
    led_state.top_last_toggle = esp_timer_get_time();
}

void led_set_selected_card(card_t card) {
    if (card == UNO_CARD_NONE) {
        led_state.selected_color = UNO_COLOR_NONE;
    } else {
        led_state.selected_color = UNO_CARD_COLOR(card);
    }
    led_state.selected_state = true;
    led_state.selected_last_toggle = esp_timer_get_time();
}

void set_custom_led(uint8_t mode) {
    led_state.custom_mode = mode;
    if (mode == UNO_LED_MODE_OFF) {
        led_state.custom_state = false;
        gpio_set_level(CUSTOM_LED_GPIO, 0);
    } else if (mode == UNO_LED_MODE_CONNECTED_SOLID) {
        led_state.custom_state = true;
        gpio_set_level(CUSTOM_LED_GPIO, 1);
    } else {
        led_state.custom_state = true;
        led_state.custom_last_toggle = esp_timer_get_time();
        gpio_set_level(CUSTOM_LED_GPIO, 1);
    }
}

void uno_led_tick(void) {
    bool force_off = false;
    uno_update_color_led(TOP_CARD_LED_GPIO, led_state.top_color, &led_state.top_state, &led_state.top_last_toggle, &force_off);
    if (force_off) {
        led_state.top_state = false;
    }

    force_off = false;
    uno_update_color_led(SELECTED_CARD_LED_GPIO, led_state.selected_color, &led_state.selected_state, &led_state.selected_last_toggle, &force_off);
    if (force_off) {
        led_state.selected_state = false;
    }

    int64_t now = esp_timer_get_time();
    switch (led_state.custom_mode) {
        case UNO_LED_MODE_OFF:
            gpio_set_level(CUSTOM_LED_GPIO, 0);
            break;
        case UNO_LED_MODE_CONNECTED_SOLID:
            gpio_set_level(CUSTOM_LED_GPIO, 1);
            break;
        case UNO_LED_MODE_TURN_BLINK:
        case UNO_LED_MODE_WILD_CHOICE_FLASH: {
            uint32_t period_ms = (led_state.custom_mode == UNO_LED_MODE_TURN_BLINK) ? 400 : 120;
            if (now - led_state.custom_last_toggle >= (int64_t)period_ms * 1000) {
                led_state.custom_state = !led_state.custom_state;
                led_state.custom_last_toggle = now;
                gpio_set_level(CUSTOM_LED_GPIO, led_state.custom_state ? 1 : 0);
            }
            break;
        }
        default:
            break;
    }
}

__attribute__((weak)) bool uno_button_up(void) {
    return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_UP);
}

__attribute__((weak)) bool uno_button_down(void) {
    return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_DOWN);
}

__attribute__((weak)) bool uno_button_play(void) {
    return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_A);
}

__attribute__((weak)) bool uno_button_draw(void) {
    return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_B);
}

__attribute__((weak)) bool uno_button_exit(void) {
    return NuttyInput_waitSingleButtonHoldLongNonBlock(NUTTYINPUT_BTN_START);
}

__attribute__((weak)) uint8_t uno_get_bot_count(void) {
    return 0;
}

__attribute__((weak)) const char *uno_get_custom_player_name(uint8_t player_index) {
    (void)player_index;
    return NULL;
}
