#ifndef NUTTY_UNO_COMMON_H
#define NUTTY_UNO_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "services/NuttyInput/NuttyInput.h"

#ifdef CONFIG_BT_ENABLED
#include "host/ble_hs.h"
#define UNO_CONN_HANDLE_NONE BLE_HS_CONN_HANDLE_NONE
#else
#define UNO_CONN_HANDLE_NONE 0xFFFF
#endif

#define UNO_MAX_PLAYERS 4
#define UNO_DECK_SIZE 108
#define UNO_MAX_HAND UNO_DECK_SIZE
#define UNO_NAME_MAX_LEN 12
#define UNO_LAST_ACTION_LEN 48
#define UNO_VISIBLE_CARDS 6

#define UNO_CARD_NONE 0xFF
#define UNO_COLOR_NONE 0xFF

typedef uint8_t card_t;

typedef enum {
    UNO_COLOR_RED = 0,
    UNO_COLOR_GREEN = 1,
    UNO_COLOR_BLUE = 2,
    UNO_COLOR_YELLOW = 3,
    UNO_COLOR_WILD = 4
} uno_color_t;

typedef enum {
    UNO_VALUE_0 = 0,
    UNO_VALUE_1 = 1,
    UNO_VALUE_2 = 2,
    UNO_VALUE_3 = 3,
    UNO_VALUE_4 = 4,
    UNO_VALUE_5 = 5,
    UNO_VALUE_6 = 6,
    UNO_VALUE_7 = 7,
    UNO_VALUE_8 = 8,
    UNO_VALUE_9 = 9,
    UNO_VALUE_SKIP = 10,
    UNO_VALUE_REVERSE = 11,
    UNO_VALUE_DRAW_TWO = 12,
    UNO_VALUE_WILD = 13,
    UNO_VALUE_WILD_DRAW_FOUR = 14
} uno_value_t;

typedef enum {
    UNO_ACTION_PLAY_CARD = 1,
    UNO_ACTION_DRAW = 2,
    UNO_ACTION_WILD_COLOR = 3
} uno_action_type_t;

typedef enum {
    UNO_PHASE_LOBBY = 0,
    UNO_PHASE_PLAYING = 1,
    UNO_PHASE_GAME_OVER = 2
} uno_phase_t;

typedef enum {
    UNO_LED_MODE_OFF = 0,
    UNO_LED_MODE_TURN_BLINK = 1,
    UNO_LED_MODE_CONNECTED_SOLID = 2,
    UNO_LED_MODE_WILD_CHOICE_FLASH = 3
} uno_led_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t arg0;
    uint8_t arg1;
} uno_action_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t phase;
    uint8_t local_player;
    uint8_t current_player;
    int8_t direction;
    uint8_t active_color;
    card_t top_card;
    uint8_t deck_count;
    uint8_t discard_count;
    uint8_t pending_draw;
    uint8_t pending_wild;
    uint8_t pending_wild_player;
    uint8_t winner;
    uint8_t player_count;
    uint8_t player_hand_counts[UNO_MAX_PLAYERS];
    uint8_t local_hand_count;
    card_t local_hand[UNO_MAX_HAND];
    char player_names[UNO_MAX_PLAYERS][UNO_NAME_MAX_LEN];
    char last_action[UNO_LAST_ACTION_LEN];
} uno_state_packet_t;

#define UNO_STATE_VERSION 1
#define UNO_UUID_SUFFIX_SERVICE 0x01
#define UNO_UUID_SUFFIX_STATE 0x02
#define UNO_UUID_SUFFIX_ACTION 0x03

static inline card_t UNO_MAKE_CARD(uint8_t color, uint8_t value) {
    return (card_t)(((color & 0x0F) << 4) | (value & 0x0F));
}

static inline uint8_t UNO_CARD_COLOR(card_t card) {
    return (uint8_t)((card >> 4) & 0x0F);
}

static inline uint8_t UNO_CARD_VALUE(card_t card) {
    return (uint8_t)(card & 0x0F);
}

static inline void uno_build_uuid_bytes(uint8_t channel, uint8_t suffix, uint8_t out[16]) {
    out[0] = 0x9E;
    out[1] = 0xCA;
    out[2] = 0xDC;
    out[3] = 0x24;
    out[4] = 0x0E;
    out[5] = 0xE5;
    out[6] = 0xA9;
    out[7] = 0xE0;
    out[8] = 0x93;
    out[9] = 0xF3;
    out[10] = 0xA3;
    out[11] = 0xB5;
    out[12] = suffix;
    out[13] = channel;
    out[14] = 0x40;
    out[15] = 0x6E;
}

void uno_led_init(void);
void uno_led_tick(void);
void led_set_top_card(card_t card);
void led_set_selected_card(card_t card);
void set_custom_led(uint8_t mode);

bool uno_button_up(void);
bool uno_button_down(void);
bool uno_button_play(void);
bool uno_button_draw(void);
bool uno_button_exit(void);

uint8_t uno_get_bot_count(void);
const char *uno_get_custom_player_name(uint8_t player_index);

#endif /* NUTTY_UNO_COMMON_H */
