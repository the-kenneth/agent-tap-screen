#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// JC2432W328C: ST7789 240x320 on HSPI, CST820 capacitive touch (CST816S-compatible) on I2C.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_CST816S touch;

 public:
  LGFX() {
    {
      auto cfg = bus.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      bus.config(cfg);
      panel.setBus(&bus);
    }
    {
      auto cfg = panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.readable = true;
      panel.config(cfg);
    }
    {
      auto cfg = light.config();
      cfg.pin_bl = 27;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light.config(cfg);
      panel.setLight(&light);
    }
    {
      auto cfg = touch.config();
      cfg.i2c_port = 0;
      cfg.pin_sda = 33;
      cfg.pin_scl = 32;
      cfg.pin_rst = 25;
      cfg.pin_int = 21;
      cfg.i2c_addr = 0x15;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      touch.config(cfg);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};
