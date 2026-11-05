#pragma once

// Konfiguracja wyświetlacza 4.0" SPI: ST7796S 480x320 + dotyk XPT2046.
// Wyświetlacz i dotyk dzielą jedną magistralę SPI (SPI2 / FSPI).
//
//  Moduł          ESP32-S3
//  -----------    --------
//  VCC            3V3
//  GND            GND
//  CS             GPIO 10
//  RESET          GPIO 14
//  DC/RS          GPIO 9
//  SDI(MOSI)      GPIO 11   (razem z T_DIN)
//  SCK            GPIO 12   (razem z T_CLK)
//  LED            GPIO 21   (PWM podświetlenia)
//  SDO(MISO)      NIE PODŁĄCZAĆ - w modułach ST7796 ta linia nie przechodzi
//                 w stan wysokiej impedancji i psuje odczyt dotyku
//  T_CLK          GPIO 12
//  T_CS           GPIO 4
//  T_DIN          GPIO 11
//  T_DO           GPIO 13
//  T_IRQ          GPIO 5

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define PIN_TFT_SCLK 12
#define PIN_TFT_MOSI 11
#define PIN_TFT_MISO 13 // używany tylko przez dotyk (T_DO)
#define PIN_TFT_CS 10
#define PIN_TFT_DC 9
#define PIN_TFT_RST 14
#define PIN_TFT_BL 21
#define PIN_TOUCH_CS 4
#define PIN_TOUCH_IRQ -1 // 5

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
      cfg.readable = false;  // SDO(MISO) wyświetlacza nie jest podłączone
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
      cfg.freq = 1000000;
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
