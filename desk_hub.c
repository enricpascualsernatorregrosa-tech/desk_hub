#include "desk_hub.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdlib.h>
#include <stdio.h>

#define FOCUS_50_SEC        3000U /* 50:00 */
#define BREAK_10_SEC        600U  /* 10:00 */
#define FOCUS_25_SEC        1500U /* 25:00 */
#define BREAK_5_SEC         300U  /* 05:00 */
#define LCD_BRIGHTNESS      55U
#define SENSOR_INVALID      255U
#define MULTI_CLICK_MS      380UL

/* Notas musicales en Hz */
#define NOTE_A4  440U
#define NOTE_C5  523U
#define NOTE_CS5 554U
#define NOTE_E5  659U
#define NOTE_G5  784U
#define NOTE_A5  880U
#define NOTE_C6  1047U
#define NOTE_REST 0U

static void sys_sync_clock(DeskHubSystem *sys) {
    uint16_t now_tcnt = 0U;
    uint16_t delta = 0U;
    if (sys != NULL) {
        now_tcnt = TCNT1;
        delta = (uint16_t)(now_tcnt - sys->last_tcnt);
        sys->last_tcnt = now_tcnt;
        sys->tick_accum = (uint16_t)(sys->tick_accum + delta);
        while (sys->tick_accum >= 250U) {
            sys->tick_accum = (uint16_t)(sys->tick_accum - 250U);
            sys->uptime_ms++;
        }
    }
    return;
}

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

static void lcd_reset_controller(void) {
    _delay_ms(25);
    PORTA &= ~(1 << PA0);
    lcd_write_nibble(0x03U);
    _delay_ms(5);
    lcd_write_nibble(0x03U);
    _delay_us(150);
    lcd_write_nibble(0x03U);
    lcd_write_nibble(0x02U);

    lcd_send_byte(0x28U, 0U);
    lcd_send_byte(0x0CU, 0U);
    lcd_send_byte(0x06U, 0U);
    lcd_send_byte(0x01U, 0U);
    return;
}

static void lcd_set_power(DeskHubSystem *sys, uint8_t on) {
    if (sys != NULL) {
        sys->backlight_on = (on != 0U) ? 1U : 0U;
        if (sys->backlight_on != 0U) {
            PORTH |= (1 << PH3);
            lcd_reset_controller();
            OCR3A = LCD_BRIGHTNESS;
            TCCR3A |= (1 << COM3A1);
        } else {
            TCCR3A &= ~((1 << COM3A1) | (1 << COM3A0));
            PORTE &= ~(1 << PE3);
            PORTA &= ~((1 << PA0) | (1 << PA1) | (1 << PA2) | (1 << PA3) | (1 << PA4) | (1 << PA5));
            PORTH &= ~(1 << PH3);
        }
    }
    return;
}

static void lcd_init_hw(void) {
    DDRA |= (1 << PA0) | (1 << PA1) | (1 << PA2) | (1 << PA3) | (1 << PA4) | (1 << PA5);
    DDRE |= (1 << PE3);
    DDRH |= (1 << PH3);

    TCCR3A = (1 << COM3A1) | (1 << WGM30);
    TCCR3B = (1 << WGM32) | (1 << CS31) | (1 << CS30);
    OCR3A = LCD_BRIGHTNESS;

    PORTH |= (1 << PH3);
    lcd_reset_controller();
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

static void buzzer_play_tone(DeskHubSystem *sys, uint16_t freq_hz, uint16_t duration_ms) {
    uint32_t end_ms = 0UL;
    uint16_t half_ticks = 0U;
    uint16_t last_toggle = 0U;

    if (sys != NULL) {
        sys_sync_clock(sys);
        end_ms = sys->uptime_ms + (uint32_t)duration_ms;

        if (freq_hz > 0U) {
            half_ticks = (uint16_t)(125000UL / (uint32_t)freq_hz);
            last_toggle = TCNT1;
            while (sys->uptime_ms < end_ms) {
                sys_sync_clock(sys);
                if ((uint16_t)(TCNT1 - last_toggle) >= half_ticks) {
                    PORTG ^= (1 << PG5);
                    last_toggle = TCNT1;
                }
            }
            PORTG &= ~(1 << PG5);
        } else {
            PORTG &= ~(1 << PG5);
            while (sys->uptime_ms < end_ms) {
                sys_sync_clock(sys);
            }
        }
    }
    return;
}

static void buzzer_play_melody(DeskHubSystem *sys, MelodyId id) {
    Note seq[6];
    uint8_t count = 0U;
    uint8_t i = 0U;

    if (sys != NULL) {
        if (id == MELODY_BOOT) {
            seq[0] = (Note){NOTE_C5, 70U};
            seq[1] = (Note){NOTE_E5, 70U};
            seq[2] = (Note){NOTE_G5, 110U};
            count = 3U;
        } else if (id == MELODY_START) {
            seq[0] = (Note){NOTE_E5, 65U};
            seq[1] = (Note){NOTE_G5, 95U};
            count = 2U;
        } else if (id == MELODY_PAUSE) {
            seq[0] = (Note){NOTE_G5, 65U};
            seq[1] = (Note){NOTE_E5, 95U};
            count = 2U;
        } else if (id == MELODY_MODE) {
            seq[0] = (Note){NOTE_A4, 60U};
            seq[1] = (Note){NOTE_CS5, 60U};
            seq[2] = (Note){NOTE_E5, 90U};
            count = 3U;
        } else if (id == MELODY_RESET) {
            seq[0] = (Note){NOTE_G5, 70U};
            seq[1] = (Note){NOTE_E5, 70U};
            seq[2] = (Note){NOTE_C5, 130U};
            count = 3U;
        } else if (id == MELODY_FOCUS_END) {
            seq[0] = (Note){NOTE_C5, 110U};
            seq[1] = (Note){NOTE_E5, 110U};
            seq[2] = (Note){NOTE_G5, 110U};
            seq[3] = (Note){NOTE_C6, 220U};
            seq[4] = (Note){NOTE_G5, 100U};
            seq[5] = (Note){NOTE_C6, 280U};
            count = 6U;
        } else {
            seq[0] = (Note){NOTE_E5, 110U};
            seq[1] = (Note){NOTE_G5, 110U};
            seq[2] = (Note){NOTE_E5, 110U};
            seq[3] = (Note){NOTE_C5, 120U};
            seq[4] = (Note){NOTE_G5, 240U};
            count = 5U;
        }

        for (i = 0U; i < count; i++) {
            buzzer_play_tone(sys, seq[i].freq_hz, seq[i].duration_ms);
            if ((i + 1U) < count) {
                buzzer_play_tone(sys, NOTE_REST, 25U);
            }
        }
    }
    return;
}

static uint8_t read_ldr_percent(void) {
    uint16_t raw = 0U;
    uint8_t pct = 0U;
    ADMUX = (1 << REFS0);
    ADCSRA |= (1 << ADSC);
    while ((ADCSRA & (1 << ADSC)) != 0U) {
    }
    raw = ADC;
    pct = (uint8_t)((raw * 100UL) / 1023UL);
    return pct;
}

static uint16_t dht_measure_pulse(uint8_t level_high, uint16_t timeout_ticks) {
    uint16_t start = TCNT1;
    uint16_t elapsed = 0U;
    uint8_t done = 0U;
    while (done == 0U) {
        elapsed = (uint16_t)(TCNT1 - start);
        if (level_high != 0U) {
            if ((PINE & (1 << PE4)) == 0U) {
                done = 1U;
            }
        } else {
            if ((PINE & (1 << PE4)) != 0U) {
                done = 1U;
            }
        }
        if (elapsed >= timeout_ticks) {
            done = 1U;
        }
    }
    return elapsed;
}

static uint8_t read_dht11(uint8_t *temp_out, uint8_t *hum_out) {
    uint8_t data[5] = {0U, 0U, 0U, 0U, 0U};
    uint8_t ok = 0U;
    uint8_t i = 0U;
    uint16_t pulse_ticks = 0U;
    uint8_t abort_read = 0U;
    uint8_t sreg = SREG;

    DDRE |= (1 << PE4);
    PORTE &= ~(1 << PE4);
    _delay_ms(20);

    cli();
    DDRE &= ~(1 << PE4);
    PORTE |= (1 << PE4);

    if (dht_measure_pulse(1U, 60U) >= 60U) {
        abort_read = 1U;
    }
    if ((abort_read == 0U) && (dht_measure_pulse(0U, 60U) >= 60U)) {
        abort_read = 1U;
    }
    if ((abort_read == 0U) && (dht_measure_pulse(1U, 60U) >= 60U)) {
        abort_read = 1U;
    }

    for (i = 0U; (i < 40U) && (abort_read == 0U); i++) {
        if (dht_measure_pulse(0U, 40U) >= 40U) {
            abort_read = 1U;
        } else {
            pulse_ticks = dht_measure_pulse(1U, 50U);
            if (pulse_ticks >= 50U) {
                abort_read = 1U;
            } else if (pulse_ticks > 12U) {
                data[i / 8U] |= (uint8_t)(1U << (7U - (i % 8U)));
            }
        }
    }
    SREG = sreg;

    if ((abort_read == 0U) && ((data[0] != 0U) || (data[2] != 0U)) &&
        ((uint8_t)(data[0] + data[1] + data[2] + data[3]) == data[4])) {
        if ((temp_out != NULL) && (hum_out != NULL)) {
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
            sys->history_count = 0U;
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

static uint16_t get_focus_seconds(PomoMode mode) {
    uint16_t sec = FOCUS_50_SEC;
    if (mode == POMO_MODE_25_5) {
        sec = FOCUS_25_SEC;
    }
    return sec;
}

static uint16_t get_break_seconds(PomoMode mode) {
    uint16_t sec = BREAK_10_SEC;
    if (mode == POMO_MODE_25_5) {
        sec = BREAK_5_SEC;
    }
    return sec;
}

DeskHubSystem *desk_hub_create(void) {
    DeskHubSystem *sys = (DeskHubSystem *)malloc(sizeof(DeskHubSystem));
    if (sys != NULL) {
        sys->current.temp_c = SENSOR_INVALID;
        sys->current.hum_pct = SENSOR_INVALID;
        sys->current.lux_pct = 0U;
        sys->history = (SensorSample *)malloc(sizeof(SensorSample) * 8U);
        sys->history_count = 0U;
        sys->history_cap = (sys->history != NULL) ? 8U : 0U;

        TCCR1A = 0x00U;
        TCCR1B = (1 << CS11) | (1 << CS10);
        TCNT1 = 0U;

        sys->last_tcnt = 0U;
        sys->tick_accum = 0U;
        sys->uptime_ms = 0UL;
        sys->last_sensor_ms = 0UL;
        sys->last_lcd_ms = 0UL;
        sys->last_pomo_tick_ms = 0UL;

        sys->pomo_mode = POMO_MODE_50_10;
        sys->pomo_state = POMO_IDLE;
        sys->pomo_running = 0U;
        sys->pomo_seconds_left = FOCUS_50_SEC;

        sys->backlight_on = 1U;
        sys->btn_was_pressed = 0U;
        sys->btn_long_handled = 0U;
        sys->btn_click_count = 0U;
        sys->btn_press_start_ms = 0UL;
        sys->btn_last_release_ms = 0UL;

        DDRE &= ~(1 << PE5);
        PORTE |= (1 << PE5);
        DDRG |= (1 << PG5);
        PORTG &= ~(1 << PG5);
        ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

        lcd_init_hw();
        _delay_ms(1000);
        sys->last_tcnt = TCNT1;
        sys->current.lux_pct = read_ldr_percent();
        (void)read_dht11(&sys->current.temp_c, &sys->current.hum_pct);
        buzzer_play_melody(sys, MELODY_BOOT);
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
    uint8_t is_pressed = 0U;
    uint32_t held_ms = 0UL;
    char line0[17];
    char line1[17];
    uint16_t mins = 0U;
    uint16_t secs = 0U;
    const char *label = "50/10";

    if (sys != NULL) {
        sys_sync_clock(sys);

        /* 1. Pulsador D3: mantener 2 s = apagar/encender | 1 toque = start/pausa | 2 = modo | 3 = reset */
        is_pressed = ((PINE & (1 << PE5)) == 0U) ? 1U : 0U;
        if ((is_pressed != 0U) && (sys->btn_was_pressed == 0U)) {
            sys->btn_was_pressed = 1U;
            sys->btn_long_handled = 0U;
            sys->btn_press_start_ms = sys->uptime_ms;
        } else if ((is_pressed != 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((held_ms >= 2000UL) && (sys->btn_long_handled == 0U)) {
                sys->btn_long_handled = 1U;
                sys->btn_click_count = 0U;
                lcd_set_power(sys, (sys->backlight_on != 0U) ? 0U : 1U);
                if (sys->backlight_on != 0U) {
                    sys->last_lcd_ms = 0UL;
                    buzzer_play_melody(sys, MELODY_START);
                }
            }
        } else if ((is_pressed == 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((sys->btn_long_handled == 0U) && (held_ms >= 40UL)) {
                sys->btn_click_count++;
                sys->btn_last_release_ms = sys->uptime_ms;
            }
            sys->btn_was_pressed = 0U;
            sys->btn_long_handled = 0U;
        }

        if ((is_pressed == 0U) && (sys->btn_click_count > 0U)) {
            if (sys->btn_click_count >= 3U) {
                sys->btn_click_count = 0U;
                if (sys->backlight_on == 0U) {
                    lcd_set_power(sys, 1U);
                }
                sys->pomo_state = POMO_IDLE;
                sys->pomo_running = 0U;
                sys->pomo_seconds_left = get_focus_seconds(sys->pomo_mode);
                sys->last_lcd_ms = 0UL;
                buzzer_play_melody(sys, MELODY_RESET);
            } else if ((sys->uptime_ms - sys->btn_last_release_ms) >= MULTI_CLICK_MS) {
                if (sys->btn_click_count == 2U) {
                    sys->btn_click_count = 0U;
                    if (sys->backlight_on == 0U) {
                        lcd_set_power(sys, 1U);
                    }
                    if (sys->pomo_mode == POMO_MODE_50_10) {
                        sys->pomo_mode = POMO_MODE_25_5;
                    } else {
                        sys->pomo_mode = POMO_MODE_50_10;
                    }
                    sys->pomo_state = POMO_IDLE;
                    sys->pomo_running = 0U;
                    sys->pomo_seconds_left = get_focus_seconds(sys->pomo_mode);
                    sys->last_lcd_ms = 0UL;
                    buzzer_play_melody(sys, MELODY_MODE);
                } else {
                    sys->btn_click_count = 0U;
                    if (sys->backlight_on == 0U) {
                        lcd_set_power(sys, 1U);
                    }
                    if (sys->pomo_state == POMO_IDLE) {
                        sys->pomo_state = POMO_FOCUS;
                        sys->pomo_running = 1U;
                    } else {
                        sys->pomo_running = (sys->pomo_running != 0U) ? 0U : 1U;
                    }
                    sys->last_pomo_tick_ms = sys->uptime_ms;
                    sys->last_lcd_ms = 0UL;
                    buzzer_play_melody(sys, (sys->pomo_running != 0U) ? MELODY_START : MELODY_PAUSE);
                }
            }
        }

        /* 2. Cuenta atrás Pomodoro con melodía al finalizar cada fase */
        if ((sys->pomo_running != 0U) && ((sys->uptime_ms - sys->last_pomo_tick_ms) >= 1000UL)) {
            sys->last_pomo_tick_ms += 1000UL;
            if (sys->pomo_seconds_left > 0U) {
                sys->pomo_seconds_left--;
            } else {
                if (sys->backlight_on == 0U) {
                    lcd_set_power(sys, 1U);
                }
                sys->last_lcd_ms = 0UL;
                if (sys->pomo_state == POMO_FOCUS) {
                    sys->pomo_state = POMO_BREAK;
                    sys->pomo_seconds_left = get_break_seconds(sys->pomo_mode);
                    buzzer_play_melody(sys, MELODY_FOCUS_END);
                } else {
                    sys->pomo_state = POMO_FOCUS;
                    sys->pomo_seconds_left = get_focus_seconds(sys->pomo_mode);
                    buzzer_play_melody(sys, MELODY_BREAK_END);
                }
                sys->last_pomo_tick_ms = sys->uptime_ms;
            }
        }

        /* 3. Lectura de sensores cada 2000 ms */
        if ((sys->uptime_ms - sys->last_sensor_ms) >= 2000UL) {
            sys->last_sensor_ms = sys->uptime_ms;
            sys->current.lux_pct = read_ldr_percent();
            if (read_dht11(&sys->current.temp_c, &sys->current.hum_pct) != 0U) {
                record_sample(sys);
            }
        }

        /* 4. Refresco de pantalla cada 250 ms si está encendida */
        if ((sys->backlight_on != 0U) && ((sys->uptime_ms - sys->last_lcd_ms) >= 250UL)) {
            sys->last_lcd_ms = sys->uptime_ms;
            if (sys->pomo_state == POMO_IDLE) {
                label = (sys->pomo_mode == POMO_MODE_50_10) ? "50/10" : "25/5 ";
            } else if (sys->pomo_state == POMO_FOCUS) {
                if (sys->pomo_running != 0U) {
                    label = (sys->pomo_mode == POMO_MODE_50_10) ? "FOC50" : "FOC25";
                } else {
                    label = "PAUSA";
                }
            } else {
                if (sys->pomo_running != 0U) {
                    label = (sys->pomo_mode == POMO_MODE_50_10) ? "DES10" : "DES05";
                } else {
                    label = "PAUSA";
                }
            }
            mins = (uint16_t)(sys->pomo_seconds_left / 60U);
            secs = (uint16_t)(sys->pomo_seconds_left % 60U);

            if (sys->current.temp_c == SENSOR_INVALID) {
                (void)snprintf(line0, sizeof(line0), "--C --%% L:%3u%%",
                               (unsigned int)sys->current.lux_pct);
            } else {
                (void)snprintf(line0, sizeof(line0), "%2uC %2u%% L:%3u%%",
                               (unsigned int)sys->current.temp_c,
                               (unsigned int)sys->current.hum_pct,
                               (unsigned int)sys->current.lux_pct);
            }
            (void)snprintf(line1, sizeof(line1), "%-7s %02u:%02u",
                           label,
                           (unsigned int)mins,
                           (unsigned int)secs);

            lcd_print_line(0U, line0);
            lcd_print_line(1U, line1);
        }
    }
    return;
}
