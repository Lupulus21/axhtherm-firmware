// hardware_map.h — UNICA fonte di verità del mapping hardware.
// Riferimenti: Schematics_AxHTherm2-5V.pdf rev 1.0 (2018-10-12).
// Le voci marcate UNVERIFIED non sono ricavabili dallo schema: hanno un default
// configurabile da UI e una procedura di verifica (docs/ANALISI.md §4).
#pragma once
#include <stdint.h>

// --- Bus I2C (schema: MCP pin 1/2, RTC, connettore OLED su WD1SCL/WD2SDA) ---
constexpr uint8_t PIN_I2C_SCL = 5;   // D1 = GPIO5  (rete WD1SCL)
constexpr uint8_t PIN_I2C_SDA = 4;   // D2 = GPIO4  (rete WD2SDA)

// --- OneWire sonde DS18B20 (schema: connettore PROBE, R4 4.7k a VCC su WD3) ---
constexpr uint8_t PIN_ONEWIRE = 0;   // D3 = GPIO0 — strapping: alto al boot garantito dal pull-up

// --- Relè su GPIO diretti (schema: ULN I1/I2/I3 <- WD6/WD7/WD8) ---
// ATTENZIONE: gli stessi pin pilotano i LED (LED2<-D6, LED1<-D7, LED3<-D8):
// VIETATO ogni blink software — commuterebbe i carichi di potenza.
constexpr uint8_t PIN_REL1 = 12;     // D6 = GPIO12 -> ULN O1 -> connettore RELAY pin 1
constexpr uint8_t PIN_REL2 = 13;     // D7 = GPIO13 -> ULN O2 -> connettore RELAY pin 2
constexpr uint8_t PIN_REL3 = 15;     // D8 = GPIO15 -> ULN O3 -> RELAY pin 3 — strapping: basso al
                                     // boot garantito dal carico d'ingresso dell'ULN2803

// --- MCP23008 (schema: A0/A1/A2 a massa -> 0x20; GP0..GP4 -> ULN I4..I8 -> REL4..REL8 su P1) ---
constexpr uint8_t I2C_ADDR_MCP23008 = 0x20;  // conferma a runtime con lo scan I2C in diagnostica

// --- RTC DS1307 (schema: modulo Tiny RTC I2C; indirizzo fisso del chip) ---
constexpr uint8_t I2C_ADDR_DS1307 = 0x68;
// Il modulo Tiny RTC porta tipicamente una EEPROM AT24C32 @0x50: ignorata. // UNVERIFIED

// --- OLED (schema: solo connettore I2C "OLED bus"; controller NON indicato) ---
// UNVERIFIED: SSD1306 o SH1106, 0x3C o 0x3D — driver e indirizzo scelti da configurazione
// (default SSD1306 @ 0x3C). Verifica: scan I2C + prova driver dalla pagina di diagnostica.
constexpr uint8_t I2C_ADDR_OLED_DEFAULT = 0x3C;

// --- Assegnazione carichi alle uscite ---
// UNVERIFIED: lo schema non etichetta REL1..REL3 per funzione. Carichi reali: frigo +
// cavo riscaldante. Mapping configurato da UI dopo il test manuale relè; finché il flag
// persistito "armed" è falso, l'automazione NON pilota alcuna uscita.

// Pin NON usati dal firmware, esposti su P1: D0=GPIO16, D4=GPIO2 (strapping, LED WEMOS),
// D5=GPIO14, A0 (schema: header P1 pin 14..17). Lasciati in alta impedenza.
