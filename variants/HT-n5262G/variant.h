#pragma once

// Adapted from Heltec's HT-n5262G variant for the stock Adafruit nRF52 core.
// Keep the core ABI declarations here; hardware-specific use belongs in
// t096_pins.h.
#define VARIANT_MCK (64000000ul)
#define USE_LFXO

#include "WVariant.h"

#define PINS_COUNT 48
#define NUM_DIGITAL_PINS 48
#define NUM_ANALOG_INPUTS 8
#define NUM_ANALOG_OUTPUTS 0

#define PIN_LED1 28
#define LED_BUILTIN PIN_LED1
#define LED_STATE_ON 1
#define PIN_BUTTON1 42

#define PIN_A0 21
#define PIN_A1 22
#define PIN_A2 23
#define PIN_A3 24
#define PIN_A4 25
#define PIN_A5 26
#define PIN_A6 27
#define PIN_A7 28
#define ADC_RESOLUTION 14

#define PIN_SERIAL1_RX 9
#define PIN_SERIAL1_TX 10

#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO 14
#define PIN_SPI_MOSI 11
#define PIN_SPI_SCK 40

static const uint8_t SS = 5;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA 7
#define PIN_WIRE_SCL 8
#define GPS_UC6580
#define GPS_BAUDRATE 115200
#define PIN_GPS_EN 6
#define GPS_EN_ACTIVE LOW
#define PIN_GPS_RESET 46
#define GPS_RESET_MODE LOW
#define PIN_GPS_PPS 43
#define GPS_RX_PIN 23
#define GPS_TX_PIN 25
#define PIN_SERIAL2_RX GPS_RX_PIN
#define PIN_SERIAL2_TX GPS_TX_PIN
#define PIN_BAT_ADC 3
#define PIN_BAT_ADC_CTL 47
#define BAT_AMPLIFY 4.9
