#include "desk_hub.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FOCUS_50_SEC        3000U
#define BREAK_10_SEC        600U
#define FOCUS_25_SEC        1500U
#define BREAK_5_SEC         300U
#define LCD_BRIGHTNESS      55U
#define SENSOR_INVALID      255U
#define MULTI_CLICK_MS      380UL
#define EEPROM_ADDR_SESS    0x0010U

static void copy_fixed_str(char *dst, const char *src, uint8_t max_len) {
    uint8_t i = 0U;
    if ((dst != NULL) && (max_len > 0U)) {
        while ((i < (uint8_t)(max_len - 1U)) && (src != NULL) && (src[i] != '\0')) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }
    return;
}

static uint16_t eeprom_read_u16(uint16_t addr) {
    uint8_t lo = 0U;
    uint8_t hi = 0U;
    while ((EECR & (1 << EEPE)) != 0U) { }
    EEAR = addr;
    EECR |= (1 << EERE);
    lo = EEDR;
    EEAR = (uint16_t)(addr + 1U);
    EECR |= (1 << EERE);
    hi = EEDR;
    return (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

static void eeprom_write_u16(uint16_t addr, uint16_t val) {
    uint8_t sreg = SREG;
    cli();
    while ((EECR & (1 << EEPE)) != 0U) { }
    EEAR = addr;
    EEDR = (uint8_t)(val & 0xFFU);
    EECR |= (1 << EEMPE);
    EECR |= (1 << EEPE);
    while ((EECR & (1 << EEPE)) != 0U) { }
    EEAR = (uint16_t)(addr + 1U);
    EEDR = (uint8_t)((val >> 8) & 0xFFU);
    EECR |= (1 << EEMPE);
    EECR |= (1 << EEPE);
    SREG = sreg;
    return;
}

static void uart0_init_115200(void) {
    UCSR0A = (1 << U2X0);
    UBRR0H = 0U;
    UBRR0L = 16U; /* 16 MHz -> 115200 bps */
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    return;
}

static void uart0_print(const char *str) {
    uint16_t i = 0U;
    if (str != NULL) {
        while (str[i] != '\0') {
            while ((UCSR0A & (1 << UDRE0)) == 0U) { }
            UDR0 = (uint8_t)str[i];
            i++;
        }
    }
    return;
}

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
    if ((is_data == 0U) && (value <= 0x03U)) {
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
        if ((ended == 0U) && (text[i] == '\0')) {
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
        }
    }
    return;
}

static uint8_t read_ldr_percent(void) {
    uint16_t raw = 0U;
    ADMUX = (1 << REFS0);
    ADCSRA |= (1 << ADSC);
    while ((ADCSRA & (1 << ADSC)) != 0U) { }
    raw = ADC;
    return (uint8_t)((raw * 100UL) / 1023UL);
}

static uint16_t dht_measure_pulse(uint8_t level_high, uint16_t timeout_ticks) {
    uint16_t start = TCNT1;
    uint16_t elapsed = 0U;
    uint8_t done = 0U;
    while (done == 0U) {
        elapsed = (uint16_t)(TCNT1 - start);
        if (level_high != 0U) {
            if ((PINE & (1 << PE4)) == 0U) { done = 1U; }
        } else {
            if ((PINE & (1 << PE4)) != 0U) { done = 1U; }
        }
        if (elapsed >= timeout_ticks) { done = 1U; }
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

    if (dht_measure_pulse(1U, 60U) >= 60U) { abort_read = 1U; }
    if ((abort_read == 0U) && (dht_measure_pulse(0U, 60U) >= 60U)) { abort_read = 1U; }
    if ((abort_read == 0U) && (dht_measure_pulse(1U, 60U) >= 60U)) { abort_read = 1U; }

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
    if ((sys->history != NULL) && (sys->history_count < sys->history_cap)) {
        sys->history[sys->history_count] = sys->current;
        sys->history_count++;
    }
    if (sys->current.temp_c < sys->temp_min) { sys->temp_min = sys->current.temp_c; }
    if (sys->current.temp_c > sys->temp_max) { sys->temp_max = sys->current.temp_c; }
    return;
}

static uint16_t parse_u16_str(const char *s) {
    uint16_t val = 0U;
    uint8_t i = 0U;
    if (s != NULL) {
        while ((s[i] >= '0') && (s[i] <= '9')) {
            val = (uint16_t)((val * 10U) + (uint16_t)(s[i] - '0'));
            i++;
        }
    }
    return val;
}

static void handle_serial_command(DeskHubSystem *sys, const char *cmd) {
    const char *c1 = NULL;
    const char *c2 = NULL;
    uint16_t rem = 0U;
    if ((sys != NULL) && (cmd != NULL)) {
        if (strncmp(cmd, "SONG:", 5) == 0) {
            copy_fixed_str(sys->song_title, cmd + 5, 17U);
            sys->last_lcd_ms = 0UL;
        } else if (strncmp(cmd, "EVENT:", 6) == 0) {
            copy_fixed_str(sys->event_title, cmd + 6, 17U);
            sys->last_lcd_ms = 0UL;
        } else if (strcmp(cmd, "BUZZ") == 0) {
            if (sys->backlight_on == 0U) { lcd_set_power(sys, 1U); }
            buzzer_play_tone(sys, 784U, 120U);
            buzzer_play_tone(sys, 1047U, 180U);
        } else if (strncmp(cmd, "SCREEN:", 7) == 0) {
            uint8_t s = (uint8_t)(cmd[7] - '0');
            if (s <= 3U) {
                sys->lcd_screen = (LcdScreen)s;
                sys->last_lcd_ms = 0UL;
            }
        } else if (strcmp(cmd, "LCD:OFF") == 0) {
            lcd_set_power(sys, 0U);
        } else if (strcmp(cmd, "LCD:ON") == 0) {
            lcd_set_power(sys, 1U);
            sys->last_lcd_ms = 0UL;
        } else if (strncmp(cmd, "POMO:", 5) == 0) {
            c1 = strchr(cmd + 5, ',');
            if (c1 != NULL) {
                c2 = strchr(c1 + 1, ',');
                sys->pomo_state = (strncmp(cmd + 5, "FOCUS", 5) == 0) ? POMO_FOCUS : POMO_BREAK;
                sys->pomo_running = (c1[1] == '1') ? 1U : 0U;
                if (c2 != NULL) {
                    rem = parse_u16_str(c2 + 1);
                    if (rem > 0U) {
                        sys->pomo_seconds_left = rem;
                    }
                }
                sys->last_pomo_tick_ms = sys->uptime_ms;
            } else if (strstr(cmd, "START") != NULL) {
                if (sys->pomo_state == POMO_IDLE) { sys->pomo_state = POMO_FOCUS; }
                sys->pomo_running = 1U;
            } else if (strstr(cmd, "PAUSE") != NULL) {
                sys->pomo_running = 0U;
            } else if (strstr(cmd, "RESET") != NULL) {
                sys->pomo_state = POMO_IDLE;
                sys->pomo_running = 0U;
                sys->pomo_seconds_left = (sys->pomo_mode == POMO_MODE_50_10) ? FOCUS_50_SEC : FOCUS_25_SEC;
            }
            sys->last_lcd_ms = 0UL;
        }
    }
    return;
}

DeskHubSystem *desk_hub_create(void) {
    DeskHubSystem *sys = (DeskHubSystem *)malloc(sizeof(DeskHubSystem));
    uint16_t stored = 0U;
    if (sys != NULL) {
        sys->current.temp_c = SENSOR_INVALID;
        sys->current.hum_pct = SENSOR_INVALID;
        sys->current.lux_pct = 0U;
        sys->history = (SensorSample *)malloc(sizeof(SensorSample) * 8U);
        sys->history_count = 0U;
        sys->history_cap = (sys->history != NULL) ? 8U : 0U;
        sys->temp_min = 99U;
        sys->temp_max = 0U;

        stored = eeprom_read_u16(EEPROM_ADDR_SESS);
        sys->eeprom_sessions = (stored == 0xFFFFU) ? 0U : stored;

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

        sys->lcd_screen = LCD_SCR_CLIMATE;
        sys->backlight_on = 1U;
        sys->btn_was_pressed = 0U;
        sys->btn_stage = 0U;
        sys->btn_click_count = 0U;
        sys->btn_press_start_ms = 0UL;
        sys->btn_last_release_ms = 0UL;

        copy_fixed_str(sys->song_title, "Lo-Fi Beats", 17U);
        copy_fixed_str(sys->event_title, "Sin pendientes", 17U);
        sys->rx_buf[0] = '\0';
        sys->rx_len = 0U;

        DDRE &= ~(1 << PE5);
        PORTE |= (1 << PE5);
        DDRG |= (1 << PG5);
        PORTG &= ~(1 << PG5);
        ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

        uart0_init_115200();
        lcd_init_hw();
        _delay_ms(800);
        sys->last_tcnt = TCNT1;
        sys->current.lux_pct = read_ldr_percent();
        if (read_dht11(&sys->current.temp_c, &sys->current.hum_pct) != 0U) {
            record_sample(sys);
        }
        buzzer_play_tone(sys, 523U, 70U);
        buzzer_play_tone(sys, 784U, 90U);
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
    char tx_json[72];
    uint16_t mins = 0U;
    uint16_t secs = 0U;
    const char *label = "50/10";
    uint8_t ch = 0U;

    if (sys != NULL) {
        sys_sync_clock(sys);

        /* 1. Recepcion UART0 desde Tablet Android OTG */
        while ((UCSR0A & (1 << RXC0)) != 0U) {
            ch = UDR0;
            if ((ch == '\n') || (ch == '\r')) {
                if (sys->rx_len > 0U) {
                    sys->rx_buf[sys->rx_len] = '\0';
                    handle_serial_command(sys, sys->rx_buf);
                    sys->rx_len = 0U;
                }
            } else if (sys->rx_len < 38U) {
                sys->rx_buf[sys->rx_len] = (char)ch;
                sys->rx_len++;
            }
        }

        /* 2. Pulsador fisico D3: ~0.9s rota pantallas [1..4] | ~2.8s apaga LCD | 1/2/3 toques Pomodoro */
        is_pressed = ((PINE & (1 << PE5)) == 0U) ? 1U : 0U;
        if ((is_pressed != 0U) && (sys->btn_was_pressed == 0U)) {
            sys->btn_was_pressed = 1U;
            sys->btn_stage = 0U;
            sys->btn_press_start_ms = sys->uptime_ms;
        } else if ((is_pressed != 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((held_ms >= 900UL) && (sys->btn_stage == 0U)) {
                sys->btn_stage = 1U;
                sys->btn_click_count = 0U;
                if (sys->backlight_on == 0U) {
                    lcd_set_power(sys, 1U);
                } else {
                    sys->lcd_screen = (LcdScreen)(((uint8_t)sys->lcd_screen + 1U) % 4U);
                }
                sys->last_lcd_ms = 0UL;
                buzzer_play_tone(sys, 659U, 50U);
            } else if ((held_ms >= 2800UL) && (sys->btn_stage == 1U)) {
                sys->btn_stage = 2U;
                lcd_set_power(sys, 0U);
            }
        } else if ((is_pressed == 0U) && (sys->btn_was_pressed != 0U)) {
            held_ms = sys->uptime_ms - sys->btn_press_start_ms;
            if ((sys->btn_stage == 0U) && (held_ms >= 40UL)) {
                sys->btn_click_count++;
                sys->btn_last_release_ms = sys->uptime_ms;
            }
            sys->btn_was_pressed = 0U;
            sys->btn_stage = 0U;
        }

        if ((is_pressed == 0U) && (sys->btn_click_count > 0U)) {
            if (sys->btn_click_count >= 3U) {
                sys->btn_click_count = 0U;
                if (sys->backlight_on == 0U) { lcd_set_power(sys, 1U); }
                sys->pomo_state = POMO_IDLE;
                sys->pomo_running = 0U;
                sys->pomo_seconds_left = (sys->pomo_mode == POMO_MODE_50_10) ? FOCUS_50_SEC : FOCUS_25_SEC;
                sys->last_lcd_ms = 0UL;
                buzzer_play_tone(sys, 523U, 80U);
            } else if ((sys->uptime_ms - sys->btn_last_release_ms) >= MULTI_CLICK_MS) {
                if (sys->btn_click_count == 2U) {
                    sys->btn_click_count = 0U;
                    if (sys->backlight_on == 0U) { lcd_set_power(sys, 1U); }
                    sys->pomo_mode = (sys->pomo_mode == POMO_MODE_50_10) ? POMO_MODE_25_5 : POMO_MODE_50_10;
                    sys->pomo_state = POMO_IDLE;
                    sys->pomo_running = 0U;
                    sys->pomo_seconds_left = (sys->pomo_mode == POMO_MODE_50_10) ? FOCUS_50_SEC : FOCUS_25_SEC;
                    sys->last_lcd_ms = 0UL;
                    buzzer_play_tone(sys, 659U, 60U);
                } else {
                    sys->btn_click_count = 0U;
                    if (sys->backlight_on == 0U) { lcd_set_power(sys, 1U); }
                    if (sys->pomo_state == POMO_IDLE) {
                        sys->pomo_state = POMO_FOCUS;
                        sys->pomo_running = 1U;
                    } else {
                        sys->pomo_running = (sys->pomo_running != 0U) ? 0U : 1U;
                    }
                    sys->last_pomo_tick_ms = sys->uptime_ms;
                    sys->last_lcd_ms = 0UL;
                    buzzer_play_tone(sys, 784U, 50U);
                }
            }
        }

        /* 3. Cuenta atras Pomodoro y guardado en EEPROM */
        if ((sys->pomo_running != 0U) && ((sys->uptime_ms - sys->last_pomo_tick_ms) >= 1000UL)) {
            sys->last_pomo_tick_ms += 1000UL;
            if (sys->pomo_seconds_left > 0U) {
                sys->pomo_seconds_left--;
            } else {
                if (sys->backlight_on == 0U) { lcd_set_power(sys, 1U); }
                sys->last_lcd_ms = 0UL;
                if (sys->pomo_state == POMO_FOCUS) {
                    sys->eeprom_sessions++;
                    eeprom_write_u16(EEPROM_ADDR_SESS, sys->eeprom_sessions);
                    sys->pomo_state = POMO_BREAK;
                    sys->pomo_seconds_left = (sys->pomo_mode == POMO_MODE_50_10) ? BREAK_10_SEC : BREAK_5_SEC;
                } else {
                    sys->pomo_state = POMO_FOCUS;
                    sys->pomo_seconds_left = (sys->pomo_mode == POMO_MODE_50_10) ? FOCUS_50_SEC : FOCUS_25_SEC;
                }
                buzzer_play_tone(sys, 1047U, 200U);
                sys->last_pomo_tick_ms = sys->uptime_ms;
            }
        }

        /* 4. Muestreo cada 2000 ms y envio JSON a Tablet */
        if ((sys->uptime_ms - sys->last_sensor_ms) >= 2000UL) {
            sys->last_sensor_ms = sys->uptime_ms;
            sys->current.lux_pct = read_ldr_percent();
            if (read_dht11(&sys->current.temp_c, &sys->current.hum_pct) != 0U) {
                record_sample(sys);
            }
            (void)snprintf(tx_json, sizeof(tx_json),
                           "{\"t\":%u,\"h\":%u,\"l\":%u,\"ee\":%u,\"scr\":%u}\n",
                           (unsigned int)sys->current.temp_c,
                           (unsigned int)sys->current.hum_pct,
                           (unsigned int)sys->current.lux_pct,
                           (unsigned int)sys->eeprom_sessions,
                           (unsigned int)sys->lcd_screen);
            uart0_print(tx_json);
        }

        /* 5. Refresco de las 4 pantallas del LCD1602 */
        if ((sys->backlight_on != 0U) && ((sys->uptime_ms - sys->last_lcd_ms) >= 250UL)) {
            sys->last_lcd_ms = sys->uptime_ms;
            if (sys->lcd_screen == LCD_SCR_CLIMATE) {
                if (sys->pomo_state == POMO_IDLE) {
                    label = (sys->pomo_mode == POMO_MODE_50_10) ? "50/10" : "25/5 ";
                } else if (sys->pomo_state == POMO_FOCUS) {
                    label = (sys->pomo_running != 0U) ? ((sys->pomo_mode == POMO_MODE_50_10) ? "FOC50" : "FOC25") : "PAUSA";
                } else {
                    label = (sys->pomo_running != 0U) ? ((sys->pomo_mode == POMO_MODE_50_10) ? "DES10" : "DES05") : "PAUSA";
                }
                mins = (uint16_t)(sys->pomo_seconds_left / 60U);
                secs = (uint16_t)(sys->pomo_seconds_left % 60U);
                if (sys->current.temp_c == SENSOR_INVALID) {
                    (void)snprintf(line0, sizeof(line0), "--C --%% L:%3u%%", (unsigned int)sys->current.lux_pct);
                } else {
                    (void)snprintf(line0, sizeof(line0), "%2uC %2u%% L:%3u%%",
                                   (unsigned int)sys->current.temp_c,
                                   (unsigned int)sys->current.hum_pct,
                                   (unsigned int)sys->current.lux_pct);
                }
                (void)snprintf(line1, sizeof(line1), "%-7s %02u:%02u", label, (unsigned int)mins, (unsigned int)secs);
            } else if (sys->lcd_screen == LCD_SCR_MUSIC) {
                (void)snprintf(line0, sizeof(line0), "SONANDO:   [2/4]");
                (void)snprintf(line1, sizeof(line1), "%-16s", sys->song_title);
            } else if (sys->lcd_screen == LCD_SCR_EVENT) {
                (void)snprintf(line0, sizeof(line0), "PROX EVENTO[3/4]");
                (void)snprintf(line1, sizeof(line1), "%-16s", sys->event_title);
            } else {
                (void)snprintf(line0, sizeof(line0), "SESIONES:%02u[4/4]", (unsigned int)sys->eeprom_sessions);
                if (sys->temp_min <= sys->temp_max) {
                    (void)snprintf(line1, sizeof(line1), "Min:%2uC Max:%2uC", (unsigned int)sys->temp_min, (unsigned int)sys->temp_max);
                } else {
                    (void)snprintf(line1, sizeof(line1), "Min:--C Max:--C");
                }
            }
            lcd_print_line(0U, line0);
            lcd_print_line(1U, line1);
        }
    }
    return;
}
