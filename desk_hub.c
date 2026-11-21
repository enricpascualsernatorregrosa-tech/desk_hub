#include "desk_hub.h"
#include <avr/io.h>
#include <util/delay.h>
#include <stdlib.h>
#include <stdio.h>

#define FOCUS_DURATION_SEC 1500U /* 25:00 */
#define BREAK_DURATION_SEC 300U  /* 05:00 */

static void lcd_pulse_enable(void) {
    PORTA |= (1 << PA1);
    _delay_us(2);
    PORTA &= ~(1 << PA1);
    _delay_us(50);
    return;
}

static void lcd_write_nibble(uint8_t nibble) {
    PORTA &= ~((1 << PA2) | (1 << PA3) | (1 << PA4) | (1 << PA5));
    if ((nibble & 0x01U) != 0U) { PORTA |= (1 << PA2); }
    if ((nibble & 0x02U) != 0U) { PORTA |= (1 << PA3); }
    if ((nibble & 0x04U) != 0U) { PORTA |= (1 << PA4); }
    if ((nibble & 0x08U) != 0U) { PORTA |= (1 << PA5); }
    lcd_pulse_enable();
    return;
}

static void lcd_send_byte(uint8_t value, uint8_t is_data) {
    if (is_data != 0U) {
        PORTA |= (1 << PA0);
    } else {
        PORTA &= ~(1 << PA0);
    }
    lcd_write_nibble((uint8_t)(value >> 4));
    lcd_write_nibble((uint8_t)(value & 0x0FU));
    if (is_data == 0U && value <= 0x03U) {
        _delay_ms(2);
    }
    return;
}

static void lcd_init_hw(void) {
    DDRA |= (1 << PA0) | (1 << PA1) | (1 << PA2) | (1 << PA3) | (1 << PA4) | (1 << PA5);
    DDRE |= (1 << PE3);  /* D5: luz de fondo LCD */
    PORTE |= (1 << PE3); /* Encendida al arrancar */
    _delay_ms(40);

    PORTA &= ~(1 << PA0);
    lcd_write_nibble(0x03U);
    _delay_ms(5);
    lcd_write_nibble(0x03U);
    _delay_us(150);
    lcd_write_nibble(0x03U);
    lcd_write_nibble(0x02U);

    lcd_send_byte(0x28U, 0U); /* 4 bits, 2 líneas */
    lcd_send_byte(0x0CU, 0U); /* Display ON, cursor OFF */
    lcd_send_byte(0x06U, 0U); /* Auto-incremento */
    lcd_send_byte(0x01U, 0U); /* Limpiar pantalla */
    return;
}

static void lcd_print_line(uint8_t row, const char *text) {
    uint8_t i = 0U;
    uint8_t ended = 0U;
    lcd_send_byte((row == 0U) ? 0x80U : 0xC0U, 0U);
    for (i = 0U; i < 16U; i++) {
        if (ended == 0U && text[i] == '\0') {
            ended = 1U;
        }
        lcd_send_byte((ended == 0U) ? (uint8_t)text[i] : (uint8_t)' ', 1U);
    }
    return;
}

static void buzzer_beep(uint8_t count, uint16_t duration_ms) {
    uint8_t i = 0U;
    uint16_t t = 0U;
    for (i = 0U; i < count; i++) {
        PORTG |= (1 << PG5); /* D4 HIGH */
        for (t = 0U; t < duration_ms; t++) { _delay_ms(1); }
        PORTG &= ~(1 << PG5); /* D4 LOW */
        if ((i + 1U) < count) {
            for (t = 0U; t < 80U; t++) { _delay_ms(1); }
        }
    }
    return;
}

static uint8_t read_ldr_percent(void) {
    uint16_t raw = 0U;
    uint8_t pct = 0U;
    ADMUX = (1 << REFS0); /* Referencia AVCC 5V, canal ADC0 (A0) */
    ADCSRA |= (1 << ADSC);
    while ((ADCSRA & (1 << ADSC)) != 0U) { }
    raw = ADC;
    pct = (uint8_t)((raw * 100UL) / 1023UL);
    return pct;
}

static uint8_t read_dht11(uint8_t *temp_out, uint8_t *hum_out) {
    uint8_t data[5] = {0U, 0U, 0U, 0U, 0U};
    uint8_t ok = 0U;
    uint8_t i = 0U;
    uint16_t guard = 0U;

    /* Señal de inicio en D2 (PE4) */
    DDRE |= (1 << PE4);
    PORTE &= ~(1 << PE4);
    _delay_ms(18);
    PORTE |= (1 << PE4);
    _delay_us(30);
    DDRE &= ~(1 << PE4);

    /* Respuesta del DHT11 */
    guard = 0U;
    while (((PINE & (1 << PE4)) != 0U) && (guard < 200U)) { _delay_us(1); guard++; }
    if (guard < 200U) {
        guard = 0U;
        while (((PINE & (1 << PE4)) == 0U) && (guard < 200U)) { _delay_us(1); guard++; }
        guard = 0U;
        while (((PINE & (1 << PE4)) != 0U) && (guard < 200U)) { _delay_us(1); guard++; }

        for (i = 0U; i < 40U; i++) {
            guard = 0U;
            while (((PINE & (1 << PE4)) == 0U) && (guard < 150U)) { _delay_us(1); guard++; }
            _delay_us(35);
            if ((PINE & (1 << PE4)) != 0U) {
                data[i / 8U] |= (uint8_t)(1U << (7U - (i % 8U)));
            }
            guard = 0U;
            while (((PINE & (1 << PE4)) != 0U) && (guard < 150U)) { _delay_us(1); guard++; }
        }

        if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) == data[4]) {
            *hum_out = data[0];
            *temp_out = data[2];
            ok = 1U;
        }
    }
    return ok;
}

static void record_sample(DeskHubSystem *sys) {
    SensorSample *resized = NULL;
    uint16_t new_cap = 0U;
    if (sys->history_count >= sys->history_cap) {
        new_cap = (sys->history_cap == 0U) ? 8U : (uint16_t)(sys->history_cap * 2U);
        if (new_cap > 64U) {
            sys->history_count = 0U; /* Buffer circular al llenar 64 muestras */
        } else {
            resized = (SensorSample *)realloc(sys->history, sizeof(SensorSample) * new_cap);
            if (resized != NULL) {
                sys->history = resized;
                sys->history_cap = new_cap;
            }
        }
    }
    if (sys->history != NULL && sys->history_count < sys->history_cap) {
        sys->history[sys->history_count] = sys->current;
        sys->history_count++;
    }
    return;
}

DeskHubSystem *desk_hub_create(void) {
    DeskHubSystem *sys = (DeskHubSystem *)malloc(sizeof(DeskHubSystem));
    if (sys != NULL) {
        sys->current.temp_c = 0U;
        sys->current.hum_pct = 0U;
        sys->current.lux_pct = 0U;
        sys->history = (SensorSample *)malloc(sizeof(SensorSample) * 8U);
        sys->history_count = 0U;
        sys->history_cap = (sys->history != NULL) ? 8U : 0U;

        /* Timer1 prescaler 64 -> 250 ticks = 1 ms exacto a 16 MHz */
        TCCR1A = 0x00U;
        TCCR1B = (1 << CS11) | (1 << CS10);
        TCNT1 = 0U;

        sys->last_tcnt = 0U;
        sys->tick_accum = 0U;
        sys->uptime_ms = 0UL;
        sys->last_sensor_ms = 0UL;
        sys->last_lcd_ms = 0UL;
        sys->last_pomo_tick_ms = 0UL;

        sys->pomo_state = POMO_IDLE;
        sys->pomo_running = 0U;
        sys->pomo_seconds_left = FOCUS_DURATION_SEC;

        sys->backlight_on = 1U;
        sys->btn_was_pressed = 0U;
        sys->btn_long_handled = 0U;
        sys->btn_press_start_ms = 0UL;

        /* Pines: D3 (PE5) botón pull-up, D4 (PG5) buzzer, ADC0 habilitado */
        DDRE &= ~(1 << PE5);
        PORTE |= (1 << PE5);
        DDRG |= (1 << PG5);
        PORTG &= ~(1 << PG5);
        ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

        lcd_init_hw();
        sys->current.lux_pct = read_ldr_percent();
        (void)read_dht11(&sys->current.temp_c, &sys->current.hum_pct);
        buzzer_beep(1U, 60U);
    }
    return sys;
}

void desk_hub_destroy(DeskHubSystem *sys) {
    if (sys != NULL) {
        if (sys->history != NULL) {
            free(sys->history);
            sys->history = NULL;
        }
        free(sys);
    }
    return;
}

void desk_hub_step(DeskHubSystem *sys) {
    uint16_t now_tcnt = 0U;
    uint16_t delta = 0U;
    uint8_t is_pressed = 0U;
    uint32_t held_ms = 0UL;
    char line0[17];
    char line1[17];
    uint16_t mins = 0U;
    uint16_t secs = 0U;
    const char *label = "LISTO";

    if (sys != NULL) {
        /* 1. Actualizar reloj interno en milisegundos */
        now_tcnt = TCNT1;
        delta = (uint16_t)(now_tcnt - sys->last_tcnt);
        sys->last_tcnt = now_tcnt;
        sys->tick_accum = (uint16_t)(sys->tick_accum + delta);
        while (sys->tick_accum >= 250U) {
            sys->tick_accum = (uint16_t)(sys->tick_accum - 250U);
            sys->uptime_ms++;
        }

        /* 2. Pulsador D3: toque corto = Pomodoro | 2 s = Apagar/Encender LCD */
        is_pressed = ((PINE & (1 << PE5)) == 0U) ? 1U : 0U;
        if ((is_pressed != 0U) && (sys->btn_was_pressed == 0U)) {
            sys->btn_was_pressed = 1U;
            sys->btn_long_handled = 0U;
            sys->btn_press_start_ms = sys->uptime_ms;
        } else if ((is_pressed != 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((held_ms >= 2000UL) && (sys->btn_long_handled == 0U)) {
                sys->btn_long_handled = 1U;
                sys->backlight_on = (sys->backlight_on != 0U) ? 0U : 1U;
                if (sys->backlight_on != 0U) {
                    PORTE |= (1 << PE3);
                    lcd_send_byte(0x0CU, 0U);
                    buzzer_beep(1U, 40U);
                } else {
                    PORTE &= ~(1 << PE3);
                    lcd_send_byte(0x08U, 0U);
                }
            }
        } else if ((is_pressed == 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((sys->btn_long_handled == 0U) && (held_ms >= 50UL) && (sys->backlight_on != 0U)) {
                if (sys->pomo_state == POMO_IDLE) {
                    sys->pomo_state = POMO_FOCUS;
                    sys->pomo_running = 1U;
                } else {
                    sys->pomo_running = (sys->pomo_running != 0U) ? 0U : 1U;
                }
                sys->last_pomo_tick_ms = sys->uptime_ms;
                buzzer_beep(1U, 40U);
            }
            sys->btn_was_pressed = 0U;
            sys->btn_long_handled = 0U;
        }

        /* 3. Cuenta atrás del Pomodoro cada 1000 ms */
        if ((sys->pomo_running != 0U) && ((sys->uptime_ms - sys->last_pomo_tick_ms) >= 1000UL)) {
            sys->last_pomo_tick_ms += 1000UL;
            if (sys->pomo_seconds_left > 0U) {
                sys->pomo_seconds_left--;
            } else {
                buzzer_beep(3U, 120U);
                if (sys->pomo_state == POMO_FOCUS) {
                    sys->pomo_state = POMO_BREAK;
                    sys->pomo_seconds_left = BREAK_DURATION_SEC;
                } else {
                    sys->pomo_state = POMO_FOCUS;
                    sys->pomo_seconds_left = FOCUS_DURATION_SEC;
                }
            }
        }

        /* 4. Lectura de sensores cada 2000 ms */
        if ((sys->uptime_ms - sys->last_sensor_ms) >= 2000UL) {
            sys->last_sensor_ms = sys->uptime_ms;
            sys->current.lux_pct = read_ldr_percent();
            (void)read_dht11(&sys->current.temp_c, &sys->current.hum_pct);
            record_sample(sys);
        }

        /* 5. Refresco de pantalla LCD cada 250 ms si está encendida */
        if ((sys->backlight_on != 0U) && ((sys->uptime_ms - sys->last_lcd_ms) >= 250UL)) {
            sys->last_lcd_ms = sys->uptime_ms;
            if (sys->pomo_state == POMO_FOCUS) {
                label = (sys->pomo_running != 0U) ? "FOCUS" : "PAUSA";
            } else if (sys->pomo_state == POMO_BREAK) {
                label = (sys->pomo_running != 0U) ? "DESC " : "PAUSA";
            }
            mins = (uint16_t)(sys->pomo_seconds_left / 60U);
            secs = (uint16_t)(sys->pomo_seconds_left % 60U);

            (void)snprintf(line0, sizeof(line0), "%2uC %2u%% L:%3u%%",
                           (unsigned int)sys->current.temp_c,
                           (unsigned int)sys->current.hum_pct,
                           (unsigned int)sys->current.lux_pct);
            (void)snprintf(line1, sizeof(line1), "%-6s %02u:%02u",
                           label,
                           (unsigned int)mins,
                           (unsigned int)secs);

            lcd_print_line(0U, line0);
            lcd_print_line(1U, line1);
        }
    }
    return;
}
