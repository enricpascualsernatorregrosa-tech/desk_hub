# Desk Hub — Embedded Setup Telemetry & Pomodoro Controller

Sistema embebido de escritorio desarrollado en **C puro** sobre un microcontrolador **ATmega2560 (Elegoo Mega 2560)** sincronizado mediante comunicación serie bidireccional con una interfaz web modular (`desk_hub.html`) ejecutada en tablet.

## Arquitectura del Sistema

```text
+-----------------------+       +----------------------------------------+       +-----------------------+
| Sensores y Controles  |       |       ATmega2560 (Firmware C Puro)     |       |   Tablet (Desk Hub)   |
|                       |       |                                        |       |                       |
| - DHT11 (Temp / Hum)  |------>| - Drivers directos por registros AVR   |       | - Reloj / Calendario  |
| - LDR (Luz en A0)     |------>| - Buffers dinámicos (malloc / realloc) |<=====>| - Pomodoro Sync       |
| - Pulsador Pomodoro   |------>| - Cero variables globales              | UART  | - Gráficas en vivo    |
+-----------------------+       +----------------------------------------+ 9600  +-----------------------+
                                                    |
                                                    v
                                       +-------------------------+
                                       | Actuadores Locales      |
                                       | - Pantalla LCD1602      |
                                       | - Buzzer de notificación|
                                       +-------------------------+
```

## Decisiones Técnicas y Reglas de Ingeniería

Todo el firmware del microcontrolador sigue restricciones estrictas de código limpio y control de bajo nivel:

* **C puro sin abstracciones de Arduino:** Acceso directo a registros de puertos (`DDRA`, `PORTA`, `PORTE`, `PORTG`), conversor analógico-digital (`ADMUX`, `ADCSRA`) y periférico UART (`UBRR0`, `UCSR0B`, `UDR0`).
* **Cero variables globales:** Todo el estado del sistema, historial de lecturas y buffers de recepción se encapsulan en la estructura `DeskHubSystem` pasada por referencia mediante punteros.
* **Flujo determinista (único `return`):** Todas las funciones mantienen un único punto de salida al final de su ejecución.
* **Gestión manual de memoria:** Uso explícito de `malloc`, `realloc` y `free` para el historial circular de sensores y el buffer dinámico de comandos UART, evitando automatismos como `calloc` o `memset`.
* **Modularidad:** Separación estricta entre interfaz pública (`desk_hub.h`), implementación de drivers y lógica (`desk_hub.c`) y bucle principal (`main.c`).

## Conexión de Hardware (Elegoo Mega 2560)

| Componente | Pin en Mega 2560 | Registro AVR | Función |
| :--- | :--- | :--- | :--- |
| **DHT11 (Data)** | `D2` | `PE4` | Lectura digital de temperatura (°C) y humedad (%) |
| **Fotorresistencia (LDR)** | `A0` | `ADC0` | Nivel de iluminación ambiental (%) |
| **Pulsador Pomodoro** | `D3` | `PE5` | Inicio / Pausa del temporizador (pull-up interno) |
| **Buzzer Activo** | `D4` | `PG5` | Aviso sonoro de cambio de estado o fin de bloque |
| **LCD1602 (`RS`, `E`)** | `D22`, `D23` | `PA0`, `PA1` | Control de comandos y habilitación del display |
| **LCD1602 (`D4`–`D7`)** | `D24`–`D27` | `PA2`–`PA5` | Bus de datos de 4 bits |

## Protocolo de Comunicación UART (9600 baudios)

* **Arduino $\rightarrow$ Tablet (cada 1 s):** `TEMP:23,HUM:48,LUX:76\n`
* **Tablet $\rightarrow$ Arduino (eventos Pomodoro):** `POMO:FOCUS,1,1500\n`