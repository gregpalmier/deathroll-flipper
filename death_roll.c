#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <inttypes.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

// --- Game Configuration ---

#define STARTING_MAX     1000
#define HISTORY_SIZE     6
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define ANIM_DURATION_MS 600
#define ANIM_TICK_MS     50

// --- Types ---

typedef enum {
    StateMenu,
    StateRolling,   // dice animation in progress
    StateResult,    // showing last roll, waiting for next player
    StateGameOver,
} GameState;

typedef struct {
    uint32_t value;
    uint8_t  player; // 1 or 2
} RollEntry;

typedef struct {
    GameState state;
    uint8_t   current_player; // 1 or 2
    uint32_t  current_max;    // upper bound for next roll
    uint32_t  last_roll;      // result of the most recent roll
    uint32_t  total_rolls;    // total number of rolls in the game
    uint8_t   loser;          // which player lost (1 or 2)

    // Rolling animation
    uint32_t anim_value;      // value shown during animation
    uint32_t anim_ticks_left; // remaining animation ticks

    // Roll history (ring buffer, newest at head)
    RollEntry history[HISTORY_SIZE];
    uint8_t   history_count;

    // Menu cursor
    uint8_t menu_index; // 0 = Play, 1 = Quit

    FuriMutex* mutex;
} DeathRollState;

// --- Helpers ---

// Generate a random number in [1, max] using hardware RNG
static uint32_t random_roll(uint32_t max) {
    if(max <= 1) return 1;
    uint32_t raw;
    furi_hal_random_fill_buf((uint8_t*)&raw, sizeof(raw));
    return (raw % max) + 1;
}

static void push_history(DeathRollState* s, uint32_t value, uint8_t player) {
    // Shift entries down
    uint8_t cap = HISTORY_SIZE;
    if(s->history_count < cap) s->history_count++;
    for(int i = (int)s->history_count - 1; i > 0; i--) {
        s->history[i] = s->history[i - 1];
    }
    s->history[0].value = value;
    s->history[0].player = player;
}

static void game_reset(DeathRollState* s) {
    s->state = StateResult;
    s->current_player = 1;
    s->current_max = STARTING_MAX;
    s->last_roll = STARTING_MAX;
    s->total_rolls = 0;
    s->loser = 0;
    s->history_count = 0;
    s->anim_ticks_left = 0;
    // Show initial prompt — "P1 rolls 1‑1000"
    s->state = StateResult;
}

// --- Drawing ---

static void draw_menu(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignTop, "DEATH ROLL");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignTop, "WoW-style dice duel");

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, "Press OK to Play");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignTop, "Press Back to Quit");
}

static void draw_game(Canvas* canvas, DeathRollState* s) {
    char buf[48];

    // ---- Top bar: roll count + range ----
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Rolls: %" PRIu32, s->total_rolls);
    canvas_draw_str(canvas, 2, 10, buf);

    snprintf(buf, sizeof(buf), "1-%" PRIu32, s->current_max);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);

    canvas_draw_line(canvas, 0, 12, SCREEN_WIDTH, 12);

    if(s->state == StateRolling) {
        // Animated tumbling number
        canvas_set_font(canvas, FontPrimary);
        snprintf(
            buf,
            sizeof(buf),
            "P%" PRIu8 " rolling...",
            s->current_player);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, buf);

        canvas_set_font(canvas, FontBigNumbers);
        snprintf(buf, sizeof(buf), "%" PRIu32, s->anim_value);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignTop, buf);
    } else {
        // StateResult: show whose turn it is and prompt
        canvas_set_font(canvas, FontPrimary);
        if(s->total_rolls == 0) {
            snprintf(buf, sizeof(buf), "P1: Press OK to start!");
        } else {
            snprintf(
                buf,
                sizeof(buf),
                "P%" PRIu8 " rolled %" PRIu32,
                (s->current_player == 1) ? 2 : 1,
                s->last_roll);
        }
        canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, buf);

        // History list
        canvas_set_font(canvas, FontSecondary);
        uint8_t y = 30;
        uint8_t shown = (s->history_count < 4) ? s->history_count : 4;
        for(uint8_t i = 0; i < shown; i++) {
            snprintf(
                buf,
                sizeof(buf),
                "#%" PRIu32 "  P%" PRIu8 ": %" PRIu32,
                s->total_rolls - i,
                s->history[i].player,
                s->history[i].value);
            canvas_draw_str(canvas, 4, y, buf);
            y += 9;
        }

        // Bottom prompt
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            buf,
            sizeof(buf),
            "P%" PRIu8 ": press OK to roll",
            s->current_player);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, buf);
    }
}

static void draw_game_over(Canvas* canvas, DeathRollState* s) {
    char buf[48];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "GAME OVER");

    canvas_set_font(canvas, FontPrimary);
    snprintf(buf, sizeof(buf), "P%" PRIu8 " rolled a 1!", s->loser);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, buf);

    uint8_t winner = (s->loser == 1) ? 2 : 1;
    snprintf(buf, sizeof(buf), "P%" PRIu8 " wins!", winner);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignTop, buf);

    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Total rolls: %" PRIu32, s->total_rolls);
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignTop, buf);

    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "OK: Again  Back: Menu");
}

static void death_roll_draw_callback(Canvas* canvas, void* ctx) {
    DeathRollState* s = ctx;
    furi_mutex_acquire(s->mutex, FuriWaitForever);

    canvas_clear(canvas);

    switch(s->state) {
    case StateMenu:
        draw_menu(canvas);
        break;
    case StateRolling:
    case StateResult:
        draw_game(canvas, s);
        break;
    case StateGameOver:
        draw_game_over(canvas, s);
        break;
    }

    furi_mutex_release(s->mutex);
}

// --- Input ---

static void death_roll_input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

// --- Main ---

int32_t death_roll_app(void* p) {
    UNUSED(p);

    // Allocate state
    DeathRollState* state = malloc(sizeof(DeathRollState));
    memset(state, 0, sizeof(DeathRollState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->state = StateMenu;

    // Message queue for input events
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Set up GUI
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, death_roll_draw_callback, state);
    view_port_input_callback_set(view_port, death_roll_input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    // Main loop
    bool running = true;
    InputEvent event;

    while(running) {
        FuriStatus status;

        if(state->state == StateRolling) {
            // During animation, poll with short timeout
            status = furi_message_queue_get(event_queue, &event, ANIM_TICK_MS);

            furi_mutex_acquire(state->mutex, FuriWaitForever);
            if(state->anim_ticks_left > 0) {
                state->anim_ticks_left--;
                state->anim_value = random_roll(state->current_max);
                if(state->anim_ticks_left == 0) {
                    // Animation finished — commit roll
                    uint32_t roll = random_roll(state->current_max);
                    state->last_roll = roll;
                    state->total_rolls++;
                    push_history(state, roll, state->current_player);

                    if(roll == 1) {
                        state->loser = state->current_player;
                        state->state = StateGameOver;
                        notification_message(notifications, &sequence_error);
                    } else {
                        state->current_max = roll;
                        state->current_player = (state->current_player == 1) ? 2 : 1;
                        state->state = StateResult;
                        notification_message(notifications, &sequence_single_vibro);
                    }
                }
            }
            furi_mutex_release(state->mutex);
            view_port_update(view_port);

            if(status != FuriStatusOk) continue; // timeout, no input
        } else {
            status = furi_message_queue_get(event_queue, &event, FuriWaitForever);
            if(status != FuriStatusOk) continue;
        }

        // Only handle short presses
        if(event.type != InputTypeShort) continue;

        furi_mutex_acquire(state->mutex, FuriWaitForever);

        switch(state->state) {
        case StateMenu:
            if(event.key == InputKeyOk) {
                game_reset(state);
            } else if(event.key == InputKeyBack) {
                running = false;
            }
            break;

        case StateResult:
            if(event.key == InputKeyOk) {
                // Start roll animation
                state->state = StateRolling;
                state->anim_ticks_left = ANIM_DURATION_MS / ANIM_TICK_MS;
                state->anim_value = random_roll(state->current_max);
            } else if(event.key == InputKeyBack) {
                state->state = StateMenu;
            }
            break;

        case StateRolling:
            // Ignore input during animation
            break;

        case StateGameOver:
            if(event.key == InputKeyOk) {
                game_reset(state);
            } else if(event.key == InputKeyBack) {
                state->state = StateMenu;
            }
            break;
        }

        furi_mutex_release(state->mutex);
        view_port_update(view_port);
    }

    // Cleanup
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
