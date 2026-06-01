#include "NuttyUNO.h"
#include "uno_common.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "services/NuttyRGB/NuttyRGB.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "NuttyUNO";

/* ── UI constants ─────────────────────────────────────────────────── */
#define UNO_CARD_W      20
#define UNO_CARD_H      28
#define UNO_CARD_SPACING 3

/* ── Color helpers for LVGL ───────────────────────────────────────── */
static lv_color_t uno_lv_color(uno_color_t c) {
    switch (c) {
        case UNO_COLOR_RED:    return lv_palette_main(LV_PALETTE_RED);
        case UNO_COLOR_GREEN:  return lv_palette_main(LV_PALETTE_GREEN);
        case UNO_COLOR_BLUE:   return lv_palette_main(LV_PALETTE_BLUE);
        case UNO_COLOR_YELLOW: return lv_palette_main(LV_PALETTE_YELLOW);
        default:               return lv_color_white();
    }
}

static const char *uno_color_name(uno_color_t c) {
    switch (c) {
        case UNO_COLOR_RED:    return "RED";
        case UNO_COLOR_GREEN:  return "GREEN";
        case UNO_COLOR_BLUE:   return "BLUE";
        case UNO_COLOR_YELLOW: return "YELLOW";
        default:               return "WILD";
    }
}

/* ── Card short text helper ───────────────────────────────────────── */
static void card_short_text(card_t card, char *out, size_t out_len) {
    const char *color = "W";
    char value[4] = "?";
    switch (card.color) {
        case UNO_COLOR_RED:    color = "R"; break;
        case UNO_COLOR_GREEN:  color = "G"; break;
        case UNO_COLOR_BLUE:   color = "B"; break;
        case UNO_COLOR_YELLOW: color = "Y"; break;
        default:               color = "W"; break;
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

/* ═══════════════════════════════════════════════════════════════════
 *  HOST SCREEN — Lobby with Create Room UI
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    lv_obj_t *root;
    lv_obj_t *title_label;
    lv_obj_t *status_label;
    lv_obj_t *bot_count_label;
    lv_obj_t *channel_label;
    lv_obj_t *players_label;
    lv_obj_t *instruction_label;
    lv_obj_t *card_icons[6];        /* decorative card icons */
    uint8_t bot_count;
    uint8_t connected_clients;
    bool in_game;
} uno_host_ui_t;

static uno_host_ui_t g_host_ui;

static void host_ui_draw_decorative_cards(lv_obj_t *parent) {
    /* Draw small colored card rectangles at the top as decoration */
    card_t sample_cards[] = {
        { .color = UNO_COLOR_RED,    .value = UNO_VALUE_5 },
        { .color = UNO_COLOR_BLUE,   .value = UNO_VALUE_SKIP },
        { .color = UNO_COLOR_GREEN,  .value = UNO_VALUE_2 },
        { .color = UNO_COLOR_YELLOW, .value = UNO_VALUE_REVERSE },
        { .color = UNO_COLOR_WILD,   .value = UNO_VALUE_WILD },
        { .color = UNO_COLOR_RED,    .value = UNO_VALUE_DRAW_TWO },
    };

    int x = 8;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, UNO_CARD_W, UNO_CARD_H);
        lv_obj_set_pos(card, x, 2);
        lv_obj_set_style_bg_color(card, uno_lv_color((uno_color_t)sample_cards[i].color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 3, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *lbl = lv_label_create(card);
        char txt[4];
        card_short_text(sample_cards[i], txt, sizeof(txt));
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(lbl);

        g_host_ui.card_icons[i] = card;
        x += UNO_CARD_W + UNO_CARD_SPACING;
    }
}

static void host_ui_init(void) {
    memset(&g_host_ui, 0, sizeof(g_host_ui));
    g_host_ui.bot_count = 1;  /* default 1 bot */

    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    g_host_ui.root = root;
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Decorative card strip at top */
    host_ui_draw_decorative_cards(root);

    /* Title */
    g_host_ui.title_label = lv_label_create(root);
    lv_label_set_text(g_host_ui.title_label, "UNO Host");
    lv_obj_set_pos(g_host_ui.title_label, 2, 34);
    lv_obj_set_style_text_font(g_host_ui.title_label, &lv_font_montserrat_12, LV_PART_MAIN);

    /* Channel info */
    g_host_ui.channel_label = lv_label_create(root);
    char ch_text[24];
    snprintf(ch_text, sizeof(ch_text), "Channel: %u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(g_host_ui.channel_label, ch_text);
    lv_obj_set_pos(g_host_ui.channel_label, 70, 34);
    lv_obj_set_style_text_font(g_host_ui.channel_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Bot count */
    g_host_ui.bot_count_label = lv_label_create(root);
    lv_obj_set_pos(g_host_ui.bot_count_label, 2, 48);
    lv_obj_set_style_text_font(g_host_ui.bot_count_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Connected clients status */
    g_host_ui.status_label = lv_label_create(root);
    lv_label_set_text(g_host_ui.status_label, "Waiting for clients...");
    lv_obj_set_pos(g_host_ui.status_label, 2, 62);
    lv_obj_set_style_text_font(g_host_ui.status_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Player list */
    g_host_ui.players_label = lv_label_create(root);
    lv_obj_set_pos(g_host_ui.players_label, 2, 78);
    lv_obj_set_style_text_font(g_host_ui.players_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    g_host_ui.instruction_label = lv_label_create(root);
    lv_label_set_text(g_host_ui.instruction_label, "UP/DOWN: Bots  A: Start");
    lv_obj_set_pos(g_host_ui.instruction_label, 2, 110);
    lv_obj_set_style_text_font(g_host_ui.instruction_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
    g_host_ui.in_game = false;
}

static void host_ui_update(void) {
    if (!g_host_ui.root) return;

    NuttyDisplay_lockLVGL();

    char bot_text[24];
    snprintf(bot_text, sizeof(bot_text), "Bots: %u", (unsigned)g_host_ui.bot_count);
    lv_label_set_text(g_host_ui.bot_count_label, bot_text);

    char players_text[80];
    uint8_t total = 1 + g_host_ui.bot_count + g_host_ui.connected_clients;
    snprintf(players_text, sizeof(players_text),
             "P0:Host  %s%sTotal:%u",
             g_host_ui.bot_count > 0 ? "Bots:" : "",
             g_host_ui.bot_count > 0 ? "" : "",
             (unsigned)total);
    /* Build a nicer player list */
    {
        size_t off = 0;
        off += snprintf(players_text + off, sizeof(players_text) - off, "P0:Host");
        if (g_host_ui.bot_count > 0) {
            off += snprintf(players_text + off, sizeof(players_text) - off, "  Bot:%u", (unsigned)g_host_ui.bot_count);
        }
        if (g_host_ui.connected_clients > 0) {
            off += snprintf(players_text + off, sizeof(players_text) - off, "  Link:%u", (unsigned)g_host_ui.connected_clients);
        }
        snprintf(players_text + off, sizeof(players_text) - off, "  Total:%u", (unsigned)total);
    }
    lv_label_set_text(g_host_ui.players_label, players_text);

    NuttyDisplay_unlockLVGL();
}

static void host_ui_set_connected(uint8_t count) {
    g_host_ui.connected_clients = count;
}

static void host_ui_set_bot_count(uint8_t count) {
    g_host_ui.bot_count = count;
}

static uint8_t host_ui_get_bot_count(void) {
    return g_host_ui.bot_count;
}

/* ═══════════════════════════════════════════════════════════════════
 *  CLIENT SCREEN — Join with Connect UI
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    CLIENT_STATE_IDLE,
    CLIENT_STATE_SCANNING,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_ERROR,
} client_ui_state_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *title_label;
    lv_obj_t *status_label;
    lv_obj_t *channel_label;
    lv_obj_t *icon_label;       /* Big icon/text for visual feedback */
    lv_obj_t *instruction_label;
    lv_obj_t *card_icons[6];
    client_ui_state_t state;
    bool needs_redraw;
} uno_client_ui_t;

static uno_client_ui_t g_client_ui;

static void client_ui_draw_decorative_cards(lv_obj_t *parent) {
    card_t sample_cards[] = {
        { .color = UNO_COLOR_BLUE,   .value = UNO_VALUE_3 },
        { .color = UNO_COLOR_RED,    .value = UNO_VALUE_7 },
        { .color = UNO_COLOR_YELLOW, .value = UNO_VALUE_SKIP },
        { .color = UNO_COLOR_GREEN,  .value = UNO_VALUE_0 },
        { .color = UNO_COLOR_WILD,   .value = UNO_VALUE_WILD_DRAW_FOUR },
        { .color = UNO_COLOR_BLUE,   .value = UNO_VALUE_DRAW_TWO },
    };
    int x = 8;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, UNO_CARD_W, UNO_CARD_H);
        lv_obj_set_pos(card, x, 2);
        lv_obj_set_style_bg_color(card, uno_lv_color((uno_color_t)sample_cards[i].color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 3, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *lbl = lv_label_create(card);
        char txt[4];
        card_short_text(sample_cards[i], txt, sizeof(txt));
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(lbl);

        g_client_ui.card_icons[i] = card;
        x += UNO_CARD_W + UNO_CARD_SPACING;
    }
}

static void client_ui_init(void) {
    memset(&g_client_ui, 0, sizeof(g_client_ui));
    g_client_ui.state = CLIENT_STATE_IDLE;
    g_client_ui.needs_redraw = true;

    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    g_client_ui.root = root;
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Decorative card strip */
    client_ui_draw_decorative_cards(root);

    /* Title */
    g_client_ui.title_label = lv_label_create(root);
    lv_label_set_text(g_client_ui.title_label, "UNO Join");
    lv_obj_set_pos(g_client_ui.title_label, 2, 34);
    lv_obj_set_style_text_font(g_client_ui.title_label, &lv_font_montserrat_12, LV_PART_MAIN);

    /* Channel */
    g_client_ui.channel_label = lv_label_create(root);
    char ch_text[24];
    snprintf(ch_text, sizeof(ch_text), "Channel: %u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(g_client_ui.channel_label, ch_text);
    lv_obj_set_pos(g_client_ui.channel_label, 70, 34);
    lv_obj_set_style_text_font(g_client_ui.channel_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Big status icon */
    g_client_ui.icon_label = lv_label_create(root);
    lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_WIFI);
    lv_obj_set_pos(g_client_ui.icon_label, 50, 55);
    lv_obj_set_style_text_font(g_client_ui.icon_label, &lv_font_montserrat_12, LV_PART_MAIN);

    /* Status text */
    g_client_ui.status_label = lv_label_create(root);
    lv_label_set_text(g_client_ui.status_label, "Press A to connect");
    lv_obj_set_pos(g_client_ui.status_label, 2, 80);
    lv_obj_set_style_text_font(g_client_ui.status_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Instructions */
    g_client_ui.instruction_label = lv_label_create(root);
    lv_label_set_text(g_client_ui.instruction_label, "A: Connect  START: Back");
    lv_obj_set_pos(g_client_ui.instruction_label, 2, 110);
    lv_obj_set_style_text_font(g_client_ui.instruction_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

static void client_ui_set_state(client_ui_state_t state) {
    if (g_client_ui.state != state) {
        g_client_ui.state = state;
        g_client_ui.needs_redraw = true;
    }
}

static void client_ui_update(void) {
    if (!g_client_ui.root || !g_client_ui.needs_redraw) return;

    NuttyDisplay_lockLVGL();

    switch (g_client_ui.state) {
        case CLIENT_STATE_IDLE:
            lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_WIFI);
            lv_label_set_text(g_client_ui.status_label, "Press A to connect");
            break;
        case CLIENT_STATE_SCANNING:
            lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_REFRESH);
            lv_label_set_text(g_client_ui.status_label, "Scanning for host...");
            break;
        case CLIENT_STATE_CONNECTING:
            lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_DOWNLOAD);
            lv_label_set_text(g_client_ui.status_label, "Connecting...");
            break;
        case CLIENT_STATE_CONNECTED:
            lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_OK);
            lv_label_set_text(g_client_ui.status_label, "Connected! Waiting...");
            break;
        case CLIENT_STATE_ERROR:
            lv_label_set_text(g_client_ui.icon_label, LV_SYMBOL_CLOSE);
            lv_label_set_text(g_client_ui.status_label, "Failed. Retry?");
            break;
    }

    NuttyDisplay_unlockLVGL();
    g_client_ui.needs_redraw = false;
}

/* ═══════════════════════════════════════════════════════════════════
 *  NUTTY UNO MENU — Main entry point with Host / Join submenu
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    MENU_SCREEN_MAIN,       /* Host / Join selection */
    MENU_SCREEN_HOST,       /* Host lobby */
    MENU_MENU_CLIENT,       /* Client connect screen */
} menu_screen_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *title_label;
    lv_obj_t *host_btn;
    lv_obj_t *host_label;
    lv_obj_t *join_btn;
    lv_obj_t *join_label;
    lv_obj_t *channel_label;
    lv_obj_t *card_icons[6];
    uint8_t selected;       /* 0 = Host, 1 = Join */
} uno_menu_ui_t;

static uno_menu_ui_t g_menu_ui;

static void menu_draw_decorative_cards(lv_obj_t *parent) {
    card_t sample_cards[] = {
        { .color = UNO_COLOR_RED,    .value = UNO_VALUE_0 },
        { .color = UNO_COLOR_BLUE,   .value = UNO_VALUE_5 },
        { .color = UNO_COLOR_GREEN,  .value = UNO_VALUE_3 },
        { .color = UNO_COLOR_YELLOW, .value = UNO_VALUE_9 },
        { .color = UNO_COLOR_WILD,   .value = UNO_VALUE_WILD },
        { .color = UNO_COLOR_RED,    .value = UNO_VALUE_DRAW_TWO },
    };
    int x = 8;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, UNO_CARD_W, UNO_CARD_H);
        lv_obj_set_pos(card, x, 2);
        lv_obj_set_style_bg_color(card, uno_lv_color((uno_color_t)sample_cards[i].color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 3, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *lbl = lv_label_create(card);
        char txt[4];
        card_short_text(sample_cards[i], txt, sizeof(txt));
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(lbl);

        g_menu_ui.card_icons[i] = card;
        x += UNO_CARD_W + UNO_CARD_SPACING;
    }
}

static void menu_ui_init(void) {
    memset(&g_menu_ui, 0, sizeof(g_menu_ui));
    g_menu_ui.selected = 0;

    lv_obj_t *root = NuttyDisplay_getUserAppArea();
    g_menu_ui.root = root;
    NuttyDisplay_lockLVGL();
    lv_obj_clean(root);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Decorative card strip */
    menu_draw_decorative_cards(root);

    /* Title */
    g_menu_ui.title_label = lv_label_create(root);
    lv_label_set_text(g_menu_ui.title_label, "NuttyUNO");
    lv_obj_set_pos(g_menu_ui.title_label, 2, 34);
    lv_obj_set_style_text_font(g_menu_ui.title_label, &lv_font_montserrat_12, LV_PART_MAIN);

    /* Channel info */
    g_menu_ui.channel_label = lv_label_create(root);
    char ch_text[24];
    snprintf(ch_text, sizeof(ch_text), "Ch: %u", (unsigned)UNO_GAME_CHANNEL);
    lv_label_set_text(g_menu_ui.channel_label, ch_text);
    lv_obj_set_pos(g_menu_ui.channel_label, 80, 34);
    lv_obj_set_style_text_font(g_menu_ui.channel_label, &cg_pixel_4x5_mono, LV_PART_MAIN);

    /* Host button */
    g_menu_ui.host_btn = lv_obj_create(root);
    lv_obj_set_size(g_menu_ui.host_btn, 110, 28);
    lv_obj_set_pos(g_menu_ui.host_btn, 10, 50);
    lv_obj_set_style_bg_color(g_menu_ui.host_btn, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_menu_ui.host_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_menu_ui.host_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_menu_ui.host_btn, 2, LV_PART_MAIN);

    g_menu_ui.host_label = lv_label_create(g_menu_ui.host_btn);
    lv_label_set_text(g_menu_ui.host_label, LV_SYMBOL_PLUS "  Host");
    lv_obj_set_style_text_color(g_menu_ui.host_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(g_menu_ui.host_label);

    /* Join button */
    g_menu_ui.join_btn = lv_obj_create(root);
    lv_obj_set_size(g_menu_ui.join_btn, 110, 28);
    lv_obj_set_pos(g_menu_ui.join_btn, 10, 84);
    lv_obj_set_style_bg_color(g_menu_ui.join_btn, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_menu_ui.join_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_menu_ui.join_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_menu_ui.join_btn, 2, LV_PART_MAIN);

    g_menu_ui.join_label = lv_label_create(g_menu_ui.join_btn);
    lv_label_set_text(g_menu_ui.join_label, LV_SYMBOL_DOWNLOAD "  Join");
    lv_obj_set_style_text_color(g_menu_ui.join_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(g_menu_ui.join_label);

    /* Instructions */
    lv_obj_t *instr = lv_label_create(root);
    lv_label_set_text(instr, "UP/DOWN: Select  A: OK");
    lv_obj_set_pos(instr, 10, 118);
    lv_obj_set_style_text_font(instr, &cg_pixel_4x5_mono, LV_PART_MAIN);

    NuttyDisplay_unlockLVGL();
}

static void menu_ui_update_selection(void) {
    if (!g_menu_ui.root) return;

    NuttyDisplay_lockLVGL();

    if (g_menu_ui.selected == 0) {
        lv_obj_set_style_border_color(g_menu_ui.host_btn, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(g_menu_ui.host_btn, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(g_menu_ui.join_btn, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(g_menu_ui.join_btn, 1, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_color(g_menu_ui.host_btn, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(g_menu_ui.host_btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(g_menu_ui.join_btn, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(g_menu_ui.join_btn, 3, LV_PART_MAIN);
    }

    NuttyDisplay_unlockLVGL();
}

/* ── Main menu loop ──────────────────────────────────────────────── */

static void uno_menu_main(void) {
    ESP_LOGI(TAG, "Starting NuttyUNO Menu");

    uno_led_init();
    uno_display_init();
    NuttyInput_clearButtonHoldState(NUTTYINPUT_BTN_ALL);

    menu_ui_init();
    menu_ui_update_selection();

    bool running = true;
    while (running) {
        /* Navigation */
        if (uno_btn_up_pressed()) {
            if (g_menu_ui.selected > 0) {
                g_menu_ui.selected--;
                menu_ui_update_selection();
            }
        }
        if (uno_btn_down_pressed()) {
            if (g_menu_ui.selected < 1) {
                g_menu_ui.selected++;
                menu_ui_update_selection();
            }
        }

        /* Select */
        if (uno_btn_play_pressed()) {
            if (g_menu_ui.selected == 0) {
                /* ── Launch Host ── */
                NuttyDisplay_lockLVGL();
                lv_obj_clean(g_menu_ui.root);
                NuttyDisplay_unlockLVGL();
                uno_led_init();

                /* Run the host app directly */
                NuttyApps_launchAppByEntry(uno_host_main);
                running = false;
                break;
            } else {
                /* ── Launch Client ── */
                NuttyDisplay_lockLVGL();
                lv_obj_clean(g_menu_ui.root);
                NuttyDisplay_unlockLVGL();
                uno_led_init();

                /* Run the client app directly */
                NuttyApps_launchAppByEntry(uno_client_main);
                running = false;
                break;
            }
        }

        /* Back */
        if (uno_btn_back_pressed()) {
            running = false;
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

/* Keep host/client definitions available for direct launch */
NuttyAppDefinition NuttyUNOHost = {
    .appName = "UNO Host",
    .appMainEntry = uno_host_main,
    .appHidden = true  /* Hidden from main menu, accessed via NuttyUNO submenu */
};

NuttyAppDefinition NuttyUNOClient = {
    .appName = "UNO Client",
    .appMainEntry = uno_client_main,
    .appHidden = true  /* Hidden from main menu, accessed via NuttyUNO submenu */
};
