/****************************************************************************
 * boards/xtensa/esp32s2/esp32s2-saola-1/src/esp32s2_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <debug.h>

#include <errno.h>
#include <nuttx/fs/fs.h>

#ifdef CONFIG_USERLED
#  include <nuttx/leds/userled.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_TIMER
#  include "esp32s2_tim_lowerhalf.h"
#endif

#ifdef CONFIG_ESPRESSIF_WLAN
#  include "esp32s2_board_wlan.h"
#endif

#ifdef CONFIG_ESP32S2_I2C
#  include "esp32s2_i2c.h"
#endif

#ifdef CONFIG_ESP32S2_RT_TIMER
#  include "esp32s2_rt_timer.h"
#endif

#ifdef CONFIG_ESP32S2_EFUSE
#  include "esp32s2_efuse.h"
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
#  include "espressif/esp_ledc.h"
#endif

#ifdef CONFIG_WATCHDOG
#  include "esp32s2_board_wdt.h"
#endif

#ifdef CONFIG_SENSORS_MAX6675
#  include "esp32s2_max6675.h"
#endif

#ifdef CONFIG_SPI_DRIVER
#  include "esp32s2_spi.h"
#  include "esp32s2_board_spidev.h"
#endif

#ifdef CONFIG_SPI_SLAVE_DRIVER
#  include "esp32s2_spi.h"
#  include "esp32s2_board_spislavedev.h"
#endif

#ifdef CONFIG_RTC_DRIVER
#  include "esp32s2_rtc_lowerhalf.h"
#endif

#ifdef CONFIG_ESP_RMT
#  include "esp32s2_board_rmt.h"
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
#  include "espressif/esp_temperature_sensor.h"
#endif

#ifdef CONFIG_ESP_PCNT
#  include "espressif/esp_pcnt.h"
#  include "esp32s2_board_pcnt.h"
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
#  include "espressif/esp_nxdiag.h"
#endif

#ifdef CONFIG_ESPRESSIF_ADC
#  include "esp32s2_board_adc.h"
#endif

#ifdef CONFIG_ESP_SDM
#  include "espressif/esp_sdm.h"
#endif

#ifdef CONFIG_ESPRESSIF_SHA_ACCELERATOR
#  include "espressif/esp_sha.h"
#endif

#ifdef CONFIG_MMCSD_SPI
#  include "esp32s2_board_sdmmc.h"
#endif

#include "esp32s2-saola-1.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s2_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=n && CONFIG_BOARDCTL=y :
 *     Called from the NSH library
 *
 ****************************************************************************/

int esp32s2_bringup(void)
{
  int ret;

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the tmpfs file system */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at %s: %d\n",
             CONFIG_LIBC_TMPDIR, ret);
    }
#endif

#if defined(CONFIG_ESP32S2_EFUSE)
  ret = esp32s2_efuse_initialize("/dev/efuse");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init EFUSE: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESPRESSIF_SHA_ACCELERATOR) && \
    !defined(CONFIG_CRYPTO_CRYPTODEV_HARDWARE)
  ret = esp_sha_init();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to initialize SHA: %d\n", ret);
    }
#endif

#ifdef CONFIG_WATCHDOG
  /* Configure watchdog timer */

  ret = board_wdt_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize watchdog timer: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
  ret = esp32s2_pwm_setup();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp32s2_pwm_setup() failed: %d\n", ret);
    }
#endif /* CONFIG_ESPRESSIF_LEDC */

#ifdef CONFIG_ESPRESSIF_SPIFLASH
  ret = board_spiflash_init();
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI Flash\n");
    }
#endif

#ifdef CONFIG_DEV_GPIO
  ret = esp32s2_gpio_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize GPIO Driver: %d\n", ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S2_SPI2
# ifdef CONFIG_SPI_DRIVER
  ret = board_spidev_initialize(ESP32S2_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d driver: %d\n",
             ESP32S2_SPI2, ret);
    }
# elif defined(CONFIG_SPI_SLAVE_DRIVER) && defined(CONFIG_ESP32S2_SPI2_SLAVE)
  ret = board_spislavedev_initialize(ESP32S2_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
              ESP32S2_SPI2, ret);
    }
# endif
#endif

#if defined(CONFIG_SPI_SLAVE_DRIVER) && defined(CONFIG_ESP32S2_SPI3_SLAVE)
  ret = board_spislavedev_initialize(ESP32S2_SPI3);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
              ESP32S2_SPI3, ret);
    }
#endif

  /* Register the timer drivers */

#ifdef CONFIG_TIMER

#if defined(CONFIG_ESP32S2_TIMER0) && !defined(CONFIG_ONESHOT)
  ret = esp32s2_timer_initialize("/dev/timer0", TIMER0);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to initialize timer driver: %d\n",
             ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S2_TIMER1
  ret = esp32s2_timer_initialize("/dev/timer1", TIMER1);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to initialize timer driver: %d\n",
             ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S2_TIMER2
  ret = esp32s2_timer_initialize("/dev/timer2", TIMER2);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to initialize timer driver: %d\n",
             ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S2_TIMER3
  ret = esp32s2_timer_initialize("/dev/timer3", TIMER3);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to initialize timer driver: %d\n",
             ret);
      return ret;
    }
#endif

#endif /* CONFIG_TIMER */

#ifdef CONFIG_ESP32S2_RT_TIMER
  ret = esp32s2_rt_timer_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize RT timer: %d\n", ret);
    }

#endif
  /* Now register one oneshot driver */

#if defined(CONFIG_ONESHOT) && defined(CONFIG_ESP32S2_TIMER0)

  ret = board_oneshot_init(ONESHOT_TIMER, ONESHOT_RESOLUTION_US);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_oneshot_init() failed: %d\n", ret);
    }

#endif /* CONFIG_ONESHOT */

#ifdef CONFIG_I2C_DRIVER
  /* Configure I2C peripheral interfaces */

  ret = board_i2c_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2C driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32S2_TWAI

  /* Initialize TWAI and register the TWAI driver. */

  ret = board_twai_setup();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIRELESS

#ifdef CONFIG_ESPRESSIF_WLAN
  ret = board_wlan_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize wlan subsystem=%d\n",
             ret);
    }
#endif

#endif

#ifdef CONFIG_SENSORS_BMP180
  /* Try to register BMP180 device in I2C0 */

  ret = board_bmp180_initialize(0, ESP32S2_I2C0);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "Failed to initialize BMP180 driver for I2C0: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS
  /* Register the BUTTON driver */

  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_SENSORS_MAX6675
  ret = board_max6675_initialize(0, 2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MAX6675 initialization failed: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESPRESSIF_I2S) || defined(CONFIG_ESPRESSIF_I2S)

#ifdef CONFIG_AUDIO_CS4344

  /* Configure CS4344 audio on I2S0 */

  ret = esp32s2_cs4344_initialize();
  if (ret != OK)
    {
      syslog(LOG_ERR, "Failed to initialize CS4344 audio: %d\n", ret);
    }
#else

  bool i2s_enable_tx;
  bool i2s_enable_rx;

#if defined(CONFIG_ESPRESSIF_I2S_TX) || defined(CONFIG_ESPRESSIF_I2S0_TX)
  i2s_enable_tx = true;
#else
  i2s_enable_tx = false;
#endif /* CONFIG_ESPRESSIF_I2S_TX || CONFIG_ESPRESSIF_I2S0_TX */

#if defined(CONFIG_ESPRESSIF_I2S_RX) || defined(CONFIG_ESPRESSIF_I2S0_RX)
    i2s_enable_rx = true;
#else
    i2s_enable_rx = false;
#endif /* CONFIG_ESPRESSIF_I2S_RX || CONFIG_ESPRESSIF_I2S0_RX */

  /* Configure I2S generic audio on I2S0 */

  ret = board_i2sdev_initialize(i2s_enable_tx, i2s_enable_rx);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2S0 driver: %d\n", ret);
    }
#endif /* CONFIG_AUDIO_CS4344 */

#endif /* CONFIG_ESPRESSIF_I2S || CONFIG_ESPRESSIF_I2S */

#ifdef CONFIG_ESP_RMT
  ret = board_rmt_txinitialize(RMT_TXCHANNEL, RMT_OUTPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }

  ret = board_rmt_rxinitialize(RMT_RXCHANNEL, RMT_INPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
  struct esp_temp_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG(10, 50);
  ret = esp_temperature_sensor_initialize(cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize temperature sensor driver: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESP_SDM
  struct esp_sdm_chan_config_s config =
  {
    .gpio_num = 5,
    .sample_rate_hz = 1000 * 1000,
    .flags = 0,
  };

  struct dac_dev_s *dev = esp_sdminitialize(config);
  ret = dac_register("/dev/dac0", dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize DAC driver: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESP_PCNT
  ret = board_pcnt_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_pcnt_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  /* Instantiate the ESP32 RTC driver */

  ret = esp32s2_rtc_driverinit();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: Failed to Instantiate the RTC driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
  ret = esp_nxdiag_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_nxdiag_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ADC
  ret = board_adc_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_adc_init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_MMCSD_SPI
  ret = board_sdmmc_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SDMMC: %d\n", ret);
    }
#endif

  /* If we got here then perhaps not all initialization was successful, but
   * at least enough succeeded to bring-up NSH with perhaps reduced
   * capabilities.
   */

  UNUSED(ret);
  return OK;
}
