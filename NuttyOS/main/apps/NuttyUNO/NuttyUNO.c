#include "NuttyUNO.h"
#include "uno_common.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "services/NuttyRGB/NuttyRGB.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "NuttyUNO";

/*
 * Display: 128 x 59 pixels
 * Font: cg_pixel_4x5_mono = 4w x 5h per char
 * System tray takes top ~5px, app area starts at y=5
 *
 * Layout strategy: use tiny 4x5 font for everything.
 * Each text line is 5px tall. We get ~11 lines in 59px.
 */

/* ── Card label helper ────────────────────────────────────────────── */
static void card_label(card_t card, char *out, size_t out_len) {
    const char *c = "W";
    switch (card.color) {
        case UNO_COLOR_RED:    c = "R"; break;
        case UNO_COLOR_GREEN:  c = "G"; break;
        case UNO_COLOR_BLUE:   c = "B"; break;
        case UNO_COLOR_YELLOW: c = "Y"; break;
        default:               c = "W"; break;
    }
    if (card.value <= 9) {
        snprintf(out, out_len, "%s%u", c, (unsigned)card.value);
    } else if (card.value == UNO_VALUE_SKIP) {
        snprintf(out, out_len, "%sS", c);
    } else if (card.value == UNO_VALUE_REVERSE) {
        snprintf(out, out_len, "%sR", c);
    } else if (card.value == UNO_VALUE_DRAW_TWO) {
        snprintf(out, out_len, "%s+", c);
    } else if (card.value == UNO_VALUE_WILD) {
        snprintf(out, out_len, "W");
    } else {
        snprintf(out, out_len, "W+");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN MENU — Host / Join selection
 * ═══════════════════════════════════════════════════════════════════ */

static void menu_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Title line */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "NuttyUNO");
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    /* Channel */
    lv_obj_t *ch = lv_label_create(root);
    char chbuf[16];
    snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(ch, chbuf);
    lv_obj_set_pos(ch, 80, 0);
    lv_obj_set_style_text_font(ch, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Host option */
    lv_obj_t *host = lv_label_create(root);
    lv_label_set_text(host, "> Host");
    lv_obj_set_pos(host, 4, 14);
    lv_obj_set_style_text_font(host, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Join option */
    lv_obj_t *join = lv_label_create(root);
    lv_label_set_text(join, "  Join");
    lv_obj_set_pos(join, 4, 22);
    lv_obj_set_style_text_font(join, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "UP/DN:SEL  A:OK");
    lv_obj_set_pos(instr, 4, 52);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

static void menu_update_sel(uint8_t sel) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();

    lv_obj_t *host = lv_obj_get_child(root, 3);
    lv_label_set_text(host, sel == 0 ? "> Host" : "  Host");

    lv_obj_t *join = lv_obj_get_child(root, 4);
    lv_label_set_text(join, sel == 1 ? "> Join" : "  Join");

    NuttyDisplay_unlockLVGL();
}

/* ═══════════════════════════════════════════════════════════════════
 *  HOST LOBBY — Bot count, start game
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t g_bot_count = 1;

static void host_lobby_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "UNO Host");
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    /* Channel */
    lv_obj_t *ch = lv_label_create(root);
    char chbuf[16];
    snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(ch, chbuf);
    lv_obj_set_pos(ch, 80, 0);
    lv_obj_set_style_text_font(ch, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Bot count */
    lv_obj_t *bot = lv_label_create(root);
    char botbuf[20];
    snprintf(botbuf, sizeof(botbuf), "Bots: %u", (unsigned)g_bot_count);
    lv_label_set_text(bot, botbuf);
    lv_obj_set_pos(bot, 4, 14);
    lv_obj_set_style_text_font(bot, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Total players */
    lv_obj_t *tot = lv_label_create(root);
    char totbuf[20];
    snprintf(totbuf, sizeof(totbuf), "Total: %u", (unsigned)(1 + g_bot_count));
    lv_label_set_text(tot, totbuf);
    lv_obj_set_pos(tot, 4, 22);
    lv_obj_set_style_text_font(tot, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Status */
    lv_obj_t *st = lv_label_create(root);
    lv_label_set_text(st, "Waiting...");
    lv_obj_set_pos(st, 4, 34);
    lv_obj_set_style_text_font(st, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "UP/DN:Bot  A:Start");
    lv_obj_set_pos(instr, 4, 52);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

/* ═══════════════════════════════════════════════════════════════════
 *  CLIENT CONNECT — Scan / connect status
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    CSTATE_IDLE,
    CSTATE_SCANNING,
    CSTATE_CONNECTED,
} cstate_t;

static cstate_t g_cstate = CSTATE_IDLE;

static void client_connect_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "UNO Join");
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    /* Channel */
    lv_obj_t *ch = lv_label_create(root);
    char chbuf[16];
    snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(ch, chbuf);
    lv_obj_set_pos(ch, 80, 0);
    lv_obj_set_style_text_font(ch, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Status */
    lv_obj_t *st = lv_label_create(root);
    const char *stxt = "Press A";
    if (g_cstate == CSTATE_SCANNING) stxt = "Scanning...";
    if (g_cstate == CSTATE_CONNECTED) stxt = "Connected!";
    lv_label_set_text(st, stxt);
    lv_obj_set_pos(st, 4, 20);
    lv_obj_set_style_text_font(st, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "A:Connect  B:Back");
    lv_obj_set_pos(instr, 4, 52);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN ENTRY — Menu loop
 * ═══════════════════════════════════════════════════════════════════ */

static void uno_menu_main(void) {
    ESP_LOGI(TAG, "Starting NuttyUNO Menu");

    uno_led_init();
    uno_display_init();
    NuttyInput_clearButtonHoldState(NUTTYINPUT_BTN_ALL);

    /* ── Main menu: Host / Join ── */
    uint8_t sel = 0;
    menu_draw();
    menu_update_sel(sel);

    bool in_menu = true;
    while (in_menu) {
        if (uno_btn_up_pressed()) {
            if (sel > 0) { sel--; menu_update_sel(sel); }
        }
        if (uno_btn_down_pressed()) {
            if (sel < 1) { sel++; menu_update_sel(sel); }
        }
        if (uno_btn_play_pressed()) {
            if (sel == 0) {
                /* ── HOST path ── */
                g_bot_count = 1;
                host_lobby_draw();

                bool in_lobby = true;
                while (in_lobby) {
                    if (uno_btn_up_pressed()) {
                        if (g_bot_count < 3) {
                            g_bot_count++;
                            lv_obj_t *root = NuttyDisplay_getUserAppArea();
                            NuttyDisplay_lockLVGL();
                            lv_obj_t *bot = lv_obj_get_child(root, 3);
                            char botbuf[20];
                            snprintf(botbuf, sizeof(botbuf), "Bots: %u", (unsigned)g_bot_count);
                            lv_label_set_text(bot, botbuf);
                            lv_obj_t *tot = lv_obj_get_child(root, 4);
                            snprintf(botbuf, sizeof(botbuf), "Total: %u", (unsigned)(1 + g_bot_count));
                            lv_label_set_text(tot, botbuf);
                            NuttyDisplay_unlockLVGL();
                        }
                    }
                    if (uno_btn_down_pressed()) {
                        if (g_bot_count > 0) {
                            g_bot_count--;
                            lv_obj_t *root = NuttyDisplay_getUserAppArea();
                            NuttyDisplay_lockLVGL();
                            lv_obj_t *bot = lv_obj_get_child(root, 3);
                            char botbuf[20];
                            snprintf(botbuf, sizeof(botbuf), "Bots: %u", (unsigned)g_bot_count);
                            lv_label_set_text(bot, botbuf);
                            lv_obj_t *tot = lv_obj_get_child(root, 4);
                            snprintf(botbuf, sizeof(botbuf), "Total: %u", (unsigned)(1 + g_bot_count));
                            lv_label_set_text(tot, botbuf);
                            NuttyDisplay_unlockLVGL();
                        }
                    }
                    if (uno_btn_play_pressed()) {
                        in_lobby = false;
                        in_menu = false;
                        uno_display_clear();
                        uno_host_main();
                        return;
                    }
                    if (uno_btn_draw_pressed() || uno_btn_back_pressed()) {
                        in_lobby = false;
                        menu_draw();
                        menu_update_sel(sel);
                    }
                    uno_custom_led_update();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            } else {
                /* ── CLIENT path ── */
                g_cstate = CSTATE_IDLE;
                client_connect_draw();

                bool in_client = true;
                while (in_client) {
                    if (uno_btn_play_pressed()) {
                        g_cstate = CSTATE_SCANNING;
                        client_connect_draw();
                        in_client = false;
                        in_menu = false;
                        uno_display_clear();
                        uno_client_main();
                        return;
                    }
                    if (uno_btn_draw_pressed() || uno_btn_back_pressed()) {
                        in_client = false;
                        menu_draw();
                        menu_update_sel(sel);
                    }
                    uno_custom_led_update();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
        }
        if (uno_btn_back_pressed()) {
            in_menu = false;
        }
        uno_custom_led_update();
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uno_display_clear();
    NuttyApps_launchAppByIndex(0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  App Definitions
 * ═══════════════════════════════════════════════════════════════════ */

NuttyAppDefinition NuttyUNO = {
    .appName = "NuttyUNO",
    .appMainEntry = uno_menu_main,
    .appHidden = false
};

NuttyAppDefinition NuttyUNOHost = {
    .appName = "UNO Host",
    .appMainEntry = uno_host_main,
    .appHidden = true
};

NuttyAppDefinition NuttyUNOClient = {
    .appName = "UNO Client",
    .appMainEntry = uno_client_main,
    .appHidden = true
};
