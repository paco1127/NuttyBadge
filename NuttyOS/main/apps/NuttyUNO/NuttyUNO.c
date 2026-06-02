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
 *  MAIN MENU — Channel select + Host / Join
 * ═══════════════════════════════════════════════════════════════════ */

static lv_obj_t *menu_host_lbl;
static lv_obj_t *menu_join_lbl;
static lv_obj_t *menu_ch_lbl;

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

    /* Host / Join options */
    menu_host_lbl = lv_label_create(root);
    lv_label_set_text(menu_host_lbl, "> Host");
    lv_obj_set_pos(menu_host_lbl, 4, 14);
    lv_obj_set_style_text_font(menu_host_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    menu_join_lbl = lv_label_create(root);
    lv_label_set_text(menu_join_lbl, "  Join");
    lv_obj_set_pos(menu_join_lbl, 4, 22);
    lv_obj_set_style_text_font(menu_join_lbl, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Card legend / tutorial */
    lv_obj_t *leg1 = lv_label_create(root);
    lv_label_set_text(leg1, "Cards:R0G7B2Y9");
    lv_obj_set_pos(leg1, 4, 32);
    lv_obj_set_style_text_font(leg1, &cg_pixel_4x5_mono, LV_PART_MAIN);

    lv_obj_t *leg2 = lv_label_create(root);
    lv_label_set_text(leg2, "S=Skip R=Rev +2=D2");
    lv_obj_set_pos(leg2, 4, 40);
    lv_obj_set_style_text_font(leg2, &cg_pixel_4x5_mono, LV_PART_MAIN);

    lv_obj_t *leg3 = lv_label_create(root);
    lv_label_set_text(leg3, "W=Wild +4=W+4");
    lv_obj_set_pos(leg3, 4, 48);
    lv_obj_set_style_text_font(leg3, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Button help */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "L/R:Ch U/D:Sel A:OK");
    lv_obj_set_pos(instr, 4, 56);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

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

    uint8_t sel = 0;  /* 0=Host, 1=Join */
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
                NuttyDisplay_lockLVGL();
                lv_label_set_text(menu_host_lbl, "> Host");
                lv_label_set_text(menu_join_lbl, "  Join");
                NuttyDisplay_unlockLVGL();
            }
        }
        if (uno_btn_down_pressed()) {
            if (sel < 1) {
                sel++;
                NuttyDisplay_lockLVGL();
                lv_label_set_text(menu_host_lbl, "  Host");
                lv_label_set_text(menu_join_lbl, "> Join");
                NuttyDisplay_unlockLVGL();
            }
        }
        if (uno_btn_play_pressed()) {
            if (sel == 0) {
                /* ── HOST path ── */
                uno_set_requested_bots(1);
                in_menu = false;
                uno_host_main();
                return;
            } else {
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
