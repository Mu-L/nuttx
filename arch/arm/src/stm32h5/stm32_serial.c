/****************************************************************************
 * arch/arm/src/stm32h5/stm32_serial.c
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

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <semaphore.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/serial/serial.h>
#include <nuttx/spinlock.h>
#include <nuttx/power/pm.h>

#ifdef CONFIG_SERIAL_TERMIOS
#  include <termios.h>
#endif

#include <arch/board/board.h>

#include "chip.h"
#include "stm32_gpio.h"
#include "stm32_uart.h"
#include "stm32_dma.h"

#include "stm32_rcc.h"
#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Some sanity checks *******************************************************/

/* DMA configuration */

/* If DMA is enabled on any USART, then very that other pre-requisites
 * have also been selected.
 * UART DMA1 DMA2
 *    1  X    X
 *    2  X
 *    3  X
 *    4       X
 *    5       X
 */

#ifdef CONFIG_SERIAL_IFLOWCONTROL
#  warning STM32H5 Serial IFLOWCONTROL is untested
#endif

#ifdef SERIAL_HAVE_DMA

/* Verify that DMA has been enabled and the DMA channel has been defined.
 */

#if !defined(CONFIG_STM32H5_DMA1) && !defined(CONFIG_STM32H5_DMA2)
#  error STM32H5 Serial DMA requires one of DMA1 or DMA2 to be enabled
#endif

/* Currently RS-485 support cannot be enabled when RXDMA is in use due to
 * lack of testing - RS-485 support was developed on STM32F1x
 */

#  if (defined(CONFIG_USART1_RXDMA) && defined(CONFIG_USART1_RS485)) || \
      (defined(CONFIG_USART2_RXDMA) && defined(CONFIG_USART2_RS485)) || \
      (defined(CONFIG_USART3_RXDMA) && defined(CONFIG_USART3_RS485)) || \
      (defined(CONFIG_UART4_RXDMA) && defined(CONFIG_UART4_RS485))   || \
      (defined(CONFIG_UART5_RXDMA) && defined(CONFIG_UART5_RS485))
#    error "RXDMA and RS-485 cannot be enabled at the same time for the same U[S]ART"
#  endif

/* The DMA buffer size when using RX DMA to emulate a FIFO.
 *
 * When streaming data, the generic serial layer will be called
 * every time the FIFO receives half this number of bytes.
 *
 * If there ever is a STM32H5 with D-cache, the buffer size
 * should be an even multiple of ARMV7M_DCACHE_LINESIZE, so that it
 * can be individually invalidated.
 */

#  if !defined(CONFIG_STM32H5_SERIAL_RXDMA_BUFFER_SIZE) || \
      CONFIG_STM32H5_SERIAL_RXDMA_BUFFER_SIZE == 0
#    define RXDMA_BUFFER_SIZE 32
#  else
#    define RXDMA_BUFFER_SIZE ((CONFIG_STM32H5_SERIAL_RXDMA_BUFFER_SIZE + 31) & ~31)
#  endif

#endif

/* Power management definitions */

#if defined(CONFIG_PM) && !defined(CONFIG_STM32H5_PM_SERIAL_ACTIVITY)
#  define CONFIG_STM32H5_PM_SERIAL_ACTIVITY  10
#endif

/* Keep track if a Break was set
 *
 * Note:
 *
 * 1) This value is set in the priv->ie but never written to the control
 *    register. It must not collide with USART_CR1_USED_INTS or USART_CR3_EIE
 * 2) USART_CR3_EIE is also carried in the up_dev_s ie member.
 *
 * See stm32serial_restoreusartint where the masking is done.
 */

#ifdef CONFIG_STM32H5_SERIALBRK_BSDCOMPAT
#  define USART_CR1_IE_BREAK_INPROGRESS_SHFTS 15
#  define USART_CR1_IE_BREAK_INPROGRESS (1 << USART_CR1_IE_BREAK_INPROGRESS_SHFTS)
#endif

#ifdef USE_SERIALDRIVER
#ifdef HAVE_UART

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct stm32_serial_s
{
  struct uart_dev_s dev;       /* Generic UART device */
  uint16_t          ie;        /* Saved interrupt mask bits value */
  uint16_t          sr;        /* Saved status bits */

  /* Has been initialized and HW is setup. */

  bool              initialized;

#ifdef CONFIG_PM
  bool              suspended; /* UART device has been suspended. */

  /* Interrupt mask value stored before suspending for stop mode. */

  uint16_t          suspended_ie;
#endif

  /* If termios are supported, then the following fields may vary at
   * runtime.
   */

#ifdef CONFIG_SERIAL_TERMIOS
  uint8_t           parity;    /* 0=none, 1=odd, 2=even */
  uint8_t           bits;      /* Number of bits (7 or 8) */
  bool              stopbits2; /* True: Configure with 2 stop bits instead of 1 */
#ifdef CONFIG_SERIAL_OFLOWCONTROL
  bool              oflow;     /* output flow control (CTS) enabled */
#endif
  uint32_t          baud;      /* Configured baud */
#else
  const uint8_t     parity;    /* 0=none, 1=odd, 2=even */
  const uint8_t     bits;      /* Number of bits (7 or 8) */
  const bool        stopbits2; /* True: Configure with 2 stop bits instead of 1 */
#ifdef CONFIG_SERIAL_OFLOWCONTROL
  const bool        oflow;     /* output flow control (CTS) enabled */
#endif
  const uint32_t    baud;      /* Configured baud */
#endif
  const uint8_t     irq;       /* IRQ associated with this USART */
  const uint32_t    apbclock;  /* PCLK 1 or 2 frequency */
  const uint32_t    usartbase; /* Base address of USART registers */
  const uint32_t    tx_gpio;   /* U[S]ART TX GPIO pin configuration */
  const uint32_t    rx_gpio;   /* U[S]ART RX GPIO pin configuration */
#ifdef CONFIG_SERIAL_IFLOWCONTROL
  const uint32_t    rts_gpio;  /* U[S]ART RTS GPIO pin configuration */
#endif
#ifdef CONFIG_SERIAL_OFLOWCONTROL
  const uint32_t    cts_gpio;  /* U[S]ART CTS GPIO pin configuration */
#endif
  const bool        iflow;     /* input flow control (RTS) enabled */

  /* RX DMA state */

#ifdef SERIAL_HAVE_DMA
  DMA_HANDLE        rxdma;     /* currently-open receive DMA stream */
  bool              rxenable;  /* DMA-based reception en/disable */
#ifdef CONFIG_PM
  bool              rxdmasusp; /* Rx DMA suspended */
#endif
  uint32_t          rxdmanext; /* Next byte in the DMA buffer to be read */
  uint16_t          rxdma_req; /* GPDMA Request number */
  char          *const rxfifo; /* Receive DMA buffer */
#endif

#ifdef HAVE_RS485
  const uint32_t    rs485_dir_gpio;     /* U[S]ART RS-485 DIR GPIO pin configuration */
  const bool        rs485_dir_polarity; /* U[S]ART RS-485 DIR pin state for TX enabled */
#endif
  const bool        islpuart;  /* Is this device a Low Power UART? */
  spinlock_t        lock;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#ifndef CONFIG_SUPPRESS_UART_CONFIG
static void stm32serial_setformat(struct uart_dev_s *dev);
#endif
static int  stm32serial_setup(struct uart_dev_s *dev);
static void stm32serial_shutdown(struct uart_dev_s *dev);
static int  stm32serial_attach(struct uart_dev_s *dev);
static void stm32serial_detach(struct uart_dev_s *dev);
static int  stm32serial_interrupt(int irq, void *context, void *arg);
static int  stm32serial_ioctl(struct file *filep,
                                int cmd, unsigned long arg);
#ifndef SERIAL_HAVE_ONLY_DMA
static int  stm32serial_receive(struct uart_dev_s *dev,
                                unsigned int *status);
static void stm32serial_rxint(struct uart_dev_s *dev, bool enable);
static bool stm32serial_rxavailable(struct uart_dev_s *dev);
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
static bool stm32serial_rxflowcontrol(struct uart_dev_s *dev,
                                        unsigned int nbuffered, bool upper);
#endif
static void stm32serial_send(struct uart_dev_s *dev, int ch);
static void stm32serial_txint(struct uart_dev_s *dev, bool enable);
static bool stm32serial_txready(struct uart_dev_s *dev);

#ifdef SERIAL_HAVE_DMA
static int  stm32serial_dmasetup(struct uart_dev_s *dev);
static void stm32serial_dmashutdown(struct uart_dev_s *dev);
static int  stm32serial_dmareceive(struct uart_dev_s *dev,
                                     unsigned int *status);
static void stm32serial_dmareenable(struct stm32_serial_s *priv);
#ifdef CONFIG_SERIAL_IFLOWCONTROL
static bool stm32serial_dmaiflowrestart(struct stm32_serial_s *priv);
#endif
static void stm32serial_dmarxint(struct uart_dev_s *dev, bool enable);
static bool stm32serial_dmarxavailable(struct uart_dev_s *dev);

static void stm32serial_dmarxcallback(DMA_HANDLE handle, uint8_t status,
                                        void *arg);
#endif

#ifdef CONFIG_PM
static void stm32serial_setsuspend(struct uart_dev_s *dev, bool suspend);
static void stm32serial_pm_setsuspend(bool suspend);
static void stm32serial_pmnotify(struct pm_callback_s *cb, int domain,
                                   enum pm_state_e pmstate);
static int  stm32serial_pmprepare(struct pm_callback_s *cb, int domain,
                                    enum pm_state_e pmstate);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifndef SERIAL_HAVE_ONLY_DMA
static const struct uart_ops_s g_uart_ops =
{
  .setup          = stm32serial_setup,
  .shutdown       = stm32serial_shutdown,
  .attach         = stm32serial_attach,
  .detach         = stm32serial_detach,
  .ioctl          = stm32serial_ioctl,
  .receive        = stm32serial_receive,
  .rxint          = stm32serial_rxint,
  .rxavailable    = stm32serial_rxavailable,
#  ifdef CONFIG_SERIAL_IFLOWCONTROL
  .rxflowcontrol  = stm32serial_rxflowcontrol,
#  endif
  .send           = stm32serial_send,
  .txint          = stm32serial_txint,
  .txready        = stm32serial_txready,
  .txempty        = stm32serial_txready,
};
#endif

#ifdef SERIAL_HAVE_DMA
static const struct uart_ops_s g_uart_dma_ops =
{
  .setup          = stm32serial_dmasetup,
  .shutdown       = stm32serial_dmashutdown,
  .attach         = stm32serial_attach,
  .detach         = stm32serial_detach,
  .ioctl          = stm32serial_ioctl,
  .receive        = stm32serial_dmareceive,
  .rxint          = stm32serial_dmarxint,
  .rxavailable    = stm32serial_dmarxavailable,
#  ifdef CONFIG_SERIAL_IFLOWCONTROL
  .rxflowcontrol  = stm32serial_rxflowcontrol,
#  endif
  .send           = stm32serial_send,
  .txint          = stm32serial_txint,
  .txready        = stm32serial_txready,
  .txempty        = stm32serial_txready,
};
#endif

/* I/O buffers */

#ifdef CONFIG_STM32H5_LPUART1_SERIALDRIVER
static char g_lpuart1rxbuffer[CONFIG_LPUART1_RXBUFSIZE];
static char g_lpuart1txbuffer[CONFIG_LPUART1_TXBUFSIZE];
#  ifdef CONFIG_LPUART1_RXDMA
static char g_lpuart1rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART1_SERIALDRIVER
static char g_usart1rxbuffer[CONFIG_USART1_RXBUFSIZE];
static char g_usart1txbuffer[CONFIG_USART1_TXBUFSIZE];
#  ifdef CONFIG_USART1_RXDMA
static char g_usart1rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART2_SERIALDRIVER
static char g_usart2rxbuffer[CONFIG_USART2_RXBUFSIZE];
static char g_usart2txbuffer[CONFIG_USART2_TXBUFSIZE];
#  ifdef CONFIG_USART2_RXDMA
static char g_usart2rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART3_SERIALDRIVER
static char g_usart3rxbuffer[CONFIG_USART3_RXBUFSIZE];
static char g_usart3txbuffer[CONFIG_USART3_TXBUFSIZE];
#  ifdef CONFIG_USART3_RXDMA
static char g_usart3rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_UART4_SERIALDRIVER
static char g_uart4rxbuffer[CONFIG_UART4_RXBUFSIZE];
static char g_uart4txbuffer[CONFIG_UART4_TXBUFSIZE];
#  ifdef CONFIG_UART4_RXDMA
static char g_uart4rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_UART5_SERIALDRIVER
static char g_uart5rxbuffer[CONFIG_UART5_RXBUFSIZE];
static char g_uart5txbuffer[CONFIG_UART5_TXBUFSIZE];
#  ifdef CONFIG_UART5_RXDMA
static char g_uart5rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART6_SERIALDRIVER
static char g_usart6rxbuffer[CONFIG_USART6_RXBUFSIZE];
static char g_usart6txbuffer[CONFIG_USART6_TXBUFSIZE];
#  ifdef CONFIG_USART6_RXDMA
static char g_usart6rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H7_UART7_SERIALDRIVER
static char g_uart7rxbuffer[CONFIG_UART7_RXBUFSIZE];
static char g_uart7txbuffer[CONFIG_UART7_TXBUFSIZE];
#  ifdef CONFIG_UART7_RXDMA
static char g_uart7rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H8_UART8_SERIALDRIVER
static char g_uart8rxbuffer[CONFIG_UART8_RXBUFSIZE];
static char g_uart8txbuffer[CONFIG_UART8_TXBUFSIZE];
#  ifdef CONFIG_UART8_RXDMA
static char g_uart8rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART10_SERIALDRIVER
static char g_usart10rxbuffer[CONFIG_USART10_RXBUFSIZE];
static char g_usart10txbuffer[CONFIG_USART10_TXBUFSIZE];
#  ifdef CONFIG_USART10_RXDMA
static char g_usart10rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H5_USART11_SERIALDRIVER
static char g_usart11rxbuffer[CONFIG_USART11_RXBUFSIZE];
static char g_usart11txbuffer[CONFIG_USART11_TXBUFSIZE];
#  ifdef CONFIG_USART11_RXDMA
static char g_usart11rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

#ifdef CONFIG_STM32H12_UART12_SERIALDRIVER
static char g_uart12rxbuffer[CONFIG_UART12_RXBUFSIZE];
static char g_uart12txbuffer[CONFIG_UART12_TXBUFSIZE];
#  ifdef CONFIG_UART12_RXDMA
static char g_uart12rxfifo[RXDMA_BUFFER_SIZE];
#  endif
#endif

/* This describes the state of the STM32 USART1 ports. */

#ifdef CONFIG_STM32H5_LPUART1_SERIALDRIVER
static struct stm32_serial_s g_lpuart1priv =
{
  .dev =
    {
#  if CONSOLE_UART == 1
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_LPUART1_RXBUFSIZE,
        .buffer  = g_lpuart1rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_LPUART1_TXBUFSIZE,
        .buffer  = g_lpuart1txbuffer,
      },
#  ifdef CONFIG_LPUART1_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_lpuart1priv,
    },

  .islpuart      = true,
  .irq           = STM32_IRQ_LPUART1,
  .parity        = CONFIG_LPUART1_PARITY,
  .bits          = CONFIG_LPUART1_BITS,
  .stopbits2     = CONFIG_LPUART1_2STOP,
  .baud          = CONFIG_LPUART1_BAUD,
  .apbclock      = STM32_PCLK3_FREQUENCY,
  .usartbase     = STM32_LPUART1_BASE,
  .tx_gpio       = GPIO_LPUART1_TX,
  .rx_gpio       = GPIO_LPUART1_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_LPUART1_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_LPUART1_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_LPUART1_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_LPUART1_RTS,
#  endif
#  ifdef CONFIG_LPUART1_RXDMA
  .rxfifo        = g_lpuart1rxfifo,
  .rxdma_req     = GPDMA_REQ_LPUART1_RX,
#  endif

#  ifdef CONFIG_USART1_RS485
  .rs485_dir_gpio = GPIO_LPUART1_RS485_DIR,
#    if (CONFIG_USART1_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

#ifdef CONFIG_STM32H5_USART1_SERIALDRIVER
static struct stm32_serial_s g_usart1priv =
{
  .dev =
    {
#  if CONSOLE_UART == 2
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART1_RXBUFSIZE,
        .buffer  = g_usart1rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART1_TXBUFSIZE,
        .buffer  = g_usart1txbuffer,
      },
#  ifdef CONFIG_USART1_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart1priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART1,
  .parity        = CONFIG_USART1_PARITY,
  .bits          = CONFIG_USART1_BITS,
  .stopbits2     = CONFIG_USART1_2STOP,
  .baud          = CONFIG_USART1_BAUD,
  .apbclock      = STM32_PCLK2_FREQUENCY,
  .usartbase     = STM32_USART1_BASE,
  .tx_gpio       = GPIO_USART1_TX,
  .rx_gpio       = GPIO_USART1_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART1_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART1_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART1_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART1_RTS,
#  endif
#  ifdef CONFIG_USART1_RXDMA
  .rxfifo        = g_usart1rxfifo,
  .rxdma_req     = GPDMA_REQ_USART1_RX,
#  endif

#  ifdef CONFIG_USART1_RS485
  .rs485_dir_gpio = GPIO_USART1_RS485_DIR,
#    if (CONFIG_USART1_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 USART2 port. */

#ifdef CONFIG_STM32H5_USART2_SERIALDRIVER
static struct stm32_serial_s g_usart2priv =
{
  .dev =
    {
#  if CONSOLE_UART == 3
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART2_RXBUFSIZE,
        .buffer  = g_usart2rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART2_TXBUFSIZE,
        .buffer  = g_usart2txbuffer,
      },
#  ifdef CONFIG_USART2_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart2priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART2,
  .parity        = CONFIG_USART2_PARITY,
  .bits          = CONFIG_USART2_BITS,
  .stopbits2     = CONFIG_USART2_2STOP,
  .baud          = CONFIG_USART2_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_USART2_BASE,
  .tx_gpio       = GPIO_USART2_TX,
  .rx_gpio       = GPIO_USART2_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART2_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART2_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART2_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART2_RTS,
#  endif
#  ifdef CONFIG_USART2_RXDMA
  .rxfifo        = g_usart2rxfifo,
  .rxdma_req     = GPDMA_REQ_USART2_RX,
#  endif

#  ifdef CONFIG_USART2_RS485
  .rs485_dir_gpio = GPIO_USART2_RS485_DIR,
#    if (CONFIG_USART2_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 USART3 port. */

#ifdef CONFIG_STM32H5_USART3_SERIALDRIVER
static struct stm32_serial_s g_usart3priv =
{
  .dev =
    {
#  if CONSOLE_UART == 4
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART3_RXBUFSIZE,
        .buffer  = g_usart3rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART3_TXBUFSIZE,
        .buffer  = g_usart3txbuffer,
      },
#  ifdef CONFIG_USART3_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart3priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART3,
  .parity        = CONFIG_USART3_PARITY,
  .bits          = CONFIG_USART3_BITS,
  .stopbits2     = CONFIG_USART3_2STOP,
  .baud          = CONFIG_USART3_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_USART3_BASE,
  .tx_gpio       = GPIO_USART3_TX,
  .rx_gpio       = GPIO_USART3_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART3_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART3_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART3_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART3_RTS,
#  endif
#  ifdef CONFIG_USART3_RXDMA
  .rxfifo        = g_usart3rxfifo,
  .rxdma_req     = GPDMA_REQ_USART3_RX,
#  endif

#  ifdef CONFIG_USART3_RS485
  .rs485_dir_gpio = GPIO_USART3_RS485_DIR,
#    if (CONFIG_USART3_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART4 port. */

#ifdef CONFIG_STM32H5_UART4_SERIALDRIVER
static struct stm32_serial_s g_uart4priv =
{
  .dev =
    {
#  if CONSOLE_UART == 5
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_UART4_RXBUFSIZE,
        .buffer  = g_uart4rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_UART4_TXBUFSIZE,
        .buffer  = g_uart4txbuffer,
      },
#  ifdef CONFIG_UART4_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_uart4priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_UART4,
  .parity        = CONFIG_UART4_PARITY,
  .bits          = CONFIG_UART4_BITS,
  .stopbits2     = CONFIG_UART4_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART4_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART4_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART4_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART4_RTS,
#  endif
  .baud          = CONFIG_UART4_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_UART4_BASE,
  .tx_gpio       = GPIO_UART4_TX,
  .rx_gpio       = GPIO_UART4_RX,
#  ifdef CONFIG_UART4_RXDMA
  .rxfifo        = g_uart4rxfifo,
  .rxdma_req     = GPDMA_REQ_UART4_RX,
#  endif

#  ifdef CONFIG_UART4_RS485
  .rs485_dir_gpio = GPIO_UART4_RS485_DIR,
#    if (CONFIG_UART4_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART5 port. */

#ifdef CONFIG_STM32H5_UART5_SERIALDRIVER
static struct stm32_serial_s g_uart5priv =
{
  .dev =
    {
#  if CONSOLE_UART == 6
      .isconsole = true,
#  endif
      .recv     =
      {
        .size   = CONFIG_UART5_RXBUFSIZE,
        .buffer = g_uart5rxbuffer,
      },
      .xmit     =
      {
        .size   = CONFIG_UART5_TXBUFSIZE,
        .buffer = g_uart5txbuffer,
      },
#  ifdef CONFIG_UART5_RXDMA
      .ops      = &g_uart_dma_ops,
#  else
      .ops      = &g_uart_ops,
#  endif
      .priv     = &g_uart5priv,
    },

  .islpuart      = false,
  .irq            = STM32_IRQ_UART5,
  .parity         = CONFIG_UART5_PARITY,
  .bits           = CONFIG_UART5_BITS,
  .stopbits2      = CONFIG_UART5_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART5_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART5_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART5_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART5_RTS,
#  endif
  .baud           = CONFIG_UART5_BAUD,
  .apbclock       = STM32_PCLK1_FREQUENCY,
  .usartbase      = STM32_UART5_BASE,
  .tx_gpio        = GPIO_UART5_TX,
  .rx_gpio        = GPIO_UART5_RX,
#  ifdef CONFIG_UART5_RXDMA
  .rxfifo        = g_uart5rxfifo,
  .rxdma_req     = GPDMA_REQ_UART5_RX,
#  endif

#  ifdef CONFIG_UART5_RS485
  .rs485_dir_gpio = GPIO_UART5_RS485_DIR,
#    if (CONFIG_UART5_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 USART6 port. */

#ifdef CONFIG_STM32H5_USART6_SERIALDRIVER
static struct stm32_serial_s g_usart6priv =
{
  .dev =
    {
#  if CONSOLE_UART == 7
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART6_RXBUFSIZE,
        .buffer  = g_usart6rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART6_TXBUFSIZE,
        .buffer  = g_usart6txbuffer,
      },
#  ifdef CONFIG_USART6_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart6priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART6,
  .parity        = CONFIG_USART6_PARITY,
  .bits          = CONFIG_USART6_BITS,
  .stopbits2     = CONFIG_USART6_2STOP,
  .baud          = CONFIG_USART6_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_USART6_BASE,
  .tx_gpio       = GPIO_USART6_TX,
  .rx_gpio       = GPIO_USART6_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART6_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART6_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART6_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART6_RTS,
#  endif
#  ifdef CONFIG_USART6_RXDMA
  .rxfifo        = g_usart6rxfifo,
  .rxdma_req     = GPDMA_REQ_USART6_RX,
#  endif

#  ifdef CONFIG_USART6_RS485
  .rs485_dir_gpio = GPIO_USART6_RS485_DIR,
#    if (CONFIG_USART6_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART7 port. */

#ifdef CONFIG_STM32H5_UART7_SERIALDRIVER
static struct stm32_serial_s g_uart7priv =
{
  .dev =
    {
#  if CONSOLE_UART == 8
      .isconsole = true,
#  endif
      .recv     =
      {
        .size   = CONFIG_UART7_RXBUFSIZE,
        .buffer = g_uart7rxbuffer,
      },
      .xmit     =
      {
        .size   = CONFIG_UART7_TXBUFSIZE,
        .buffer = g_uart7txbuffer,
      },
#  ifdef CONFIG_UART7_RXDMA
      .ops      = &g_uart_dma_ops,
#  else
      .ops      = &g_uart_ops,
#  endif
      .priv     = &g_uart7priv,
    },

  .islpuart      = false,
  .irq            = STM32_IRQ_UART7,
  .parity         = CONFIG_UART7_PARITY,
  .bits           = CONFIG_UART7_BITS,
  .stopbits2      = CONFIG_UART7_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART7_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART7_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART7_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART7_RTS,
#  endif
  .baud           = CONFIG_UART7_BAUD,
  .apbclock       = STM32_PCLK1_FREQUENCY,
  .usartbase      = STM32_UART7_BASE,
  .tx_gpio        = GPIO_UART7_TX,
  .rx_gpio        = GPIO_UART7_RX,
#  ifdef CONFIG_UART7_RXDMA
  .rxfifo        = g_uart7rxfifo,
  .rxdma_req     = GPDMA_REQ_UART7_RX,
#  endif

#  ifdef CONFIG_UART7_RS485
  .rs485_dir_gpio = GPIO_UART7_RS485_DIR,
#    if (CONFIG_UART7_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART8 port. */

#ifdef CONFIG_STM32H5_UART8_SERIALDRIVER
static struct stm32_serial_s g_uart8priv =
{
  .dev =
    {
#  if CONSOLE_UART == 9
      .isconsole = true,
#  endif
      .recv     =
      {
        .size   = CONFIG_UART8_RXBUFSIZE,
        .buffer = g_uart8rxbuffer,
      },
      .xmit     =
      {
        .size   = CONFIG_UART8_TXBUFSIZE,
        .buffer = g_uart8txbuffer,
      },
#  ifdef CONFIG_UART8_RXDMA
      .ops      = &g_uart_dma_ops,
#  else
      .ops      = &g_uart_ops,
#  endif
      .priv     = &g_uart8priv,
    },

  .islpuart      = false,
  .irq            = STM32_IRQ_UART8,
  .parity         = CONFIG_UART8_PARITY,
  .bits           = CONFIG_UART8_BITS,
  .stopbits2      = CONFIG_UART8_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART8_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART8_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART8_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART8_RTS,
#  endif
  .baud           = CONFIG_UART8_BAUD,
  .apbclock       = STM32_PCLK1_FREQUENCY,
  .usartbase      = STM32_UART8_BASE,
  .tx_gpio        = GPIO_UART8_TX,
  .rx_gpio        = GPIO_UART8_RX,
#  ifdef CONFIG_UART8_RXDMA
  .rxfifo        = g_uart8rxfifo,
  .rxdma_req     = GPDMA_REQ_UART8_RX,
#  endif

#  ifdef CONFIG_UART8_RS485
  .rs485_dir_gpio = GPIO_UART8_RS485_DIR,
#    if (CONFIG_UART8_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART9 port. */

#ifdef CONFIG_STM32H5_UART9_SERIALDRIVER
static struct stm32_serial_s g_uart9priv =
{
  .dev =
    {
#  if CONSOLE_UART == 10
      .isconsole = true,
#  endif
      .recv     =
      {
        .size   = CONFIG_UART9_RXBUFSIZE,
        .buffer = g_uart9rxbuffer,
      },
      .xmit     =
      {
        .size   = CONFIG_UART9_TXBUFSIZE,
        .buffer = g_uart9txbuffer,
      },
#  ifdef CONFIG_UART9_RXDMA
      .ops      = &g_uart_dma_ops,
#  else
      .ops      = &g_uart_ops,
#  endif
      .priv     = &g_uart9priv,
    },

  .islpuart      = false,
  .irq            = STM32_IRQ_UART9,
  .parity         = CONFIG_UART9_PARITY,
  .bits           = CONFIG_UART9_BITS,
  .stopbits2      = CONFIG_UART9_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART9_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART9_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART9_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART9_RTS,
#  endif
  .baud           = CONFIG_UART9_BAUD,
  .apbclock       = STM32_PCLK1_FREQUENCY,
  .usartbase      = STM32_UART9_BASE,
  .tx_gpio        = GPIO_UART9_TX,
  .rx_gpio        = GPIO_UART9_RX,
#  ifdef CONFIG_UART9_RXDMA
  .rxfifo        = g_uart9rxfifo,
  .rxdma_req     = GPDMA_REQ_UART9_RX,
#  endif

#  ifdef CONFIG_UART9_RS485
  .rs485_dir_gpio = GPIO_UART9_RS485_DIR,
#    if (CONFIG_UART9_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 USART10 port. */

#ifdef CONFIG_STM32H5_USART10_SERIALDRIVER
static struct stm32_serial_s g_usart10priv =
{
  .dev =
    {
#  if CONSOLE_UART == 11
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART10_RXBUFSIZE,
        .buffer  = g_usart10rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART10_TXBUFSIZE,
        .buffer  = g_usart10txbuffer,
      },
#  ifdef CONFIG_USART10_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart10priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART10,
  .parity        = CONFIG_USART10_PARITY,
  .bits          = CONFIG_USART10_BITS,
  .stopbits2     = CONFIG_USART10_2STOP,
  .baud          = CONFIG_USART10_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_USART10_BASE,
  .tx_gpio       = GPIO_USART10_TX,
  .rx_gpio       = GPIO_USART10_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART10_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART10_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART10_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART10_RTS,
#  endif
#  ifdef CONFIG_USART10_RXDMA
  .rxfifo        = g_usart10rxfifo,
  .rxdma_req     = GPDMA_REQ_USART10_RX,
#  endif

#  ifdef CONFIG_USART10_RS485
  .rs485_dir_gpio = GPIO_USART10_RS485_DIR,
#    if (CONFIG_USART10_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 USART11 port. */

#ifdef CONFIG_STM32H5_USART11_SERIALDRIVER
static struct stm32_serial_s g_usart11priv =
{
  .dev =
    {
#  if CONSOLE_UART == 12
      .isconsole = true,
#  endif
      .recv      =
      {
        .size    = CONFIG_USART11_RXBUFSIZE,
        .buffer  = g_usart11rxbuffer,
      },
      .xmit      =
      {
        .size    = CONFIG_USART11_TXBUFSIZE,
        .buffer  = g_usart11txbuffer,
      },
#  ifdef CONFIG_USART11_RXDMA
      .ops       = &g_uart_dma_ops,
#  else
      .ops       = &g_uart_ops,
#  endif
      .priv      = &g_usart11priv,
    },

  .islpuart      = false,
  .irq           = STM32_IRQ_USART11,
  .parity        = CONFIG_USART11_PARITY,
  .bits          = CONFIG_USART11_BITS,
  .stopbits2     = CONFIG_USART11_2STOP,
  .baud          = CONFIG_USART11_BAUD,
  .apbclock      = STM32_PCLK1_FREQUENCY,
  .usartbase     = STM32_USART11_BASE,
  .tx_gpio       = GPIO_USART11_TX,
  .rx_gpio       = GPIO_USART11_RX,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_USART11_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_USART11_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_USART11_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_USART11_RTS,
#  endif
#  ifdef CONFIG_USART11_RXDMA
  .rxfifo        = g_usart11rxfifo,
  .rxdma_req     = GPDMA_REQ_USART11_RX,
#  endif

#  ifdef CONFIG_USART11_RS485
  .rs485_dir_gpio = GPIO_USART11_RS485_DIR,
#    if (CONFIG_USART11_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This describes the state of the STM32 UART12 port. */

#ifdef CONFIG_STM32H5_UART12_SERIALDRIVER
static struct stm32_serial_s g_uart12priv =
{
  .dev =
    {
#  if CONSOLE_UART == 13
      .isconsole = true,
#  endif
      .recv     =
      {
        .size   = CONFIG_UART12_RXBUFSIZE,
        .buffer = g_uart12rxbuffer,
      },
      .xmit     =
      {
        .size   = CONFIG_UART12_TXBUFSIZE,
        .buffer = g_uart12txbuffer,
      },
#  ifdef CONFIG_UART12_RXDMA
      .ops      = &g_uart_dma_ops,
#  else
      .ops      = &g_uart_ops,
#  endif
      .priv     = &g_uart12priv,
    },

  .islpuart      = false,
  .irq            = STM32_IRQ_UART12,
  .parity         = CONFIG_UART12_PARITY,
  .bits           = CONFIG_UART12_BITS,
  .stopbits2      = CONFIG_UART12_2STOP,
#  if defined(CONFIG_SERIAL_OFLOWCONTROL) && defined(CONFIG_UART12_OFLOWCONTROL)
  .oflow         = true,
  .cts_gpio      = GPIO_UART12_CTS,
#  endif
#  if defined(CONFIG_SERIAL_IFLOWCONTROL) && defined(CONFIG_UART12_IFLOWCONTROL)
  .iflow         = true,
  .rts_gpio      = GPIO_UART12_RTS,
#  endif
  .baud           = CONFIG_UART12_BAUD,
  .apbclock       = STM32_PCLK1_FREQUENCY,
  .usartbase      = STM32_UART12_BASE,
  .tx_gpio        = GPIO_UART12_TX,
  .rx_gpio        = GPIO_UART12_RX,
#  ifdef CONFIG_UART12_RXDMA
  .rxfifo        = g_uart12rxfifo,
  .rxdma_req     = GPDMA_REQ_UART12_RX,
#  endif

#  ifdef CONFIG_UART12_RS485
  .rs485_dir_gpio = GPIO_UART12_RS485_DIR,
#    if (CONFIG_UART12_RS485_DIR_POLARITY == 0)
  .rs485_dir_polarity = false,
#    else
  .rs485_dir_polarity = true,
#    endif
#  endif
  .lock               = SP_UNLOCKED,
};
#endif

/* This table lets us iterate over the configured USARTs */

static struct stm32_serial_s * const
  g_uart_devs[STM32H5_NLPUART + STM32H5_NUSART + STM32H5_NUART] =
{
#ifdef CONFIG_STM32H5_LPUART1_SERIALDRIVER
  [0] = &g_lpuart1priv,
#endif
#ifdef CONFIG_STM32H5_USART1_SERIALDRIVER
  [1] = &g_usart1priv,
#endif
#ifdef CONFIG_STM32H5_USART2_SERIALDRIVER
  [2] = &g_usart2priv,
#endif
#ifdef CONFIG_STM32H5_USART3_SERIALDRIVER
  [3] = &g_usart3priv,
#endif
#ifdef CONFIG_STM32H5_UART4_SERIALDRIVER
  [4] = &g_uart4priv,
#endif
#ifdef CONFIG_STM32H5_UART5_SERIALDRIVER
  [5] = &g_uart5priv,
#endif
#ifdef CONFIG_STM32H5_USART6_SERIALDRIVER
  [6] = &g_usart6priv,
#endif
#ifdef CONFIG_STM32H5_UART7_SERIALDRIVER
  [7] = &g_uart7priv,
#endif
#ifdef CONFIG_STM32H5_UART8_SERIALDRIVER
  [8] = &g_uart8priv,
#endif
#ifdef CONFIG_STM32H5_UART9_SERIALDRIVER
  [9] = &g_uart9priv,
#endif
#ifdef CONFIG_STM32H5_USART10_SERIALDRIVER
  [10] = &g_usart10priv,
#endif
#ifdef CONFIG_STM32H5_USART11_SERIALDRIVER
  [11] = &g_usart11priv,
#endif
#ifdef CONFIG_STM32H5_UART12_SERIALDRIVER
  [12] = &g_uart12priv,
#endif
};

#ifdef CONFIG_PM
struct serialpm_s
{
  struct pm_callback_s pm_cb;
  bool serial_suspended;
};

static struct serialpm_s g_serialpm =
{
  .pm_cb.notify  = stm32serial_pmnotify,
  .pm_cb.prepare = stm32serial_pmprepare,
  .serial_suspended = false
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32serial_getreg
 ****************************************************************************/

static inline
uint32_t stm32serial_getreg(struct stm32_serial_s *priv, int offset)
{
  return getreg32(priv->usartbase + offset);
}

/****************************************************************************
 * Name: stm32serial_putreg
 ****************************************************************************/

static inline
void stm32serial_putreg(struct stm32_serial_s *priv,
                          int offset, uint32_t value)
{
  putreg32(value, priv->usartbase + offset);
}

/****************************************************************************
 * Name: stm32serial_setusartint
 ****************************************************************************/

static inline
void stm32serial_setusartint(struct stm32_serial_s *priv,
                               uint16_t ie)
{
  uint32_t cr;

  /* Save the interrupt mask */

  priv->ie = ie;

  /* And restore the interrupt state (see the interrupt enable/usage table
   * above)
   */

  cr = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
  cr &= ~(USART_CR1_USED_INTS);
  cr |= (ie & (USART_CR1_USED_INTS));
  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr);

  cr = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);
  cr &= ~USART_CR3_EIE;
  cr |= (ie & USART_CR3_EIE);
  stm32serial_putreg(priv, STM32_USART_CR3_OFFSET, cr);
}

/****************************************************************************
 * Name: up_restoreusartint
 ****************************************************************************/

static void stm32serial_restoreusartint(struct stm32_serial_s *priv,
                                          uint16_t ie)
{
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);

  stm32serial_setusartint(priv, ie);

  spin_unlock_irqrestore(&priv->lock, flags);
}

/****************************************************************************
 * Name: stm32serial_disableusartint
 ****************************************************************************/

static void stm32serial_disableusartint(struct stm32_serial_s *priv,
                                          uint16_t *ie)
{
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);

  if (ie)
    {
      uint32_t cr1;
      uint32_t cr3;

      /* USART interrupts:
       *
       * Enable           Status         Meaning                Usage
       * ---------------- -------------- ---------------------- -------------
       * USART_CR1_IDLEIE USART_ISR_IDLE Idle Line Detected     (not used)
       * USART_CR1_RXNEIE USART_ISR_RXNE Received Data Ready to
       *                                 be Read
       * "              " USART_ISR_ORE  Overrun Error Detected
       * USART_CR1_TCIE   USART_ISR_TC   Transmission Complete  (only RS-485)
       * USART_CR1_TXEIE  USART_ISR_TXE  Transmit Data Register
       *                                 Empty
       * USART_CR1_PEIE   USART_ISR_PE   Parity Error
       *
       * USART_CR2_LBDIE  USART_ISR_LBD  Break Flag             (not used)
       * USART_CR3_EIE    USART_ISR_FE   Framing Error
       * "           "    USART_ISR_NF   Noise Flag
       * "           "    USART_ISR_ORE  Overrun Error Detected
       * USART_CR3_CTSIE  USART_ISR_CTS  CTS flag               (not used)
       */

      cr1 = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
      cr3 = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);

      /* Return the current interrupt mask value for the used interrupts.
       * Notice that this depends on the fact that none of the used interrupt
       * enable bits overlap.  This logic would fail if we needed the break
       * interrupt!
       */

      *ie = (cr1 & (USART_CR1_USED_INTS)) | (cr3 & USART_CR3_EIE);
    }

  /* Disable all interrupts */

  stm32serial_setusartint(priv, 0);

  spin_unlock_irqrestore(&priv->lock, flags);
}

/****************************************************************************
 * Name: stm32serial_dmanextrx
 *
 * Description:
 *   Returns the index into the RX FIFO where the DMA will place the next
 *   byte that it receives.
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static int stm32serial_dmanextrx(struct stm32_serial_s *priv)
{
  size_t dmaresidual;

  dmaresidual = stm32_dmaresidual(priv->rxdma);

  return (RXDMA_BUFFER_SIZE - (int)dmaresidual);
}
#endif

/****************************************************************************
 * Name: stm32serial_setformat
 *
 * Description:
 *   Set the serial line format and speed.
 *
 ****************************************************************************/

#ifndef CONFIG_SUPPRESS_UART_CONFIG
static void stm32serial_setformat(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  uint32_t regval;
  uint32_t brr;
  uint32_t cr1;

  /* This first implementation is for U[S]ARTs that support oversampling
   * by 8 in additional to the standard oversampling by 16.
   */
#ifdef CONFIG_STM32H5_LPUART1
  if (priv->islpuart == true)
    {
      /* LPUART BRR (19:00) = (256*apbclock_hz/baud_rate) */

      uint32_t apbclock_whole = priv->apbclock;
      uint32_t clock_baud_ratio = apbclock_whole / priv->baud;
      uint32_t presc_reg = 0x0;

      /* LPUART PRESC (3:0)
       * Divide the apbclock if necessary for low baud rates
       * 3 * baud_rate <= apbclock_whole <= 4096 * baud_rate
       */

      if (clock_baud_ratio <= 4096)
        {
          presc_reg = 0x0;
        }
      else if (clock_baud_ratio > 4096 && clock_baud_ratio <= 8192)
        {
          presc_reg = 0x1;
          apbclock_whole >>= 1;
        }
      else if (clock_baud_ratio > 8192 && clock_baud_ratio <= 16384)
        {
          presc_reg = 0x2;
          apbclock_whole >>= 2;
        }
      else if (clock_baud_ratio > 16384 && clock_baud_ratio <= 24576)
        {
          presc_reg = 0x3;
          apbclock_whole /= 6;
        }
      else if (clock_baud_ratio > 24576 && clock_baud_ratio <= 32768)
        {
          presc_reg = 0x4;
          apbclock_whole >>= 3;
        }
      else if (clock_baud_ratio > 32768 && clock_baud_ratio <= 40960)
        {
          presc_reg = 0x5;
          apbclock_whole /= 10;
        }
      else if (clock_baud_ratio > 40960 && clock_baud_ratio <= 49152)
        {
          presc_reg = 0x6;
          apbclock_whole /= 12;
        }
      else if (clock_baud_ratio > 32768 && clock_baud_ratio <= 65536)
        {
          presc_reg = 0x7;
          apbclock_whole >>= 4;
        }
      else if (clock_baud_ratio > 65536  && clock_baud_ratio <= 131072)
        {
          presc_reg = 0x8;
          apbclock_whole >>= 5;
        }
      else if (clock_baud_ratio > 131072  && clock_baud_ratio <= 262144)
        {
          presc_reg = 0x9;
          apbclock_whole >>= 6;
        }
      else if (clock_baud_ratio > 262144  && clock_baud_ratio <= 524288)
        {
          presc_reg = 0xa;
          apbclock_whole >>= 7;
        }
      else
        {
          presc_reg = 0xb;
          apbclock_whole >>= 8;
        }

      /* Write the PRESC register */

      stm32serial_putreg(priv, STM32_USART_PRESC_OFFSET, presc_reg);

      /* Set the LPUART BRR value after setting Prescaler
       * BRR = ( (256 * apbclock_whole) + baud_rate / 2 ) / baud_rate
       */

      brr = (((uint64_t)apbclock_whole << 8) + (priv->baud >> 1)) / \
               priv->baud;
    }
  else
#endif /* CONFIG_STM32H5_LPUART1 */
    {
      uint32_t usartdiv8;

      /* In case of oversampling by 8, the equation is:
       *
       *   baud      = 2 * fCK / usartdiv8
       *   usartdiv8 = 2 * fCK / baud
       */

      usartdiv8 = ((priv->apbclock << 1) + (priv->baud >> 1)) / priv->baud;

      /* Baud rate for standard USART (SPI mode included):
       *
       * In case of oversampling by 16, the equation is:
       *   baud       = fCK / usartdiv16
       *   usartdiv16 = fCK / baud
       *              = 2 * usartdiv8
       */

      /* Use oversamply by 8 only if divisor is small. But what is small? */

      cr1 = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
      if (usartdiv8 > 2000)
        {
          /* Use usartdiv16 */

          brr  = (usartdiv8 + 1) >> 1;

          /* Clear oversampling by 8 to enable oversampling by 16 */

          cr1 &= ~USART_CR1_OVER8;
        }
      else
        {
          DEBUGASSERT(usartdiv8 >= 8);

          /* Perform mysterious operations on bits 0-3 */

          brr  = ((usartdiv8 & 0xfff0) | ((usartdiv8 & 0x000f) >> 1));

          /* Set oversampling by 8 */

          cr1 |= USART_CR1_OVER8;
        }
    }

  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1);
  stm32serial_putreg(priv, STM32_USART_BRR_OFFSET, brr);

  /* Configure parity mode */

  regval  = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
  regval &= ~(USART_CR1_PCE | USART_CR1_PS | USART_CR1_M0 | USART_CR1_M1);

  if (priv->parity == 1)       /* Odd parity */
    {
      regval |= (USART_CR1_PCE | USART_CR1_PS);
    }
  else if (priv->parity == 2)  /* Even parity */
    {
      regval |= USART_CR1_PCE;
    }

  /* Configure word length (parity uses one of configured bits)
   *
   * Default: 1 start, 8 data (no parity), n stop, OR
   *          1 start, 7 data + parity, n stop
   */

  if (priv->bits == 9 || (priv->bits == 8 && priv->parity != 0))
    {
      /* Select: 1 start, 8 data + parity, n stop, OR
       *         1 start, 9 data (no parity), n stop.
       */

      regval |= USART_CR1_M0;
    }
  else if (priv->bits == 7 && priv->parity == 0)
    {
      /* Select: 1 start, 7 data (no parity), n stop, OR
       */

      regval |= USART_CR1_M1;
    }

  /* Else Select: 1 start, 7 data + parity, n stop, OR
   *              1 start, 8 data (no parity), n stop.
   */

  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, regval);

  /* Configure STOP bits */

  regval = stm32serial_getreg(priv, STM32_USART_CR2_OFFSET);
  regval &= ~(USART_CR2_STOP_MASK);

  if (priv->stopbits2)
    {
      regval |= USART_CR2_STOP2;
    }

  stm32serial_putreg(priv, STM32_USART_CR2_OFFSET, regval);

  /* Configure hardware flow control */

  regval  = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);
  regval &= ~(USART_CR3_CTSE | USART_CR3_RTSE);

#if defined(CONFIG_SERIAL_IFLOWCONTROL) && !defined(CONFIG_STM32H5_FLOWCONTROL_BROKEN)
  if (priv->iflow && (priv->rts_gpio != 0))
    {
      regval |= USART_CR3_RTSE;
    }
#endif

#ifdef CONFIG_SERIAL_OFLOWCONTROL
  if (priv->oflow && (priv->cts_gpio != 0))
    {
      regval |= USART_CR3_CTSE;
    }
#endif

  stm32serial_putreg(priv, STM32_USART_CR3_OFFSET, regval);
}
#endif /* CONFIG_SUPPRESS_UART_CONFIG */

/****************************************************************************
 * Name: stm32serial_setsuspend
 *
 * Description:
 *   Suspend or resume serial peripheral.
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static void stm32serial_setsuspend(struct uart_dev_s *dev, bool suspend)
{
  struct stm32_serial_s *priv = (struct stm32_serial_s *)dev->priv;
#ifdef SERIAL_HAVE_DMA
  bool dmarestored = false;
#endif

  if (priv->suspended == suspend)
    {
      return;
    }

  priv->suspended = suspend;

  if (suspend)
    {
#ifdef CONFIG_SERIAL_IFLOWCONTROL
      if (priv->iflow)
        {
          /* Force RTS high to prevent further Rx. */

          stm32_configgpio((priv->rts_gpio & ~GPIO_MODE_MASK)
                             | (GPIO_OUTPUT | GPIO_OUTPUT_SET));
        }
#endif

      /* Disable interrupts to prevent Tx. */

      stm32serial_disableusartint(priv, &priv->suspended_ie);

      /* Wait last Tx to complete. */

      while ((stm32serial_getreg(priv, STM32_USART_ISR_OFFSET) &
              USART_ISR_TC) == 0);

#ifdef SERIAL_HAVE_DMA
      if (priv->dev.ops == &g_uart_dma_ops && !priv->rxdmasusp)
        {
#ifdef CONFIG_SERIAL_IFLOWCONTROL
          if (priv->iflow && priv->rxdmanext == RXDMA_BUFFER_SIZE)
            {
              /* Rx DMA in non-circular iflow mode and already stopped
               * at end of DMA buffer. No need to suspend.
               */
            }
          else
#endif
            {
              /* Suspend Rx DMA. */

              stm32h5_dmastop(priv->rxdma);
              priv->rxdmasusp = true;
            }
        }
#endif
    }
  else
    {
#ifdef SERIAL_HAVE_DMA
      if (priv->dev.ops == &g_uart_dma_ops && priv->rxdmasusp)
        {
#ifdef CONFIG_SERIAL_IFLOWCONTROL
          if (priv->iflow)
            {
              stm32serial_dmaiflowrestart(priv);
            }
          else
#endif
            {
              /* This USART does not have HW flow-control. Unconditionally
               * re-enable DMA (might loss unprocessed bytes received
               * to DMA buffer before suspending).
               */

              stm32serial_dmareenable(priv);
              priv->rxdmasusp = false;
            }

          dmarestored = true;
        }
#endif

      /* Re-enable interrupts to resume Tx. */

      stm32serial_restoreusartint(priv, priv->suspended_ie);

#ifdef CONFIG_SERIAL_IFLOWCONTROL
      if (priv->iflow)
        {
          /* Restore peripheral RTS control. */

          stm32_configgpio(priv->rts_gpio);
        }
#endif
    }

#ifdef SERIAL_HAVE_DMA
  if (dmarestored)
    {
      irqstate_t flags;

      flags = enter_critical_section();

      /* Perform initial Rx DMA buffer fetch to wake-up serial device
       * activity.
       */

      if (priv->rxdma != NULL)
        {
          stm32serial_dmarxcallback(priv->rxdma, 0, priv);
        }

      leave_critical_section(flags);
    }
#endif
}
#endif

/****************************************************************************
 * Name: stm32serial_pm_setsuspend
 *
 * Description:
 *   Suspend or resume serial peripherals for/from deep-sleep/stop modes.
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static void stm32serial_pm_setsuspend(bool suspend)
{
  int n;

  /* Already in desired state? */

  if (suspend == g_serialpm.serial_suspended)
    return;

  g_serialpm.serial_suspended = suspend;

  for (n = 0; n < STM32H5_NLPUART + STM32H5_NUSART + STM32H5_NUART; n++)
    {
      struct stm32_serial_s *priv = g_uart_devs[n];

      if (!priv || !priv->initialized)
        {
          continue;
        }

      stm32serial_setsuspend(&priv->dev, suspend);
    }
}
#endif

/****************************************************************************
 * Name: stm32serial_setapbclock
 *
 * Description:
 *   Enable or disable APB clock for the USART peripheral
 *
 * Input Parameters:
 *   dev - A reference to the UART driver state structure
 *   on  - Enable clock if 'on' is 'true' and disable if 'false'
 *
 ****************************************************************************/

static void stm32serial_setapbclock(struct uart_dev_s *dev, bool on)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  uint32_t rcc_en;
  uint32_t regaddr;

  /* Determine which USART to configure */

  switch (priv->usartbase)
    {
    default:
      return;
#ifdef CONFIG_STM32H5_LPUART1_SERIALDRIVER
    case STM32_LPUART1_BASE:
      rcc_en = RCC_APB3ENR_LPUART1EN ;
      regaddr = STM32_RCC_APB3ENR;
      break;
#endif
#ifdef CONFIG_STM32H5_USART1_SERIALDRIVER
    case STM32_USART1_BASE:
      rcc_en = RCC_APB2ENR_USART1EN ;
      regaddr = STM32_RCC_APB2ENR;
      break;
#endif
#ifdef CONFIG_STM32H5_USART2_SERIALDRIVER
    case STM32_USART2_BASE:
      rcc_en = RCC_APB1LENR_USART2EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_USART3_SERIALDRIVER
    case STM32_USART3_BASE:
      rcc_en = RCC_APB1LENR_USART3EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART4_SERIALDRIVER
    case STM32_UART4_BASE:
      rcc_en = RCC_APB1LENR_UART4EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART5_SERIALDRIVER
    case STM32_UART5_BASE:
      rcc_en = RCC_APB1LENR_UART5EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_USART6_SERIALDRIVER
    case STM32_USART6_BASE:
      rcc_en = RCC_APB1LENR_USART6EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART7_SERIALDRIVER
    case STM32_UART7_BASE:
      rcc_en = RCC_APB1LENR_UART7EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART8_SERIALDRIVER
    case STM32_UART8_BASE:
      rcc_en = RCC_APB1LENR_UART8EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART9_SERIALDRIVER
    case STM32_UART9_BASE:
      rcc_en = RCC_APB1HENR_UART9EN;
      regaddr = STM32_RCC_APB1HENR;
      break;
#endif

#ifdef CONFIG_STM32H5_USART10_SERIALDRIVER
    case STM32_USART10_BASE:
      rcc_en = RCC_APB1LENR_USART10EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_USART11_SERIALDRIVER
    case STM32_USART11_BASE:
      rcc_en = RCC_APB1LENR_USART11EN;
      regaddr = STM32_RCC_APB1LENR;
      break;
#endif
#ifdef CONFIG_STM32H5_UART12_SERIALDRIVER
    case STM32_UART12_BASE:
      rcc_en = RCC_APB1HENR_UART12EN;
      regaddr = STM32_RCC_APB1HENR;
      break;
#endif
    }

  /* Enable/disable APB 1/2 clock for USART */

  if (on)
    {
      modifyreg32(regaddr, 0, rcc_en);
    }
  else
    {
      modifyreg32(regaddr, rcc_en, 0);
    }
}

/****************************************************************************
 * Name: stm32serial_setup
 *
 * Description:
 *   Configure the USART baud, bits, parity, etc. This method is called the
 *   first time that the serial port is opened.
 *
 ****************************************************************************/

static int stm32serial_setup(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

#ifndef CONFIG_SUPPRESS_UART_CONFIG
  uint32_t regval;

  /* Note: The logic here depends on the fact that that the USART module
   * was enabled in stm32_lowsetup().
   */

  /* Enable USART APB1/2 clock */

  stm32serial_setapbclock(dev, true);

  /* Configure pins for USART use */

  stm32_configgpio(priv->tx_gpio);
  stm32_configgpio(priv->rx_gpio);

#ifdef CONFIG_SERIAL_OFLOWCONTROL
  if (priv->cts_gpio != 0)
    {
      stm32_configgpio(priv->cts_gpio);
    }
#endif

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  if (priv->rts_gpio != 0)
    {
      uint32_t config = priv->rts_gpio;

#ifdef CONFIG_STM32H5_FLOWCONTROL_BROKEN
      /* Instead of letting hw manage this pin, we will bitbang */

      config = (config & ~GPIO_MODE_MASK) | GPIO_OUTPUT;
#endif
      stm32_configgpio(config);
    }
#endif

#ifdef HAVE_RS485
  if (priv->rs485_dir_gpio != 0)
    {
      stm32_configgpio(priv->rs485_dir_gpio);
      stm32_gpiowrite(priv->rs485_dir_gpio, !priv->rs485_dir_polarity);
    }
#endif

  /* Configure CR2 */

  /* Clear STOP, CLKEN, CPOL, CPHA, LBCL, and interrupt enable bits */

  regval  = stm32serial_getreg(priv, STM32_USART_CR2_OFFSET);
  if (priv->islpuart == true)
    {
      regval &= ~(USART_CR2_STOP_MASK | USART_CR2_CLKEN);
    }
  else
    {
      regval &= ~(USART_CR2_STOP_MASK | USART_CR2_CLKEN | USART_CR2_CPOL |
                  USART_CR2_CPHA | USART_CR2_LBCL | USART_CR2_LBDIE);
    }

  /* Configure STOP bits */

  if (priv->stopbits2)
    {
      regval |= USART_CR2_STOP2;
    }

  stm32serial_putreg(priv, STM32_USART_CR2_OFFSET, regval);

  /* Configure CR1 */

  /* Clear TE, REm and all interrupt enable bits */

  regval  = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);

#ifdef CONFIG_STM32_LPUART1
  if (priv->islpuart == true)
    {
      regval &= ~(USART_CR1_TE | USART_CR1_RE | LPUART_CR1_ALLINTS);
    }
  else
#endif
    {
      regval &= ~(USART_CR1_TE | USART_CR1_RE | USART_CR1_ALLINTS);
    }

  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, regval);

  /* Configure CR3 */

  /* Clear CTSE, RTSE, and all interrupt enable bits */

  regval  = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);
  regval &= ~(USART_CR3_CTSIE | USART_CR3_CTSE | USART_CR3_RTSE |
              USART_CR3_EIE);

  stm32serial_putreg(priv, STM32_USART_CR3_OFFSET, regval);

  /* Configure the USART line format and speed. */

  stm32serial_setformat(dev);

  /* Enable Rx, Tx, and the USART */

  regval      = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
  regval     |= (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, regval);

#endif /* CONFIG_SUPPRESS_UART_CONFIG */

  /* Set up the cached interrupt enables value */

  priv->ie    = 0;

  /* Mark device as initialized. */

  priv->initialized = true;

  return OK;
}

/****************************************************************************
 * Name: serial_rxdmacfg
 *
 * Description:
 *   Generate the required DMA configuration structure for oneshot mode based
 *   on the serial configuration.
 *
 * Input Parameters:
 *   priv     - serial instance structure
 *   cfg      - DMA configuration structure
 *   circular - 0 = oneshot, 1 = circular
 *
 * Returned Value:
 *   None
 ****************************************************************************/

 #ifdef SERIAL_HAVE_DMA
static void serial_rxdmacfg(struct stm32_serial_s *priv,
                            struct stm32_gpdma_cfg_s *cfg)
{
  cfg->src_addr   = priv->usartbase + STM32_USART_RDR_OFFSET;
  cfg->dest_addr  = (uint32_t)priv->rxfifo;

  cfg->request    = priv->rxdma_req;

  cfg->priority   = GPMDACFG_PRIO_LH;

  cfg->mode       = GPDMACFG_MODE_CIRC;

  cfg->ntransfers = RXDMA_BUFFER_SIZE;

  /* Write SDW and DDW to 0 for 8-bit beats */

  cfg->tr1        = GPDMA_CXTR1_DINC;  /* dest-inc, source fixed */
}
#endif

/****************************************************************************
 * Name: stm32serial_dmasetup
 *
 * Description:
 *   Configure and start circular RX DMA for USART:
 *     - Allocate a GPDMA channel
 *     - Set up source (USART RDR), destination (RX buffer), REQSEL,
 *       circular mode
 *     - Program DMA and reset read index
 *     - Enable USART CR3.DMAR
 *     - Start DMA with half‑ and full‑transfer callbacks
 *
 * Returned Value:
 *   OK on success; negative errno on failure.
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static int stm32serial_dmasetup(struct uart_dev_s *dev)
{
  struct stm32_serial_s   *priv = (struct stm32_serial_s *)dev->priv;
  struct stm32_gpdma_cfg_s dmacfg;
  uint32_t                 regval;
  int                      ret;

  if (!dev->isconsole)
    {
      ret = stm32serial_setup(dev);
      if (ret != OK)
        {
          return ret;
        }
    }

  priv->rxdma = stm32_dmachannel(GPDMA_TTYPE_P2M);
  if (!priv->rxdma)
    {
      return -EBUSY;
    }

  serial_rxdmacfg(priv, &dmacfg);

  stm32_dmasetup(priv->rxdma, &dmacfg);

  priv->rxdmanext = 0;

  regval  = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);
  regval |= USART_CR3_DMAR;
  stm32serial_putreg(priv, STM32_USART_CR3_OFFSET, regval);

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  if (priv->iflow)
    {
      /* Start the DMA channel, and arrange for callbacks at the full point
       * in the FIFO. After buffer gets full, hardware flow-control kicks
       * in and DMA transfer is stopped.
       */

      stm32_dmastart(priv->rxdma, stm32serial_dmarxcallback,
                      (void *)priv, false);
    }
  else
#endif
    {
      /* Start the DMA channel, and arrange for callbacks at the half and
       * full points in the FIFO.  This ensures that we have half a FIFO
       * worth of time to claim bytes before they are overwritten.
       */

      stm32_dmastart(priv->rxdma, stm32serial_dmarxcallback,
                      (void *)priv, true);
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: stm32serial_shutdown
 *
 * Description:
 *   Disable the USART.  This method is called when the serial
 *   port is closed
 *
 ****************************************************************************/

static void stm32serial_shutdown(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  uint32_t regval;

  /* Mark device as uninitialized. */

  priv->initialized = false;

  /* Disable all interrupts */

  stm32serial_disableusartint(priv, NULL);

  /* Disable USART APB1/2 clock */

  stm32serial_setapbclock(dev, false);

  /* Disable Rx, Tx, and the UART */

  regval  = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
  regval &= ~(USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
  stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, regval);

  /* Release pins. "If the serial-attached device is powered down, the TX
   * pin causes back-powering, potentially confusing the device to the point
   * of complete lock-up."
   *
   * REVISIT:  Is unconfiguring the pins appropriate for all device?  If not,
   * then this may need to be a configuration option.
   */

  stm32_unconfiggpio(priv->tx_gpio);
  stm32_unconfiggpio(priv->rx_gpio);

#ifdef CONFIG_SERIAL_OFLOWCONTROL
  if (priv->cts_gpio != 0)
    {
      stm32_unconfiggpio(priv->cts_gpio);
    }
#endif

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  if (priv->rts_gpio != 0)
    {
      stm32_unconfiggpio(priv->rts_gpio);
    }
#endif

#ifdef HAVE_RS485
  if (priv->rs485_dir_gpio != 0)
    {
      stm32_unconfiggpio(priv->rs485_dir_gpio);
    }
#endif
}

/****************************************************************************
 * Name: stm32serial_dmashutdown
 *
 * Description:
 *   Disable the USART.  This method is called when the serial
 *   port is closed
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static void stm32serial_dmashutdown(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

  /* Perform the normal UART shutdown */

  stm32serial_shutdown(dev);

  /* Stop the DMA channel */

  stm32_dmastop(priv->rxdma);

  priv->rxenable = false;

  /* Release the DMA channel */

  stm32_dmafree(priv->rxdma);
  priv->rxdma = NULL;
}
#endif

/****************************************************************************
 * Name: stm32serial_attach
 *
 * Description:
 *   Configure the USART to operation in interrupt driven mode.  This method
 *   is called when the serial port is opened.  Normally, this is just after
 *   the setup() method is called, however, the serial console may operate in
 *   a non-interrupt driven mode during the boot phase.
 *
 *   RX and TX interrupts are not enabled when by the attach method (unless
 *   the hardware supports multiple levels of interrupt enabling).  The RX
 *   and TX interrupts are not enabled until the txint() and rxint() methods
 *   are called.
 *
 ****************************************************************************/

static int stm32serial_attach(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  int ret;

  /* Attach and enable the IRQ */

  ret = irq_attach(priv->irq, stm32serial_interrupt, priv);

  if (ret == OK)
    {
      /* Enable the interrupt (RX and TX interrupts are still disabled
       * in the USART
       */

      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: stm32serial_detach
 *
 * Description:
 *   Detach USART interrupts.  This method is called when the serial port is
 *   closed normally just before the shutdown method is called.  The
 *   exception is the serial console which is never shutdown.
 *
 ****************************************************************************/

static void stm32serial_detach(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  up_disable_irq(priv->irq);
  irq_detach(priv->irq);
}

/****************************************************************************
 * Name: stm32serial_interrupt
 *
 * Description:
 *   This is the USART interrupt handler.  It will be invoked when an
 *   interrupt is received on the 'irq'.  It should call uart_xmitchars or
 *   uart_recvchars to perform the appropriate data transfers.  The
 *   interrupt handling logic must be able to map the 'arg' to the
 *   appropriate uart_dev_s structure in order to call these functions.
 *
 ****************************************************************************/

static int stm32serial_interrupt(int irq, void *context, void *arg)
{
  struct stm32_serial_s *priv = (struct stm32_serial_s *)arg;
  int  passes;
  bool handled;

  DEBUGASSERT(priv != NULL);

  /* Report serial activity to the power management logic */

#if defined(CONFIG_PM) && CONFIG_STM32H5_PM_SERIAL_ACTIVITY > 0
  pm_activity(PM_IDLE_DOMAIN, CONFIG_STM32H5_PM_SERIAL_ACTIVITY);
#endif

  /* Loop until there are no characters to be transferred or,
   * until we have been looping for a long time.
   */

  handled = true;
  for (passes = 0; passes < 256 && handled; passes++)
    {
      handled = false;

      /* Get the masked USART status word. */

      priv->sr = stm32serial_getreg(priv, STM32_USART_ISR_OFFSET);

      /* USART interrupts:
       *
       * Enable           Status         Meaning                Usage
       * ---------------- -------------- ---------------------- -------------
       * USART_CR1_IDLEIE USART_ISR_IDLE Idle Line Detected     (not used)
       * USART_CR1_RXNEIE USART_ISR_RXNE Received Data Ready to
       *                                 be Read
       * "              " USART_ISR_ORE  Overrun Error Detected
       * USART_CR1_TCIE   USART_ISR_TC   Transmission Complete  (only RS-485)
       * USART_CR1_TXEIE  USART_ISR_TXE  Transmit Data Register
       *                                 Empty
       * USART_CR1_PEIE   USART_ISR_PE   Parity Error
       *
       * USART_CR2_LBDIE  USART_ISR_LBD  Break Flag             (not used)
       * USART_CR3_EIE    USART_ISR_FE   Framing Error
       * "           "    USART_ISR_NE   Noise Error
       * "           "    USART_ISR_ORE  Overrun Error Detected
       * USART_CR3_CTSIE  USART_ISR_CTS  CTS flag               (not used)
       *
       * NOTE: Some of these status bits must be cleared by explicitly
       * writing one to the ICR register: USART_ICR_CTSCF, USART_ICR_LBDCF.
       * None of those are currently being used.
       */

#ifdef HAVE_RS485
      /* Transmission of whole buffer is over - TC is set, TXEIE is cleared.
       * Note - this should be first, to have the most recent TC bit value
       * from SR register - sending data affects TC, but without refresh we
       * will not know that...
       */

      if ((priv->sr & USART_ISR_TC) != 0 &&
          (priv->ie & USART_CR1_TCIE) != 0 &&
          (priv->ie & USART_CR1_TXEIE) == 0)
        {
          stm32_gpiowrite(priv->rs485_dir_gpio, !priv->rs485_dir_polarity);
          stm32serial_restoreusartint(priv, priv->ie & ~USART_CR1_TCIE);
        }
#endif

      /* Handle incoming, receive bytes. */

      if ((priv->sr & USART_ISR_RXNE) != 0 &&
          (priv->ie & USART_CR1_RXNEIE) != 0)
        {
          /* Received data ready... process incoming bytes.  NOTE the check
           * for RXNEIE:  We cannot call uart_recvchards of RX interrupts are
           * disabled.
           */

          uart_recvchars(&priv->dev);
          handled = true;
        }

      /* We may still have to read from the DR register to clear any pending
       * error conditions.
       */

      else if ((priv->sr & (USART_ISR_ORE | USART_ISR_NF | USART_ISR_FE))
               != 0)
        {
          /* These errors are cleared by writing the corresponding bit to the
           * interrupt clear register (ICR).
           */

          stm32serial_putreg(priv, STM32_USART_ICR_OFFSET,
                               (USART_ICR_NCF | USART_ICR_ORECF |
                                USART_ICR_FECF));
        }

      /* Handle outgoing, transmit bytes */

      if ((priv->sr & USART_ISR_TXE) != 0 &&
          (priv->ie & USART_CR1_TXEIE) != 0)
        {
          /* Transmit data register empty ... process outgoing bytes */

          uart_xmitchars(&priv->dev);
          handled = true;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: stm32serial_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method
 *
 ****************************************************************************/

static int stm32serial_ioctl(struct file *filep, int cmd,
                               unsigned long arg)
{
#if defined(CONFIG_SERIAL_TERMIOS) || defined(CONFIG_SERIAL_TIOCSERGSTRUCT)
  struct inode      *inode = filep->f_inode;
  struct uart_dev_s *dev   = inode->i_private;
#endif
#if defined(CONFIG_SERIAL_TERMIOS)
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
#endif
  int                ret    = OK;

  switch (cmd)
    {
#ifdef CONFIG_SERIAL_TIOCSERGSTRUCT
    case TIOCSERGSTRUCT:
      {
        struct stm32_serial_s *user;

        user = (struct stm32_serial_s *)arg;

        if (!user)
          {
            ret = -EINVAL;
          }
        else
          {
            memcpy(user, dev, sizeof(struct stm32_serial_s));
          }
      }
      break;
#endif

#ifdef CONFIG_STM32H5_USART_SINGLEWIRE
    case TIOCSSINGLEWIRE:
      {
        uint32_t cr1;
        uint32_t cr1_ue;
        irqstate_t flags;

        flags = enter_critical_section();

        /* Get the original state of UE */

        cr1    = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
        cr1_ue = cr1 & USART_CR1_UE;
        cr1   &= ~USART_CR1_UE;

        /* Disable UE, HDSEL can only be written when UE=0 */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1);

        /* Change the TX port to be open-drain/push-pull and enable/disable
         * half-duplex mode.
         */

        uint32_t cr = stm32serial_getreg(priv, STM32_USART_CR3_OFFSET);

        if ((arg & SER_SINGLEWIRE_ENABLED) != 0)
          {
            uint32_t gpio_val = GPIO_OPENDRAIN;

            if ((arg & SER_SINGLEWIRE_PULL_MASK) == SER_SINGLEWIRE_PULLUP)
              {
                gpio_val |= GPIO_PULLUP;
              }
            else
              {
                gpio_val |= GPIO_FLOAT;
              }

            if ((arg & SER_SINGLEWIRE_PULL_MASK) == SER_SINGLEWIRE_PULLDOWN)
              {
                gpio_val |= GPIO_PULLDOWN;
              }
            else
              {
                gpio_val |= GPIO_FLOAT;
              }

            stm32_configgpio((priv->tx_gpio &
                                ~(GPIO_PUPD_MASK | GPIO_OPENDRAIN)) |
                               gpio_val);

            cr |= USART_CR3_HDSEL;
          }
        else
          {
            stm32_configgpio((priv->tx_gpio &
                                ~(GPIO_PUPD_MASK | GPIO_OPENDRAIN)) |
                               GPIO_PUSHPULL);
            cr &= ~USART_CR3_HDSEL;
          }

        stm32serial_putreg(priv, STM32_USART_CR3_OFFSET, cr);

        /* Re-enable UE if appropriate */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1 | cr1_ue);
        leave_critical_section(flags);
      }
     break;
#endif

#ifdef CONFIG_STM32H5_USART_INVERT
    case TIOCSINVERT:
      {
        uint32_t cr1;
        uint32_t cr1_ue;
        irqstate_t flags;

        flags = enter_critical_section();

        /* Get the original state of UE */

        cr1    = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
        cr1_ue = cr1 & USART_CR1_UE;
        cr1   &= ~USART_CR1_UE;

        /* Disable UE, {R,T}XINV can only be written when UE=0 */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1);

        /* Enable/disable signal inversion. */

        uint32_t cr = stm32serial_getreg(priv, STM32_USART_CR2_OFFSET);

        if (arg & SER_INVERT_ENABLED_RX)
          {
            cr |= USART_CR2_RXINV;
          }
        else
          {
            cr &= ~USART_CR2_RXINV;
          }

        if (arg & SER_INVERT_ENABLED_TX)
          {
            cr |= USART_CR2_TXINV;
          }
        else
          {
            cr &= ~USART_CR2_TXINV;
          }

        stm32serial_putreg(priv, STM32_USART_CR2_OFFSET, cr);

        /* Re-enable UE if appropriate */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1 | cr1_ue);
        leave_critical_section(flags);
      }
     break;
#endif

#ifdef CONFIG_STM32H5_USART_SWAP
    case TIOCSSWAP:
      {
        uint32_t cr1;
        uint32_t cr1_ue;
        irqstate_t flags;

        flags = enter_critical_section();

        /* Get the original state of UE */

        cr1    = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
        cr1_ue = cr1 & USART_CR1_UE;
        cr1   &= ~USART_CR1_UE;

        /* Disable UE, SWAP can only be written when UE=0 */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1);

        /* Enable/disable Swap mode. */

        uint32_t cr = stm32serial_getreg(priv, STM32_USART_CR2_OFFSET);

        if (arg == SER_SWAP_ENABLED)
          {
            cr |= USART_CR2_SWAP;
          }
        else
          {
            cr &= ~USART_CR2_SWAP;
          }

        stm32serial_putreg(priv, STM32_USART_CR2_OFFSET, cr);

        /* Re-enable UE if appropriate */

        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET, cr1 | cr1_ue);
        leave_critical_section(flags);
      }
     break;
#endif

#ifdef CONFIG_SERIAL_TERMIOS
    case TCGETS:
      {
        struct termios *termiosp = (struct termios *)arg;

        if (!termiosp)
          {
            ret = -EINVAL;
            break;
          }

        cfsetispeed(termiosp, priv->baud);

        /* Note that since we only support 8/9 bit modes and
         * there is no way to report 9-bit mode, we always claim 8.
         */

        termiosp->c_cflag =
          ((priv->parity != 0) ? PARENB : 0) |
          ((priv->parity == 1) ? PARODD : 0) |
          ((priv->stopbits2) ? CSTOPB : 0) |
#ifdef CONFIG_SERIAL_OFLOWCONTROL
          ((priv->oflow) ? CCTS_OFLOW : 0) |
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
          ((priv->iflow) ? CRTS_IFLOW : 0) |
#endif
          CS8;

        /* TODO: CRTS_IFLOW, CCTS_OFLOW */
      }
      break;

    case TCSETS:
      {
        struct termios *termiosp = (struct termios *)arg;

        if (!termiosp)
          {
            ret = -EINVAL;
            break;
          }

        /* Perform some sanity checks before accepting any changes */

        if (((termiosp->c_cflag & CSIZE) != CS8)
#ifdef CONFIG_SERIAL_OFLOWCONTROL
            || ((termiosp->c_cflag & CCTS_OFLOW) && (priv->cts_gpio == 0))
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
            || ((termiosp->c_cflag & CRTS_IFLOW) && (priv->rts_gpio == 0))
#endif
           )
          {
            ret = -EINVAL;
            break;
          }

        if (termiosp->c_cflag & PARENB)
          {
            priv->parity = (termiosp->c_cflag & PARODD) ? 1 : 2;
          }
        else
          {
            priv->parity = 0;
          }

        priv->stopbits2 = (termiosp->c_cflag & CSTOPB) != 0;
#ifdef CONFIG_SERIAL_OFLOWCONTROL
        priv->oflow = (termiosp->c_cflag & CCTS_OFLOW) != 0;
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
        priv->iflow = (termiosp->c_cflag & CRTS_IFLOW) != 0;
#endif

        /* Note that since there is no way to request 9-bit mode
         * and no way to support 5/6/7-bit modes, we ignore them
         * all here.
         */

        /* Note that only cfgetispeed is used because we have knowledge
         * that only one speed is supported.
         */

        priv->baud = cfgetispeed(termiosp);

        /* Effect the changes immediately - note that we do not implement
         * TCSADRAIN / TCSAFLUSH
         */

        stm32serial_setformat(dev);
      }
      break;
#endif /* CONFIG_SERIAL_TERMIOS */

#ifdef CONFIG_STM32H5_USART_BREAKS
#  ifdef CONFIG_STM32H5_SERIALBRK_BSDCOMPAT
    case TIOCSBRK:  /* BSD compatibility: Turn break on, unconditionally */
      {
        irqstate_t flags;
        uint32_t tx_break;

        flags = enter_critical_section();

        /* Disable any further tx activity */

        priv->ie |= USART_CR1_IE_BREAK_INPROGRESS;

        stm32serial_txint(dev, false);

        /* Configure TX as a GPIO output pin and Send a break signal */

        tx_break = GPIO_OUTPUT |
                   (~(GPIO_MODE_MASK | GPIO_OUTPUT_SET) & priv->tx_gpio);
        stm32_configgpio(tx_break);

        leave_critical_section(flags);
      }
      break;

    case TIOCCBRK:  /* BSD compatibility: Turn break off, unconditionally */
      {
        irqstate_t flags;

        flags = enter_critical_section();

        /* Configure TX back to U(S)ART */

        stm32_configgpio(priv->tx_gpio);

        priv->ie &= ~USART_CR1_IE_BREAK_INPROGRESS;

        /* Enable further tx activity */

        stm32serial_txint(dev, true);

        leave_critical_section(flags);
      }
      break;
#  else
    case TIOCSBRK:  /* No BSD compatibility: Turn break on for M bit times */
      {
        uint32_t cr1;
        irqstate_t flags;

        flags = enter_critical_section();
        cr1   = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET,
                             cr1 | USART_CR1_SBK);
        leave_critical_section(flags);
      }
      break;

    case TIOCCBRK:  /* No BSD compatibility: May turn off break too soon */
      {
        uint32_t cr1;
        irqstate_t flags;

        flags = enter_critical_section();
        cr1   = stm32serial_getreg(priv, STM32_USART_CR1_OFFSET);
        stm32serial_putreg(priv, STM32_USART_CR1_OFFSET,
                             cr1 & ~USART_CR1_SBK);
        leave_critical_section(flags);
      }
      break;
#  endif
#endif

    default:
      ret = -ENOTTY;
      break;
    }

  return ret;
}

/****************************************************************************
 * Name: stm32serial_receive
 *
 * Description:
 *   Called (usually) from the interrupt level to receive one
 *   character from the USART.  Error bits associated with the
 *   receipt are provided in the return 'status'.
 *
 ****************************************************************************/

#ifndef SERIAL_HAVE_ONLY_DMA
static int stm32serial_receive(struct uart_dev_s *dev,
                                 unsigned int *status)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  uint32_t rdr;

  /* Get the Rx byte */

  rdr      = stm32serial_getreg(priv, STM32_USART_RDR_OFFSET);

  /* Get the Rx byte plux error information.  Return those in status */

  *status  = priv->sr << 16 | rdr;
  priv->sr = 0;

  /* Then return the actual received byte */

  return rdr & 0xff;
}
#endif

/****************************************************************************
 * Name: stm32serial_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts
 *
 ****************************************************************************/

#ifndef SERIAL_HAVE_ONLY_DMA
static void stm32serial_rxint(struct uart_dev_s *dev, bool enable)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  irqstate_t flags;
  uint16_t ie;

  /* USART receive interrupts:
   *
   * Enable           Status         Meaning                  Usage
   * ---------------- -------------- ---------------------- ----------
   * USART_CR1_IDLEIE USART_ISR_IDLE Idle Line Detected     (not used)
   * USART_CR1_RXNEIE USART_ISR_RXNE Received Data Ready
   *                                    to be Read
   * "              " USART_ISR_ORE  Overrun Error Detected
   * USART_CR1_PEIE   USART_ISR_PE   Parity Error
   *
   * USART_CR2_LBDIE  USART_ISR_LBD  Break Flag              (not used)
   * USART_CR3_EIE    USART_ISR_FE   Framing Error
   * "           "    USART_ISR_NF   Noise Flag
   * "           "    USART_ISR_ORE  Overrun Error Detected
   */

  flags = enter_critical_section();
  ie = priv->ie;
  if (enable)
    {
      /* Receive an interrupt when their is anything in the Rx data
       * register (or an Rx timeout occurs).
       */

#ifndef CONFIG_SUPPRESS_SERIAL_INTS
#ifdef CONFIG_USART_ERRINTS
      ie |= (USART_CR1_RXNEIE | USART_CR1_PEIE | USART_CR3_EIE);
#else
      ie |= USART_CR1_RXNEIE;
#endif
#endif
    }
  else
    {
      ie &= ~(USART_CR1_RXNEIE | USART_CR1_PEIE | USART_CR3_EIE);
    }

  /* Then set the new interrupt state */

  stm32serial_restoreusartint(priv, ie);
  leave_critical_section(flags);
}
#endif

/****************************************************************************
 * Name: stm32serial_rxavailable
 *
 * Description:
 *   Return true if the receive register is not empty
 *
 ****************************************************************************/

#ifndef SERIAL_HAVE_ONLY_DMA
static bool stm32serial_rxavailable(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

  return ((stm32serial_getreg(priv, STM32_USART_ISR_OFFSET) &
           USART_ISR_RXNE) != 0);
}
#endif

/****************************************************************************
 * Name: stm32serial_rxflowcontrol
 *
 * Description:
 *   Called when Rx buffer is full (or exceeds configured watermark levels
 *   if CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS is defined).
 *   Return true if UART activated RX flow control to block more incoming
 *   data
 *
 * Input Parameters:
 *   dev       - UART device instance
 *   nbuffered - the number of characters currently buffered
 *               (if CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS is
 *               not defined the value will be 0 for an empty buffer or the
 *               defined buffer size for a full buffer)
 *   upper     - true indicates the upper watermark was crossed where
 *               false indicates the lower watermark has been crossed
 *
 * Returned Value:
 *   true if RX flow control activated.
 *
 ****************************************************************************/

#ifdef CONFIG_SERIAL_IFLOWCONTROL
static bool stm32serial_rxflowcontrol(struct uart_dev_s *dev,
                                        unsigned int nbuffered, bool upper)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

#if defined(CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS) && \
    defined(CONFIG_STM32H5_FLOWCONTROL_BROKEN)
  if (priv->iflow && (priv->rts_gpio != 0))
    {
      /* Assert/de-assert nRTS set it high resume/stop sending */

      stm32_gpiowrite(priv->rts_gpio, upper);

      if (upper)
        {
          /* With heavy Rx traffic, RXNE might be set and data pending.
           * Returning 'true' in such case would cause RXNE left unhandled
           * and causing interrupt storm. Sending end might be also be slow
           * to react on nRTS, and returning 'true' here would prevent
           * processing that data.
           *
           * Therefore, return 'false' so input data is still being processed
           * until sending end reacts on nRTS signal and stops sending more.
           */

          return false;
        }

      return upper;
    }

#else
  if (priv->iflow)
    {
      /* Is the RX buffer full? */

      if (upper)
        {
          /* Disable Rx interrupt to prevent more data being from
           * peripheral.  When hardware RTS is enabled, this will
           * prevent more data from coming in.
           *
           * This function is only called when UART recv buffer is full,
           * that is: "dev->recv.head + 1 == dev->recv.tail".
           *
           * Logic in "uart_read" will automatically toggle Rx interrupts
           * when buffer is read empty and thus we do not have to re-
           * enable Rx interrupts.
           */

          uart_disablerxint(dev);
          return true;
        }

      /* No.. The RX buffer is empty */

      else
        {
          /* We might leave Rx interrupt disabled if full recv buffer was
           * read empty.  Enable Rx interrupt to make sure that more input is
           * received.
           */

          uart_enablerxint(dev);
        }
    }
#endif

  return false;
}
#endif

/****************************************************************************
 * Name: stm32serial_dmareceive
 *
 * Description:
 *   Retrieve one character from the RX FIFO filled by circular DMA.  Also
 *   report any USART error flags in *status.
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static int stm32serial_dmareceive(struct uart_dev_s *dev,
                                     unsigned int *status)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  unsigned int next;
  int          ch  = -1;
  uint32_t     sr;

  /* 1) Capture USART error flags */

  sr      = getreg32(priv->usartbase + STM32_USART_ISR_OFFSET);
  *status = sr & (USART_ISR_ORE | USART_ISR_NF |
                  USART_ISR_FE  | USART_ISR_PE);

  /* 2) Where will DMA write the next byte? */

  next = stm32serial_dmanextrx(priv);

  /* 3) Pull one byte if available */

  if (next != priv->rxdmanext)
    {
      ch = priv->rxfifo[priv->rxdmanext++];

      /* 4) End‑of‑buffer wrap or flow‑control pause */

      if (priv->rxdmanext >= RXDMA_BUFFER_SIZE)
        {
#ifdef CONFIG_SERIAL_IFLOWCONTROL
          if (priv->iflow)
            {
              /* Pause DMA callbacks */

              stm32serial_dmarxint(&priv->dev, false);

              /* Assert RTS to halt sender */

              (void)stm32serial_rxflowcontrol(&priv->dev,
                                              RXDMA_BUFFER_SIZE, true);
            }
          else
#endif
            {
              /* Simply wrap to buffer start */

              priv->rxdmanext = 0;
            }
        }
    }

  return ch;
}
#endif

/****************************************************************************
 * Name: stm32serial_dmareenable
 *
 * Description:
 *   Call to re-enable RX DMA.
 *
 ****************************************************************************/

#if defined(SERIAL_HAVE_DMA)
static void stm32serial_dmareenable(struct stm32_serial_s *priv)
{
  struct stm32_gpdma_cfg_s dmacfg;

  serial_rxdmacfg(priv, &dmacfg);
  stm32_dmasetup(priv->rxdma, &dmacfg);

  /* Reset our DMA shadow pointer to match the address just
   * programmed above.
   */

  priv->rxdmanext = 0;

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  if (priv->iflow)
    {
      /* Start the DMA channel, and arrange for callbacks at the full point
       * in the FIFO. After buffer gets full, hardware flow-control kicks
       * in and DMA transfer is stopped.
       */

      stm32_dmastart(priv->rxdma, stm32serial_dmarxcallback,
                      (void *)priv, false);
    }
  else
#endif
    {
      /* Start the DMA channel, and arrange for callbacks at the half and
       * full points in the FIFO.  This ensures that we have half a FIFO
       * worth of time to claim bytes before they are overwritten.
       */

      stm32_dmastart(priv->rxdma, stm32serial_dmarxcallback,
                      (void *)priv, true);
    }

#ifdef CONFIG_PM
  /* Clear DMA suspended flag. */

  priv->rxdmasusp = false;
#endif
}
#endif

/****************************************************************************
 * Name: stm32serial_dmaiflowrestart
 *
 * Description:
 *   Call to restart RX DMA for input flow-controlled USART
 *
 ****************************************************************************/

#if defined(SERIAL_HAVE_DMA) && defined(CONFIG_SERIAL_IFLOWCONTROL)
static bool stm32serial_dmaiflowrestart(struct stm32_serial_s *priv)
{
  if (!priv->rxenable)
    {
      /* Rx not enabled by upper layer. */

      return false;
    }

  if (priv->rxdmanext != RXDMA_BUFFER_SIZE)
    {
#ifdef CONFIG_PM
      if (priv->rxdmasusp)
        {
          /* Rx DMA in suspended state. */

          if (stm32serial_dmarxavailable(&priv->dev))
            {
              /* DMA buffer has unprocessed data, do not re-enable yet. */

              return false;
            }
        }
      else
#endif
        {
          return false;
        }
    }

  /* DMA is stopped or suspended and DMA buffer does not have pending data,
   * re-enabling without data loss is now safe.
   */

  stm32serial_dmareenable(priv);

  return true;
}
#endif

/****************************************************************************
 * Name: stm32serial_dmarxint
 *
 * Description:
 *   Call to enable or disable RX interrupts
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static void stm32serial_dmarxint(struct uart_dev_s *dev, bool enable)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

  /* En/disable DMA reception.
   *
   * Note that it is not safe to check for available bytes and immediately
   * pass them to uart_recvchars as that could potentially recurse back
   * to us again.  Instead, bytes must wait until the next up_dma_poll or
   * DMA event.
   */

  priv->rxenable = enable;

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  if (priv->iflow)
    {
      /* Re-enable RX DMA. */

      stm32serial_dmaiflowrestart(priv);
    }
#endif
}
#endif

/****************************************************************************
 * Name: stm32serial_dmarxavailable
 *
 * Description:
 *   Return true if the receive register is not empty
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
static bool stm32serial_dmarxavailable(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

  /* Compare our receive pointer to the current DMA pointer, if they
   * do not match, then there are bytes to be received.
   */

  return (stm32serial_dmanextrx(priv) != priv->rxdmanext);
}
#endif

/****************************************************************************
 * Name: stm32serial_send
 *
 * Description:
 *   This method will send one byte on the USART
 *
 ****************************************************************************/

static void stm32serial_send(struct uart_dev_s *dev, int ch)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

#ifdef HAVE_RS485
  if (priv->rs485_dir_gpio != 0)
    {
      stm32_gpiowrite(priv->rs485_dir_gpio, priv->rs485_dir_polarity);
    }
#endif

  stm32serial_putreg(priv, STM32_USART_TDR_OFFSET, (uint32_t)ch);
}

/****************************************************************************
 * Name: stm32serial_txint
 *
 * Description:
 *   Call to enable or disable TX interrupts
 *
 ****************************************************************************/

static void stm32serial_txint(struct uart_dev_s *dev, bool enable)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;
  irqstate_t flags;

  /* USART transmit interrupts:
   *
   * Enable          Status        Meaning                      Usage
   * --------------- ------------- ---------------------------- -------------
   * USART_CR1_TCIE  USART_ISR_TC  Transmission Complete        (only RS-485)
   * USART_CR1_TXEIE USART_ISR_TXE Transmit Data Register Empty
   * USART_CR3_CTSIE USART_ISR_CTS CTS flag                     (not used)
   */

  flags = enter_critical_section();
  if (enable)
    {
      /* Set to receive an interrupt when the TX data register is empty */

#ifndef CONFIG_SUPPRESS_SERIAL_INTS
      uint16_t ie = priv->ie | USART_CR1_TXEIE;

      /* If RS-485 is supported on this U[S]ART, then also enable the
       * transmission complete interrupt.
       */

#  ifdef HAVE_RS485
      if (priv->rs485_dir_gpio != 0)
        {
          ie |= USART_CR1_TCIE;
        }
#  endif

#  ifdef CONFIG_STM32H5_SERIALBRK_BSDCOMPAT
      if (priv->ie & USART_CR1_IE_BREAK_INPROGRESS)
        {
          leave_critical_section(flags);
          return;
        }
#  endif

      stm32serial_restoreusartint(priv, ie);

      /* Fake a TX interrupt here by just calling uart_xmitchars() with
       * interrupts disabled (note this may recurse).
       */

      uart_xmitchars(dev);
#endif
    }
  else
    {
      /* Disable the TX interrupt */

      stm32serial_restoreusartint(priv, priv->ie & ~USART_CR1_TXEIE);
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: stm32serial_txready
 *
 * Description:
 *   Return true if the transmit data register is empty
 *
 ****************************************************************************/

static bool stm32serial_txready(struct uart_dev_s *dev)
{
  struct stm32_serial_s *priv =
    (struct stm32_serial_s *)dev->priv;

  return ((stm32serial_getreg(priv, STM32_USART_ISR_OFFSET) &
           USART_ISR_TXE) != 0);
}

/****************************************************************************
 * Name: stm32serial_dmarxcallback
 *
 * Description:
 *   DMA callback for STM32H5 USART RX.  Called on half and full‐transfer
 *   events.  Reads and clears the GPDMA status flags, notifies the NuttX
 *   serial core of newly arrived bytes, signals end‐of‐buffer when a
 *   full transfer completes, handles RTS flow control restart, and
 *   clears any lingering UART error flags to keep RX‐DMA running.
 *
 * Input Parameters:
 *   handle         - DMA channel handle returned by stm32_dmachannel()
 *   status         - Raw status byte passed by the DMA ISR (ignored here)
 *   arg            - Pointer to the STM32 serial driver state
 *                    (struct stm32_serial_s)
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void stm32serial_dmarxcallback(DMA_HANDLE handle,
                                      uint8_t status,
                                      void *arg)
{
  struct stm32_serial_s *priv = arg;

  /* pull whatever is in the buffer now */

  uart_recvchars(&priv->dev);

  /* If it really was a full‑buffer event, signal “done” so the
   * serial core can rearm/restart the DMA behind the scenes:
   */

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  /* If you had paused the DMA on RTS flow control, restart it now */

  if (priv->iflow)
    {
      stm32serial_dmaiflowrestart(priv);
    }
#endif

  /* Clear any USART framing/overrun errors so RX‑DMA
   * doesn’t get stuck waiting for the UART to clear them.
   */

  priv->sr = getreg32(priv->usartbase + STM32_USART_ISR_OFFSET);
  if (priv->sr & (USART_ISR_ORE | USART_ISR_NF | USART_ISR_FE))
    {
      stm32serial_putreg(priv, STM32_USART_ICR_OFFSET,
                        USART_ICR_ORECF | USART_ICR_NCF | USART_ICR_FECF);
    }
}

/****************************************************************************
 * Name: stm32serial_pmnotify
 *
 * Description:
 *   Notify the driver of new power state. This callback is  called after
 *   all drivers have had the opportunity to prepare for the new power state.
 *
 * Input Parameters:
 *
 *    cb - Returned to the driver. The driver version of the callback
 *         structure may include additional, driver-specific state data at
 *         the end of the structure.
 *
 *    pmstate - Identifies the new PM state
 *
 * Returned Value:
 *   None - The driver already agreed to transition to the low power
 *   consumption state when when it returned OK to the prepare() call.
 *
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static void stm32serial_pmnotify(struct pm_callback_s *cb, int domain,
                                   enum pm_state_e pmstate)
{
  switch (pmstate)
    {
      case PM_NORMAL:
        {
          stm32serial_pm_setsuspend(false);
        }
        break;

      case PM_IDLE:
        {
          stm32serial_pm_setsuspend(false);
        }
        break;

      case PM_STANDBY:
        {
          /* TODO: Alternative configuration and logic for enabling serial in
           *       Stop 1 mode with HSI16 missing. Current logic allows
           *       suspending serial peripherals for Stop 0/1/2 when serial
           *       Rx/Tx buffers are empty (checked in pmprepare).
           */

          stm32serial_pm_setsuspend(true);
        }
        break;

      case PM_SLEEP:
        {
          stm32serial_pm_setsuspend(true);
        }
        break;

      default:

        /* Should not get here */

        break;
    }
}
#endif

/****************************************************************************
 * Name: stm32serial_pmprepare
 *
 * Description:
 *   Request the driver to prepare for a new power state. This is a warning
 *   that the system is about to enter into a new power state. The driver
 *   should begin whatever operations that may be required to enter power
 *   state. The driver may abort the state change mode by returning a
 *   non-zero value from the callback function.
 *
 * Input Parameters:
 *
 *    cb - Returned to the driver. The driver version of the callback
 *         structure may include additional, driver-specific state data at
 *         the end of the structure.
 *
 *    pmstate - Identifies the new PM state
 *
 * Returned Value:
 *   Zero - (OK) means the event was successfully processed and that the
 *          driver is prepared for the PM state change.
 *
 *   Non-zero - means that the driver is not prepared to perform the tasks
 *              needed achieve this power setting and will cause the state
 *              change to be aborted. NOTE: The prepare() method will also
 *              be called when reverting from lower back to higher power
 *              consumption modes (say because another driver refused a
 *              lower power state change). Drivers are not permitted to
 *              return non-zero values when reverting back to higher power
 *              consumption modes!
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static int stm32serial_pmprepare(struct pm_callback_s *cb, int domain,
                                   enum pm_state_e pmstate)
{
  int n;

  /* Logic to prepare for a reduced power state goes here. */

  switch (pmstate)
    {
    case PM_NORMAL:
    case PM_IDLE:
      break;

    case PM_STANDBY:
    case PM_SLEEP:

#ifdef SERIAL_HAVE_DMA
      /* Flush Rx DMA buffers before checking state of serial device
       * buffers.
       */

      stm32_serial_dma_poll();
#endif

      /* Check if any of the active ports have data pending on Tx/Rx
       * buffers.
       */

      for (n = 0; n < STM32H5_NLPUART + STM32H5_NUSART + STM32H5_NUART; n++)
        {
          struct stm32_serial_s *priv = g_uart_devs[n];

          if (!priv || !priv->initialized)
            {
              /* Not active, skip. */

              continue;
            }

          if (priv->suspended)
            {
              /* Port already suspended, skip. */

              continue;
            }

          /* Check if port has data pending (Rx & Tx). */

          if (priv->dev.xmit.head != priv->dev.xmit.tail)
            {
              return ERROR;
            }

          if (priv->dev.recv.head != priv->dev.recv.tail)
            {
              return ERROR;
            }
        }
      break;

    default:

      /* Should not get here */

      break;
    }

  return OK;
}
#endif

#endif /* HAVE_UART */
#endif /* USE_SERIALDRIVER */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef USE_SERIALDRIVER

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Performs the low level USART initialization early in debug so that the
 *   serial console will be available during boot up.  This must be called
 *   before arm_serialinit.
 *
 ****************************************************************************/

#ifdef USE_EARLYSERIALINIT
void arm_earlyserialinit(void)
{
#ifdef HAVE_UART
  unsigned i;

  /* Disable all USART interrupts */

  for (i = 0; i < STM32H5_NLPUART + STM32H5_NUSART + STM32H5_NUART; i++)
    {
      if (g_uart_devs[i])
        {
          stm32serial_disableusartint(g_uart_devs[i], NULL);
        }
    }

  /* Configure whichever one is the console */

#if CONSOLE_UART > 0
  stm32serial_setup(&g_uart_devs[CONSOLE_UART - 1]->dev);
#endif
#endif /* HAVE UART */
}
#endif /* USE_EARLYSERIALINIT */

/****************************************************************************
 * Name: arm_serialinit
 *
 * Description:
 *   Register serial console and serial ports.  This assumes
 *   that arm_earlyserialinit was called previously.
 *
 ****************************************************************************/

void arm_serialinit(void)
{
#ifdef HAVE_UART
  char devname[16];
  unsigned i;
  unsigned minor = 0;
#ifdef CONFIG_PM
  int ret;
#endif

  /* Register to receive power management callbacks */

#ifdef CONFIG_PM
  ret = pm_register(&g_serialpm.pm_cb);
  DEBUGASSERT(ret == OK);
  UNUSED(ret);
#endif

  /* Register the console */

#if CONSOLE_UART > 0
  uart_register("/dev/console", &g_uart_devs[CONSOLE_UART - 1]->dev);

#ifndef CONFIG_STM32H5_SERIAL_DISABLE_REORDERING
  /* If not disabled, register the console UART to ttyS0 and exclude
   * it from initializing it further down
   */

  uart_register("/dev/ttyS0", &g_uart_devs[CONSOLE_UART - 1]->dev);
  minor = 1;
#endif

#ifdef SERIAL_HAVE_CONSOLE_DMA
  /* If we need to re-initialise the console to enable DMA do that here. */

  stm32serial_dmasetup(&g_uart_devs[CONSOLE_UART - 1]->dev);
#endif
#endif /* CONSOLE_UART > 0 */

  /* Register all remaining USARTs */

  strlcpy(devname, "/dev/ttySx", sizeof(devname));

  for (i = 0; i < STM32H5_NLPUART + STM32H5_NUSART + STM32H5_NUART; i++)
    {
      /* Don't create a device for non-configured ports. */

      if (g_uart_devs[i] == 0)
        {
          continue;
        }

#ifndef CONFIG_STM32H5_SERIAL_DISABLE_REORDERING
      /* Don't create a device for the console - we did that above */

      if (g_uart_devs[i]->dev.isconsole)
        {
          continue;
        }
#endif

      /* Register USARTs as devices in increasing order */

      devname[9] = '0' + minor++;
      uart_register(devname, &g_uart_devs[i]->dev);
    }
#endif /* HAVE UART */
}

/****************************************************************************
 * Name: stm32_serial_dma_poll
 *
 * Description:
 *   Checks receive DMA buffers for received bytes that have not accumulated
 *   to the point where the DMA half/full interrupt has triggered.
 *
 *   This function should be called from a timer or other periodic context.
 *
 ****************************************************************************/

#ifdef SERIAL_HAVE_DMA
void stm32_serial_dma_poll(void)
{
    irqstate_t flags;

    flags = enter_critical_section();

#ifdef CONFIG_LPUART1_RXDMA
  if (g_lpuart1priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_lpuart1priv.rxdma, 0, &g_lpuart1priv);
    }
#endif

#ifdef CONFIG_USART1_RXDMA
  if (g_usart1priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_usart1priv.rxdma, 0, &g_usart1priv);
    }
#endif

#ifdef CONFIG_USART2_RXDMA
  if (g_usart2priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_usart2priv.rxdma, 0, &g_usart2priv);
    }
#endif

#ifdef CONFIG_USART3_RXDMA
  if (g_usart3priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_usart3priv.rxdma, 0, &g_usart3priv);
    }
#endif

#ifdef CONFIG_UART4_RXDMA
  if (g_uart4priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_uart4priv.rxdma, 0, &g_uart4priv);
    }
#endif

#ifdef CONFIG_UART5_RXDMA
  if (g_uart5priv.rxdma != NULL)
    {
      stm32serial_dmarxcallback(g_uart5priv.rxdma, 0, &g_uart5priv);
    }
#endif

  leave_critical_section(flags);
}
#endif

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
#if CONSOLE_UART > 0
  struct stm32_serial_s *priv = g_uart_devs[CONSOLE_UART - 1];
  uint16_t ie;

  stm32serial_disableusartint(priv, &ie);

  /* Check for LF */

  if (ch == '\n')
    {
      /* Add CR */

      arm_lowputc('\r');
    }

  arm_lowputc(ch);
  stm32serial_restoreusartint(priv, ie);
#endif
}

#else /* USE_SERIALDRIVER */

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
#if CONSOLE_UART > 0
  /* Check for LF */

  if (ch == '\n')
    {
      /* Add CR */

      arm_lowputc('\r');
    }

  arm_lowputc(ch);
#endif
}

#endif /* USE_SERIALDRIVER */
