#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include "desk_hub.h"
#include <avr/io.h>
#include <util/delay.h>
#include <stdlib.h>

/* --- Driver LCD1602 en modo 4 bits (PORTA: PA0=RS, PA1=E, PA2..PA5=D4..D7) --- */
static void lcd_pulse_enable(void) {
    PORTA |= (1 << PA1);
    _delay_us(2);
    PORTA &= ~(1 << PA1);
    _delay_us(50);
}

static void lcd_write_nibble(uint8_t nibble) {
    PORTA = (uint8_t)((PORTA & 0xC3U) | ((nibble & 0x0FU) << 2));
    lcd_pulse_enable();
}

static void lcd_send_byte(uint8_t val, uint8_t is_data) {
    if (is_data != 0U) {
        PORTA |= (1 << PA0);
    } else {
        PORTA &= ~(1 << PA0);
    }
    lcd_write_nibble((uint8_t)(val >> 4));
    lcd_write_nibble((uint8_t)(val & 0x0FU));
}

static void lcd_init_hw(void) {
    DDRA |= 0x3FU;
    PORTA &= (uint8_t)(~0x3FU);
    _delay_ms(40);
    lcd_write_nibble(0x03U);
    _delay_ms(5);
    lcd_write_nibble(0x03U);
    _delay_us(150);
    lcd_write_nibble(0x03U);
    lcd_write_nibble(0x02U);
    lcd_send_byte(0x28U, 0U); /* 4 bits, 2 lineas */
    lcd_send_byte(0x0CU, 0U); /* Display ON, cursor OFF */
    lcd_send_byte(0x06U, 0U); /* Auto-incremento */
    lcd_send_byte(0x01U, 0U); /* Limpiar pantalla */
    _delay_ms(2);
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t addr = (uint8_t)(col + ((row == 0U) ? 0x00U : 0x40U));
    lcd_send_byte((uint8_t)(0x80U | addr), 0U);
}

static void lcd_print_str(const char *s) {
    size_t i = 0U;
    while (s[i] != '\0') {
        lcd_send_byte((uint8_t)s[i], 1U);
        i++;
    }
}

static void lcd_print_u16(uint16_t val, uint8_t pad2) {
    char buf[6];
    uint8_t idx = 0U;
    if ((pad2 != 0U) && (val < 10U)) {
        lcd_send_byte((uint8_t)'0', 1U);
    }
    if (val == 0U) {
        lcd_send_byte((uint8_t)'0', 1U);
    } else {
        while (val > 0U) {
            buf[idx] = (char)('0' + (val % 10U));
            val /= 10U;
            idx++;
        }
        while (idx > 0U) {
            idx--;
            lcd_send_byte((uint8_t)buf[idx], 1U);
        }
    }
}

/* --- Driver UART0 (9600 baudios hacia Tablet / PC) --- */
static void uart_init_hw(void) {
    UBRR0H = 0U;
    UBRR0L = 103U;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void uart_tx_byte(uint8_t b) {
    while ((UCSR0A & (1 << UDRE0)) == 0U) {
    }
    UDR0 = b;
}

static void uart_tx_str(const char *s) {
    size_t i = 0U;
    while (s[i] != '\0') {
        uart_tx_byte((uint8_t)s[i]);
        i++;
    }
}

static void uart_tx_u8(uint8_t val) {
    char buf[4];
    uint8_t idx = 0U;
    if (val == 0U) {
        uart_tx_byte((uint8_t)'0');
    } else {
        while (val > 0U) {
            buf[idx] = (char)('0' + (val % 10U));
            val /= 10U;
            idx++;
        }
        while (idx > 0U) {
            idx--;
            uart_tx_byte((uint8_t)buf[idx]);
        }
    }
}

/* --- Sensores: ADC0 (LDR en A0) y DHT11 (en D2 / PE4) --- */
static uint8_t read_light_pct(void) {
    uint16_t raw = 0U;
    ADMUX = (1 << REFS0);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
    ADCSRA |= (1 << ADSC);
    while ((ADCSRA & (1 << ADSC)) != 0U) {
    }
    raw = ADC;
    return (uint8_t)((raw * 100UL) / 1023UL);
}

static int read_dht11(EnvReading *out) {
    uint8_t data[5];
    uint8_t i = 0U;
    uint16_t guard = 0U;
    int ok = 0;

    for (i = 0U; i < 5U; i++) {
        data[i] = 0U;
    }

    DDRE |= (1 << PE4);
    PORTE &= ~(1 << PE4);
    _delay_ms(18);
    PORTE |= (1 << PE4);
    _delay_us(30);
    DDRE &= ~(1 << PE4);

    guard = 0U;
    while (((PINE & (1 << PE4)) != 0U) && (guard < 200U)) {
        _delay_us(1);
        guard++;
    }
    if (guard < 200U) {
        guard = 0U;
        while (((PINE & (1 << PE4)) == 0U) && (guard < 200U)) {
            _delay_us(1);
            guard++;
        }
        guard = 0U;
        while (((PINE & (1 << PE4)) != 0U) && (guard < 200U)) {
            _delay_us(1);
            guard++;
        }
        for (i = 0U; i < 40U; i++) {
            guard = 0U;
            while (((PINE & (1 << PE4)) == 0U) && (guard < 200U)) {
                _delay_us(1);
                guard++;
            }
            _delay_us(35);
            data[i / 8U] <<= 1;
            if ((PINE & (1 << PE4)) != 0U) {
                data[i / 8U] |= 1U;
            }
            guard = 0U;
            while (((PINE & (1 << PE4)) != 0U) && (guard < 200U)) {
                _delay_us(1);
                guard++;
            }
        }
        if (((uint8_t)(data[0] + data[1] + data[2] + data[3])) == data[4]) {
            out->hum_pct = data[0];
            out->temp_c = data[2];
            ok = 1;
        }
    }
    return ok;
}

/* --- Memoria dinamica con realloc para historial de lecturas --- */
static int push_history(DeskHubSystem *sys, const EnvReading *sample) {
    int ok = 1;
    size_t i = 0U;

    if (sys->history_count == sys->history_cap) {
        if (sys->history_cap < HUB_MAX_HISTORY) {
            size_t next_cap = sys->history_cap * 2U;
            EnvReading *resized = (EnvReading *)realloc(sys->history, next_cap * sizeof(EnvReading));
            if (resized != NULL) {
                sys->history = resized;
                sys->history_cap = next_cap;
            } else {
                ok = 0;
            }
        } else {
            for (i = 1U; i < sys->history_count; i++) {
                sys->history[i - 1U] = sys->history[i];
            }
            sys->history_count--;
        }
    }

    if (ok != 0) {
        sys->history[sys->history_count] = *sample;
        sys->history_count++;
    }
    return ok;
}

static EnvReading compute_average(const DeskHubSystem *sys) {
    EnvReading avg;
    uint16_t sum_t = 0U;
    uint16_t sum_h = 0U;
    uint16_t sum_l = 0U;
    size_t i = 0U;

    avg.temp_c = 22U;
    avg.hum_pct = 45U;
    avg.light_pct = 50U;

    if (sys->history_count > 0U) {
        for (i = 0U; i < sys->history_count; i++) {
            sum_t = (uint16_t)(sum_t + sys->history[i].temp_c);
            sum_h = (uint16_t)(sum_h + sys->history[i].hum_pct);
            sum_l = (uint16_t)(sum_l + sys->history[i].light_pct);
        }
        avg.temp_c = (uint8_t)(sum_t / sys->history_count);
        avg.hum_pct = (uint8_t)(sum_h / sys->history_count);
        avg.light_pct = (uint8_t)(sum_l / sys->history_count);
    }
    return avg;
}

/* --- Recepcion desde Tablet (ej. POMO:FOCUS,1,1500) con realloc --- */
static void parse_tablet_line(DeskHubSystem *sys) {
    if ((sys->rx_len >= 10U) &&
        (sys->rx_buf[0] == 'P') && (sys->rx_buf[1] == 'O') &&
        (sys->rx_buf[2] == 'M') && (sys->rx_buf[3] == 'O') &&
        (sys->rx_buf[4] == ':')) {
        size_t i = 5U;
        uint8_t commas = 0U;
        uint16_t secs = 0U;

        if (sys->rx_buf[5] == 'F') {
            sys->pomo.mode = POMO_FOCUS;
        } else if (sys->rx_buf[5] == 'B') {
            sys->pomo.mode = POMO_BREAK;
        }

        while (i < sys->rx_len) {
            if (sys->rx_buf[i] == ',') {
                commas++;
            } else if (commas == 1U) {
                sys->pomo.running = (uint8_t)((sys->rx_buf[i] == '1') ? 1U : 0U);
            } else if ((commas == 2U) && (sys->rx_buf[i] >= '0') && (sys->rx_buf[i] <= '9')) {
                secs = (uint16_t)((secs * 10U) + (uint16_t)(sys->rx_buf[i] - '0'));
            }
            i++;
        }
        if (commas >= 2U) {
            sys->pomo.remaining_sec = secs;
        }
    }
}

static void poll_uart_rx(DeskHubSystem *sys) {
    while ((UCSR0A & (1 << RXC0)) != 0U) {
        char c = (char)UDR0;
        if (c == '\n') {
            sys->rx_buf[sys->rx_len] = '\0';
            parse_tablet_line(sys);
            sys->rx_len = 0U;
        } else if (c != '\r') {
            if ((sys->rx_len + 1U) >= sys->rx_cap) {
                if (sys->rx_cap < HUB_RX_MAX_CAP) {
                    size_t next_cap = sys->rx_cap * 2U;
                    char *bigger = (char *)realloc(sys->rx_buf, next_cap);
                    if (bigger != NULL) {
                        sys->rx_buf = bigger;
                        sys->rx_cap = next_cap;
                    }
                } else {
                    sys->rx_len = 0U;
                }
            }
            if ((sys->rx_len + 1U) < sys->rx_cap) {
                sys->rx_buf[sys->rx_len] = c;
                sys->rx_len++;
            }
        }
    }
}

static void refresh_lcd(const DeskHubSystem *sys, const EnvReading *avg) {
    uint16_t mins = (uint16_t)(sys->pomo.remaining_sec / 60U);
    uint16_t secs = (uint16_t)(sys->pomo.remaining_sec % 60U);

    lcd_set_cursor(0U, 0U);
    lcd_print_str("T:");
    lcd_print_u16(avg->temp_c, 0U);
    lcd_print_str("C H:");
    lcd_print_u16(avg->hum_pct, 0U);
    lcd_print_str("% L:");
    lcd_print_u16(avg->light_pct, 0U);
    lcd_print_str("%  ");

    lcd_set_cursor(0U, 1U);
    if (sys->pomo.mode == POMO_BREAK) {
        lcd_print_str("DESC  ");
    } else {
        lcd_print_str("FOCUS ");
    }
    lcd_print_u16(mins, 1U);
    lcd_print_str(":");
    lcd_print_u16(secs, 1U);
    if (sys->pomo.running != 0U) {
        lcd_print_str(" RUN ");
    } else {
        lcd_print_str(" PAUS");
    }
}

int desk_hub_init(DeskHubSystem **out_sys) {
    int status = 0;
    DeskHubSystem *sys = NULL;

    if (out_sys != NULL) {
        sys = (DeskHubSystem *)malloc(sizeof(DeskHubSystem));
        if (sys != NULL) {
            sys->history_cap = 4U;
            sys->history_count = 0U;
            sys->history = (EnvReading *)malloc(sys->history_cap * sizeof(EnvReading));
            sys->rx_cap = HUB_RX_INIT_CAP;
            sys->rx_len = 0U;
            sys->rx_buf = (char *)malloc(sys->rx_cap);

            if ((sys->history != NULL) && (sys->rx_buf != NULL)) {
                sys->rx_buf[0] = '\0';
                sys->pomo.mode = POMO_FOCUS;
                sys->pomo.running = 0U;
                sys->pomo.remaining_sec = 1500U;
                sys->pomo.last_btn_state = 1U;
                sys->pomo.beep_ticks = 0U;
                sys->tick_ms = 0U;

                /* Boton D3 (PE5) pull-up y Buzzer D4 (PG5) salida */
                DDRE &= ~(1 << PE5);
                PORTE |= (1 << PE5);
                DDRG |= (1 << PG5);
                PORTG &= ~(1 << PG5);

                uart_init_hw();
                lcd_init_hw();
                *out_sys = sys;
                status = 1;
            } else {
                free(sys->history);
                free(sys->rx_buf);
                free(sys);
            }
        }
    }
    return status;
}

void desk_hub_step(DeskHubSystem *sys) {
    uint8_t btn_now = (uint8_t)((PINE & (1 << PE5)) != 0U ? 1U : 0U);

    poll_uart_rx(sys);

    /* Flanco de bajada en boton fisico D3: Start / Pause */
    if ((sys->pomo.last_btn_state == 1U) && (btn_now == 0U)) {
        sys->pomo.running = (uint8_t)((sys->pomo.running == 0U) ? 1U : 0U);
        sys->pomo.beep_ticks = 1U;
    }
    sys->pomo.last_btn_state = btn_now;

    if (sys->pomo.beep_ticks > 0U) {
        PORTG |= (1 << PG5);
        sys->pomo.beep_ticks--;
    } else {
        PORTG &= ~(1 << PG5);
    }

    _delay_ms(50);
    sys->tick_ms = (uint16_t)(sys->tick_ms + 50U);

    if (sys->tick_ms >= 1000U) {
        EnvReading sample;
        EnvReading avg;

        sys->tick_ms = 0U;
        if ((sys->pomo.running != 0U) && (sys->pomo.remaining_sec > 0U)) {
            sys->pomo.remaining_sec--;
            if (sys->pomo.remaining_sec == 0U) {
                sys->pomo.running = 0U;
                sys->pomo.beep_ticks = 6U;
            }
        }

        sample.temp_c = 22U;
        sample.hum_pct = 45U;
        if (sys->history_count > 0U) {
            sample = sys->history[sys->history_count - 1U];
        }
        (void)read_dht11(&sample);
        sample.light_pct = read_light_pct();
        (void)push_history(sys, &sample);

        avg = compute_average(sys);
        refresh_lcd(sys, &avg);

        /* Envio de telemetria exacto para desk_hub.html */
        uart_tx_str("TEMP:");
        uart_tx_u8(avg.temp_c);
        uart_tx_str(",HUM:");
        uart_tx_u8(avg.hum_pct);
        uart_tx_str(",LUX:");
        uart_tx_u8(avg.light_pct);
        uart_tx_str("\n");
    }
}

void desk_hub_destroy(DeskHubSystem *sys) {
    if (sys != NULL) {
        free(sys->history);
        free(sys->rx_buf);
        free(sys);
    }
}