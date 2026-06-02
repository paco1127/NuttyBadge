#include "apps/NuttyUNO/NuttyUNO.h"
#include "apps/NuttyUNO/uno_common.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#define UNO_HAND_VISIBLE 6
#define UNO_CARD_W 18
#define UNO_CARD_H 16
#define UNO_CARD_SPACING 2
#define UNO_ACTION_QUEUE_LEN 8
#define UNO_INVALID_CONN_HANDLE 0xFFFF

static const char *TAG = "UNOHost";

typedef struct {
    bool active;
    bool connected;
    bool is_bot;
    uint16_t conn_handle;
    uint8_t hand_count;
    card_t hand[UNO_MAX_HAND];
    char name[UNO_NAME_MAX];
} uno_player_t;

typedef struct {
    bool active;
    uint8_t player_id;
    uint8_t hand_index;
    card_t card;
} uno_pending_wild_t;

typedef struct {
    card_t deck[UNO_DECK_SIZE];
    uint8_t deck_count;
    card_t discard[UNO_DECK_SIZE];
    uint8_t discard_count;
    uno_player_t players[UNO_MAX_PLAYERS];
    uint8_t player_count;
    uint8_t current_player;
    int8_t direction;
    uno_color_t active_color;
    uint8_t pending_draw;
    uint8_t state_version;
    bool game_started;
    bool game_over;
    uint8_t requested_bots;
    char last_action[UNO_LAST_ACTION_MAX];
    uno_pending_wild_t pending_wild;
} uno_host_game_t;

typedef struct {
    bool initialized;
    void *root;
    uno_display_label_t title_label;
    uno_display_label_t deck_label;
    uno_display_label_t players_label;
    uno_display_label_t log_label;
    uno_display_label_t wild_label;
    uno_display_label_t left_arrow;
    uno_display_label_t right_arrow;
    uno_display_card_t top_card;
    uno_display_card_t hand_cards[UNO_HAND_VISIBLE];
    uint8_t selected_index;
    uint8_t scroll_offset;
    bool wild_select_active;
    uint8_t wild_color;
} uno_ui_t;

static uno_host_game_t g_game;
static uno_ui_t g_ui;
static bool g_ui_dirty = false;
static bool g_state_dirty = false;

static StaticQueue_t g_action_queue_struct;
static uint8_t g_action_queue_storage[UNO_ACTION_QUEUE_LEN * sizeof(uno_msg_action_t)];
static QueueHandle_t g_action_queue = NULL;

static const char *g_custom_player_names[UNO_MAX_PLAYERS] = { NULL, NULL, NULL, NULL };

#ifdef CONFIG_BT_ENABLED
static ble_uuid128_t g_uno_svc_uuid;
static ble_uuid128_t g_uno_state_uuid;
static ble_uuid128_t g_uno_cmd_uuid;
static uint16_t g_uno_state_handle = 0;
static uint16_t g_uno_cmd_handle = 0;
static uint8_t g_own_addr_type;

static void uno_ble_advertise(void);
#endif

static void uno_set_last_action(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_game.last_action, sizeof(g_game.last_action), fmt, args);
    va_end(args);
    g_ui_dirty = true;
}

static void uno_set_player_name(uint8_t index, const char *name) {
    if (index >= UNO_MAX_PLAYERS) {
        return;
    }

    const char *custom = g_custom_player_names[index];
    const char *source = custom ? custom : name;
    if (source == NULL) {
        snprintf(g_game.players[index].name, sizeof(g_game.players[index].name), "Player %u", (unsigned)index);
    } else {
        snprintf(g_game.players[index].name, sizeof(g_game.players[index].name), "%s", source);
    }
}

static void uno_reset_players(void) {
    memset(&g_game.players, 0, sizeof(g_game.players));
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        g_game.players[i].conn_handle = UNO_INVALID_CONN_HANDLE;
        g_game.players[i].active = false;
        g_game.players[i].connected = false;
        g_game.players[i].is_bot = false;
        g_game.players[i].hand_count = 0;
        uno_set_player_name(i, NULL);
    }

    g_game.players[0].active = true;
    g_game.players[0].connected = true;
    g_game.players[0].is_bot = false;
}

static uint8_t uno_count_active_players(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        if (g_game.players[i].active) {
            count++;
        }
    }
    return count;
}

static uint8_t uno_next_active_player(uint8_t start, int8_t direction) {
    uint8_t idx = start;
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        idx = (uint8_t)((idx + (direction > 0 ? 1 : (UNO_MAX_PLAYERS - 1))) % UNO_MAX_PLAYERS);
        if (g_game.players[idx].active) {
            return idx;
        }
    }
    return start;
}

static void uno_build_deck(void) {
    uint16_t idx = 0;

    for (uint8_t color = 0; color < 4; color++) {
        g_game.deck[idx++] = (card_t){ .color = color, .value = UNO_VALUE_0 };
        for (uint8_t value = UNO_VALUE_1; value <= UNO_VALUE_9; value++) {
            g_game.deck[idx++] = (card_t){ .color = color, .value = value };
            g_game.deck[idx++] = (card_t){ .color = color, .value = value };
        }
        for (uint8_t value = UNO_VALUE_SKIP; value <= UNO_VALUE_DRAW_TWO; value++) {
            g_game.deck[idx++] = (card_t){ .color = color, .value = value };
            g_game.deck[idx++] = (card_t){ .color = color, .value = value };
        }
    }

    for (uint8_t i = 0; i < 4; i++) {
        g_game.deck[idx++] = (card_t){ .color = UNO_COLOR_WILD, .value = UNO_VALUE_WILD };
        g_game.deck[idx++] = (card_t){ .color = UNO_COLOR_WILD, .value = UNO_VALUE_WILD_DRAW_FOUR };
    }

    g_game.deck_count = (uint8_t)idx;
}

static void uno_shuffle_deck(card_t *deck, uint8_t count) {
    if (count == 0) {
        return;
    }

    for (uint8_t i = count - 1; i > 0; i--) {
        uint32_t r = esp_random();
        uint8_t j = (uint8_t)(r % (i + 1));
        card_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

static bool uno_refill_deck(void) {
    if (g_game.discard_count <= 1) {
        return false;
    }

    uint8_t new_count = (uint8_t)(g_game.discard_count - 1);
    for (uint8_t i = 0; i < new_count; i++) {
        g_game.deck[i] = g_game.discard[i];
    }
    g_game.deck_count = new_count;

    card_t top = g_game.discard[g_game.discard_count - 1];
    g_game.discard[0] = top;
    g_game.discard_count = 1;

    uno_shuffle_deck(g_game.deck, g_game.deck_count);
    return true;
}

static card_t uno_draw_card(void) {
    if (g_game.deck_count == 0) {
        if (!uno_refill_deck()) {
            return uno_card_none();
        }
    }

    card_t card = g_game.deck[g_game.deck_count - 1];
    g_game.deck_count--;
    return card;
}

static void uno_discard(card_t card) {
    if (g_game.discard_count < UNO_DECK_SIZE) {
        g_game.discard[g_game.discard_count++] = card;
    }
}

static card_t uno_top_card(void) {
    if (g_game.discard_count == 0) {
        return uno_card_none();
    }
    return g_game.discard[g_game.discard_count - 1];
}

static bool uno_is_playable(card_t card) {
    if (g_game.pending_draw > 0) {
        return (card.value == UNO_VALUE_DRAW_TWO || card.value == UNO_VALUE_WILD_DRAW_FOUR);
    }

    if (uno_card_is_wild(card)) {
        return true;
    }

    card_t top = uno_top_card();
    if (card.color == g_game.active_color) {
        return true;
    }
    if (!uno_card_is_none(top) && card.value == top.value) {
        return true;
    }

    return false;
}

static void uno_advance_turn(uint8_t steps) {
    if (g_game.player_count <= 1) {
        return;
    }

    uint8_t next = g_game.current_player;
    for (uint8_t i = 0; i < steps; i++) {
        next = uno_next_active_player(next, g_game.direction);
    }
    g_game.current_player = next;
}

static void uno_draw_cards(uint8_t player_id, uint8_t count) {
    if (player_id >= UNO_MAX_PLAYERS) {
        return;
    }

    uno_player_t *player = &g_game.players[player_id];
    for (uint8_t i = 0; i < count; i++) {
        if (player->hand_count >= UNO_MAX_HAND) {
            break;
        }
        card_t card = uno_draw_card();
        if (uno_card_is_none(card)) {
            break;
        }
        player->hand[player->hand_count++] = card;
    }
    g_state_dirty = true;
}

static void uno_apply_card_effect(card_t card) {
    if (card.value == UNO_VALUE_SKIP) {
        uno_advance_turn(2);
    } else if (card.value == UNO_VALUE_REVERSE) {
        g_game.direction = (int8_t)-g_game.direction;
        if (g_game.player_count == 2) {
            uno_advance_turn(2);
        } else {
            uno_advance_turn(1);
        }
    } else if (card.value == UNO_VALUE_DRAW_TWO) {
        g_game.pending_draw = (uint8_t)(g_game.pending_draw + 2);
        uno_advance_turn(1);
    } else if (card.value == UNO_VALUE_WILD_DRAW_FOUR) {
        g_game.pending_draw = (uint8_t)(g_game.pending_draw + 4);
        uno_advance_turn(1);
    } else {
        uno_advance_turn(1);
    }
}

static void uno_play_card(uint8_t player_id, uint8_t hand_index, uint8_t wild_color) {
    if (player_id >= UNO_MAX_PLAYERS) {
        return;
    }

    uno_player_t *player = &g_game.players[player_id];
    if (hand_index >= player->hand_count) {
        return;
    }

    card_t card = player->hand[hand_index];
    if (!uno_is_playable(card)) {
        uno_set_last_action("Invalid play");
        return;
    }

    for (uint8_t i = hand_index; i + 1 < player->hand_count; i++) {
        player->hand[i] = player->hand[i + 1];
    }
    if (player->hand_count > 0) {
        player->hand_count--;
    }

    if (player_id == 0 && g_ui.selected_index >= player->hand_count && player->hand_count > 0) {
        g_ui.selected_index = (uint8_t)(player->hand_count - 1);
    }

    uno_discard(card);
    g_game.active_color = uno_card_is_wild(card) ? (uno_color_t)wild_color : (uno_color_t)card.color;
    g_state_dirty = true;

    if (player->hand_count == 0) {
        g_game.game_over = true;
        uno_set_last_action("%s wins", player->name);
    } else {
        uno_set_last_action("%s played", player->name);
    }

    led_set_top_card(card);

    if (card.value == UNO_VALUE_DRAW_TWO || card.value == UNO_VALUE_WILD_DRAW_FOUR) {
        uno_apply_card_effect(card);
        return;
    }

    if (card.value == UNO_VALUE_SKIP || card.value == UNO_VALUE_REVERSE) {
        uno_apply_card_effect(card);
        return;
    }

    g_game.pending_draw = 0;
    uno_advance_turn(1);
}

static void uno_bot_take_turn(uint8_t player_id) {
    if (player_id >= UNO_MAX_PLAYERS) {
        return;
    }

    uno_player_t *player = &g_game.players[player_id];
    if (!player->active || !player->is_bot) {
        return;
    }

    if (g_game.pending_draw > 0) {
        for (uint8_t i = 0; i < player->hand_count; i++) {
            card_t card = player->hand[i];
            if (card.value == UNO_VALUE_DRAW_TWO || card.value == UNO_VALUE_WILD_DRAW_FOUR) {
                uint8_t wild_color = UNO_COLOR_RED;
                if (uno_card_is_wild(card)) {
                    /* Pick the most common non-wild color in hand */
                    uint8_t color_counts[4] = {0};
                    for (uint8_t h = 0; h < player->hand_count; h++) {
                        if (!uno_card_is_wild(player->hand[h])) {
                            color_counts[player->hand[h].color]++;
                        }
                    }
                    wild_color = UNO_COLOR_RED;
                    uint8_t best_count = 0;
                    for (uint8_t c = 0; c < 4; c++) {
                        if (color_counts[c] > best_count) {
                            best_count = color_counts[c];
                            wild_color = c;
                        }
                    }
                }
                uno_play_card(player_id, i, wild_color);
                return;
            }
        }
        uno_draw_cards(player_id, g_game.pending_draw);
        uno_set_last_action("%s drew %u", player->name, (unsigned)g_game.pending_draw);
        g_game.pending_draw = 0;
        uno_advance_turn(1);
        return;
    }

    for (uint8_t i = 0; i < player->hand_count; i++) {
        card_t card = player->hand[i];
        if (uno_is_playable(card)) {
            uint8_t wild_color = UNO_COLOR_RED;
            if (uno_card_is_wild(card)) {
                /* Pick the most common non-wild color in hand */
                uint8_t color_counts[4] = {0};
                for (uint8_t h = 0; h < player->hand_count; h++) {
                    if (!uno_card_is_wild(player->hand[h])) {
                        color_counts[player->hand[h].color]++;
                    }
                }
                wild_color = UNO_COLOR_RED;
                uint8_t best_count = 0;
                for (uint8_t c = 0; c < 4; c++) {
                    if (color_counts[c] > best_count) {
                        best_count = color_counts[c];
                        wild_color = c;
                    }
                }
            }
            uno_play_card(player_id, i, wild_color);
            return;
        }
    }

    uno_draw_cards(player_id, 1);
    uno_set_last_action("%s drew", player->name);
    uno_advance_turn(1);
}

static void uno_handle_action(const uno_msg_action_t *action) {
    if (action == NULL || !g_game.game_started || g_game.game_over) {
        return;
    }

    if (action->player_id >= UNO_MAX_PLAYERS) {
        return;
    }

    if (action->player_id != g_game.current_player) {
        return;
    }

    if (action->action == UNO_ACTION_DRAW) {
        uint8_t draw_count = (g_game.pending_draw > 0) ? g_game.pending_draw : 1;
        uno_draw_cards(action->player_id, draw_count);
        uno_set_last_action("%s drew %u", g_game.players[action->player_id].name, (unsigned)draw_count);
        g_game.pending_draw = 0;
        uno_advance_turn(1);
        return;
    }

    if (action->action == UNO_ACTION_PLAY) {
        uno_play_card(action->player_id, action->card_index, action->wild_color);
    }
}

static void uno_init_game_state(void) {
    memset(&g_game, 0, sizeof(g_game));
    g_game.direction = 1;
    g_game.active_color = UNO_COLOR_RED;
    g_game.pending_draw = 0;
    g_game.state_version = 0;
    g_game.game_started = false;
    g_game.game_over = false;
    g_game.requested_bots = 0;
    g_game.pending_wild.active = false;
    g_state_dirty = false;
    uno_reset_players();
    uno_set_last_action("Waiting for players");
}

static void uno_start_game(void) {
    uint8_t connected = 0;
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (g_game.players[i].connected) {
            connected++;
        }
    }

    uint8_t bots = g_game.requested_bots;
    if (connected + bots > 3) {
        bots = (uint8_t)(3 - connected);
    }

    uint8_t total = (uint8_t)(1 + connected + bots);
    if (total < 2) {
        uno_set_last_action("Need 2 players");
        return;
    }

    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (g_game.players[i].connected) {
            g_game.players[i].active = true;
            g_game.players[i].is_bot = false;
        } else if (bots > 0) {
            g_game.players[i].active = true;
            g_game.players[i].is_bot = true;
            g_game.players[i].connected = false;
            snprintf(g_game.players[i].name, sizeof(g_game.players[i].name), "Bot %u", (unsigned)i);
            bots--;
        } else {
            g_game.players[i].active = false;
            g_game.players[i].is_bot = false;
        }
    }

    g_game.player_count = uno_count_active_players();
    g_game.current_player = 0;
    g_game.direction = 1;
    g_game.pending_draw = 0;
    g_game.game_started = true;
    g_game.game_over = false;
    g_state_dirty = true;

    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        g_game.players[i].hand_count = 0;
    }

    g_game.deck_count = 0;
    g_game.discard_count = 0;
    uno_build_deck();
    uno_shuffle_deck(g_game.deck, g_game.deck_count);

    for (uint8_t deal = 0; deal < 7; deal++) {
        for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
            if (!g_game.players[i].active) {
                continue;
            }
            card_t card = uno_draw_card();
            if (!uno_card_is_none(card)) {
                g_game.players[i].hand[g_game.players[i].hand_count++] = card;
            }
        }
    }

    card_t top = uno_draw_card();
    if (uno_card_is_none(top)) {
        top = (card_t){ .color = UNO_COLOR_RED, .value = UNO_VALUE_0 };
    }

    uno_discard(top);
    g_game.active_color = uno_card_is_wild(top) ? UNO_COLOR_RED : (uno_color_t)top.color;
    led_set_top_card(top);

    if (top.value == UNO_VALUE_SKIP || top.value == UNO_VALUE_REVERSE || top.value == UNO_VALUE_DRAW_TWO) {
        uno_apply_card_effect(top);
    }

    g_ui.selected_index = 0;
    g_ui.scroll_offset = 0;
    uno_set_last_action("Game started");
    g_ui_dirty = true;
}

/* uno_build_players_label removed — compact format done inline in ui_update */

static void uno_ui_init(void) {
    uno_display_init();
    g_ui.root = uno_display_get_root();

    uno_display_lock();
    uno_display_clear();

    /* Title row: "UNO" + "Deck:NN" */
    uno_display_label_create(&g_ui.title_label, g_ui.root, 2, 0, "UNO", false);
    uno_display_label_create(&g_ui.deck_label, g_ui.root, 90, 0, "D:0", false);

    /* Top card icon: small 12x10 at y=10 */
    uno_display_card_create(&g_ui.top_card, g_ui.root, 2, 10, 12, 10);

    /* Status/log line below top card */
    uno_display_label_create(&g_ui.log_label, g_ui.root, 18, 12, "", true);

    /* Player counts line */
    uno_display_label_create(&g_ui.players_label, g_ui.root, 2, 22, "", true);

    /* Wild selection label */
    uno_display_label_create(&g_ui.wild_label, g_ui.root, 2, 30, "", true);

    /* Hand cards: tiny 8x12, 4 visible, starting at y=38 */
    int card_y = 38;
    int card_x = 4;
    for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
        uno_display_card_create(&g_ui.hand_cards[i], g_ui.root, card_x, card_y, 8, 12);
        card_x += 10;
    }

    /* Scroll arrows */
    uno_display_label_create(&g_ui.left_arrow, g_ui.root, 2, 52, "<", true);
    uno_display_label_create(&g_ui.right_arrow, g_ui.root, 120, 52, ">", true);

    uno_display_unlock();

    g_ui.initialized = true;
    g_ui.selected_index = 0;
    g_ui.scroll_offset = 0;
    g_ui.wild_select_active = false;
    g_ui.wild_color = UNO_COLOR_RED;
}

static void uno_ui_update(void) {
    if (!g_ui.initialized) return;

    char buf[24];
    uno_display_lock();

    /* ── Lobby mode ────────────────────────────────────────────────── */
    if (!g_game.game_started) {
        snprintf(buf, sizeof(buf), "UNO Host Ch:%u", (unsigned)g_game_channel);
        uno_display_label_set_text(&g_ui.title_label, buf);

        snprintf(buf, sizeof(buf), "Bots:%u", (unsigned)g_game.requested_bots);
        uno_display_label_set_text(&g_ui.deck_label, buf);

        uint8_t conn = 0;
        for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++)
            if (g_game.players[i].connected) conn++;
        snprintf(buf, sizeof(buf), "Conn:%u", (unsigned)conn);
        uno_display_label_set_text(&g_ui.log_label, buf);

        /* Player status: P2:OK P3:-- P4:-- */
        int po = 0;
        for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
            po += snprintf(buf + po, sizeof(buf) - po, "P%u:%s ",
                           (unsigned)i, g_game.players[i].connected ? "OK" : "--");
        }
        uno_display_label_set_text(&g_ui.players_label, buf);

        uno_display_label_set_text(&g_ui.wild_label, "U/D:Bot A:Start");

        uno_display_card_set(&g_ui.top_card, uno_card_none(), false);
        for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++)
            uno_display_card_set(&g_ui.hand_cards[i], uno_card_none(), false);
        uno_display_obj_set_hidden(g_ui.left_arrow.obj, true);
        uno_display_obj_set_hidden(g_ui.right_arrow.obj, true);
        uno_display_unlock();
        g_ui_dirty = false;
        led_set_selected_card(uno_card_none());
        return;
    }

    /* ── Game mode ─────────────────────────────────────────────────── */
    /* Title: whose turn */
    uint8_t cp = g_game.current_player;
    if (g_game.game_over) {
        snprintf(buf, sizeof(buf), "WINNER:P%u!", (unsigned)cp);
    } else if (cp == 0) {
        snprintf(buf, sizeof(buf), ">> My Turn");
    } else {
        snprintf(buf, sizeof(buf), "P%u's Turn", (unsigned)cp);
    }
    uno_display_label_set_text(&g_ui.title_label, buf);

    /* Deck count */
    snprintf(buf, sizeof(buf), "Deck:%u", (unsigned)g_game.deck_count);
    uno_display_label_set_text(&g_ui.deck_label, buf);

    /* Player hand sizes: P0:5 P1:3 P2:7 P3:2 */
    int po = 0;
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        if (!g_game.players[i].active) continue;
        po += snprintf(buf + po, sizeof(buf) - po, "P%u:%u ",
                       (unsigned)i, (unsigned)g_game.players[i].hand_count);
    }
    uno_display_label_set_text(&g_ui.players_label, buf);

    /* Bottom row: action log or wild select or button hint */
    if (g_ui.wild_select_active) {
        const char *cn = "R";
        if (g_ui.wild_color == 1) cn = "G";
        else if (g_ui.wild_color == 2) cn = "B";
        else if (g_ui.wild_color == 3) cn = "Y";
        snprintf(buf, sizeof(buf), "Color:%s A=OK B=Can", cn);
        uno_display_label_set_text(&g_ui.wild_label, buf);
    } else if (cp == 0) {
        uno_display_label_set_text(&g_ui.wild_label, "<>:Card A:Play B:Draw");
    } else {
        strncpy(buf, g_game.last_action, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        uno_display_label_set_text(&g_ui.wild_label, buf);
    }

    /* Top card — large display */
    card_t top = uno_top_card();
    uno_display_card_set(&g_ui.top_card, top, false);

    /* Host hand — numbered cards */
    uno_player_t *host = &g_game.players[0];
    if (host->hand_count == 0) {
        g_ui.selected_index = 0; g_ui.scroll_offset = 0;
    } else if (g_ui.selected_index >= host->hand_count) {
        g_ui.selected_index = (uint8_t)(host->hand_count - 1);
    }
    if (g_ui.selected_index < g_ui.scroll_offset) g_ui.scroll_offset = g_ui.selected_index;
    if (g_ui.selected_index >= g_ui.scroll_offset + UNO_HAND_VISIBLE)
        g_ui.scroll_offset = (uint8_t)(g_ui.selected_index - (UNO_HAND_VISIBLE - 1));

    for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
        uint8_t idx = (uint8_t)(g_ui.scroll_offset + i);
        if (idx < host->hand_count) {
            uno_display_card_set(&g_ui.hand_cards[i], host->hand[idx], idx == g_ui.selected_index);
        } else {
            uno_display_card_set(&g_ui.hand_cards[i], uno_card_none(), false);
        }
    }
    uno_display_obj_set_hidden(g_ui.left_arrow.obj, g_ui.scroll_offset == 0);
    uno_display_obj_set_hidden(g_ui.right_arrow.obj,
        (g_ui.scroll_offset + UNO_HAND_VISIBLE) >= host->hand_count);

    uno_display_unlock();

    /* LED: selected card color */
    if (host->hand_count > 0)
        led_set_selected_card(host->hand[g_ui.selected_index]);
    else
        led_set_selected_card(uno_card_none());

    g_ui_dirty = false;
}

static void uno_handle_lobby_input(void) {
    if (uno_btn_up_pressed()) {
        if (g_game.requested_bots < 3) {
            g_game.requested_bots++;
            g_ui_dirty = true;
        }
    }

    if (uno_btn_down_pressed()) {
        if (g_game.requested_bots > 0) {
            g_game.requested_bots--;
            g_ui_dirty = true;
        }
    }

    if (uno_btn_play_pressed()) {
        uno_start_game();
        g_ui_dirty = true;
    }
}

static void uno_handle_wild_select_input(uint8_t player_id, uint8_t hand_index) {
    if (uno_btn_up_pressed() || uno_btn_left_pressed()) {
        g_ui.wild_color = (uint8_t)((g_ui.wild_color + 3) % 4);
        g_ui_dirty = true;
    }

    if (uno_btn_down_pressed() || uno_btn_right_pressed()) {
        g_ui.wild_color = (uint8_t)((g_ui.wild_color + 1) % 4);
        g_ui_dirty = true;
    }

    if (uno_btn_play_pressed()) {
        g_ui.wild_select_active = false;
        g_game.pending_wild.active = false;
        uno_msg_action_t action = {
            .player_id = player_id,
            .action = UNO_ACTION_PLAY,
            .card_index = hand_index,
            .wild_color = g_ui.wild_color
        };
        uno_handle_action(&action);
        g_ui_dirty = true;
    }

    if (uno_btn_draw_pressed()) {
        g_ui.wild_select_active = false;
        g_game.pending_wild.active = false;
        g_ui_dirty = true;
    }
}

static void uno_handle_local_input(void) {
    if (!g_game.game_started || g_game.game_over) {
        return;
    }

    uno_player_t *host = &g_game.players[0];

    if (g_ui.wild_select_active && g_game.pending_wild.active) {
        uno_handle_wild_select_input(0, g_game.pending_wild.hand_index);
        return;
    }

    if (host->hand_count > 0) {
        if (uno_btn_up_pressed() || uno_btn_left_pressed()) {
            if (g_ui.selected_index > 0) {
                g_ui.selected_index--;
                g_ui_dirty = true;
            }
        }

        if (uno_btn_down_pressed() || uno_btn_right_pressed()) {
            if (g_ui.selected_index + 1 < host->hand_count) {
                g_ui.selected_index++;
                g_ui_dirty = true;
            }
        }
    }

    if (uno_btn_play_pressed() && g_game.current_player == 0) {
        if (host->hand_count == 0) {
            return;
        }
        card_t card = host->hand[g_ui.selected_index];
        if (uno_card_is_wild(card)) {
            g_ui.wild_select_active = true;
            g_ui.wild_color = UNO_COLOR_RED;
            g_game.pending_wild.active = true;
            g_game.pending_wild.player_id = 0;
            g_game.pending_wild.hand_index = g_ui.selected_index;
            g_game.pending_wild.card = card;
            set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
            g_ui_dirty = true;
        } else {
            uno_msg_action_t action = {
                .player_id = 0,
                .action = UNO_ACTION_PLAY,
                .card_index = g_ui.selected_index,
                .wild_color = UNO_COLOR_RED
            };
            uno_handle_action(&action);
        }
    }

    if (uno_btn_draw_pressed() && g_game.current_player == 0) {
        uno_msg_action_t action = {
            .player_id = 0,
            .action = UNO_ACTION_DRAW,
            .card_index = 0,
            .wild_color = 0
        };
        uno_handle_action(&action);
    }
}

static void uno_process_action_queue(void) {
    if (g_action_queue == NULL) {
        return;
    }

    uno_msg_action_t action;
    while (xQueueReceive(g_action_queue, &action, 0) == pdTRUE) {
        uno_handle_action(&action);
    }
}

static void uno_update_custom_led(void) {
    if (g_ui.wild_select_active) {
        set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
    } else if (g_game.game_started && g_game.current_player == 0) {
        set_custom_led(UNO_LED_MODE_TURN_BLINK);
    } else {
        bool any_connected = false;
        for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
            if (g_game.players[i].connected) {
                any_connected = true;
                break;
            }
        }
        set_custom_led(any_connected ? UNO_LED_MODE_CONNECTED_SOLID : UNO_LED_MODE_OFF);
    }
}

#ifdef CONFIG_BT_ENABLED
static void uno_ble_uuid128_init(ble_uuid128_t *uuid, uint8_t suffix) {
    *uuid = (ble_uuid128_t)BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24,
        0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5,
        suffix, g_game_channel, 0x40, 0x6e);
}

static int uno_player_id_for_conn(uint16_t conn_handle) {
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (g_game.players[i].conn_handle == conn_handle && g_game.players[i].connected) {
            return i;
        }
    }
    return -1;
}

static void uno_build_state_for_player(uint8_t player_id, uno_msg_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->msg_type = UNO_MSG_STATE;
    state->version = g_game.state_version;
    state->game_started = g_game.game_started ? 1 : 0;
    state->current_player = g_game.current_player;
    state->direction = (g_game.direction < 0) ? 1 : 0;
    state->active_color = (uint8_t)g_game.active_color;
    state->top_card = uno_card_encode(uno_top_card());
    state->deck_count = g_game.deck_count;
    state->pending_draw = g_game.pending_draw;
    state->player_count = g_game.player_count;
    state->your_player_id = player_id;
    state->your_hand_total = g_game.players[player_id].hand_count;
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        state->hand_sizes[i] = g_game.players[i].active ? g_game.players[i].hand_count : 0;
    }
}

static void uno_build_log_msg(uno_msg_log_t *log_msg) {
    memset(log_msg, 0, sizeof(*log_msg));
    log_msg->msg_type = UNO_MSG_LOG;
    log_msg->version = g_game.state_version;
    size_t len = strnlen(g_game.last_action, sizeof(g_game.last_action));
    if (len > sizeof(log_msg->text)) {
        len = sizeof(log_msg->text);
    }
    log_msg->text_len = (uint8_t)len;
    memcpy(log_msg->text, g_game.last_action, len);
}

static void uno_notify_player(uint8_t player_id) {
    if (player_id == 0 || !g_game.players[player_id].connected) {
        return;
    }

    if (g_uno_state_handle == 0) {
        return;
    }

    uno_msg_state_t state_msg;
    uno_build_state_for_player(player_id, &state_msg);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&state_msg, sizeof(state_msg));
    if (om != NULL) {
        ble_gatts_notify_custom(g_game.players[player_id].conn_handle, g_uno_state_handle, om);
    }

    uno_msg_log_t log_msg;
    uno_build_log_msg(&log_msg);
    om = ble_hs_mbuf_from_flat(&log_msg, sizeof(log_msg));
    if (om != NULL) {
        ble_gatts_notify_custom(g_game.players[player_id].conn_handle, g_uno_state_handle, om);
    }

    uint8_t total = g_game.players[player_id].hand_count;
    uint8_t offset = 0;
    while (offset < total) {
        uno_msg_hand_chunk_t chunk = {0};
        chunk.msg_type = UNO_MSG_HAND_CHUNK;
        chunk.version = g_game.state_version;
        chunk.offset = offset;
        chunk.count = (uint8_t)((total - offset) > UNO_HAND_CHUNK_CARDS ? UNO_HAND_CHUNK_CARDS : (total - offset));
        for (uint8_t i = 0; i < chunk.count; i++) {
            chunk.cards[i] = uno_card_encode(g_game.players[player_id].hand[offset + i]);
        }
        om = ble_hs_mbuf_from_flat(&chunk, sizeof(chunk));
        if (om != NULL) {
            ble_gatts_notify_custom(g_game.players[player_id].conn_handle, g_uno_state_handle, om);
        }
        offset = (uint8_t)(offset + chunk.count);
    }
}

static void uno_broadcast_state(void) {
    g_game.state_version++;
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (g_game.players[i].connected) {
            uno_notify_player(i);
        }
    }
}

static int uno_gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == g_uno_state_handle) {
        int player_id = uno_player_id_for_conn(conn_handle);
        if (player_id < 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uno_msg_state_t state_msg;
        uno_build_state_for_player((uint8_t)player_id, &state_msg);
        int rc = os_mbuf_append(ctxt->om, &state_msg, sizeof(state_msg));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == g_uno_cmd_handle) {
        uint8_t buf[32];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0 || len == 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint8_t msg_type = buf[0];
        if (msg_type == UNO_MSG_ACTION && len >= sizeof(uno_msg_action_t)) {
            int player_id = uno_player_id_for_conn(conn_handle);
            if (player_id >= 0) {
                uno_msg_action_t action;
                memcpy(&action, buf, sizeof(action));
                action.player_id = (uint8_t)player_id;
                xQueueSend(g_action_queue, &action, 0);
            }
        } else if (msg_type == UNO_MSG_HELLO && len >= sizeof(uno_msg_hello_t)) {
            int player_id = uno_player_id_for_conn(conn_handle);
            if (player_id >= 0) {
                uno_msg_hello_t hello;
                memcpy(&hello, buf, sizeof(hello));
                hello.name[UNO_NAME_MAX - 1] = '\0';
                uno_set_player_name((uint8_t)player_id, hello.name);
                g_ui_dirty = true;
            }
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def g_uno_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_uno_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_uno_state_uuid.u,
                .access_cb = uno_gatt_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_uno_state_handle,
            },
            {
                .uuid = &g_uno_cmd_uuid.u,
                .access_cb = uno_gatt_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &g_uno_cmd_handle,
            },
            { 0 }
        }
    },
    { 0 }
};

static int uno_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status != 0) {
                uno_ble_advertise();
                return 0;
            }

            uint16_t conn_handle = event->connect.conn_handle;
            bool assigned = false;
            for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
                if (!g_game.players[i].connected && g_game.players[i].active) {
                    g_game.players[i].connected = true;
                    g_game.players[i].is_bot = false;
                    g_game.players[i].conn_handle = conn_handle;
                    assigned = true;
                    break;
                }
            }

            if (!assigned && !g_game.game_started) {
                for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
                    if (!g_game.players[i].connected && !g_game.players[i].active) {
                        g_game.players[i].connected = true;
                        g_game.players[i].active = true;
                        g_game.players[i].is_bot = false;
                        g_game.players[i].conn_handle = conn_handle;
                        assigned = true;
                        break;
                    }
                }
            }

            if (!assigned) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                g_ui_dirty = true;
                g_state_dirty = true;
                uno_notify_player((uint8_t)uno_player_id_for_conn(conn_handle));
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT: {
            int player_id = uno_player_id_for_conn(event->disconnect.conn.conn_handle);
            if (player_id >= 0) {
                g_game.players[player_id].connected = false;
                g_game.players[player_id].conn_handle = UNO_INVALID_CONN_HANDLE;
                if (g_game.game_started) {
                    g_game.players[player_id].is_bot = true;
                    g_game.players[player_id].active = true;
                    snprintf(g_game.players[player_id].name, sizeof(g_game.players[player_id].name), "Bot %hu", (unsigned short)(uint8_t)player_id);
                } else {
                    g_game.players[player_id].active = false;
                    g_game.players[player_id].is_bot = false;
                    uno_set_player_name((uint8_t)player_id, NULL);
                }
                g_ui_dirty = true;
                g_state_dirty = true;
            }

            uno_ble_advertise();
            return 0;
        }
        default:
            return 0;
    }
}

static void uno_ble_advertise(void) {
    char adv_name[16];
    snprintf(adv_name, sizeof(adv_name), "UNO_%u", (unsigned)g_game_channel);

    ble_svc_gap_device_name_set(adv_name);

    /* Stop any active advertising before reconfiguring */
    ble_gap_adv_stop();

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)adv_name;
    fields.name_len = strlen(adv_name);
    fields.name_is_complete = 1;
    fields.uuids128 = &g_uno_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int adv_rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, uno_gap_event, NULL);
    if (adv_rc != 0 && adv_rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_start failed: %d", adv_rc);
    }
}

static void uno_ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    uno_ble_advertise();
}

static void uno_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void uno_ble_init(void) {
    ESP_LOGI(TAG, "uno_ble_init: starting");
    esp_err_t nimble_ret = nimble_port_init();
    if (nimble_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "NimBLE already initialized, re-advertising...");
        /* NimBLE already running — just restart advertising with new channel */
        uno_ble_advertise();
        return;
    }
    if (nimble_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(nimble_ret));
        return;
    }
    ESP_LOGI(TAG, "uno_ble_init: nimble_port_init OK");

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_LOGI(TAG, "uno_ble_init: gap+gatt init OK");

    uno_ble_uuid128_init(&g_uno_svc_uuid, 0x01);
    uno_ble_uuid128_init(&g_uno_state_uuid, 0x02);
    uno_ble_uuid128_init(&g_uno_cmd_uuid, 0x03);

    ble_gatts_count_cfg(g_uno_svcs);
    int rc = ble_gatts_add_svcs(g_uno_svcs);
    if (rc != 0) {
        ESP_LOGW(TAG, "GATT add services failed: %d", rc);
    }
    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGW(TAG, "GATT start failed: %d", rc);
    }
    ESP_LOGI(TAG, "uno_ble_init: GATT services OK");

    ble_hs_cfg.sync_cb = uno_ble_on_sync;
    nimble_port_freertos_init(uno_ble_host_task);
    ESP_LOGI(TAG, "uno_ble_init: host task started");
}
#endif

void uno_set_requested_bots(uint8_t count) {
    g_game.requested_bots = count;
}

/* ── Host main ──────────────────────────────────────────────────────
 * Uses a single UI (g_ui) for both lobby and game screens.
 * uno_ui_update() shows lobby info when !game_started,
 * and game info when game_started. No separate lobby draw/init.
 */
void uno_host_main(void) {
    ESP_LOGI(TAG, "UNO Host starting");

    /* 1. Init hardware */
    uno_led_init();
    uno_btn_init();

    /* 2. Init display — get root, clean it, set styles */
    uno_display_init();
    g_ui.root = uno_display_get_root();
    if (g_ui.root == NULL) {
        ESP_LOGE(TAG, "NULL display root!");
        NuttyApps_launchAppByIndex(0);
        return;
    }

    /* 3. Create all UI objects (used for both lobby and game) */
    NuttyDisplay_lockLVGL();
    lv_obj_clean(g_ui.root);
    lv_obj_set_style_border_width(g_ui.root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_ui.root, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Row 0: Title (e.g. "UNO Host Ch:1" or ">P0 Turn") */
    uno_display_label_create(&g_ui.title_label, g_ui.root, 2, 0, "UNO Host", false);
    /* Row 0 right: Deck/Bots count */
    uno_display_label_create(&g_ui.deck_label, g_ui.root, 80, 0, "Bots:1", false);

    /* Separator line at y=10 */
    lv_obj_t *sep = lv_obj_create(g_ui.root);
    lv_obj_set_size(sep, 124, 1);
    lv_obj_set_pos(sep, 2, 10);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    /* Row 1 (y=14): Connection / player status */
    uno_display_label_create(&g_ui.log_label, g_ui.root, 4, 14, "Conn:0", true);
    /* Row 2 (y=22): Player hand sizes */
    uno_display_label_create(&g_ui.players_label, g_ui.root, 4, 22, "P0:0 P1:0 P2:0 P3:0", true);
    /* Row 3 (y=30): Instructions / wild select / action log */
    uno_display_label_create(&g_ui.wild_label, g_ui.root, 4, 30, "U/D:Bot A:Start", true);

    /* Top card display: larger 24x18 at left */
    uno_display_card_create(&g_ui.top_card, g_ui.root, 2, 38, 24, 18);

    /* Hand cards: numbered 1-6, small 10x14, to right of top card */
    int cx = 28;
    for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
        uno_display_card_create(&g_ui.hand_cards[i], g_ui.root, cx, 38, 10, 14);
        cx += 10;
    }

    /* Scroll arrows at bottom */
    uno_display_label_create(&g_ui.left_arrow, g_ui.root, 2, 56, "<", true);
    uno_display_label_create(&g_ui.right_arrow, g_ui.root, 120, 56, ">", true);

    NuttyDisplay_unlockLVGL();

    g_ui.initialized = true;
    g_ui.selected_index = 0;
    g_ui.scroll_offset = 0;
    g_ui.wild_select_active = false;
    g_ui.wild_color = UNO_COLOR_RED;

    /* 3. Init game state */
    g_action_queue = xQueueCreateStatic(UNO_ACTION_QUEUE_LEN, sizeof(uno_msg_action_t),
                                        g_action_queue_storage, &g_action_queue_struct);
    uno_init_game_state();

    /* 4. Start BLE */
#ifdef CONFIG_BT_ENABLED
    uno_ble_init();
#endif

    /* 5. Force first draw */
    g_ui_dirty = true;

    /* ── Lobby loop ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Entering lobby loop");
    while (!g_game.game_started) {
        if (uno_btn_up_pressed()) {
            if (g_game.requested_bots < 3) { g_game.requested_bots++; g_ui_dirty = true; }
        }
        if (uno_btn_down_pressed()) {
            if (g_game.requested_bots > 0) { g_game.requested_bots--; g_ui_dirty = true; }
        }
        if (uno_btn_play_pressed()) {
            uno_start_game();
        }
        if (uno_btn_back_pressed()) {
            break;
        }
        if (g_ui_dirty) uno_ui_update();
        uno_update_custom_led();
        uno_custom_led_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* ── Game loop ─────────────────────────────────────────────────── */
    if (g_game.game_started) {
        ESP_LOGI(TAG, "Entering game loop");
        while (1) {
            if (uno_btn_back_pressed()) break;

            uno_handle_local_input();
            uno_process_action_queue();

            if (!g_game.game_over) {
                uno_player_t *cur = &g_game.players[g_game.current_player];
                if (cur->is_bot && cur->active) uno_bot_take_turn(g_game.current_player);
            }

#ifdef CONFIG_BT_ENABLED
            if (g_state_dirty) { g_state_dirty = false; uno_broadcast_state(); }
#endif
            uno_update_custom_led();
            uno_custom_led_update();
            if (g_ui_dirty) uno_ui_update();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "UNO Host exiting");
    uno_display_clear();
    NuttyApps_launchAppByIndex(0);
}

/* NuttyUNOHost defined in NuttyUNO.c */
