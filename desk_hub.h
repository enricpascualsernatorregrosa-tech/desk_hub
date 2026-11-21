#ifndef DESK_HUB_H
#define DESK_HUB_H

#include <stdint.h>
#include <stddef.h>

#define HUB_MAX_HISTORY 16U
#define HUB_RX_INIT_CAP 32U
#define HUB_RX_MAX_CAP  128U

typedef enum {
    POMO_IDLE = 0,
    POMO_FOCUS = 1,
    POMO_BREAK = 2
} PomoMode;

typedef struct {
    uint8_t temp_c;
    uint8_t hum_pct;
    uint8_t light_pct;
} EnvReading;

typedef struct {
    PomoMode mode;
    uint8_t running;
    uint16_t remaining_sec;
    uint8_t last_btn_state;
    uint8_t beep_ticks;
} PomodoroState;

typedef struct {
    EnvReading *history;
    size_t history_count;
    size_t history_cap;
    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;
    PomodoroState pomo;
    uint16_t tick_ms;
} DeskHubSystem;

int desk_hub_init(DeskHubSystem **out_sys);
void desk_hub_step(DeskHubSystem *sys);
void desk_hub_destroy(DeskHubSystem *sys);

#endif