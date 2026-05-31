#include "uno_common.h"
#include "NuttyUNO.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "lvgl_fonts/cg_pixel_4x5_mono.h"

#include "esp_log.h"
#include "esp_random.h"

#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#endif

static const char *TAG = "UNOHost";

#ifdef CONFIG_BT_ENABLED
static int uno_gap_event(struct ble_gap_event *event, void *arg);
#endif

typedef enum {
    UNO_PLAYER_NONE = 0,
    UNO_PLAYER_HOST = 1,
    UNO_PLAYER_CLIENT = 2,
    UNO_PLAYER_BOT = 3
} uno_player_type_t;

typedef struct {
    uno_player_type_t type;
    bool connected;
    uint16_t conn_handle;
    card_t hand[UNO_MAX_HAND];
    uint8_t hand_count;
} uno_player_t;

typedef struct {
    uint8_t start_index;
    lv_obj_t *root;
    lv_obj_t *top_card_box;
    lv_obj_t *top_card_label;
    lv_obj_t *turn_label;
    lv_obj_t *deck_label;
    lv_obj_t *players_label;
    lv_obj_t *log_label;
    lv_obj_t *scroll_left;
    lv_obj_t *scroll_right;
    lv_obj_t *hand_cards[UNO_VISIBLE_CARDS];
    lv_obj_t *hand_labels[UNO_VISIBLE_CARDS];
    lv_style_t card_style;
    lv_style_t card_selected_style;
    lv_style_t text_small_style;
} uno_ui_t;

static uno_ui_t ui;

static uno_phase_t phase = UNO_PHASE_LOBBY;
static uno_player_t players[UNO_MAX_PLAYERS];
static char player_names[UNO_MAX_PLAYERS][UNO_NAME_MAX_LEN];

static card_t deck[UNO_DECK_SIZE];
static uint8_t deck_count = 0;
static card_t discard[UNO_DECK_SIZE];
static uint8_t discard_count = 0;
static card_t top_card = UNO_CARD_NONE;
static uint8_t active_color = UNO_COLOR_RED;
static uint8_t current_player = 0;
static int8_t direction = 1;
static uint8_t pending_draw = 0;
static bool pending_wild = false;
static uint8_t pending_wild_player = 0;
static uint8_t pending_wild_advance = 0;
static uint8_t winner = UNO_WINNER_NONE;
static char last_action[UNO_LAST_ACTION_LEN] = "";

static uint8_t player_count = 1;
static uint8_t bot_count = 0;
static uint8_t connected_clients = 0;
static bool state_dirty = true;
static bool drawn_this_turn = false;

static uint8_t host_selected_index = 0;
static uint8_t host_scroll_offset = 0;
static uint8_t host_wild_choice = UNO_COLOR_RED;

#define UNO_ACTION_QUEUE_SIZE 8
static uno_action_t action_queue[UNO_ACTION_QUEUE_SIZE];
static uint8_t action_queue_player[UNO_ACTION_QUEUE_SIZE];
static uint8_t action_head = 0;
static uint8_t action_tail = 0;
static portMUX_TYPE action_lock = portMUX_INITIALIZER_UNLOCKED;

#ifdef CONFIG_BT_ENABLED
static ble_uuid128_t service_uuid;
static ble_uuid128_t state_uuid;
static ble_uuid128_t action_uuid;
static uint16_t state_handle;
#endif

static void uno_set_last_action(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_action, sizeof(last_action), fmt, args);
    va_end(args);
}

static const char *uno_color_name(uint8_t color) {
    switch (color) {
        case UNO_COLOR_RED:
            return "Red";
        case UNO_COLOR_GREEN:
            return "Green";
        case UNO_COLOR_BLUE:
            return "Blue";
        case UNO_COLOR_YELLOW:
            return "Yellow";
        case UNO_COLOR_WILD:
            return "Wild";
        default:
            return "?";
    }
}

static void uno_card_to_text(card_t card, char *out, size_t len) {
    uint8_t value = UNO_CARD_VALUE(card);
    switch (value) {
        case UNO_VALUE_SKIP:
            snprintf(out, len, "S");
            break;
        case UNO_VALUE_REVERSE:
            snprintf(out, len, "R");
            break;
        case UNO_VALUE_DRAW_TWO:
            snprintf(out, len, "+2");
            break;
        case UNO_VALUE_WILD:
            snprintf(out, len, "W");
            break;
        case UNO_VALUE_WILD_DRAW_FOUR:
            snprintf(out, len, "+4");
            break;
        default:
            snprintf(out, len, "%u", value);
            break;
    }
}

static lv_color_t uno_color_to_lv(uint8_t color) {
    switch (color) {
        case UNO_COLOR_RED:
            return lv_palette_main(LV_PALETTE_RED);
        case UNO_COLOR_GREEN:
            return lv_palette_main(LV_PALETTE_GREEN);
        case UNO_COLOR_BLUE:
            return lv_palette_main(LV_PALETTE_BLUE);
        case UNO_COLOR_YELLOW:
            return lv_palette_main(LV_PALETTE_YELLOW);
        case UNO_COLOR_WILD:
            return lv_palette_main(LV_PALETTE_GREY);
        default:
            return lv_palette_main(LV_PALETTE_GREY);
    }
}

static void uno_shuffle_deck(card_t *array, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        uint8_t j = esp_random() % count;
        card_t tmp = array[i];
        array[i] = array[j];
        array[j] = tmp;
    }
}

static void uno_build_deck(void) {
    uint16_t idx = 0;
    for (uint8_t color = UNO_COLOR_RED; color <= UNO_COLOR_YELLOW; color++) {
        deck[idx++] = UNO_MAKE_CARD(color, UNO_VALUE_0);
        for (uint8_t value = UNO_VALUE_1; value <= UNO_VALUE_9; value++) {
            deck[idx++] = UNO_MAKE_CARD(color, value);
            deck[idx++] = UNO_MAKE_CARD(color, value);
        }
        for (uint8_t i = 0; i < 2; i++) {
            deck[idx++] = UNO_MAKE_CARD(color, UNO_VALUE_SKIP);
            deck[idx++] = UNO_MAKE_CARD(color, UNO_VALUE_REVERSE);
            deck[idx++] = UNO_MAKE_CARD(color, UNO_VALUE_DRAW_TWO);
        }
    }
    for (uint8_t i = 0; i < 4; i++) {
        deck[idx++] = UNO_MAKE_CARD(UNO_COLOR_WILD, UNO_VALUE_WILD);
        deck[idx++] = UNO_MAKE_CARD(UNO_COLOR_WILD, UNO_VALUE_WILD_DRAW_FOUR);
    }
    deck_count = idx;
    uno_shuffle_deck(deck, deck_count);
}

static void uno_reset_deck_from_discard(void) {
    if (discard_count <= 1) {
        return;
    }
    card_t keep = discard[discard_count - 1];
    uint8_t new_count = discard_count - 1;
    for (uint8_t i = 0; i < new_count; i++) {
        deck[i] = discard[i];
    }
    deck_count = new_count;
    discard[0] = keep;
    discard_count = 1;
    uno_shuffle_deck(deck, deck_count);
}

static card_t uno_draw_card(void) {
    if (deck_count == 0) {
        uno_reset_deck_from_discard();
    }
    if (deck_count == 0) {
        return UNO_CARD_NONE;
    }
    card_t card = deck[deck_count - 1];
    deck_count--;
    return card;
}

static void uno_add_to_discard(card_t card) {
    if (discard_count < UNO_DECK_SIZE) {
        discard[discard_count++] = card;
    }
}

static void uno_add_card_to_hand(uint8_t player, card_t card) {
    if (players[player].hand_count < UNO_MAX_HAND) {
        players[player].hand[players[player].hand_count++] = card;
    }
}

static bool uno_remove_card_from_hand(uint8_t player, card_t card) {
    for (uint8_t i = 0; i < players[player].hand_count; i++) {
        if (players[player].hand[i] == card) {
            for (uint8_t j = i + 1; j < players[player].hand_count; j++) {
                players[player].hand[j - 1] = players[player].hand[j];
            }
            players[player].hand_count--;
            return true;
        }
    }
    return false;
}

static void uno_deal_cards(void) {
    for (uint8_t p = 0; p < player_count; p++) {
        players[p].hand_count = 0;
        for (uint8_t i = 0; i < 7; i++) {
            uno_add_card_to_hand(p, uno_draw_card());
        }
    }
}

static void uno_init_player_names(void) {
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        const char *custom = uno_get_custom_player_name(i);
        if (custom != NULL) {
            snprintf(player_names[i], sizeof(player_names[i]), "%s", custom);
        } else {
            snprintf(player_names[i], sizeof(player_names[i]), "Player %u", i);
        }
    }
}

static void uno_reset_players(void) {
    memset(players, 0, sizeof(players));
    players[0].type = UNO_PLAYER_HOST;
    players[0].connected = true;
    players[0].conn_handle = UNO_CONN_HANDLE_NONE;
}

static uint8_t uno_choose_wild_color_from_hand(uint8_t player) {
    uint8_t counts[4] = {0};
    for (uint8_t i = 0; i < players[player].hand_count; i++) {
        uint8_t color = UNO_CARD_COLOR(players[player].hand[i]);
        if (color <= UNO_COLOR_YELLOW) {
            counts[color]++;
        }
    }
    uint8_t best_color = UNO_COLOR_RED;
    uint8_t best_count = counts[0];
    for (uint8_t i = 1; i < 4; i++) {
        if (counts[i] > best_count) {
            best_color = i;
            best_count = counts[i];
        }
    }
    return best_color;
}

static bool uno_is_card_playable(card_t card) {
    uint8_t value = UNO_CARD_VALUE(card);
    uint8_t color = UNO_CARD_COLOR(card);

    if (pending_draw > 0) {
        return (value == UNO_VALUE_DRAW_TWO || value == UNO_VALUE_WILD_DRAW_FOUR);
    }
    if (color == UNO_COLOR_WILD) {
        return true;
    }
    if (color == active_color) {
        return true;
    }
    if (value == UNO_CARD_VALUE(top_card)) {
        return true;
    }
    return false;
}

static void uno_advance_turn(uint8_t steps) {
    for (uint8_t i = 0; i < steps; i++) {
        int8_t next = (int8_t)current_player + direction;
        if (next < 0) {
            next = player_count - 1;
        } else if (next >= player_count) {
            next = 0;
        }
        current_player = (uint8_t)next;
    }
    drawn_this_turn = false;
}

static void uno_finish_turn_after_play(uint8_t skip_steps) {
    if (pending_wild) {
        return;
    }
    if (skip_steps == 0) {
        skip_steps = 1;
    }
    uno_advance_turn(skip_steps);
}

static void uno_apply_play_effect(card_t card) {
    uint8_t value = UNO_CARD_VALUE(card);

    if (value == UNO_VALUE_REVERSE) {
        if (player_count == 2) {
            uno_finish_turn_after_play(2);
            return;
        }
        direction = (int8_t)(-direction);
        uno_finish_turn_after_play(1);
        return;
    }

    if (value == UNO_VALUE_SKIP) {
        uno_finish_turn_after_play(2);
        return;
    }

    if (value == UNO_VALUE_DRAW_TWO) {
        pending_draw += 2;
        uno_finish_turn_after_play(1);
        return;
    }

    if (value == UNO_VALUE_WILD_DRAW_FOUR) {
        pending_draw += 4;
        pending_wild = true;
        pending_wild_player = current_player;
        pending_wild_advance = 1;
        return;
    }

    if (value == UNO_VALUE_WILD) {
        pending_wild = true;
        pending_wild_player = current_player;
        pending_wild_advance = 1;
        return;
    }

    uno_finish_turn_after_play(1);
}

static void uno_play_card(uint8_t player, card_t card) {
    if (!uno_is_card_playable(card)) {
        uno_set_last_action("%s invalid play", player_names[player]);
        return;
    }
    if (!uno_remove_card_from_hand(player, card)) {
        return;
    }

    top_card = card;
    active_color = UNO_CARD_COLOR(card);
    if (active_color == UNO_COLOR_WILD) {
        active_color = UNO_COLOR_RED;
    }

    uno_add_to_discard(card);
    char card_text[8] = {0};
    uno_card_to_text(card, card_text, sizeof(card_text));
    uno_set_last_action("%s played %s %s", player_names[player], uno_color_name(UNO_CARD_COLOR(card)), card_text);

    if (players[player].hand_count == 0) {
        winner = player;
        phase = UNO_PHASE_GAME_OVER;
        uno_set_last_action("%s wins!", player_names[player]);
        return;
    }

    if (UNO_CARD_COLOR(card) == UNO_COLOR_WILD) {
        active_color = UNO_COLOR_WILD;
    }

    uno_apply_play_effect(card);
}

static void uno_apply_wild_color(uint8_t player, uint8_t color) {
    active_color = color;
    pending_wild = false;
    uno_set_last_action("%s chose %s", player_names[player], uno_color_name(color));
    if (pending_wild_advance == 0) {
        pending_wild_advance = 1;
    }
    uno_advance_turn(pending_wild_advance);
    pending_wild_advance = 0;
}

static void uno_draw_for_player(uint8_t player, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        card_t card = uno_draw_card();
        if (card != UNO_CARD_NONE) {
            uno_add_card_to_hand(player, card);
        }
    }
}

static void uno_handle_draw(uint8_t player) {
    if (pending_draw > 0) {
        uint8_t draw_count = pending_draw;
        pending_draw = 0;
        uno_draw_for_player(player, draw_count);
        uno_set_last_action("%s drew %u", player_names[player], draw_count);
        uno_advance_turn(1);
        return;
    }

    if (!drawn_this_turn) {
        uno_draw_for_player(player, 1);
        drawn_this_turn = true;
        uno_set_last_action("%s drew a card", player_names[player]);
        return;
    }

    uno_set_last_action("%s passed", player_names[player]);
    uno_advance_turn(1);
}

static void uno_handle_bot_turn(uint8_t player) {
    if (pending_wild && pending_wild_player == player) {
        uint8_t color = uno_choose_wild_color_from_hand(player);
        uno_apply_wild_color(player, color);
        return;
    }

    if (pending_draw > 0) {
        for (uint8_t i = 0; i < players[player].hand_count; i++) {
            card_t card = players[player].hand[i];
            uint8_t value = UNO_CARD_VALUE(card);
            if (value == UNO_VALUE_DRAW_TWO || value == UNO_VALUE_WILD_DRAW_FOUR) {
                uno_play_card(player, card);
                return;
            }
        }
        uno_handle_draw(player);
        return;
    }

    for (uint8_t i = 0; i < players[player].hand_count; i++) {
        card_t card = players[player].hand[i];
        if (uno_is_card_playable(card)) {
            uno_play_card(player, card);
            return;
        }
    }
    uint8_t before_player = current_player;
    uno_handle_draw(player);
    if (current_player == before_player) {
        drawn_this_turn = false;
        uno_advance_turn(1);
    }
}

static void uno_select_initial_top_card(void) {
    card_t card = UNO_CARD_NONE;
    for (uint8_t tries = 0; tries < UNO_DECK_SIZE; tries++) {
        card = uno_draw_card();
        if (card == UNO_CARD_NONE) {
            break;
        }
        if (UNO_CARD_VALUE(card) == UNO_VALUE_WILD_DRAW_FOUR) {
            uno_add_to_discard(card);
            continue;
        }
        break;
    }
    if (card == UNO_CARD_NONE) {
        card = UNO_MAKE_CARD(UNO_COLOR_RED, UNO_VALUE_0);
    }
    top_card = card;
    active_color = UNO_CARD_COLOR(card);
    uno_add_to_discard(card);

    if (UNO_CARD_VALUE(card) == UNO_VALUE_WILD) {
        active_color = esp_random() % 4;
        uno_set_last_action("Wild start: %s", uno_color_name(active_color));
    } else if (UNO_CARD_VALUE(card) == UNO_VALUE_SKIP) {
        uno_set_last_action("Start Skip");
        uno_advance_turn(1);
    } else if (UNO_CARD_VALUE(card) == UNO_VALUE_REVERSE) {
        direction = (int8_t)(-direction);
        if (player_count == 2) {
            uno_advance_turn(1);
        }
        uno_set_last_action("Start Reverse");
    } else if (UNO_CARD_VALUE(card) == UNO_VALUE_DRAW_TWO) {
        pending_draw = 2;
        uno_set_last_action("Start +2");
    }
}

static void uno_start_game(void) {
    phase = UNO_PHASE_PLAYING;
    current_player = 0;
    direction = 1;
    pending_draw = 0;
    pending_wild = false;
    pending_wild_player = 0;
    pending_wild_advance = 0;
    winner = UNO_WINNER_NONE;
    drawn_this_turn = false;

    uno_build_deck();
    discard_count = 0;
    uno_deal_cards();
    uno_select_initial_top_card();
    state_dirty = true;
}

static void uno_update_player_slots(void) {
    bot_count = uno_get_bot_count();
    if (bot_count > (UNO_MAX_PLAYERS - 1)) {
        bot_count = UNO_MAX_PLAYERS - 1;
    }

    player_count = 1 + connected_clients + bot_count;
    if (player_count > UNO_MAX_PLAYERS) {
        player_count = UNO_MAX_PLAYERS;
    }

    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (players[i].type == UNO_PLAYER_CLIENT) {
            players[i].connected = true;
        }
    }

    uint8_t bots_needed = (player_count > (1 + connected_clients)) ? (player_count - 1 - connected_clients) : 0;
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (players[i].type == UNO_PLAYER_CLIENT) {
            continue;
        }
        if (bots_needed > 0) {
            players[i].type = UNO_PLAYER_BOT;
            players[i].connected = true;
            bots_needed--;
        } else {
            players[i].type = UNO_PLAYER_NONE;
            players[i].connected = false;
        }
    }
}

static bool uno_enqueue_action(uint8_t player, const uno_action_t *action) {
    bool queued = false;
    portENTER_CRITICAL(&action_lock);
    uint8_t next_head = (uint8_t)((action_head + 1) % UNO_ACTION_QUEUE_SIZE);
    if (next_head != action_tail) {
        action_queue[action_head] = *action;
        action_queue_player[action_head] = player;
        action_head = next_head;
        queued = true;
    }
    portEXIT_CRITICAL(&action_lock);
    return queued;
}

static bool uno_dequeue_action(uint8_t *player, uno_action_t *action) {
    bool has = false;
    portENTER_CRITICAL(&action_lock);
    if (action_tail != action_head) {
        *action = action_queue[action_tail];
        *player = action_queue_player[action_tail];
        action_tail = (uint8_t)((action_tail + 1) % UNO_ACTION_QUEUE_SIZE);
        has = true;
    }
    portEXIT_CRITICAL(&action_lock);
    return has;
}

static void uno_handle_action(uint8_t player, const uno_action_t *action) {
    if (phase != UNO_PHASE_PLAYING) {
        return;
    }

    if (pending_wild && action->type != UNO_ACTION_WILD_COLOR) {
        return;
    }

    if (action->type == UNO_ACTION_WILD_COLOR) {
        if (pending_wild && pending_wild_player == player) {
            uint8_t color = action->arg0;
            if (color > UNO_COLOR_YELLOW) {
                color = UNO_COLOR_RED;
            }
            uno_apply_wild_color(player, color);
            state_dirty = true;
        }
        return;
    }

    if (player != current_player) {
        return;
    }

    if (action->type == UNO_ACTION_PLAY_CARD) {
        uno_play_card(player, (card_t)action->arg0);
        state_dirty = true;
        return;
    }

    if (action->type == UNO_ACTION_DRAW) {
        uno_handle_draw(player);
        state_dirty = true;
    }
}

static void uno_ui_init(void) {
    memset(&ui, 0, sizeof(ui));

    lv_obj_t *draw_area = NuttyDisplay_getUserAppArea();
    NuttyDisplay_lockLVGL();

    ui.root = lv_obj_create(draw_area);
    lv_obj_set_size(ui.root, 128, 59);
    lv_obj_set_pos(ui.root, 0, 0);
    lv_obj_set_style_border_width(ui.root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_style_init(&ui.card_style);
    lv_style_set_bg_opa(&ui.card_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui.card_style, 1);
    lv_style_set_border_color(&ui.card_style, lv_color_black());

    lv_style_init(&ui.card_selected_style);
    lv_style_set_bg_opa(&ui.card_selected_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui.card_selected_style, 2);
    lv_style_set_border_color(&ui.card_selected_style, lv_color_black());

    lv_style_init(&ui.text_small_style);
    lv_style_set_text_font(&ui.text_small_style, &cg_pixel_4x5_mono);

    ui.top_card_box = lv_obj_create(ui.root);
    lv_obj_set_size(ui.top_card_box, 24, 30);
    lv_obj_set_pos(ui.top_card_box, 2, 2);
    lv_obj_add_style(ui.top_card_box, &ui.card_style, LV_PART_MAIN);

    ui.top_card_label = lv_label_create(ui.top_card_box);
    lv_obj_align(ui.top_card_label, LV_ALIGN_CENTER, 0, 0);

    ui.turn_label = lv_label_create(ui.root);
    lv_obj_add_style(ui.turn_label, &ui.text_small_style, LV_PART_MAIN);
    lv_obj_set_pos(ui.turn_label, 30, 2);

    ui.deck_label = lv_label_create(ui.root);
    lv_obj_add_style(ui.deck_label, &ui.text_small_style, LV_PART_MAIN);
    lv_obj_set_pos(ui.deck_label, 30, 10);

    ui.players_label = lv_label_create(ui.root);
    lv_obj_add_style(ui.players_label, &ui.text_small_style, LV_PART_MAIN);
    lv_obj_set_pos(ui.players_label, 30, 18);

    ui.log_label = lv_label_create(ui.root);
    lv_obj_add_style(ui.log_label, &ui.text_small_style, LV_PART_MAIN);
    lv_obj_set_pos(ui.log_label, 2, 34);

    ui.scroll_left = lv_label_create(ui.root);
    lv_label_set_text(ui.scroll_left, "<");
    lv_obj_set_pos(ui.scroll_left, 2, 46);
    lv_obj_add_style(ui.scroll_left, &ui.text_small_style, LV_PART_MAIN);

    ui.scroll_right = lv_label_create(ui.root);
    lv_label_set_text(ui.scroll_right, ">");
    lv_obj_set_pos(ui.scroll_right, 120, 46);
    lv_obj_add_style(ui.scroll_right, &ui.text_small_style, LV_PART_MAIN);

    for (uint8_t i = 0; i < UNO_VISIBLE_CARDS; i++) {
        ui.hand_cards[i] = lv_obj_create(ui.root);
        lv_obj_set_size(ui.hand_cards[i], 18, 18);
        lv_obj_set_pos(ui.hand_cards[i], 10 + i * 19, 40);
        lv_obj_add_style(ui.hand_cards[i], &ui.card_style, LV_PART_MAIN);

        ui.hand_labels[i] = lv_label_create(ui.hand_cards[i]);
        lv_obj_align(ui.hand_labels[i], LV_ALIGN_CENTER, 0, 0);
    }

    NuttyDisplay_unlockLVGL();
}

static void uno_ui_render_hand(uint8_t player) {
    if (player >= player_count) {
        return;
    }

    uint8_t hand_count = players[player].hand_count;
    if (hand_count == 0) {
        host_selected_index = 0;
        host_scroll_offset = 0;
    } else {
        if (host_selected_index >= hand_count) {
            host_selected_index = hand_count - 1;
        }
        if (host_selected_index < host_scroll_offset) {
            host_scroll_offset = host_selected_index;
        }
        if (host_selected_index >= host_scroll_offset + UNO_VISIBLE_CARDS) {
            host_scroll_offset = host_selected_index - (UNO_VISIBLE_CARDS - 1);
        }
    }

    for (uint8_t i = 0; i < UNO_VISIBLE_CARDS; i++) {
        uint8_t card_index = host_scroll_offset + i;
        if (card_index < hand_count) {
            card_t card = players[player].hand[card_index];
            uint8_t color = UNO_CARD_COLOR(card);
            char text[6] = {0};
            uno_card_to_text(card, text, sizeof(text));
            lv_obj_clear_flag(ui.hand_cards[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *card_obj = ui.hand_cards[i];
            lv_obj_reset_style_list(card_obj, LV_PART_MAIN);
            if (card_index == host_selected_index) {
                lv_obj_add_style(card_obj, &ui.card_selected_style, LV_PART_MAIN);
                led_set_selected_card(card);
            } else {
                lv_obj_add_style(card_obj, &ui.card_style, LV_PART_MAIN);
            }
            lv_obj_set_style_bg_color(card_obj, uno_color_to_lv(color), LV_PART_MAIN);
            lv_label_set_text(ui.hand_labels[i], text);
        } else {
            lv_obj_add_flag(ui.hand_cards[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (host_scroll_offset > 0) {
        lv_obj_clear_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    }

    if (hand_count > host_scroll_offset + UNO_VISIBLE_CARDS) {
        lv_obj_clear_flag(ui.scroll_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.scroll_right, LV_OBJ_FLAG_HIDDEN);
    }

    if (hand_count == 0) {
        led_set_selected_card(UNO_CARD_NONE);
    }
}

static void uno_ui_update(void) {
    NuttyDisplay_lockLVGL();

    char top_text[8] = {0};
    uno_card_to_text(top_card, top_text, sizeof(top_text));
    lv_label_set_text(ui.top_card_label, top_text);
    uint8_t top_color = (UNO_CARD_COLOR(top_card) == UNO_COLOR_WILD) ? active_color : UNO_CARD_COLOR(top_card);
    lv_obj_set_style_bg_color(ui.top_card_box, uno_color_to_lv(top_color), LV_PART_MAIN);

    char turn_text[32];
    snprintf(turn_text, sizeof(turn_text), "Turn:%s", player_names[current_player]);
    lv_label_set_text(ui.turn_label, turn_text);

    char deck_text[16];
    snprintf(deck_text, sizeof(deck_text), "Deck:%u", deck_count);
    lv_label_set_text(ui.deck_label, deck_text);

    char players_text[48] = {0};
    char line[16];
    for (uint8_t i = 0; i < player_count; i++) {
        snprintf(line, sizeof(line), "P%u:%u ", i, players[i].hand_count);
        strncat(players_text, line, sizeof(players_text) - strlen(players_text) - 1);
    }
    lv_label_set_text(ui.players_label, players_text);

    lv_label_set_text(ui.log_label, last_action);

    uno_ui_render_hand(0);

    NuttyDisplay_unlockLVGL();
}

static void uno_ui_show_lobby(void) {
    NuttyDisplay_lockLVGL();
    char text[48];
    snprintf(text, sizeof(text), "Players:%u Bots:%u", 1 + connected_clients, bot_count);
    lv_label_set_text(ui.turn_label, "UNO Lobby");
    lv_label_set_text(ui.deck_label, text);
    lv_label_set_text(ui.players_label, "UP/DOWN bots");
    lv_label_set_text(ui.log_label, "PLAY to start");
    lv_label_set_text(ui.top_card_label, "UNO");
    lv_obj_set_style_bg_color(ui.top_card_box, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    for (uint8_t i = 0; i < UNO_VISIBLE_CARDS; i++) {
        lv_obj_add_flag(ui.hand_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.scroll_right, LV_OBJ_FLAG_HIDDEN);
    NuttyDisplay_unlockLVGL();
}

static void uno_ui_show_game_over(void) {
    NuttyDisplay_lockLVGL();
    char text[48];
    snprintf(text, sizeof(text), "Winner: %s", player_names[winner]);
    lv_label_set_text(ui.turn_label, text);
    lv_label_set_text(ui.deck_label, "Hold START exit");
    lv_label_set_text(ui.players_label, "");
    lv_label_set_text(ui.log_label, last_action);
    NuttyDisplay_unlockLVGL();
}

#ifdef CONFIG_BT_ENABLED
static uint8_t uno_find_player_by_conn(uint16_t conn_handle) {
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (players[i].type == UNO_PLAYER_CLIENT && players[i].connected && players[i].conn_handle == conn_handle) {
            return i;
        }
    }
    return 0;
}

static int uno_gatt_access_state(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t player = uno_find_player_by_conn(conn_handle);
    uno_state_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.version = UNO_STATE_VERSION;
    packet.phase = phase;
    packet.local_player = player;
    packet.current_player = current_player;
    packet.direction = direction;
    packet.active_color = active_color;
    packet.top_card = top_card;
    packet.deck_count = deck_count;
    packet.discard_count = discard_count;
    packet.pending_draw = pending_draw;
    packet.pending_wild = pending_wild;
    packet.pending_wild_player = pending_wild_player;
    packet.winner = winner;
    packet.player_count = player_count;
    for (uint8_t i = 0; i < player_count; i++) {
        packet.player_hand_counts[i] = players[i].hand_count;
    }
    packet.local_hand_count = players[player].hand_count;
    memcpy(packet.local_hand, players[player].hand, players[player].hand_count);
    memcpy(packet.player_names, player_names, sizeof(player_names));
    snprintf(packet.last_action, sizeof(packet.last_action), "%s", last_action);

    int rc = os_mbuf_append(ctxt->om, &packet, sizeof(packet));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int uno_gatt_access_action(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uno_action_t action;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &action, sizeof(action), NULL);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t player = uno_find_player_by_conn(conn_handle);
    if (!uno_enqueue_action(player, &action)) {
        ESP_LOGW(TAG, "Action queue full");
    }
    return 0;
}

static const struct ble_gatt_svc_def uno_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &state_uuid.u,
                .access_cb = uno_gatt_access_state,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &state_handle,
            },
            {
                .uuid = &action_uuid.u,
                .access_cb = uno_gatt_access_action,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0}
        }
    },
    {0}
};

static void uno_host_send_state(uint16_t conn_handle, uint8_t player) {
    uno_state_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.version = UNO_STATE_VERSION;
    packet.phase = phase;
    packet.local_player = player;
    packet.current_player = current_player;
    packet.direction = direction;
    packet.active_color = active_color;
    packet.top_card = top_card;
    packet.deck_count = deck_count;
    packet.discard_count = discard_count;
    packet.pending_draw = pending_draw;
    packet.pending_wild = pending_wild;
    packet.pending_wild_player = pending_wild_player;
    packet.winner = winner;
    packet.player_count = player_count;
    for (uint8_t i = 0; i < player_count; i++) {
        packet.player_hand_counts[i] = players[i].hand_count;
    }
    packet.local_hand_count = players[player].hand_count;
    memcpy(packet.local_hand, players[player].hand, players[player].hand_count);
    memcpy(packet.player_names, player_names, sizeof(player_names));
    snprintf(packet.last_action, sizeof(packet.last_action), "%s", last_action);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&packet, sizeof(packet));
    if (om == NULL) {
        return;
    }
    ble_gatts_notify_custom(conn_handle, state_handle, om);
}

static void uno_host_broadcast_state(void) {
    for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
        if (players[i].type == UNO_PLAYER_CLIENT && players[i].connected) {
            uno_host_send_state(players[i].conn_handle, i);
        }
    }
}

static void uno_build_uuid(ble_uuid128_t *uuid, uint8_t channel, uint8_t suffix) {
    uint8_t bytes[16];
    uno_build_uuid_bytes(channel, suffix, bytes);
    uuid->u.type = BLE_UUID_TYPE_128;
    memcpy(uuid->value, bytes, sizeof(bytes));
}

static void uno_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    char name[16];

    memset(&fields, 0, sizeof(fields));
    snprintf(name, sizeof(name), "UNO_%u", (unsigned)CONFIG_GAME_CHANNEL);
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids128 = &service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, uno_gap_event, NULL);
}

static int uno_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                uint16_t handle = event->connect.conn_handle;
                if (phase != UNO_PHASE_LOBBY) {
                    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                bool assigned = false;
                for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
                    if (players[i].type != UNO_PLAYER_CLIENT) {
                        players[i].type = UNO_PLAYER_CLIENT;
                        players[i].connected = true;
                        players[i].conn_handle = handle;
                        connected_clients++;
                        uno_update_player_slots();
                        uno_set_last_action("Client joined P%u", i);
                        state_dirty = true;
                        assigned = true;
                        break;
                    }
                }
                if (!assigned) {
                    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
                }
            }
            uno_advertise();
            break;
        case BLE_GAP_EVENT_DISCONNECT: {
            uint16_t handle = event->disconnect.conn.conn_handle;
            for (uint8_t i = 1; i < UNO_MAX_PLAYERS; i++) {
                if (players[i].type == UNO_PLAYER_CLIENT && players[i].conn_handle == handle) {
                    players[i].connected = false;
                    players[i].conn_handle = UNO_CONN_HANDLE_NONE;
                    if (phase == UNO_PHASE_PLAYING) {
                        players[i].type = UNO_PLAYER_BOT;
                        uno_set_last_action("P%u bot takeover", i);
                    } else {
                        players[i].type = UNO_PLAYER_NONE;
                        if (connected_clients > 0) {
                            connected_clients--;
                        }
                        uno_update_player_slots();
                        uno_set_last_action("Client left P%u", i);
                    }
                    state_dirty = true;
                    break;
                }
            }
            uno_advertise();
            break;
        }
        default:
            break;
    }
    return 0;
}

static void uno_on_sync(void) {
    uno_advertise();
}

static void uno_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

static void uno_host_handle_buttons(void) {
    if (phase == UNO_PHASE_LOBBY) {
        if (uno_button_up()) {
            uint8_t max_bots = (connected_clients < (UNO_MAX_PLAYERS - 1)) ? (UNO_MAX_PLAYERS - 1 - connected_clients) : 0;
            if (bot_count < max_bots) {
                bot_count++;
                uno_update_player_slots();
                state_dirty = true;
            }
        }
        if (uno_button_down()) {
            if (bot_count > 0) {
                bot_count--;
                uno_update_player_slots();
                state_dirty = true;
            }
        }
        if (uno_button_play()) {
            uno_update_player_slots();
            uno_start_game();
        }
        return;
    }

    if (phase != UNO_PHASE_PLAYING) {
        return;
    }

    if (pending_wild && pending_wild_player == 0) {
        if (uno_button_up()) {
            host_wild_choice = (host_wild_choice + 3) % 4;
            state_dirty = true;
        }
        if (uno_button_down()) {
            host_wild_choice = (host_wild_choice + 1) % 4;
            state_dirty = true;
        }
        if (uno_button_play()) {
            uno_apply_wild_color(0, host_wild_choice);
            state_dirty = true;
        }
        return;
    }

    uint8_t hand_count = players[0].hand_count;
    if (uno_button_up() && hand_count > 0) {
        if (host_selected_index > 0) {
            host_selected_index--;
            state_dirty = true;
        }
    }

    if (uno_button_down() && hand_count > 0) {
        if (host_selected_index + 1 < hand_count) {
            host_selected_index++;
            state_dirty = true;
        }
    }

    if (uno_button_play() && current_player == 0 && hand_count > 0) {
        card_t card = players[0].hand[host_selected_index];
        uno_play_card(0, card);
        state_dirty = true;
    }

    if (uno_button_draw() && current_player == 0) {
        uno_handle_draw(0);
        state_dirty = true;
    }
}

static void uno_update_leds(void) {
    card_t led_card = top_card;
    if (UNO_CARD_COLOR(top_card) == UNO_COLOR_WILD) {
        led_card = UNO_MAKE_CARD(active_color, UNO_CARD_VALUE(top_card));
    }
    led_set_top_card(led_card);

    if (players[0].hand_count > 0 && host_selected_index < players[0].hand_count) {
        led_set_selected_card(players[0].hand[host_selected_index]);
    } else {
        led_set_selected_card(UNO_CARD_NONE);
    }

    if (pending_wild && pending_wild_player == 0) {
        set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
    } else if (phase == UNO_PHASE_PLAYING && current_player == 0) {
        set_custom_led(UNO_LED_MODE_TURN_BLINK);
    } else if (connected_clients > 0) {
        set_custom_led(UNO_LED_MODE_CONNECTED_SOLID);
    } else {
        set_custom_led(UNO_LED_MODE_OFF);
    }
}

static void uno_process_action_queue(void) {
    uint8_t player = 0;
    uno_action_t action;
    while (uno_dequeue_action(&player, &action)) {
        uno_handle_action(player, &action);
    }
}

static void nutty_main(void) {
    ESP_LOGI(TAG, "UNO Host start");
    uno_led_init();
    uno_init_player_names();
    uno_reset_players();
    uno_update_player_slots();

    uno_ui_init();
    uno_ui_show_lobby();

#ifdef CONFIG_BT_ENABLED
    esp_err_t nimble_ret = nimble_port_init();
    if (nimble_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "NimBLE already initialized");
    } else if (nimble_ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %d", nimble_ret);
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    char dev_name[16];
    snprintf(dev_name, sizeof(dev_name), "UNO_%u", (unsigned)CONFIG_GAME_CHANNEL);
    ble_svc_gap_device_name_set(dev_name);

    uno_build_uuid(&service_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_SERVICE);
    uno_build_uuid(&state_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_STATE);
    uno_build_uuid(&action_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_ACTION);

    ble_gatts_count_cfg(uno_gatt_svcs);
    ble_gatts_add_svcs(uno_gatt_svcs);

    ble_hs_cfg.sync_cb = uno_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    if (nimble_ret == ESP_OK) {
        nimble_port_freertos_init(uno_ble_host_task);
    }

    ble_att_set_preferred_mtu(247);
#else
    ESP_LOGW(TAG, "Bluetooth not enabled");
#endif

    while (1) {
        if (uno_button_exit()) {
            break;
        }

        uno_process_action_queue();

        if (phase == UNO_PHASE_PLAYING && players[current_player].type == UNO_PLAYER_BOT) {
            uno_handle_bot_turn(current_player);
            state_dirty = true;
        }

        uno_host_handle_buttons();

        if (phase == UNO_PHASE_LOBBY) {
            if (state_dirty) {
                uno_ui_show_lobby();
            }
        } else if (phase == UNO_PHASE_PLAYING) {
            if (state_dirty) {
                uno_ui_update();
            }
        } else if (phase == UNO_PHASE_GAME_OVER) {
            uno_ui_show_game_over();
        }

        if (state_dirty) {
#ifdef CONFIG_BT_ENABLED
            uno_host_broadcast_state();
#endif
            state_dirty = false;
        }

        uno_update_leds();
        uno_led_tick();

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    NuttyDisplay_clearUserAppArea();
    NuttyApps_launchAppByIndex(0);
}

NuttyAppDefinition NuttyUNOHost = {
    .appName = "UNO Host",
    .appMainEntry = nutty_main,
    .appHidden = false
};
