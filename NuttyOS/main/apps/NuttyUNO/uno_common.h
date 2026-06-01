#ifndef NUTTY_UNO_COMMON_H
#define NUTTY_UNO_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifndef CONFIG_GAME_CHANNEL
#define CONFIG_GAME_CHANNEL 0
#endif

#define UNO_GAME_CHANNEL ((uint8_t)(CONFIG_GAME_CHANNEL & 0xFF))

#define UNO_MAX_PLAYERS 4
#define UNO_DECK_SIZE 108
#define UNO_MAX_HAND 108
#define UNO_HAND_CHUNK_CARDS 12
#define UNO_NAME_MAX 12
#define UNO_LAST_ACTION_MAX 32
#define UNO_CARD_VALUE_NONE 0xFF

typedef enum {
    UNO_COLOR_RED = 0,
    UNO_COLOR_GREEN = 1,
    UNO_COLOR_BLUE = 2,
    UNO_COLOR_YELLOW = 3,
    UNO_COLOR_WILD = 4,
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
    UNO_VALUE_WILD_DRAW_FOUR = 14,
} uno_value_t;

typedef struct {
    uint8_t color;
    uint8_t value;
} card_t;

static inline uint8_t uno_card_encode(card_t card) {
    return (uint8_t)(((card.color & 0x0F) << 4) | (card.value & 0x0F));
}

static inline card_t uno_card_decode(uint8_t encoded) {
    card_t card;
    card.color = (uint8_t)((encoded >> 4) & 0x0F);
    card.value = (uint8_t)(encoded & 0x0F);
    return card;
}

static inline bool uno_card_is_wild(card_t card) {
    return (card.value == UNO_VALUE_WILD || card.value == UNO_VALUE_WILD_DRAW_FOUR);
}

static inline bool uno_card_is_none(card_t card) {
    return (card.value == UNO_CARD_VALUE_NONE);
}

static inline card_t uno_card_none(void) {
    card_t card = { .color = UNO_COLOR_WILD, .value = UNO_CARD_VALUE_NONE };
    return card;
}

typedef enum {
    UNO_MSG_STATE = 1,
    UNO_MSG_HAND_CHUNK = 2,
    UNO_MSG_ACTION = 3,
    UNO_MSG_LOG = 4,
    UNO_MSG_HELLO = 5,
} uno_msg_type_t;

typedef enum {
    UNO_ACTION_PLAY = 1,
    UNO_ACTION_DRAW = 2,
} uno_action_type_t;

typedef enum {
    UNO_LED_MODE_OFF = 0,
    UNO_LED_MODE_TURN_BLINK = 1,
    UNO_LED_MODE_CONNECTED_SOLID = 2,
    UNO_LED_MODE_WILD_CHOICE_FLASH = 3,
} uno_led_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t version;
    uint8_t game_started;
    uint8_t current_player;
    uint8_t direction;
    uint8_t active_color;
    uint8_t top_card;
    uint8_t deck_count;
    uint8_t pending_draw;
    uint8_t player_count;
    uint8_t your_player_id;
    uint8_t your_hand_total;
    uint8_t hand_sizes[UNO_MAX_PLAYERS];
} uno_msg_state_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t version;
    uint8_t offset;
    uint8_t count;
    uint8_t cards[UNO_HAND_CHUNK_CARDS];
} uno_msg_hand_chunk_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t player_id;
    uint8_t action;
    uint8_t card_index;
    uint8_t wild_color;
} uno_msg_action_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t version;
    uint8_t text_len;
    char text[17];
} uno_msg_log_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    char name[UNO_NAME_MAX];
} uno_msg_hello_t;

typedef struct {
    void *obj;
} uno_display_label_t;

typedef struct {
    void *box;
    void *label;
} uno_display_card_t;

void uno_btn_init(void);
void uno_display_init(void);
void uno_display_clear(void);
void uno_display_lock(void);
void uno_display_unlock(void);
void *uno_display_get_root(void);
void uno_display_label_create(uno_display_label_t *label, void *parent, int x, int y, const char *text, bool small_font);
void uno_display_label_set_text(uno_display_label_t *label, const char *text);
void uno_display_card_create(uno_display_card_t *card, void *parent, int x, int y, int w, int h);
void uno_display_card_set(uno_display_card_t *card, card_t value, bool selected);
void uno_display_obj_set_hidden(void *obj, bool hidden);

bool uno_btn_up_pressed(void);
bool uno_btn_down_pressed(void);
bool uno_btn_left_pressed(void);
bool uno_btn_right_pressed(void);
bool uno_btn_play_pressed(void);
bool uno_btn_draw_pressed(void);
bool uno_btn_back_pressed(void);

void uno_led_init(void);
void led_set_top_card(card_t card);
void led_set_selected_card(card_t card);
void set_custom_led(uint8_t mode);
void uno_custom_led_update(void);

#endif /* NUTTY_UNO_COMMON_H */
