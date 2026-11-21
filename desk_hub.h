#ifndef DESK_HUB_H
#define DESK_HUB_H

#include <stdint.h>

typedef enum {
    POMO_IDLE = 0,
    POMO_FOCUS = 1,
    POMO_BREAK = 2
} PomoState;

typedef struct {
    uint8_t temp_c;
    uint8_t hum_pct;
    uint8_t lux_pct;
} SensorSample;

typedef struct {
    /* Sensores e historial dinámico */
    SensorSample current;
    SensorSample *history;
    uint16_t history_count;
    uint16_t history_cap;

    /* Temporizador hardware (sin variables globales) */
    uint16_t last_tcnt;
    uint16_t tick_accum;
    uint32_t uptime_ms;
    uint32_t last_sensor_ms;
    uint32_t last_lcd_ms;
    uint32_t last_pomo_tick_ms;

    /* Estado Pomodoro */
    PomoState pomo_state;
    uint8_t pomo_running;
    uint16_t pomo_seconds_left;

    /* Pantalla y pulsador D3 */
    uint8_t backlight_on;
    uint8_t btn_was_pressed;
    uint8_t btn_long_handled;
    uint32_t btn_press_start_ms;
} DeskHubSystem;

DeskHubSystem *desk_hub_create(void);
void desk_hub_destroy(DeskHubSystem *sys);
void desk_hub_step(DeskHubSystem *sys);
void desk_hub_run(void);

#endif
