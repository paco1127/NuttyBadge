# NuttyUNO — Multiplayer UNO for NuttyBadge (ESP32-S3)

A complete, production-ready UNO card game implementation for the NuttyBadge
ESP32-S3 hardware, featuring BLE multiplayer (up to 4 players), bot opponents,
full UNO rules, RGB LED feedback, and a full LVGL-based UI.

## Features

- **Full UNO rules**: 108-card deck, all special cards (Skip, Reverse, Draw Two,
  Wild, Wild Draw Four), draw stacking, direction changes, win detection.
- **BLE multiplayer**: One badge acts as host + player 0, up to 3 clients connect.
- **Bot players**: Host can add 0–3 bots to fill up to 4 total players.
- **Real-time sync**: Host broadcasts game state via BLE notifications; clients
  update UI and LEDs instantly.
- **Separate game channels**: Configurable channel (0–255) for multiple
  simultaneous games in the same area.
- **RGB LED feedback**:
  - Bulb 0: Color of the top card on the discard pile.
  - Bulb 1: Color of the currently selected card in your hand.
  - GPIO 5 LED: Blinks when it's your turn, solid when connected, fast flash
    during wild color selection.
- **LVGL UI**: Shows your hand (scrollable), top card, whose turn it is,
  deck count, player card counts, and last action log.

## Hardware

| Resource       | Mapping                             |
| -------------- | ----------------------------------- |
| Display        | SPI LCD via NuttyDisplay (LVGL)     |
| RGB LED bulb 0 | Top card color (via NuttyRGB)       |
| RGB LED bulb 1 | Selected card color (via NuttyRGB)  |
| GPIO 5         | Custom LED (turn blink, status)     |
| Button UP      | Navigate left / scroll hand left    |
| Button DOWN    | Navigate right / scroll hand right  |
| Button LEFT    | Navigate left                       |
| Button RIGHT   | Navigate right                      |
| Button A       | Play selected card / confirm        |
| Button B       | Draw a card                         |
| Button START   | Long-press to exit game             |

## Build & Flash

```bash
# 1. Set target
idf.py set-target esp32s3

# 2. Configure
idf.py menuconfig
#   NuttyUNO → UNO game channel (default: 0)
#   Component config → Bluetooth → NimBLE (must be enabled)

# 3. Build
idf.py build

# 4. Flash & monitor
idf.py flash monitor
```

## How to Play

### Hosting a game
1. Launch **UNO Host** from the NuttyOS app menu.
2. The lobby screen shows connected players and bot count.
3. Press **UP/DOWN** to adjust the number of bot players (0–3).
4. Press **A** to start the game once you have at least 2 players
   (host + bots and/or connected clients).

### Joining a game
1. Launch **UNO Client** on another NuttyBadge.
2. The client automatically scans for `UNO_<channel>` and connects.
3. Wait for the host to start the game.

### In-game controls
| Button       | Action                                    |
| ------------ | ----------------------------------------- |
| UP / LEFT    | Move selection left (previous card)       |
| DOWN / RIGHT | Move selection right (next card)          |
| A            | Play the selected card                    |
| B            | Draw a card                               |
| START (hold) | Exit to menu                              |

When you play a Wild or Wild Draw Four, use UP/DOWN to cycle through colors
(Red → Green → Blue → Yellow) and press A to confirm.

## BLE Protocol

### Advertising
- **Name**: `UNO_<channel>` (e.g., `UNO_0`)
- **Service UUID**: `6E40<channel_hex>01-B5A3-F393-E0A9-E50E24DCCA9E`

### GATT Service
| Characteristic | UUID (suffix) | Properties   | Description                     |
| -------------- | ------------- | ------------ | ------------------------------- |
| State          | `...02`       | Read/Notify  | Game state broadcast to clients |
| Command        | `...03`       | Write        | Client → host actions           |

### Message Types
| Type              | Value | Direction      | Purpose                    |
| ----------------- | ----- | -------------- | -------------------------- |
| `UNO_MSG_STATE`   | 1     | Host → Client  | Full game state snapshot   |
| `UNO_MSG_HAND_CHUNK` | 2  | Host → Client  | Card data for player's hand|
| `UNO_MSG_ACTION`  | 3     | Client → Host  | Play or draw action        |
| `UNO_MSG_LOG`     | 4     | Host → Client  | Last action text           |
| `UNO_MSG_HELLO`   | 5     | Client → Host  | Player name registration   |

## Architecture

```
NuttyOS/
├── main/
│   ├── Kconfig.projbuild          ← GAME_CHANNEL config
│   ├── CMakeLists.txt             ← Registers UNO sources
│   └── apps/NuttyUNO/
│       ├── NuttyUNO.h             ← App entry points (host + client)
│       ├── uno_common.h           ← Shared types, card encoding, BLE UUIDs
│       ├── uno_hal.c              ← Display, button, LED hardware abstraction
│       ├── uno_host_player.c      ← Host + player 0 (GATT server, game logic, bots)
│       └── uno_client.c           ← Client (GATT client, UI, LEDs)
```

### Code Structure

- **`uno_common.h`** — All shared structures, card encoding/decoding, BLE UUID
  generation, message types, and hardware abstraction function declarations.
- **`uno_hal.c`** — Display initialization (LVGL), button input (NuttyInput),
  LED control (NuttyRGB for color LEDs, GPIO for custom LED). Replace this file
  if your hardware differs.
- **`uno_host_player.c`** — Host game logic:
  - GATT server with state notify + command write characteristics.
  - Lobby: accept client connections, configure bots, start game.
  - Game loop: process local input (player 0), process remote actions (queue),
    run bot turns, broadcast state to all clients.
  - Full UNO rules: deck building, shuffling, draw pile refill, card effects,
    win detection.
- **`uno_client.c`** — Client game logic:
  - GATT client: scan, connect, discover service/characteristics, subscribe.
  - Receive state updates and hand chunks via notifications.
  - Send play/draw actions to host.
  - Render UI and update LEDs.

## Memory Usage

- All memory is statically allocated (no heap allocation in game logic).
- Game state: ~4 KB (deck + discard + player hands + metadata).
- BLE buffers: ~1 KB.
- Total RAM estimate: < 10 KB.

## Customization

### Player Names
Edit the `g_custom_player_names` array in `uno_host_player.c`:
```c
static const char *g_custom_player_names[UNO_MAX_PLAYERS] = {
    "Alice", "Bob", NULL, NULL
};
```

### Adjusting BLE Range
In `menuconfig`:
- `Component config → Bluetooth → NimBLE → BLE MAX Connections` (default: 4)
- `Component config → Bluetooth → NimBLE → BLE Host Task Stack Size`

### LED Pin Assignment
The custom LED GPIO is defined in `uno_hal.c`:
```c
#define UNO_LED_CUSTOM_GPIO GPIO_NUM_5
```
The RGB LEDs use the NuttyRGB service (bulb 0 and bulb 1) and don't need
pin configuration.

## Troubleshooting

| Symptom                    | Solution                                      |
| -------------------------- | --------------------------------------------- |
| Client can't find host     | Verify same `GAME_CHANNEL` on both devices    |
| BLE connection drops       | Reduce distance; check `HOST_TASK_STACK_SIZE` |
| Display garbage            | Ensure `NuttyDisplay` is initialized by OS    |
| LEDs not working           | Verify `NuttyRGB_Init()` called by OS         |
| Game won't start           | Need at least 2 players (host + bot/client)   |

## License

MIT — See project LICENSE file.
