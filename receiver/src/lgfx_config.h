#pragma once

// Konfiguracja wyświetlacza płytki CYD 4.0" – ESP32-3248S040:
// ST7796 480x320 + dotyk rezystancyjny XPT2046. Piny zgodne z
// ../CYD/include/cyd_config.h (sprawdzone na płytce).
//
// Ekran i dotyk dzielą magistralę HSPI (SPI2_HOST), osobne linie CS:
//
//  Sygnał         GPIO
//  -----------    ----
//  SCLK           14   (ekran + dotyk)
//  MOSI           13   (ekran + dotyk)
//  MISO           12   (ekran + dotyk; pin strapping - nic z pull-upem!)
//  TFT CS         15
//  TFT DC          2
//  TFT RST         -   (sprzężony z EN)
//  Podświetlenie  27   (PWM)
//  Dotyk CS       33
//  Dotyk IRQ      36   (nieużywany - dotyk odpytywany)
//
// Fotorezystor płytki na GPIO 34 (ADC1_CH6, tylko wejście). ADC2 nie
// działa przy włączonym Wi-Fi (synchronizacja NTP) - pin musi być z ADC1.
// Dioda RGB (4/16/17, aktywna stanem niskim) jest gaszona przy starcie.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define PIN_TFT_SCLK 14
#define PIN_TFT_MOSI 13
#define PIN_TFT_MISO 12
#define PIN_TFT_CS 15
#define PIN_TFT_DC 2
#define PIN_TFT_RST -1
#define PIN_TFT_BL 27
#define PIN_TOUCH_CS 33
#define PIN_TOUCH_IRQ -1 // 36
#define PIN_LDR 34 // fotorezystor (ADC1_CH6)
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000; // przy zakłóceniach obniż do 27 MHz
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_TFT_SCLK;
      cfg.pin_mosi = PIN_TFT_MOSI;
      cfg.pin_miso = PIN_TFT_MISO;
      cfg.pin_dc = PIN_TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_TFT_CS;
      cfg.pin_rst = PIN_TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;  // odczyt z ekranu niepotrzebny
      cfg.invert = false;    // odwrócone kolory -> zmień na true
      cfg.rgb_order = false; // zamienione R z B -> zmień na true
      cfg.dlen_16bit = false;
      cfg.bus_shared = true; // magistrala dzielona z dotykiem
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_TFT_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = PIN_TOUCH_IRQ;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 2500000;
      cfg.pin_sclk = PIN_TFT_SCLK;
      cfg.pin_mosi = PIN_TFT_MOSI;
      cfg.pin_miso = PIN_TFT_MISO;
      cfg.pin_cs = PIN_TOUCH_CS;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
