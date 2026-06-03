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

/* Display: 128 x 59 pixels. Font: cg_pixel_4x5_mono = 4w x 5h per char */

/* ── Shared state ─────────────────────────────────────────────────── */
uint8_t g_game_channel = UNO_GAME_CHANNEL;

/* ═══════════════════════════════════════════════════════════════════
 *  TUTORIAL — explanation of UNO rules & controls
 * ═══════════════════════════════════════════════════════════════════ */

#define TUTORIAL_LINES 32

static const char *tutorial_text[TUTORIAL_LINES] = {
    "=== NUTTY UNO ===",
    "",
    "GOAL: Empty your hand!",
    "Last card wins the game.",
    "",
    "CARD DISPLAY:",
    " 0-9 = Number cards",
    "  -  = Skip turn",
    "  R  = Reverse dir",
    " +2  = Next draws 2",
    "  W  = Wild (pick color)",
    " WW  = Wild + draw 4",
    "",
    "LED = Active color",
    "Top LED = top card clr",
    "Bot LED = selected card",
    "Wild LED = chosen color",
    "",
    "CONTROLS (Game):",
    " </> = Scroll hand",
    "  A  = Play selected",
    "  B  = Draw a card",
    "START= Leave game",
    "",
    "CONTROLS (Lobby):",
    " L/R = Change channel",
    " U/D = Bot count",
    "  A  = Start game",
    "",
    "RULES:",
    " Match color OR number",
    " Wild plays anytime",
    "+2/WW stack (house rule)",
    "",
    "U/D:Scroll START:Back"
};

#define TUTORIAL_LINE_H 5
#define TUTORIAL_VISIBLE_LINES 12

static int tutorial_scroll = 0;
static lv_obj_t *tutorial_lbls[TUTORIAL_LINES];

static void tutorial_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    if (root == NULL) return;
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    for (int i = 0; i < TUTORIAL_LINES; i++) {
        tutorial_lbls[i] = lv_label_create(root);
        lv_label_set_text(tutorial_lbls[i], tutorial_text[i]);
        lv_obj_set_pos(tutorial_lbls[i], 2, i * TUTORIAL_LINE_H);
        lv_obj_set_style_text_font(tutorial_lbls[i], &cg_pixel_4x5_mono, LV_PART_MAIN);
    }
    NuttyDisplay_unlockLVGL();
}

static void tutorial_update_scroll(void) {
    int max_scroll = TUTORIAL_LINES - TUTORIAL_VISIBLE_LINES;
    if (max_scroll < 0) max_scroll = 0;
    if (tutorial_scroll > max_scroll) tutorial_scroll = max_scroll;
    if (tutorial_scroll < 0) tutorial_scroll = 0;

    NuttyDisplay_lockLVGL();
    for (int i = 0; i < TUTORIAL_LINES; i++) {
        int y = (i - tutorial_scroll) * TUTORIAL_LINE_H;
        lv_obj_set_pos(tutorial_lbls[i], 2, y);
        if (y < 0 || y >= TUTORIAL_VISIBLE_LINES * TUTORIAL_LINE_H) {
            lv_obj_add_flag(tutorial_lbls[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(tutorial_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    NuttyDisplay_unlockLVGL();
}

static void tutorial_main(void) {
    ESP_LOGI(TAG, "Starting UNO Tutorial");
    uno_btn_init();
    tutorial_scroll = 0;
    tutorial_draw();
    tutorial_update_scroll();

    while (1) {
        if (uno_btn_back_pressed()) break;
        if (uno_btn_up_pressed()) {
            if (tutorial_scroll > 0) {
                tutorial_scroll--;
                tutorial_update_scroll();
            }
        }
        if (uno_btn_down_pressed()) {
            int max_scroll = TUTORIAL_LINES - TUTORIAL_VISIBLE_LINES;
            if (max_scroll > 0 && tutorial_scroll < max_scroll) {
                tutorial_scroll++;
                tutorial_update_scroll();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uno_display_clear();
    NuttyApps_launchAppByIndex(0);
}

NuttyAppDefinition NuttyUNOTutorial = {
    .appName = "UNO Tutorial",
    .appMainEntry = tutorial_main,
    .appHidden = true
};

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN MENU — Channel select + Host / Join / Tutorial
 * ═══════════════════════════════════════════════════════════════════ */

static lv_obj_t *menu_host_lbl;
static lv_obj_t *menu_join_lbl;
static lv_obj_t *menu_tut_lbl;
static lv_obj_t *menu_ch_lbl;

typedef enum { MENU_HOST, MENU_JOIN, MENU_TUTORIAL, MENU_COUNT } menu_sel_t;

static void menu_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    if (root == NULL) {
        ESP_LOGW(TAG, "Menu draw: display root is NULL");
        return;
    }
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "NuttyUNO");
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    /* Channel */
    menu_ch_lbl = lv_label_create(root);
    char chbuf[16];
    snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)g_game_channel);
    lv_label_set_text(menu_ch_lbl, chbuf);
    lv_obj_set_pos(menu_ch_lbl, 70, 0);
    lv_obj_set_style_text_font(menu_ch_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Menu options */
    menu_host_lbl = lv_label_create(root);
    lv_label_set_text(menu_host_lbl, "> Host");
    lv_obj_set_pos(menu_host_lbl, 4, 14);
    lv_obj_set_style_text_font(menu_host_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    menu_join_lbl = lv_label_create(root);
    lv_label_set_text(menu_join_lbl, "  Join");
    lv_obj_set_pos(menu_join_lbl, 4, 22);
    lv_obj_set_style_text_font(menu_join_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    menu_tut_lbl = lv_label_create(root);
    lv_label_set_text(menu_tut_lbl, "  Tutorial");
    lv_obj_set_pos(menu_tut_lbl, 4, 30);
    lv_obj_set_style_text_font(menu_tut_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Button help */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "L/R:Ch U/D:Sel A:OK");
    lv_obj_set_pos(instr, 4, 52);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

static void menu_update_selection(menu_sel_t sel) {
    NuttyDisplay_lockLVGL();
    lv_label_set_text(menu_host_lbl, (sel == MENU_HOST)     ? "> Host"     : "  Host");
    lv_label_set_text(menu_join_lbl, (sel == MENU_JOIN)     ? "> Join"     : "  Join");
    lv_label_set_text(menu_tut_lbl,  (sel == MENU_TUTORIAL) ? "> Tutorial" : "  Tutorial");
    NuttyDisplay_unlockLVGL();
}

/* ═══════════════════════════════════════════════════════════════════
 *  CLIENT CONNECT — Channel select + scan / connect status
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { CSTATE_IDLE, CSTATE_SCANNING, CSTATE_CONNECTED } cstate_t;
static cstate_t g_cstate = CSTATE_IDLE;
static lv_obj_t *client_st_lbl;
static lv_obj_t *client_ch_lbl;
static lv_obj_t *client_host_lbl;

static void client_connect_draw(void) {
    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    if (root == NULL) {
        ESP_LOGW(TAG, "Client connect draw: display root is NULL");
        return;
    }
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "UNO Join");
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    /* Channel — adjustable with L/R */
    client_ch_lbl = lv_label_create(root);
    char buf[16];
    snprintf(buf, sizeof(buf), "Ch:%u", (unsigned)g_game_channel);
    lv_label_set_text(client_ch_lbl, buf);
    lv_obj_set_pos(client_ch_lbl, 80, 0);
    lv_obj_set_style_text_font(client_ch_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Host name being scanned */
    client_host_lbl = lv_label_create(root);
    snprintf(buf, sizeof(buf), "UNO_%u", (unsigned)g_game_channel);
    lv_label_set_text(client_host_lbl, buf);
    lv_obj_set_pos(client_host_lbl, 4, 16);
    lv_obj_set_style_text_font(client_host_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Status */
    client_st_lbl = lv_label_create(root);
    lv_label_set_text(client_st_lbl, "Press A scan");
    lv_obj_set_pos(client_st_lbl, 4, 26);
    lv_obj_set_style_text_font(client_st_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "L/R:Ch  A:Scan  B:Back");
    lv_obj_set_pos(instr, 4, 52);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

static void client_connect_update_ch(void) {
    char buf[16];
    NuttyDisplay_lockLVGL();
    snprintf(buf, sizeof(buf), "Ch:%u", (unsigned)g_game_channel);
    lv_label_set_text(client_ch_lbl, buf);
    snprintf(buf, sizeof(buf), "UNO_%u", (unsigned)g_game_channel);
    lv_label_set_text(client_host_lbl, buf);
    NuttyDisplay_unlockLVGL();
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN ENTRY — Menu loop
 * ═══════════════════════════════════════════════════════════════════ */

static void uno_menu_main(void) {
    ESP_LOGI(TAG, "Starting NuttyUNO Menu");

    uno_led_init();
    uno_display_init();
    uno_btn_init();

    menu_sel_t sel = MENU_HOST;
    menu_draw();

    bool in_menu = true;
    while (in_menu) {
        if (uno_btn_left_pressed()) {
            if (g_game_channel > 0) {
                g_game_channel--;
                NuttyDisplay_lockLVGL();
                char chbuf[16];
                snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)g_game_channel);
                lv_label_set_text(menu_ch_lbl, chbuf);
                NuttyDisplay_unlockLVGL();
            }
        }
        if (uno_btn_right_pressed()) {
            if (g_game_channel < 255) {
                g_game_channel++;
                NuttyDisplay_lockLVGL();
                char chbuf[16];
                snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)g_game_channel);
                lv_label_set_text(menu_ch_lbl, chbuf);
                NuttyDisplay_unlockLVGL();
            }
        }
        if (uno_btn_up_pressed()) {
            if (sel > 0) {
                sel--;
                menu_update_selection(sel);
            }
        }
        if (uno_btn_down_pressed()) {
            if (sel < MENU_COUNT - 1) {
                sel++;
                menu_update_selection(sel);
            }
        }
        if (uno_btn_play_pressed()) {
            if (sel == MENU_HOST) {
                /* ── HOST path ── */
                uno_set_requested_bots(1);
                in_menu = false;
                uno_host_main();
                return;
            } else if (sel == MENU_JOIN) {
                /* ── CLIENT path ── */
                g_cstate = CSTATE_IDLE;
                client_connect_draw();

                bool in_client = true;
                while (in_client) {
                    if (uno_btn_left_pressed()) {
                        if (g_game_channel > 0) {
                            g_game_channel--;
                            client_connect_update_ch();
                        }
                    }
                    if (uno_btn_right_pressed()) {
                        if (g_game_channel < 255) {
                            g_game_channel++;
                            client_connect_update_ch();
                        }
                    }
                    if (uno_btn_play_pressed()) {
                        g_cstate = CSTATE_SCANNING;
                        NuttyDisplay_lockLVGL();
                        lv_label_set_text(client_st_lbl, "Scanning...");
                        NuttyDisplay_unlockLVGL();
                        in_client = false;
                        in_menu = false;
                        uno_client_main();
                        return;
                    }
                    if (uno_btn_draw_pressed() || uno_btn_back_pressed()) {
                        in_client = false;
                        menu_draw();
                    }
                    uno_custom_led_update();
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else if (sel == MENU_TUTORIAL) {
                /* ── TUTORIAL path ── */
                in_menu = false;
                tutorial_main();
                return;
            }
        }
        if (uno_btn_back_pressed()) {
            in_menu = false;
        }
        uno_custom_led_update();
        vTaskDelay(pdMS_TO_TICKS(10));
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
