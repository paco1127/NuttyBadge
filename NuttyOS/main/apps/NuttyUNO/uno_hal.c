#include "apps/NuttyUNO/uno_common.h"

#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "services/NuttyRGB/NuttyRGB.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

/*
 * NuttyBadge LED mapping:
 *   - The badge has 3 RGB LED bulbs (RGB_BULBS=3) driven by the NuttyRGB service.
 *   - We use RGB bulb 0 for TOP_CARD color, bulb 1 for SELECTED_CARD color.
 *   - GPIO 5 (UNO_LED_CUSTOM_GPIO) is used for the custom LED (turn blink, etc.).
 *
 * Color mapping for RGB LEDs:
 *   Red     -> (255, 0,   0)
 *   Green   -> (0,   255, 0)
 *   Blue    -> (0,   0,   255)
 *   Yellow  -> (255, 200, 0)
 *   Wild    -> white (100, 100, 100)
 */

#define UNO_LED_CUSTOM_GPIO GPIO_NUM_5

typedef struct {
    lv_style_t font_small;
    lv_style_t font_tiny;
    bool styles_ready;
    lv_obj_t *root;
} uno_display_ctx_t;

static uno_display_ctx_t g_display;

static void uno_display_init_styles(void) {
    if (g_display.styles_ready) {
        return;
    }

    lv_style_init(&g_display.font_small);
    lv_style_set_text_font(&g_display.font_small, &lv_font_montserrat_10);

    lv_style_init(&g_display.font_tiny);
    lv_style_set_text_font(&g_display.font_tiny, &cg_pixel_4x5_mono);

    g_display.styles_ready = true;
}

void uno_display_init(void) {
    g_display.root = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();
    uno_display_init_styles();
    if (g_display.root != NULL) {
        lv_obj_set_style_border_width(g_display.root, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_display.root, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    NuttyDisplay_unlockLVGL();
}

void uno_display_clear(void) {
    NuttyDisplay_lockLVGL();
    if (g_display.root != NULL) {
        lv_obj_clean(g_display.root);
    }
    NuttyDisplay_unlockLVGL();
}

void uno_display_lock(void) {
    NuttyDisplay_lockLVGL();
}

void uno_display_unlock(void) {
    NuttyDisplay_unlockLVGL();
}

void *uno_display_get_root(void) {
    return g_display.root;
}

void uno_display_label_create(uno_display_label_t *label, void *parent, int x, int y, const char *text, bool small_font) {
    lv_obj_t *target_parent = parent ? (lv_obj_t *)parent : g_display.root;
    if (label == NULL || target_parent == NULL) {
        return;
    }

    lv_obj_t *lbl = lv_label_create(target_parent);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_pos(lbl, x, y);
    lv_obj_add_style(lbl, small_font ? &g_display.font_tiny : &g_display.font_small, LV_PART_MAIN);
    label->obj = lbl;
}

void uno_display_label_set_text(uno_display_label_t *label, const char *text) {
    if (label == NULL || label->obj == NULL) {
        return;
    }
    lv_label_set_text((lv_obj_t *)label->obj, text ? text : "");
}

void uno_display_card_create(uno_display_card_t *card, void *parent, int x, int y, int w, int h) {
    lv_obj_t *target_parent = parent ? (lv_obj_t *)parent : g_display.root;
    if (card == NULL || target_parent == NULL) {
        return;
    }

    lv_obj_t *box = lv_obj_create(target_parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(box);
    lv_obj_center(label);
    lv_obj_add_style(label, &g_display.font_tiny, LV_PART_MAIN);

    card->box = box;
    card->label = label;
}

static void uno_card_short_text(card_t card, char *out, size_t out_len) {
    const char *color = "W";
    char value[4] = "?";

    switch (card.color) {
        case UNO_COLOR_RED: color = "R"; break;
        case UNO_COLOR_GREEN: color = "G"; break;
        case UNO_COLOR_BLUE: color = "B"; break;
        case UNO_COLOR_YELLOW: color = "Y"; break;
        default: color = "W"; break;
    }

    if (card.value <= UNO_VALUE_9) {
        snprintf(value, sizeof(value), "%u", (unsigned)card.value);
    } else if (card.value == UNO_VALUE_SKIP) {
        snprintf(value, sizeof(value), "S");
    } else if (card.value == UNO_VALUE_REVERSE) {
        snprintf(value, sizeof(value), "R");
    } else if (card.value == UNO_VALUE_DRAW_TWO) {
        snprintf(value, sizeof(value), "+2");
    } else if (card.value == UNO_VALUE_WILD) {
        snprintf(value, sizeof(value), "W");
    } else if (card.value == UNO_VALUE_WILD_DRAW_FOUR) {
        snprintf(value, sizeof(value), "+4");
    }

    snprintf(out, out_len, "%s%s", color, value);
}

void uno_display_card_set(uno_display_card_t *card, card_t value, bool selected) {
    if (card == NULL || card->box == NULL || card->label == NULL) {
        return;
    }

    if (uno_card_is_none(value)) {
        lv_obj_add_flag((lv_obj_t *)card->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag((lv_obj_t *)card->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width((lv_obj_t *)card->box, selected ? 2 : 1, LV_PART_MAIN);

    char label_text[8];
    uno_card_short_text(value, label_text, sizeof(label_text));
    lv_label_set_text((lv_obj_t *)card->label, label_text);
}

void uno_display_obj_set_hidden(void *obj, bool hidden) {
    if (obj == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Rising-edge button detection ──────────────────────────────────
 * Instead of waiting for press-hold-release (which causes lag),
 * we detect the moment a button transitions from NOT-pressed to
 * pressed.  This gives instant response on the press-down event.
 *
 * uno_btn_init() must be called once before use to capture the
 * initial button state so that any already-held buttons do not
 * trigger false rising edges.
 */
static uint16_t s_prev_btn = 0;

void uno_btn_init(void) {
    /* Clear NuttyInput held state first so it doesn't bleed through.
     * Only clear the main face buttons (not USRDEF which may be unused/floating). */
    NuttyInput_clearButtonHoldState(
        NUTTYINPUT_BTN_UP | NUTTYINPUT_BTN_DOWN | NUTTYINPUT_BTN_LEFT |
        NUTTYINPUT_BTN_RIGHT | NUTTYINPUT_BTN_A | NUTTYINPUT_BTN_B |
        NUTTYINPUT_BTN_SELECT | NUTTYINPUT_BTN_START);

    /* Wait until all main buttons are fully released.
     * This is critical when transitioning from a previous screen
     * where a button (e.g. A) was pressed to trigger the transition.
     * Without this, the held button would be captured in s_prev_btn
     * and its rising edge would never fire in the new context.
     *
     * We only check the main face buttons (mask 0x00FF) to avoid
     * getting stuck on unused buttons like USRDEF that may float. */
    uint16_t main_btns = NUTTYINPUT_BTN_UP | NUTTYINPUT_BTN_DOWN |
                         NUTTYINPUT_BTN_LEFT | NUTTYINPUT_BTN_RIGHT |
                         NUTTYINPUT_BTN_A | NUTTYINPUT_BTN_B |
                         NUTTYINPUT_BTN_SELECT | NUTTYINPUT_BTN_START;
    while (NuttyInput_isOneOfTheButtonsCurrentlyPressed(main_btns)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Small extra delay to let the debounced state settle */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Now capture the (released) state so held buttons don't trigger false edges */
    s_prev_btn = 0;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_UP))    s_prev_btn |= NUTTYINPUT_BTN_UP;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_DOWN))  s_prev_btn |= NUTTYINPUT_BTN_DOWN;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_LEFT))  s_prev_btn |= NUTTYINPUT_BTN_LEFT;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_RIGHT)) s_prev_btn |= NUTTYINPUT_BTN_RIGHT;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_A))     s_prev_btn |= NUTTYINPUT_BTN_A;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(NUTTYINPUT_BTN_B))     s_prev_btn |= NUTTYINPUT_BTN_B;
}

static bool uno_btn_rising_edge(uint16_t btn) {
    uint16_t now = 0;
    if (NuttyInput_isOneOfTheButtonsCurrentlyPressed(btn)) {
        now = btn;
    }
    bool rising = (now & ~s_prev_btn) & btn;
    s_prev_btn = (s_prev_btn & ~btn) | now;
    return rising;
}

bool uno_btn_up_pressed(void)    { return uno_btn_rising_edge(NUTTYINPUT_BTN_UP); }
bool uno_btn_down_pressed(void)  { return uno_btn_rising_edge(NUTTYINPUT_BTN_DOWN); }
bool uno_btn_left_pressed(void)  { return uno_btn_rising_edge(NUTTYINPUT_BTN_LEFT); }
bool uno_btn_right_pressed(void) { return uno_btn_rising_edge(NUTTYINPUT_BTN_RIGHT); }
bool uno_btn_play_pressed(void)  { return uno_btn_rising_edge(NUTTYINPUT_BTN_A); }
bool uno_btn_draw_pressed(void)  { return uno_btn_rising_edge(NUTTYINPUT_BTN_B); }
bool uno_btn_back_pressed(void)  { return NuttyInput_waitSingleButtonHoldLongNonBlock(NUTTYINPUT_BTN_START); }

/* ── LED helpers ──────────────────────────────────────────────────── */

static void uno_led_rgb_set_bulb(uint8_t bulb, uno_color_t color) {
    switch (color) {
        case UNO_COLOR_RED:
            NuttyRGB_SetRGBWithoutDisplay(bulb, 255, 0, 0);
            break;
        case UNO_COLOR_GREEN:
            NuttyRGB_SetRGBWithoutDisplay(bulb, 0, 255, 0);
            break;
        case UNO_COLOR_BLUE:
            NuttyRGB_SetRGBWithoutDisplay(bulb, 0, 0, 255);
            break;
        case UNO_COLOR_YELLOW:
            NuttyRGB_SetRGBWithoutDisplay(bulb, 255, 200, 0);
            break;
        case UNO_COLOR_WILD:
        default:
            /* White for wild cards */
            NuttyRGB_SetRGBWithoutDisplay(bulb, 100, 100, 100);
            break;
    }
    NuttyRGB_DisplayNow();
}

void uno_led_init(void) {
    /* Configure the custom GPIO LED */
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << UNO_LED_CUSTOM_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(UNO_LED_CUSTOM_GPIO, 0);

    /* Turn off RGB bulbs */
    NuttyRGB_SetAllRGBWithoutDisplay(0, 0, 0);
    NuttyRGB_DisplayNow();
}

void led_set_top_card(card_t card) {
    if (uno_card_is_none(card)) {
        NuttyRGB_SetRGBWithoutDisplay(0, 0, 0, 0);
        NuttyRGB_DisplayNow();
        return;
    }
    uno_led_rgb_set_bulb(0, (uno_color_t)card.color);
}

void led_set_selected_card(card_t card) {
    if (uno_card_is_none(card)) {
        NuttyRGB_SetRGBWithoutDisplay(1, 0, 0, 0);
        NuttyRGB_DisplayNow();
        return;
    }
    uno_led_rgb_set_bulb(1, (uno_color_t)card.color);
}

/* ── Custom LED (GPIO) with mode-based blinking ───────────────────── */

static uint8_t g_custom_led_mode = UNO_LED_MODE_OFF;
static bool g_custom_led_level = false;
static int64_t g_custom_led_last_toggle = 0;

void set_custom_led(uint8_t mode) {
    if (mode == g_custom_led_mode) {
        return;
    }
    g_custom_led_mode = mode;
    g_custom_led_last_toggle = esp_timer_get_time();
    g_custom_led_level = (mode == UNO_LED_MODE_CONNECTED_SOLID);
    gpio_set_level(UNO_LED_CUSTOM_GPIO, g_custom_led_level ? 1 : 0);
}

void uno_custom_led_update(void) {
    int64_t now = esp_timer_get_time();
    uint32_t period_ms = 0;

    if (g_custom_led_mode == UNO_LED_MODE_OFF) {
        if (g_custom_led_level) {
            g_custom_led_level = false;
            gpio_set_level(UNO_LED_CUSTOM_GPIO, 0);
        }
        return;
    }

    if (g_custom_led_mode == UNO_LED_MODE_CONNECTED_SOLID) {
        if (!g_custom_led_level) {
            g_custom_led_level = true;
            gpio_set_level(UNO_LED_CUSTOM_GPIO, 1);
        }
        return;
    }

    /* Blinking modes */
    if (g_custom_led_mode == UNO_LED_MODE_WILD_CHOICE_FLASH) {
        period_ms = 120;  /* Fast flash for wild color selection */
    } else if (g_custom_led_mode == UNO_LED_MODE_TURN_BLINK) {
        period_ms = 400;  /* Slower blink for "your turn" */
    } else {
        return;
    }

    if ((now - g_custom_led_last_toggle) >= (int64_t)period_ms * 1000) {
        g_custom_led_last_toggle = now;
        g_custom_led_level = !g_custom_led_level;
        gpio_set_level(UNO_LED_CUSTOM_GPIO, g_custom_led_level ? 1 : 0);
    }
}
