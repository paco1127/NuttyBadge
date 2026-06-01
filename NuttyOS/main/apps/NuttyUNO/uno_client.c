#include "apps/NuttyUNO/NuttyUNO.h"
#include "apps/NuttyUNO/uno_common.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

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

static const char *TAG = "UNOClient";

typedef struct {
    bool connected;
    bool ready;
    bool game_started;
    uint16_t conn_handle;
    uint16_t state_handle;
    uint16_t cmd_handle;
    uint8_t player_id;
    uint8_t player_count;
    uint8_t current_player;
    int8_t direction;
    uno_color_t active_color;
    card_t top_card;
    uint8_t deck_count;
    uint8_t pending_draw;
    uint8_t hand_sizes[UNO_MAX_PLAYERS];
    uint8_t hand_total;
    card_t hand[UNO_MAX_HAND];
    uint8_t state_version;
    char last_action[UNO_LAST_ACTION_MAX];
    bool ui_dirty;
} uno_client_state_t;

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
    uint8_t pending_wild_index;
} uno_ui_t;

static uno_client_state_t g_client;
static uno_ui_t g_ui;

#ifdef CONFIG_BT_ENABLED
static ble_uuid128_t g_uno_svc_uuid;
static ble_uuid128_t g_uno_state_uuid;
static ble_uuid128_t g_uno_cmd_uuid;
static uint16_t g_svc_start_handle = 0;
static uint16_t g_svc_end_handle = 0;
static uint8_t g_own_addr_type;

static int uno_gap_event(struct ble_gap_event *event, void *arg);
#endif

static const char *g_client_name = NULL;
static void uno_client_send_action(uint8_t action, uint8_t card_index, uint8_t wild_color);

static void uno_client_set_last_action(const char *text) {
    if (text == NULL) {
        g_client.last_action[0] = '\0';
    } else {
        snprintf(g_client.last_action, sizeof(g_client.last_action), "%s", text);
    }
    g_client.ui_dirty = true;
}

static void uno_client_init_state(void) {
    memset(&g_client, 0, sizeof(g_client));
    g_client.conn_handle = 0xFFFF;
    g_client.state_handle = 0;
    g_client.cmd_handle = 0;
    g_client.player_id = 0xFF;
    g_client.top_card = uno_card_none();
    g_client.state_version = 0;
    g_client.ui_dirty = true;
}

static void uno_build_players_label(char *out, size_t out_len) {
    size_t offset = 0;
    out[0] = '\0';

    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        if (g_client.player_count == 0) {
            break;
        }
        int wrote = snprintf(out + offset, out_len - offset, "P%u:%u\n", (unsigned)i, (unsigned)g_client.hand_sizes[i]);
        if (wrote > 0) {
            offset += (size_t)wrote;
        }
        if (offset >= out_len) {
            break;
        }
    }
}

static void uno_ui_init(void) {
    uno_display_init();
    g_ui.root = uno_display_get_root();

    uno_display_lock();
    uno_display_clear();

    uno_display_label_create(&g_ui.title_label, g_ui.root, 2, 0, "UNO Client", false);
    uno_display_label_create(&g_ui.deck_label, g_ui.root, 80, 0, "Deck: 0", false);
    uno_display_label_create(&g_ui.players_label, g_ui.root, 40, 10, "", true);
    uno_display_label_create(&g_ui.log_label, g_ui.root, 2, 28, "", true);
    uno_display_label_create(&g_ui.wild_label, g_ui.root, 2, 22, "", true);
    uno_display_label_create(&g_ui.left_arrow, g_ui.root, 2, 46, "<", true);
    uno_display_label_create(&g_ui.right_arrow, g_ui.root, 120, 46, ">", true);

    uno_display_card_create(&g_ui.top_card, g_ui.root, 2, 8, 30, 18);

    int card_y = 40;
    int card_x = 12;
    for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
        uno_display_card_create(&g_ui.hand_cards[i], g_ui.root, card_x, card_y, UNO_CARD_W, UNO_CARD_H);
        card_x += UNO_CARD_W + UNO_CARD_SPACING;
    }

    uno_display_unlock();

    g_ui.initialized = true;
    g_ui.selected_index = 0;
    g_ui.scroll_offset = 0;
    g_ui.wild_select_active = false;
    g_ui.wild_color = UNO_COLOR_RED;
    g_ui.pending_wild_index = 0;
}

static void uno_ui_update(void) {
    if (!g_ui.initialized) {
        return;
    }

    char title_text[32];
    char deck_text[16];
    char players_text[96];

    uno_display_lock();

    if (!g_client.connected) {
        snprintf(title_text, sizeof(title_text), "Scanning...");
        uno_display_label_set_text(&g_ui.title_label, title_text);
        uno_display_label_set_text(&g_ui.deck_label, "");
        uno_display_label_set_text(&g_ui.players_label, "");
        uno_display_label_set_text(&g_ui.log_label, "");
        uno_display_label_set_text(&g_ui.wild_label, "");
        uno_display_card_set(&g_ui.top_card, uno_card_none(), false);
        for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
            uno_display_card_set(&g_ui.hand_cards[i], uno_card_none(), false);
        }
        uno_display_obj_set_hidden(g_ui.left_arrow.obj, true);
        uno_display_obj_set_hidden(g_ui.right_arrow.obj, true);
        uno_display_unlock();
        led_set_selected_card(uno_card_none());
        g_client.ui_dirty = false;
        return;
    }

    if (!g_client.game_started) {
        snprintf(title_text, sizeof(title_text), "Waiting for host");
        uno_display_label_set_text(&g_ui.title_label, title_text);
        uno_display_label_set_text(&g_ui.deck_label, "");
        uno_display_label_set_text(&g_ui.players_label, "");
        uno_display_label_set_text(&g_ui.log_label, "");
        uno_display_label_set_text(&g_ui.wild_label, "");
        uno_display_card_set(&g_ui.top_card, uno_card_none(), false);
        for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
            uno_display_card_set(&g_ui.hand_cards[i], uno_card_none(), false);
        }
        uno_display_obj_set_hidden(g_ui.left_arrow.obj, true);
        uno_display_obj_set_hidden(g_ui.right_arrow.obj, true);
        uno_display_unlock();
        led_set_selected_card(uno_card_none());
        g_client.ui_dirty = false;
        return;
    }

    snprintf(title_text, sizeof(title_text), "Turn: P%u", (unsigned)g_client.current_player);
    snprintf(deck_text, sizeof(deck_text), "Deck: %u", (unsigned)g_client.deck_count);
    uno_build_players_label(players_text, sizeof(players_text));

    uno_display_label_set_text(&g_ui.title_label, title_text);
    uno_display_label_set_text(&g_ui.deck_label, deck_text);
    uno_display_label_set_text(&g_ui.players_label, players_text);
    uno_display_label_set_text(&g_ui.log_label, g_client.last_action);

    if (g_ui.wild_select_active) {
        const char *color = "RED";
        if (g_ui.wild_color == UNO_COLOR_GREEN) color = "GREEN";
        if (g_ui.wild_color == UNO_COLOR_BLUE) color = "BLUE";
        if (g_ui.wild_color == UNO_COLOR_YELLOW) color = "YELLOW";
        char wild_text[16];
        snprintf(wild_text, sizeof(wild_text), "Wild: %s", color);
        uno_display_label_set_text(&g_ui.wild_label, wild_text);
    } else {
        uno_display_label_set_text(&g_ui.wild_label, "");
    }

    uno_display_card_set(&g_ui.top_card, g_client.top_card, false);

    if (g_client.hand_total == 0) {
        g_ui.selected_index = 0;
        g_ui.scroll_offset = 0;
    } else if (g_ui.selected_index >= g_client.hand_total) {
        g_ui.selected_index = (uint8_t)(g_client.hand_total - 1);
    }

    if (g_ui.selected_index < g_ui.scroll_offset) {
        g_ui.scroll_offset = g_ui.selected_index;
    }
    if (g_ui.selected_index >= (uint8_t)(g_ui.scroll_offset + UNO_HAND_VISIBLE)) {
        g_ui.scroll_offset = (uint8_t)(g_ui.selected_index - (UNO_HAND_VISIBLE - 1));
    }

    for (uint8_t i = 0; i < UNO_HAND_VISIBLE; i++) {
        uint8_t idx = (uint8_t)(g_ui.scroll_offset + i);
        if (idx < g_client.hand_total) {
            bool selected = (idx == g_ui.selected_index);
            uno_display_card_set(&g_ui.hand_cards[i], g_client.hand[idx], selected);
        } else {
            uno_display_card_set(&g_ui.hand_cards[i], uno_card_none(), false);
        }
    }

    uno_display_obj_set_hidden(g_ui.left_arrow.obj, g_ui.scroll_offset == 0);
    uno_display_obj_set_hidden(g_ui.right_arrow.obj, (g_ui.scroll_offset + UNO_HAND_VISIBLE) >= g_client.hand_total);

    uno_display_unlock();

    if (g_client.hand_total > 0) {
        led_set_selected_card(g_client.hand[g_ui.selected_index]);
    } else {
        led_set_selected_card(uno_card_none());
    }

    g_client.ui_dirty = false;
}

static bool uno_client_is_my_turn(void) {
    return (g_client.game_started && g_client.player_id != 0xFF && g_client.player_id == g_client.current_player);
}

#ifdef CONFIG_BT_ENABLED
static void uno_ble_uuid128_init(ble_uuid128_t *uuid, uint8_t suffix) {
    *uuid = (ble_uuid128_t)BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24,
        0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5,
        suffix, UNO_GAME_CHANNEL, 0x40, 0x6e);
}

static void uno_client_send_hello(void) {
    if (!g_client.ready || g_client.cmd_handle == 0) {
        return;
    }

    uno_msg_hello_t hello = {0};
    hello.msg_type = UNO_MSG_HELLO;
    const char *name = g_client_name ? g_client_name : "Player";
    snprintf(hello.name, sizeof(hello.name), "%s", name);
    ble_gattc_write_flat(g_client.conn_handle, g_client.cmd_handle, &hello, sizeof(hello), NULL, NULL);
}

static void uno_client_send_action(uint8_t action, uint8_t card_index, uint8_t wild_color) {
    if (!g_client.ready || g_client.cmd_handle == 0) {
        return;
    }

    uno_msg_action_t msg = {
        .msg_type = UNO_MSG_ACTION,
        .player_id = g_client.player_id,
        .action = action,
        .card_index = card_index,
        .wild_color = wild_color
    };
    ble_gattc_write_flat(g_client.conn_handle, g_client.cmd_handle, &msg, sizeof(msg), NULL, NULL);
}

static void uno_client_handle_state(const uno_msg_state_t *state) {
    g_client.game_started = state->game_started != 0;
    g_client.current_player = state->current_player;
    g_client.direction = state->direction ? -1 : 1;
    g_client.active_color = (uno_color_t)state->active_color;
    g_client.top_card = uno_card_decode(state->top_card);
    g_client.deck_count = state->deck_count;
    g_client.pending_draw = state->pending_draw;
    g_client.player_count = state->player_count;
    g_client.player_id = state->your_player_id;
    g_client.hand_total = state->your_hand_total;
    for (uint8_t i = 0; i < UNO_MAX_PLAYERS; i++) {
        g_client.hand_sizes[i] = state->hand_sizes[i];
    }

    if (g_client.state_version != state->version) {
        g_client.state_version = state->version;
        for (uint8_t i = 0; i < g_client.hand_total; i++) {
            g_client.hand[i] = uno_card_none();
        }
    }

    led_set_top_card(g_client.top_card);
    g_client.ui_dirty = true;
}

static void uno_client_handle_hand_chunk(const uno_msg_hand_chunk_t *chunk) {
    if (chunk->version != g_client.state_version) {
        return;
    }

    if (chunk->offset >= UNO_MAX_HAND) {
        return;
    }

    uint8_t count = chunk->count;
    if (count > UNO_HAND_CHUNK_CARDS) {
        count = UNO_HAND_CHUNK_CARDS;
    }
    if ((uint16_t)(chunk->offset + count) > UNO_MAX_HAND) {
        count = (uint8_t)(UNO_MAX_HAND - chunk->offset);
    }

    for (uint8_t i = 0; i < count; i++) {
        g_client.hand[chunk->offset + i] = uno_card_decode(chunk->cards[i]);
    }
    g_client.ui_dirty = true;
}

static void uno_client_handle_log(const uno_msg_log_t *log_msg) {
    uint8_t len = log_msg->text_len;
    if (len > sizeof(log_msg->text)) {
        len = sizeof(log_msg->text);
    }
    memcpy(g_client.last_action, log_msg->text, len);
    g_client.last_action[len] = '\0';
    g_client.ui_dirty = true;
}

static bool uno_adv_name_matches(const struct ble_gap_disc_desc *disc) {
    const uint8_t *data = disc->data;
    uint8_t data_len = disc->length_data;
    uint8_t pos = 0;

    char target[16];
    snprintf(target, sizeof(target), "UNO_%u", (unsigned)UNO_GAME_CHANNEL);

    while (pos < data_len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || (pos + field_len) >= data_len) {
            break;
        }
        uint8_t field_type = data[pos + 1];
        if (field_type == 0x09 || field_type == 0x08) {
            const uint8_t *name_data = &data[pos + 2];
            uint8_t name_len = field_len - 1;
            if (name_len == strlen(target) && memcmp(name_data, target, name_len) == 0) {
                return true;
            }
        }
        pos += field_len + 1;
    }
    return false;
}

static int uno_chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    if (error->status == 0) {
        if (ble_uuid_cmp(chr->uuid, &g_uno_state_uuid.u) == 0) {
            g_client.state_handle = chr->val_handle;
        } else if (ble_uuid_cmp(chr->uuid, &g_uno_cmd_uuid.u) == 0) {
            g_client.cmd_handle = chr->val_handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (g_client.state_handle != 0) {
            uint16_t cccd = (uint16_t)(g_client.state_handle + 1);
            uint16_t notify = 0x0001;
            ble_gattc_write_flat(conn_handle, cccd, &notify, sizeof(notify), NULL, NULL);
            g_client.ready = true;
            uno_client_send_hello();
        }
    }
    return 0;
}

static int uno_svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    if (error->status == 0 && svc != NULL) {
        g_svc_start_handle = svc->start_handle;
        g_svc_end_handle = svc->end_handle;
        ble_gattc_disc_all_chrs(conn_handle, g_svc_start_handle, g_svc_end_handle, uno_chr_disc_cb, NULL);
        return 0;
    }
    return error->status == BLE_HS_EDONE ? 0 : error->status;
}

static void uno_start_scan(void) {
    struct ble_gap_disc_params scan_params;
    memset(&scan_params, 0, sizeof(scan_params));
    scan_params.itvl = BLE_GAP_SCAN_ITVL_MS(100);
    scan_params.window = BLE_GAP_SCAN_WIN_MS(50);
    scan_params.filter_duplicates = 1;

    ble_gap_disc(g_own_addr_type, BLE_HS_FOREVER, &scan_params, uno_gap_event, NULL);
}

static int uno_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (!g_client.connected && uno_adv_name_matches(&event->disc)) {
                ble_gap_disc_cancel();
                ble_gap_connect(g_own_addr_type, &event->disc.addr, 30000, NULL, uno_gap_event, NULL);
            }
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (!g_client.connected) {
                uno_start_scan();
            }
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                g_client.connected = false;
                uno_start_scan();
                return 0;
            }
            g_client.connected = true;
            g_client.conn_handle = event->connect.conn_handle;
            g_client.ready = false;
            g_client.state_handle = 0;
            g_client.cmd_handle = 0;
            ble_gattc_disc_svc_by_uuid(g_client.conn_handle, &g_uno_svc_uuid.u, uno_svc_disc_cb, NULL);
            g_client.ui_dirty = true;
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            g_client.connected = false;
            g_client.ready = false;
            g_client.conn_handle = 0xFFFF;
            g_client.ui_dirty = true;
            uno_start_scan();
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint8_t buf[32];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) != 0 || len == 0) {
                return 0;
            }

            uint8_t msg_type = buf[0];
            if (msg_type == UNO_MSG_STATE && len >= sizeof(uno_msg_state_t)) {
                uno_msg_state_t state;
                memcpy(&state, buf, sizeof(state));
                uno_client_handle_state(&state);
            } else if (msg_type == UNO_MSG_HAND_CHUNK && len >= sizeof(uno_msg_hand_chunk_t)) {
                uno_msg_hand_chunk_t chunk;
                memcpy(&chunk, buf, sizeof(chunk));
                uno_client_handle_hand_chunk(&chunk);
            } else if (msg_type == UNO_MSG_LOG && len >= sizeof(uno_msg_log_t)) {
                uno_msg_log_t log_msg;
                memcpy(&log_msg, buf, sizeof(log_msg));
                uno_client_handle_log(&log_msg);
            }
            return 0;
        }
        default:
            return 0;
    }
}

static void uno_ble_on_sync(void) {
    ble_hs_id_infer_auto(0, &g_own_addr_type);
    uno_start_scan();
}

static void uno_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void uno_ble_init(void) {
    esp_err_t nimble_ret = nimble_port_init();
    if (nimble_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "NimBLE already initialized, continuing...");
    } else if (nimble_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(nimble_ret));
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    uno_ble_uuid128_init(&g_uno_svc_uuid, 0x01);
    uno_ble_uuid128_init(&g_uno_state_uuid, 0x02);
    uno_ble_uuid128_init(&g_uno_cmd_uuid, 0x03);

    ble_hs_cfg.sync_cb = uno_ble_on_sync;
    nimble_port_freertos_init(uno_ble_host_task);

    if (nimble_ret == ESP_ERR_INVALID_STATE) {
        uno_start_scan();
    }
}
#else
static void uno_client_send_action(uint8_t action, uint8_t card_index, uint8_t wild_color) {
    (void)action;
    (void)card_index;
    (void)wild_color;
}
#endif

static void uno_handle_wild_select_input(void) {
    if (uno_btn_up_pressed() || uno_btn_left_pressed()) {
        g_ui.wild_color = (uint8_t)((g_ui.wild_color + 3) % 4);
        g_client.ui_dirty = true;
    }

    if (uno_btn_down_pressed() || uno_btn_right_pressed()) {
        g_ui.wild_color = (uint8_t)((g_ui.wild_color + 1) % 4);
        g_client.ui_dirty = true;
    }

    if (uno_btn_play_pressed()) {
        g_ui.wild_select_active = false;
        uno_client_send_action(UNO_ACTION_PLAY, g_ui.pending_wild_index, g_ui.wild_color);
        g_client.ui_dirty = true;
    }

    if (uno_btn_draw_pressed()) {
        g_ui.wild_select_active = false;
        g_client.ui_dirty = true;
    }
}

static void uno_handle_input(void) {
    if (!g_client.connected || !g_client.game_started) {
        return;
    }

    if (g_ui.wild_select_active) {
        uno_handle_wild_select_input();
        return;
    }

    if (g_client.hand_total > 0) {
        if (uno_btn_up_pressed() || uno_btn_left_pressed()) {
            if (g_ui.selected_index > 0) {
                g_ui.selected_index--;
                g_client.ui_dirty = true;
            }
        }

        if (uno_btn_down_pressed() || uno_btn_right_pressed()) {
            if (g_ui.selected_index + 1 < g_client.hand_total) {
                g_ui.selected_index++;
                g_client.ui_dirty = true;
            }
        }
    }

    if (uno_btn_play_pressed() && uno_client_is_my_turn()) {
        if (g_client.hand_total == 0) {
            return;
        }
        card_t card = g_client.hand[g_ui.selected_index];
        if (uno_card_is_wild(card)) {
            g_ui.wild_select_active = true;
            g_ui.wild_color = UNO_COLOR_RED;
            g_ui.pending_wild_index = g_ui.selected_index;
            set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
            g_client.ui_dirty = true;
        } else {
            uno_client_send_action(UNO_ACTION_PLAY, g_ui.selected_index, UNO_COLOR_RED);
        }
    }

    if (uno_btn_draw_pressed() && uno_client_is_my_turn()) {
        uno_client_send_action(UNO_ACTION_DRAW, 0, 0);
    }
}

static void uno_update_custom_led(void) {
    if (g_ui.wild_select_active) {
        set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
    } else if (uno_client_is_my_turn()) {
        set_custom_led(UNO_LED_MODE_TURN_BLINK);
    } else if (g_client.connected) {
        set_custom_led(UNO_LED_MODE_CONNECTED_SOLID);
    } else {
        set_custom_led(UNO_LED_MODE_OFF);
    }
}

static void uno_client_main(void) {
    ESP_LOGI(TAG, "Starting UNO Client");

    uno_led_init();
    uno_display_init();
    NuttyInput_clearButtonHoldState(NUTTYINPUT_BTN_ALL);

    uno_client_init_state();
    uno_ui_init();
    uno_ui_update();

#ifdef CONFIG_BT_ENABLED
    uno_ble_init();
#else
    uno_client_set_last_action("BT disabled");
#endif

    while (1) {
        if (uno_btn_back_pressed()) {
            break;
        }

        uno_handle_input();
        uno_update_custom_led();
        uno_custom_led_update();

        if (g_client.ui_dirty) {
            uno_ui_update();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uno_display_clear();
    NuttyApps_launchAppByIndex(0);
}

NuttyAppDefinition NuttyUNOClient = {
    .appName = "UNO Client",
    .appMainEntry = uno_client_main,
    .appHidden = false
};
