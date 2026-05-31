#include "uno_common.h"
#include "NuttyUNO.h"

#include "services/NuttyApps/NuttyApps.h"
#include "services/NuttyDisplay/NuttyDisplay.h"
#include "services/NuttyInput/NuttyInput.h"
#include "lvgl_fonts/cg_pixel_4x5_mono.h"

#include "esp_log.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gattc.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

static const char *TAG = "UNOClient";

#ifdef CONFIG_BT_ENABLED
static void uno_start_scan(void);
#endif

typedef struct {
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
static uno_state_packet_t state;
static bool state_ready = false;
static bool state_dirty = false;
static uint8_t selected_index = 0;
static uint8_t scroll_offset = 0;
static uint8_t wild_choice = UNO_COLOR_RED;
static uint16_t conn_handle = UNO_CONN_HANDLE_NONE;

#ifdef CONFIG_BT_ENABLED
static ble_uuid128_t service_uuid;
static ble_uuid128_t state_uuid;
static ble_uuid128_t action_uuid;
static uint16_t state_handle = 0;
static uint16_t action_handle = 0;
static uint16_t state_ccc_handle = 0;
static bool subscribed = false;
static bool scanning = false;
static uint16_t service_start = 0;
static uint16_t service_end = 0;
#endif

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

static void uno_ui_render_hand(void) {
    uint8_t hand_count = state.local_hand_count;
    if (hand_count == 0) {
        selected_index = 0;
        scroll_offset = 0;
    } else {
        if (selected_index >= hand_count) {
            selected_index = hand_count - 1;
        }
        if (selected_index < scroll_offset) {
            scroll_offset = selected_index;
        }
        if (selected_index >= scroll_offset + UNO_VISIBLE_CARDS) {
            scroll_offset = selected_index - (UNO_VISIBLE_CARDS - 1);
        }
    }

    for (uint8_t i = 0; i < UNO_VISIBLE_CARDS; i++) {
        uint8_t card_index = scroll_offset + i;
        if (card_index < hand_count) {
            card_t card = state.local_hand[card_index];
            uint8_t color = UNO_CARD_COLOR(card);
            char text[6] = {0};
            uno_card_to_text(card, text, sizeof(text));
            lv_obj_clear_flag(ui.hand_cards[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *card_obj = ui.hand_cards[i];
            lv_obj_reset_style_list(card_obj, LV_PART_MAIN);
            if (card_index == selected_index) {
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

    if (scroll_offset > 0) {
        lv_obj_clear_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    }

    if (hand_count > scroll_offset + UNO_VISIBLE_CARDS) {
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
    uno_card_to_text(state.top_card, top_text, sizeof(top_text));
    lv_label_set_text(ui.top_card_label, top_text);
    uint8_t top_color = (UNO_CARD_COLOR(state.top_card) == UNO_COLOR_WILD) ? state.active_color : UNO_CARD_COLOR(state.top_card);
    lv_obj_set_style_bg_color(ui.top_card_box, uno_color_to_lv(top_color), LV_PART_MAIN);

    char turn_text[32];
    snprintf(turn_text, sizeof(turn_text), "Turn:%s", state.player_names[state.current_player]);
    lv_label_set_text(ui.turn_label, turn_text);

    char deck_text[16];
    snprintf(deck_text, sizeof(deck_text), "Deck:%u", state.deck_count);
    lv_label_set_text(ui.deck_label, deck_text);

    char players_text[48] = {0};
    char line[16];
    for (uint8_t i = 0; i < state.player_count; i++) {
        snprintf(line, sizeof(line), "P%u:%u ", i, state.player_hand_counts[i]);
        strncat(players_text, line, sizeof(players_text) - strlen(players_text) - 1);
    }
    lv_label_set_text(ui.players_label, players_text);

    lv_label_set_text(ui.log_label, state.last_action);

    uno_ui_render_hand();

    NuttyDisplay_unlockLVGL();
}

static void uno_ui_show_status(const char *status, const char *detail) {
    NuttyDisplay_lockLVGL();
    lv_label_set_text(ui.turn_label, status);
    lv_label_set_text(ui.deck_label, detail != NULL ? detail : "");
    lv_label_set_text(ui.players_label, "");
    lv_label_set_text(ui.log_label, "");
    lv_label_set_text(ui.top_card_label, "UNO");
    lv_obj_set_style_bg_color(ui.top_card_box, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    for (uint8_t i = 0; i < UNO_VISIBLE_CARDS; i++) {
        lv_obj_add_flag(ui.hand_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(ui.scroll_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.scroll_right, LV_OBJ_FLAG_HIDDEN);
    NuttyDisplay_unlockLVGL();
}

static void uno_send_action(uint8_t type, uint8_t arg0, uint8_t arg1) {
#ifdef CONFIG_BT_ENABLED
    if (conn_handle == UNO_CONN_HANDLE_NONE || action_handle == 0) {
        return;
    }
    uno_action_t action = {
        .type = type,
        .arg0 = arg0,
        .arg1 = arg1
    };
    ble_gattc_write_no_rsp_flat(conn_handle, action_handle, &action, sizeof(action));
#else
    (void)type;
    (void)arg0;
    (void)arg1;
#endif
}

#ifdef CONFIG_BT_ENABLED
static void uno_build_uuid(ble_uuid128_t *uuid, uint8_t channel, uint8_t suffix) {
    uint8_t bytes[16];
    uno_build_uuid_bytes(channel, suffix, bytes);
    uuid->u.type = BLE_UUID_TYPE_128;
    memcpy(uuid->value, bytes, sizeof(bytes));
}

static void uno_reset_discovery(void) {
    service_start = 0;
    service_end = 0;
    state_handle = 0;
    action_handle = 0;
    state_ccc_handle = 0;
    subscribed = false;
}

static int uno_state_notify_cb(uint16_t conn_handle_cb, uint16_t attr_handle,
                               struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle_cb;
    (void)attr_handle;
    (void)error;
    (void)arg;
    if (attr == NULL || attr->om == NULL) {
        return 0;
    }
    if (ble_hs_mbuf_to_flat(attr->om, &state, sizeof(state), NULL) == 0) {
        state_ready = true;
        state_dirty = true;
    }
    return 0;
}

static int uno_disc_dsc_cb(uint16_t conn_handle_cb, const struct ble_gatt_error *error,
                           const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn_handle_cb;
    (void)arg;
    if (error->status == 0 && dsc != NULL) {
        const ble_uuid16_t ccc_uuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);
        if (ble_uuid_cmp(dsc->uuid, &ccc_uuid.u) == 0) {
            state_ccc_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (state_ccc_handle != 0 && conn_handle != UNO_CONN_HANDLE_NONE) {
            ble_gattc_subscribe(conn_handle, state_handle, state_ccc_handle,
                                BLE_GATT_SUBSCRIBE_NOTIFY, uno_state_notify_cb, NULL);
            subscribed = true;
        } else {
        }
    }
    return 0;
}

static int uno_disc_chr_cb(uint16_t conn_handle_cb, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle_cb;
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &state_uuid.u) == 0) {
            state_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &action_uuid.u) == 0) {
            action_handle = chr->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (state_handle != 0) {
            ble_gattc_disc_all_dscs(conn_handle, state_handle, service_end, uno_disc_dsc_cb, NULL);
        } else {
        }
    }
    return 0;
}

static int uno_disc_svc_cb(uint16_t conn_handle_cb, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
    (void)conn_handle_cb;
    (void)arg;
    if (error->status == 0 && service != NULL) {
        service_start = service->start_handle;
        service_end = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (service_start != 0) {
            ble_gattc_disc_all_chrs(conn_handle, service_start, service_end, uno_disc_chr_cb, NULL);
        } else {
        }
    }
    return 0;
}

static int uno_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                scanning = false;
                state_ready = false;
                state_dirty = true;
                uno_reset_discovery();
                ble_gattc_exchange_mtu(conn_handle, NULL, NULL);
                ble_gattc_disc_svc_by_uuid(conn_handle, &service_uuid.u, uno_disc_svc_cb, NULL);
            } else {
                conn_handle = UNO_CONN_HANDLE_NONE;
                scanning = false;
                uno_start_scan();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = UNO_CONN_HANDLE_NONE;
            subscribed = false;
            state_ready = false;
            state_dirty = true;
            scanning = false;
            uno_start_scan();
            break;
        case BLE_GAP_EVENT_DISC: {
            if (event->disc.length_data == 0) {
                break;
            }
            char target[16];
            snprintf(target, sizeof(target), "UNO_%u", (unsigned)CONFIG_GAME_CHANNEL);
            uint8_t target_len = strlen(target);
            for (uint8_t i = 0; i < event->disc.length_data; ) {
                uint8_t len = event->disc.data[i];
                if (len == 0 || i + len >= event->disc.length_data) {
                    break;
                }
                uint8_t type = event->disc.data[i + 1];
                if (type == 0x09 || type == 0x08) {
                    uint8_t name_len = len - 1;
                    const uint8_t *name = &event->disc.data[i + 2];
                    if (name_len == target_len && memcmp(name, target, target_len) == 0) {
                        ESP_LOGI(TAG, "Found host %s", target);
                        ble_gap_disc_cancel();
                        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 30000, NULL, uno_gap_event, NULL);
                        break;
                    }
                }
                i += len + 1;
            }
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            scanning = false;
            break;
        default:
            break;
    }
    return 0;
}

static void uno_start_scan(void) {
    if (scanning) {
        return;
    }
    struct ble_gap_disc_params scan_params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .window = BLE_GAP_SCAN_WIN_MS(50),
        .filter_policy = 0,
        .limited = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &scan_params, uno_gap_event, NULL);
    if (rc == 0) {
        scanning = true;
    } else {
        scanning = false;
    }
}

static void uno_on_sync(void) {
    uno_start_scan();
}

static void uno_ble_client_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

static void uno_client_handle_buttons(void) {
    if (!state_ready || state.phase != UNO_PHASE_PLAYING) {
        return;
    }

    if (state.pending_wild && state.pending_wild_player == state.local_player) {
        if (uno_button_up()) {
            wild_choice = (wild_choice + 3) % 4;
            state_dirty = true;
        }
        if (uno_button_down()) {
            wild_choice = (wild_choice + 1) % 4;
            state_dirty = true;
        }
        if (uno_button_play()) {
            uno_send_action(UNO_ACTION_WILD_COLOR, wild_choice, 0);
        }
        return;
    }

    if (uno_button_up() && state.local_hand_count > 0) {
        if (selected_index > 0) {
            selected_index--;
            state_dirty = true;
        }
    }

    if (uno_button_down() && state.local_hand_count > 0) {
        if (selected_index + 1 < state.local_hand_count) {
            selected_index++;
            state_dirty = true;
        }
    }

    if (uno_button_play() && state.current_player == state.local_player && state.local_hand_count > 0) {
        uno_send_action(UNO_ACTION_PLAY_CARD, state.local_hand[selected_index], 0);
    }

    if (uno_button_draw() && state.current_player == state.local_player) {
        uno_send_action(UNO_ACTION_DRAW, 0, 0);
    }
}

static void uno_update_leds(void) {
    if (!state_ready) {
        led_set_top_card(UNO_CARD_NONE);
        led_set_selected_card(UNO_CARD_NONE);
        set_custom_led(UNO_LED_MODE_OFF);
        return;
    }

    card_t led_card = state.top_card;
    if (UNO_CARD_COLOR(state.top_card) == UNO_COLOR_WILD) {
        led_card = UNO_MAKE_CARD(state.active_color, UNO_CARD_VALUE(state.top_card));
    }
    led_set_top_card(led_card);

    if (state.local_hand_count > 0 && selected_index < state.local_hand_count) {
        led_set_selected_card(state.local_hand[selected_index]);
    } else {
        led_set_selected_card(UNO_CARD_NONE);
    }

    if (state.pending_wild && state.pending_wild_player == state.local_player) {
        set_custom_led(UNO_LED_MODE_WILD_CHOICE_FLASH);
    } else if (state.phase == UNO_PHASE_PLAYING && state.current_player == state.local_player) {
        set_custom_led(UNO_LED_MODE_TURN_BLINK);
    } else if (conn_handle != UNO_CONN_HANDLE_NONE) {
        set_custom_led(UNO_LED_MODE_CONNECTED_SOLID);
    } else {
        set_custom_led(UNO_LED_MODE_OFF);
    }
}

static void nutty_main(void) {
    ESP_LOGI(TAG, "UNO Client start");
    uno_led_init();
    uno_ui_init();
    uno_ui_show_status("UNO Client", "Scanning...");

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
    snprintf(dev_name, sizeof(dev_name), "UNOClient_%u", (unsigned)CONFIG_GAME_CHANNEL);
    ble_svc_gap_device_name_set(dev_name);

    uno_build_uuid(&service_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_SERVICE);
    uno_build_uuid(&state_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_STATE);
    uno_build_uuid(&action_uuid, CONFIG_GAME_CHANNEL, UNO_UUID_SUFFIX_ACTION);

    ble_hs_cfg.sync_cb = uno_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_att_set_preferred_mtu(247);

    if (nimble_ret == ESP_OK) {
        nimble_port_freertos_init(uno_ble_client_task);
    }
#else
    ESP_LOGW(TAG, "Bluetooth not enabled");
#endif

    while (1) {
        if (uno_button_exit()) {
            break;
        }

#ifdef CONFIG_BT_ENABLED
        if (conn_handle == UNO_CONN_HANDLE_NONE && !scanning) {
            uno_start_scan();
        }
        if (conn_handle == UNO_CONN_HANDLE_NONE) {
            uno_ui_show_status("UNO Client", "Scanning...");
        }
#endif

        uno_client_handle_buttons();

        if (state_ready && state_dirty) {
            if (state.phase == UNO_PHASE_GAME_OVER) {
                char text[48];
                snprintf(text, sizeof(text), "Winner: %s", state.player_names[state.winner]);
                uno_ui_show_status(text, state.last_action);
            } else {
                uno_ui_update();
            }
            state_dirty = false;
        }

        uno_update_leds();
        uno_led_tick();

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    NuttyDisplay_clearUserAppArea();
    NuttyApps_launchAppByIndex(0);
}

NuttyAppDefinition NuttyUNOClient = {
    .appName = "UNO Client",
    .appMainEntry = nutty_main,
    .appHidden = false
};
