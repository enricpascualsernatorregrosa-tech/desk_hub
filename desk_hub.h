#ifndef DESK_HUB_H
#define DESK_HUB_H

#include <stdint.h>

typedef enum {
    POMO_MODE_50_10 = 0,
    POMO_MODE_25_5  = 1
} PomoMode;

typedef enum {
    POMO_IDLE  = 0,
    POMO_FOCUS = 1,
    POMO_BREAK = 2
} PomoState;

typedef enum {
    LCD_SCR_CLIMATE = 0, /* [1/4] Temperatura, humedad, luz y Pomodoro */
    LCD_SCR_MUSIC   = 1, /* [2/4] Cancion o canal actual en la tablet  */
    LCD_SCR_EVENT   = 2, /* [3/4] Proximo evento o tarea pendiente     */
    LCD_SCR_STATS   = 3  /* [4/4] Sesiones EEPROM y min/max termico    */
} LcdScreen;

typedef struct {
    uint8_t temp_c;
    uint8_t hum_pct;
    uint8_t lux_pct;
} SensorSample;

typedef struct {
    SensorSample current;
    SensorSample *history;
    uint16_t history_count;
    uint16_t history_cap;
    uint8_t temp_min;
    uint8_t temp_max;
    uint16_t eeprom_sessions;

    uint16_t last_tcnt;
    uint16_t tick_accum;
    uint32_t uptime_ms;
    uint32_t last_sensor_ms;
    uint32_t last_lcd_ms;
    uint32_t last_pomo_tick_ms;

    PomoMode pomo_mode;
    PomoState pomo_state;
    uint8_t pomo_running;
    uint16_t pomo_seconds_left;

    LcdScreen lcd_screen;
    uint8_t backlight_on;
    uint8_t btn_was_pressed;
    uint8_t btn_stage;
    uint8_t btn_click_count;
    uint32_t btn_press_start_ms;
    uint32_t btn_last_release_ms;

    char song_title[17];
    char event_title[17];
    char rx_buf[40];
    uint8_t rx_len;
} DeskHubSystem;

DeskHubSystem *desk_hub_create(void);
void desk_hub_destroy(DeskHubSystem *sys);
void desk_hub_step(DeskHubSystem *sys);
void desk_hub_run(void);

#endif
