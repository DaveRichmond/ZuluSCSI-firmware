// Platform-specific definitions for ZuluSCSI.
//
// This file is example platform definition that can easily be
// customized for a different board / CPU.

#pragma once

/* Add any platform-specific includes you need here */
#include <gd32f30x.h>
#include <gd32f30x_gpio.h>
#include <gd32f30x_usart.h>
#include <stdint.h>
#include <scsi2sd.h>
#include "ZuluSCSI_platform_gpio.h"
#include "ZuluSCSI_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These are used in debug output and default SCSI strings */
extern const char *g_azplatform_name;
#define PLATFORM_NAME "BlueSCSI w/ BluePill+ GD32F303CC"
#define PLATFORM_REVISION "1.0"

// Debug logging function, can be used to print to e.g. serial port.
// May get called from interrupt handlers.
void azplatform_log(const char *s);

// Timing and delay functions.
// Arduino platform already provides these
unsigned long millis(void);
void delay(unsigned long ms);
void delay_ns(unsigned long ns);

static inline void delay_us(unsigned long us){
    if(us > 0){
        delay_ns(us * 1000);
    }
}

static inline void delay_100ns(){
    asm volatile("nop \n nop \n nop \n nop \n nop \n nop");
}

// Initialize SD card and GPIO configuration
void azplatform_init();

// Initialization for main application, not used for bootloader
void azplatform_late_init();

// Setup soft watchdog if supported
void azplatform_reset_watchdog();

// Reinitialise SD card connection and save log from interrupt context
// this can be used in crash handlers
void azplatform_emergency_log_save();

// Set callback that will be called during data transfer to/from SD card.
// This can be used to implement simultaneous transfer to SCSI bus.
typedef void (*sd_callback_t)(uint32_t bytes_complete);
void azplatform_set_sd_callback(sd_callback_t func, const uint8_t *buffer);

// This function is called by scsiPhy.cpp.
// It resets the systick counter to give 1 millisecond of uninterrupted transfer time.
// The total number of skips is kept track of to keep the correct time on average.
void SysTick_Handle_PreEmptively();

// Below are GPIO access definitions that are used from scsiPhy.cpp.
// The definitions shown will work for STM32 style devices, other platforms
// will need adaptations.

// Write a single SCSI pin.
// Example use: SCSI_OUT(ATN, 1) sets SCSI_ATN to low (active) state.
#define SCSI_OUT(pin, state) \
    GPIO_BOP(SCSI_OUT_ ## pin ## _PORT) = (SCSI_OUT_ ## pin ## _PIN) << (state ? 16 : 0)

// Read a single SCSI pin.
// Example use: SCSI_IN(ATN), returns 1 for active low state.
#define SCSI_IN(pin) \
    ((GPIO_ISTAT(SCSI_ ## pin ## _PORT) & (SCSI_ ## pin ## _PIN)) ? 0 : 1)

// Write SCSI data bus, also sets REQ to inactive.
extern const uint32_t g_scsi_out_byte_to_bop[256];
#define SCSI_OUT_DATA(data) \
    GPIO_BOP(SCSI_OUT_PORT) = g_scsi_out_byte_to_bop[(uint8_t)(data)]

// Release SCSI data bus and REQ signal
#define SCSI_RELEASE_DATA_REQ() \
    GPIO_BOP(SCSI_OUT_PORT) = SCSI_OUT_DATA_MASK | SCSI_OUT_REQ

// Release all SCSI outputs
#define SCSI_RELEASE_OUTPUTS() \
    GPIO_BOP(SCSI_OUT_PORT) = SCSI_OUT_DATA_MASK | SCSI_OUT_REQ, \
    GPIO_BOP(SCSI_OUT_IO_PORT)  = SCSI_OUT_IO_PIN, \
    GPIO_BOP(SCSI_OUT_CD_PORT)  = SCSI_OUT_CD_PIN, \
    GPIO_BOP(SCSI_OUT_SEL_PORT) = SCSI_OUT_SEL_PIN, \
    GPIO_BOP(SCSI_OUT_MSG_PORT) = SCSI_OUT_MSG_PIN, \
    GPIO_BOP(SCSI_OUT_RST_PORT) = SCSI_OUT_RST_PIN, \
    GPIO_BOP(SCSI_OUT_BSY_PORT) = SCSI_OUT_BSY_PIN

// Read SCSI data bus
#define SCSI_IN_DATA(data) \
    (((~GPIO_ISTAT(SCSI_IN_PORT)) & SCSI_IN_MASK) >> SCSI_IN_SHIFT)

#ifdef __cplusplus
}

// SD card driver for SdFat
class SdSpiConfig;
extern SdSpiConfig g_sd_spi_config;
#define SD_CONFIG g_sd_spi_config
#define SD_CONFIG_CRASH g_sd_spi_config

#endif