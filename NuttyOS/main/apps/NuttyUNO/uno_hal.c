#include "apps/NuttyUNO/uno_common.h"

#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "services/NuttyRGB/NuttyRGB.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#define UNO_LED_CUSTOM_GPIO GPIO_NUM_5

/* ── Display context ──────────────────────────────────────────────── */
typedef struct {
    lv_style_t font_small;
    lv_style_t font_tiny;
    bool styles_ready;
    lv_obj_t *root;
} uno_display_ctx_t;

static uno_display_ctx_t g_display;

static void uno_display_init_styles(void) {
    if (g_display.styles_ready) return;
    lv_style_init(&g_display.font_small);
    lv_style_set_text_font(&g_display.font_small, &lv_font_montserrat_10);
    lv_style_init(&g_display.font_tiny);
    lv_style_set_text_font(&g_display.font_tiny, &cg_pixel_4x5_mono);
    g_display.styles_ready = true;
}

void uno_display_init(void) {
    g_display.root = NuttyDisplay_getUserAppArea();
    uno_display_init_styles();
}

void uno_display_clear(void) {
    NuttyDisplay_lockLVGL();
    if (g_display.root != NULL) {
        lv_obj_clean(g_display.root);
    }
    NuttyDisplay_unlockLVGL();
}

void uno_display_lock(void) { NuttyDisplay_lockLVGL(); }
void uno_display_unlock(void) { NuttyDisplay_unlockLVGL(); }
void *uno_display_get_root(void) { return g_display.root; }

void uno_display_label_create(uno_display_label_t *label, void *parent, int x, int y, const char *text, bool small_font) {
    lv_obj_t *tp = parent ? (lv_obj_t *)parent : g_display.root;
    if (label == NULL || tp == NULL) return;
    lv_obj_t *lbl = lv_label_create(tp);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_pos(lbl, x, y);
    lv_obj_add_style(lbl, small_font ? &g_display.font_tiny : &g_display.font_small, LV_PART_MAIN);
    label->obj = lbl;
}

void uno_display_label_set_text(uno_display_label_t *label, const char *text) {
    if (label == NULL || label->obj == NULL) return;
    lv_label_set_text((lv_obj_t *)label->obj, text ? text : "");
}

void uno_display_card_create(uno_display_card_t *card, void *parent, int x, int y, int w, int h) {
    lv_obj_t *tp = parent ? (lv_obj_t *)parent : g_display.root;
    if (card == NULL || tp == NULL) return;
    lv_obj_t *box = lv_obj_create(tp);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_center(lbl);
    lv_obj_add_style(lbl, &g_display.font_tiny, LV_PART_MAIN);
    card->box = box;
    card->label = lbl;
}

static void uno_card_short_text(card_t card, char *out, size_t out_len) {
    if (card.value <= 9)            snprintf(out, out_len, "%u", (unsigned)card.value);
    else if (card.value == 10)      snprintf(out, out_len, "-");   /* Skip */
    else if (card.value == 11)      snprintf(out, out_len, "R");   /* Reverse */
    else if (card.value == 12)      snprintf(out, out_len, "+2");  /* Draw 2 */
    else if (card.value == 13)      snprintf(out, out_len, "W");   /* Wild */
    else if (card.value == 14)      snprintf(out, out_len, "WW");  /* Wild Draw 4 */
    else                             snprintf(out, out_len, "?");
}

void uno_display_card_set(uno_display_card_t *card, card_t value, bool selected) {
    if (card == NULL || card->box == NULL || card->label == NULL) return;
    if (uno_card_is_none(value)) {
        lv_obj_add_flag((lv_obj_t *)card->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag((lv_obj_t *)card->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width((lv_obj_t *)card->box, selected ? 2 : 1, LV_PART_MAIN);
    char buf[8];
    uno_card_short_text(value, buf, sizeof(buf));
    lv_label_set_text((lv_obj_t *)card->label, buf);
}

void uno_display_obj_set_hidden(void *obj, bool hidden) {
    if (obj == NULL) return;
    if (hidden) lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}

/* ── Button handling ───────────────────────────────────────────────
 * Use NuttyInput's debounced hold+release detection.
 * This is non-blocking and works alongside the LVGL input device
 * without fighting over button state.
 *
 * uno_btn_init() waits for all buttons to be released after a screen
 * transition so that the physical button press that triggered the
 * transition does not immediately fire in the new screen.
 */

void uno_btn_init(void) {
    /* Clear all held state so nothing bleeds through from previous screen */
    NuttyInput_clearButtonHoldState(
        NUTTYINPUT_BTN_UP | NUTTYINPUT_BTN_DOWN | NUTTYINPUT_BTN_LEFT |
        NUTTYINPUT_BTN_RIGHT | NUTTYINPUT_BTN_A | NUTTYINPUT_BTN_B |
        NUTTYINPUT_BTN_SELECT | NUTTYINPUT_BTN_START);

    /* Wait until all main buttons are physically released.
     * Only check 0x00FF (not USRDEF) to avoid stuck loops on floating pins. */
    uint16_t mask = NUTTYINPUT_BTN_UP | NUTTYINPUT_BTN_DOWN | NUTTYINPUT_BTN_LEFT |
                    NUTTYINPUT_BTN_RIGHT | NUTTYINPUT_BTN_A | NUTTYINPUT_BTN_B |
                    NUTTYINPUT_BTN_SELECT | NUTTYINPUT_BTN_START;
    while (NuttyInput_isOneOfTheButtonsCurrentlyPressed(mask)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Extra settle time for debounce */
    vTaskDelay(pdMS_TO_TICKS(30));

    /* Clear state again after settle */
    NuttyInput_clearButtonHoldState(
        NUTTYINPUT_BTN_UP | NUTTYINPUT_BTN_DOWN | NUTTYINPUT_BTN_LEFT |
        NUTTYINPUT_BTN_RIGHT | NUTTYINPUT_BTN_A | NUTTYINPUT_BTN_B |
        NUTTYINPUT_BTN_SELECT | NUTTYINPUT_BTN_START);
}

/* Each button uses NuttyInput's hold+release detection.
 * Returns true once per press-and-release cycle. */
bool uno_btn_up_pressed(void)    { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_UP); }
bool uno_btn_down_pressed(void)  { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_DOWN); }
bool uno_btn_left_pressed(void)  { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_LEFT); }
bool uno_btn_right_pressed(void) { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_RIGHT); }
bool uno_btn_play_pressed(void)  { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_A); }
bool uno_btn_draw_pressed(void)  { return NuttyInput_waitSingleButtonHoldAndReleasedNonBlock(NUTTYINPUT_BTN_B); }
bool uno_btn_back_pressed(void)  { return NuttyInput_waitSingleButtonHoldLongNonBlock(NUTTYINPUT_BTN_START); }

/* ── LED helpers ──────────────────────────────────────────────────── */

static void uno_led_rgb_set_bulb(uint8_t bulb, uno_color_t color) {
    switch (color) {
        case UNO_COLOR_RED:    NuttyRGB_SetRGBWithoutDisplay(bulb, 255, 0, 0);   break;
        case UNO_COLOR_GREEN:  NuttyRGB_SetRGBWithoutDisplay(bulb, 0, 255, 0);   break;
        case UNO_COLOR_BLUE:   NuttyRGB_SetRGBWithoutDisplay(bulb, 0, 0, 255);   break;
        case UNO_COLOR_YELLOW: NuttyRGB_SetRGBWithoutDisplay(bulb, 255, 200, 0); break;
        default:               NuttyRGB_SetRGBWithoutDisplay(bulb, 100, 100, 100); break;
    }
    NuttyRGB_DisplayNow();
}

void uno_led_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << UNO_LED_CUSTOM_GPIO),
    };
    gpio_config(&io_conf);
    gpio_set_level(UNO_LED_CUSTOM_GPIO, 0);
    NuttyRGB_SetAllRGBWithoutDisplay(0, 0, 0);
    NuttyRGB_DisplayNow();
}

void led_set_top_card(card_t card, uno_color_t active_color) {
    if (uno_card_is_none(card)) {
        NuttyRGB_SetRGBWithoutDisplay(0, 0, 0, 0);
    } else if (uno_card_is_wild(card)) {
        uno_led_rgb_set_bulb(0, active_color);
    } else {
        uno_led_rgb_set_bulb(0, (uno_color_t)card.color);
    }
}

void led_set_selected_card(card_t card) {
    if (uno_card_is_none(card)) {
        NuttyRGB_SetRGBWithoutDisplay(1, 0, 0, 0);
    } else {
        uno_led_rgb_set_bulb(1, (uno_color_t)card.color);
    }
}

/* ── Custom LED (GPIO) with mode-based blinking ───────────────────── */
static uint8_t g_custom_led_mode = UNO_LED_MODE_OFF;
static bool g_custom_led_level = false;
static int64_t g_custom_led_last_toggle = 0;

void set_custom_led(uint8_t mode) {
    if (mode == g_custom_led_mode) return;
    g_custom_led_mode = mode;
    g_custom_led_last_toggle = esp_timer_get_time();
    g_custom_led_level = (mode == UNO_LED_MODE_CONNECTED_SOLID);
    gpio_set_level(UNO_LED_CUSTOM_GPIO, g_custom_led_level ? 1 : 0);
}

void uno_custom_led_update(void) {
    int64_t now = esp_timer_get_time();
    if (g_custom_led_mode == UNO_LED_MODE_OFF) {
        if (g_custom_led_level) { g_custom_led_level = false; gpio_set_level(UNO_LED_CUSTOM_GPIO, 0); }
        return;
    }
    if (g_custom_led_mode == UNO_LED_MODE_CONNECTED_SOLID) {
        if (!g_custom_led_level) { g_custom_led_level = true; gpio_set_level(UNO_LED_CUSTOM_GPIO, 1); }
        return;
    }
    uint32_t period_ms = 0;
    if (g_custom_led_mode == UNO_LED_MODE_WILD_CHOICE_FLASH) period_ms = 120;
    else if (g_custom_led_mode == UNO_LED_MODE_TURN_BLINK)  period_ms = 400;
    else return;

    if ((now - g_custom_led_last_toggle) >= (int64_t)period_ms * 1000) {
        g_custom_led_last_toggle = now;
        g_custom_led_level = !g_custom_led_level;
        gpio_set_level(UNO_LED_CUSTOM_GPIO, g_custom_led_level ? 1 : 0);
    }
}
