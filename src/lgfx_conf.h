#include <LovyanGFX.hpp>

/// 显示屏配置 — ST7789
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

public:
  LGFX(void)
  {
    // --- SPI 总线配置 ---
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI3_HOST;
      cfg.spi_mode    = 0;               // SPI mode 0
      cfg.freq_write  = 40000000;        // 降到 40MHz，提高稳定性
      cfg.freq_read   = 6000000;
      cfg.spi_3wire   = true;            // 无 MISO，共用 MOSI
      cfg.use_lock    = true;            // 启用 SPI 事务锁
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = -1;              // 不接 MISO
      cfg.pin_dc      = 11;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    // --- 面板配置 ---
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs      = 10;
      cfg.pin_rst     = 12;
      cfg.pin_busy    = -1;

      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.invert          = true;        // 大多数 ST7789 需要反色
      cfg.rgb_order       = false;       // BGR 顺序
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      cfg.readable        = true;

      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};
