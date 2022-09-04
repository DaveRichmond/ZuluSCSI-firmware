#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include <SdFat.h>
#include <scsi.h>
#include <assert.h>

extern "C" {

const char *g_azplatform_name = PLATFORM_NAME;

/*
 * Timing functions
 */
static volatile uint32_t g_millisecond_counter;
static volatile uint32_t g_watchdog_timeout;
static uint32_t g_ns_to_cycles; 

static void watchdog_handler(uint32_t *sp);

unsigned long millis(){
    return g_millisecond_counter;
}

void delay(unsigned long ms){
    uint32_t start = g_millisecond_counter;
    while((uint32_t)(g_millisecond_counter - start) < ms);
}

void delay_ns(unsigned long ns){
    uint32_t CNT_start = DWT->CYCCNT;
    if(ns <= 100) return; // Approximate call overhead
    ns -= 100;

    uint32_t cycles = ((uint64_t)ns * g_ns_to_cycles) >> 32;
    while((uint32_t)(DWT->CYCCNT - CNT_start) < cycles);
}

void SysTick_Handler_inner(uint32_t *sp){
    g_millisecond_counter++;

    if(g_watchdog_timeout > 0){
        g_watchdog_timeout--;

        const uint32_t busreset_time = WATCHDOG_CRASH_TIMEOUT - WATCHDOG_BUS_RESET_TIMEOUT;
        if(g_watchdog_timeout <= busreset_time){
            if(!scsiDev.resetFlag){
                azlog("WATCHDOG TIMEOUT at PC ", sp[6], " LR ", sp[5], " attempting bus reset");
                scsiDev.resetFlag = 1;
            }

            if(g_watchdog_timeout == 0){
                watchdog_handler(sp);
            }
        }
    }
}

__attribute__((interrupt, naked))
void SysTick_Handler(void){
    // Take note of stack pointer so that we can print debug
    // info in watchdog handler
    asm("mrs r0, msp\n"
        "b SysTick_Handler_inner": : : "r0");
}

void SysTick_Handle_PreEmptively(){
    static int skipped_clocks = 0;

    __disable_irq();
    uint32_t loadval = SysTick->LOAD;
    skipped_clocks += loadval - SysTick->VAL;
    SysTick->VAL = 0;

    if(skipped_clocks > loadval){
        // we have skipped enough ticks that it is time to fake a call
        // to SysTick interrupt handler
        skipped_clocks -= loadval;
        uint32_t stack_frame[8] = {0};
        stack_frame[6] = (uint32_t)__builtin_return_address(0);
        SysTick_Handler_inner(stack_frame);
    }
    __enable_irq();
}

/***************/
/* GPIO init   */
/***************/

void azplatform_init()
{
    SystemCoreClockUpdate();

    // Enable SysTick to drive millis()
    g_millisecond_counter = 0;
    SysTick_Config(SystemCoreClock / 1000U);
    NVIC_SetPriority(SysTick_IRQn, 0x00U);

    // enable DWT counter to drive delay_ns()
    g_ns_to_cycles = ((uint64_t)SystemCoreClock << 32) / 1000000000;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // No SWO, debug output somehow?

    //rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);

    SCSI_RELEASE_OUTPUTS();
    gpio_init(SCSI_OUT_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_DATA_MASK | SCSI_OUT_REQ);
    gpio_init(SCSI_OUT_IO_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_IO_PIN);
    gpio_init(SCSI_OUT_CD_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_CD_PIN);
    gpio_init(SCSI_OUT_SEL_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_SEL_PIN);
    gpio_init(SCSI_OUT_MSG_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_MSG_PIN);
    gpio_init(SCSI_OUT_RST_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_RST_PIN);
    gpio_init(SCSI_OUT_BSY_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_MAX, SCSI_OUT_BSY_PIN);

    //gpio_init(SCSI_IN_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_IN_MASK);
    gpio_init(SCSI_ATN_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_ATN_PIN);
    gpio_init(SCSI_BSY_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_BSY_PIN);
    gpio_init(SCSI_SEL_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_SEL_PIN);
    gpio_init(SCSI_ACK_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_ACK_PIN);
    gpio_init(SCSI_RST_PORT, GPIO_MODE_IN_FLOATING, 0, SCSI_RST_PIN);

    // SD Card on SPI
    gpio_init(SD_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_MAX, SD_CS_PIN);
    gpio_init(SD_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_MAX, SD_CLK_PIN);
    gpio_init(SD_PORT, GPIO_MODE_IPU, 0, SD_MISO_PIN);
    gpio_init(SD_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_MAX, SD_MOSI_PIN);

    // no dip switches

    // LED pins
    gpio_bit_set(LED_PORT, LED_PIN);
    gpio_init(LED_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, LED_PIN);
}

void azplatform_late_init()
{
    /* This function can usually be left empty.
     * It can be used for initialization code that should not run in bootloader.
     */

    // fake dip switch stuff here
    g_azlog_debug = true; // debug on for now
}


/*
 * Crash Handler
 */
extern SdFs SD;
void azplatform_emergency_log_save(){
    azplatform_set_sd_callback(NULL, NULL);

    SD.begin(SD_CONFIG_CRASH);
    FsFile crashFile = SD.open(CRASHFILE, O_WRONLY|O_CREAT|O_TRUNC);

    if(!crashFile.isOpen()){
        int max_retry = 10;
        while(max_retry-- > 0 && !SD.begin(SD_CONFIG_CRASH));

        crashFile = SD.open(CRASHFILE, O_WRONLY|O_CREAT|O_TRUNC);
    }

    uint32_t startpos = 0;
    crashFile.write(azlog_get_buffer(&startpos));
    crashFile.write(azlog_get_buffer(&startpos));
    crashFile.flush();
    crashFile.close();
}

extern uint32_t _estack;

__attribute__((noinline))
void show_hardfault(uint32_t *sp){
    uint32_t pc = sp[6];
    uint32_t lr = sp[5];
    uint32_t cfsr = SCB->CFSR;

    azlog("-----------");
    azlog("CRASH!");
    azlog("Platform: ", g_azplatform_name);
    azlog("FW Version: ", g_azlog_firmwareversion);
    azlog("CFSR: ", cfsr);
    azlog("SP: ", (uint32_t)sp);
    azlog("PC: ", pc);
    azlog("LR: ", lr);
    azlog("R0: ", sp[0]);
    azlog("R1: ", sp[1]);
    azlog("R2: ", sp[2]);
    azlog("R3: ", sp[3]);

    uint32_t *p = (uint32_t *)((uint32_t)sp & ~3);
    for(int i = 0; i < 8; i++){
        if(p == &_estack) break; // end of stack
        azlog("STACK ", (uint32_t)p, ":    ", p[0], " ", p[1], " ", p[2], " ", p[3]);
    }

    azplatform_emergency_log_save();

    while(1){
        // Flush the crash address on the LED
        // short pulse means 0, long means 1    
        int base_delay = 1000;
        for(int i = 31; i >= 0; i--){
            LED_OFF();
            for(int j = 0; j < base_delay; j++) delay_ns(100000);
            
            int delay = (pc & 1 << i) ? (3 * base_delay) : base_delay;
            LED_ON();
            for(int j = 0; j < delay; j++) delay_ns(100000);
            LED_OFF();
        }

        for(int j = 0; j < base_delay * 10; j++) delay_ns(100000);
    }
}

__attribute__((naked, interrupt))
void HardFault_Handler(void){
    asm("mrs r0, msp\n"
        "b show_hardfault": : :"r0");
}

__attribute__((naked, interrupt))
void MemManage_Handler(void){
    asm("mrs r0, msp\n"
        "b show_hardfault": : :"r0");   
}

__attribute((naked, interrupt))
void BusFault_Handler(void){
    asm("mrs r0, msp\n"
        "b show_hardfault": : :"r0");
}

__attribute__((naked, interrupt))
void UsageFault_Handler(void){
    asm("mrs r0, msp\n"
        "b show_hardfault": : :"r0");
}

void __assert_func(const char *file, int line, const char *func, const char *expr){
    uint32_t dummy = 0;

    azlog("--------------");
    azlog("ASSERT FAILED!");
    azlog("Platform: ", g_azplatform_name);
    azlog("FW Version: ", g_azlog_firmwareversion);
    azlog("Assert failed: ", file , ":", line, " in ", func, ":", expr);

    uint32_t *p = (uint32_t*)((uint32_t)&dummy & ~3);
    for (int i = 0; i < 8; i++)
    {
        if (p == &_estack) break; // End of stack

        azlog("STACK ", (uint32_t)p, ":    ", p[0], " ", p[1], " ", p[2], " ", p[3]);
        p += 4;
    }

    azplatform_emergency_log_save();

    while(1)
    {
        LED_OFF();
        for (int j = 0; j < 1000; j++) delay_ns(100000);
        LED_ON();
        for (int j = 0; j < 1000; j++) delay_ns(100000);
    }
}

static void watchdog_handler(uint32_t *sp){
    azlog("------- WATCHDOG TIMEOUT");
    show_hardfault(sp);
}

/*****************************************/
/* Debug logging and watchdor            */
/*****************************************/


// This function is called for every log message.
// It can e.g. write the log to serial port in real time.
// It can also be left empty to use only the debug log file on SD card.
void azplatform_log(const char *s)
{
}

// This function can be used to periodically reset watchdog timer for crash handling.
// It can also be left empty if the platform does not use a watchdog timer.
void azplatform_reset_watchdog(){
    g_watchdog_timeout = WATCHDOG_CRASH_TIMEOUT;
}

/**********************************************/
/* Mapping from data bytes to GPIO BOP values */
/**********************************************/

/* A lookup table is the fastest way to calculate parity and convert the IO pin mapping for
 * data bus. The method below uses the BOP register of GD32, this is called BSRR on STM32.
 * If there are no other pins on the same port, you can also use direct writes to the GPIO.
 */

#define PARITY(n) ((1 ^ (n) ^ ((n)>>1) ^ ((n)>>2) ^ ((n)>>3) ^ ((n)>>4) ^ ((n)>>5) ^ ((n)>>6) ^ ((n)>>7)) & 1)
#define X(n) (\
    ((n & 0x01) ? (SCSI_OUT_DB0 << 16) : SCSI_OUT_DB0) | \
    ((n & 0x02) ? (SCSI_OUT_DB1 << 16) : SCSI_OUT_DB1) | \
    ((n & 0x04) ? (SCSI_OUT_DB2 << 16) : SCSI_OUT_DB2) | \
    ((n & 0x08) ? (SCSI_OUT_DB3 << 16) : SCSI_OUT_DB3) | \
    ((n & 0x10) ? (SCSI_OUT_DB4 << 16) : SCSI_OUT_DB4) | \
    ((n & 0x20) ? (SCSI_OUT_DB5 << 16) : SCSI_OUT_DB5) | \
    ((n & 0x40) ? (SCSI_OUT_DB6 << 16) : SCSI_OUT_DB6) | \
    ((n & 0x80) ? (SCSI_OUT_DB7 << 16) : SCSI_OUT_DB7) | \
    (PARITY(n)  ? (SCSI_OUT_DBP << 16) : SCSI_OUT_DBP) | \
    (SCSI_OUT_REQ) \
)

const uint32_t g_scsi_out_byte_to_bop[256] =
{
    X(0x00), X(0x01), X(0x02), X(0x03), X(0x04), X(0x05), X(0x06), X(0x07), X(0x08), X(0x09), X(0x0a), X(0x0b), X(0x0c), X(0x0d), X(0x0e), X(0x0f),
    X(0x10), X(0x11), X(0x12), X(0x13), X(0x14), X(0x15), X(0x16), X(0x17), X(0x18), X(0x19), X(0x1a), X(0x1b), X(0x1c), X(0x1d), X(0x1e), X(0x1f),
    X(0x20), X(0x21), X(0x22), X(0x23), X(0x24), X(0x25), X(0x26), X(0x27), X(0x28), X(0x29), X(0x2a), X(0x2b), X(0x2c), X(0x2d), X(0x2e), X(0x2f),
    X(0x30), X(0x31), X(0x32), X(0x33), X(0x34), X(0x35), X(0x36), X(0x37), X(0x38), X(0x39), X(0x3a), X(0x3b), X(0x3c), X(0x3d), X(0x3e), X(0x3f),
    X(0x40), X(0x41), X(0x42), X(0x43), X(0x44), X(0x45), X(0x46), X(0x47), X(0x48), X(0x49), X(0x4a), X(0x4b), X(0x4c), X(0x4d), X(0x4e), X(0x4f),
    X(0x50), X(0x51), X(0x52), X(0x53), X(0x54), X(0x55), X(0x56), X(0x57), X(0x58), X(0x59), X(0x5a), X(0x5b), X(0x5c), X(0x5d), X(0x5e), X(0x5f),
    X(0x60), X(0x61), X(0x62), X(0x63), X(0x64), X(0x65), X(0x66), X(0x67), X(0x68), X(0x69), X(0x6a), X(0x6b), X(0x6c), X(0x6d), X(0x6e), X(0x6f),
    X(0x70), X(0x71), X(0x72), X(0x73), X(0x74), X(0x75), X(0x76), X(0x77), X(0x78), X(0x79), X(0x7a), X(0x7b), X(0x7c), X(0x7d), X(0x7e), X(0x7f),
    X(0x80), X(0x81), X(0x82), X(0x83), X(0x84), X(0x85), X(0x86), X(0x87), X(0x88), X(0x89), X(0x8a), X(0x8b), X(0x8c), X(0x8d), X(0x8e), X(0x8f),
    X(0x90), X(0x91), X(0x92), X(0x93), X(0x94), X(0x95), X(0x96), X(0x97), X(0x98), X(0x99), X(0x9a), X(0x9b), X(0x9c), X(0x9d), X(0x9e), X(0x9f),
    X(0xa0), X(0xa1), X(0xa2), X(0xa3), X(0xa4), X(0xa5), X(0xa6), X(0xa7), X(0xa8), X(0xa9), X(0xaa), X(0xab), X(0xac), X(0xad), X(0xae), X(0xaf),
    X(0xb0), X(0xb1), X(0xb2), X(0xb3), X(0xb4), X(0xb5), X(0xb6), X(0xb7), X(0xb8), X(0xb9), X(0xba), X(0xbb), X(0xbc), X(0xbd), X(0xbe), X(0xbf),
    X(0xc0), X(0xc1), X(0xc2), X(0xc3), X(0xc4), X(0xc5), X(0xc6), X(0xc7), X(0xc8), X(0xc9), X(0xca), X(0xcb), X(0xcc), X(0xcd), X(0xce), X(0xcf),
    X(0xd0), X(0xd1), X(0xd2), X(0xd3), X(0xd4), X(0xd5), X(0xd6), X(0xd7), X(0xd8), X(0xd9), X(0xda), X(0xdb), X(0xdc), X(0xdd), X(0xde), X(0xdf),
    X(0xe0), X(0xe1), X(0xe2), X(0xe3), X(0xe4), X(0xe5), X(0xe6), X(0xe7), X(0xe8), X(0xe9), X(0xea), X(0xeb), X(0xec), X(0xed), X(0xee), X(0xef),
    X(0xf0), X(0xf1), X(0xf2), X(0xf3), X(0xf4), X(0xf5), X(0xf6), X(0xf7), X(0xf8), X(0xf9), X(0xfa), X(0xfb), X(0xfc), X(0xfd), X(0xfe), X(0xff)
};

#undef X

} /* extern "C" */

// Dragging in the zuluscsi boards sd_card_spi file should remove these...
#if 0
/* The SdFat library is used for SD card access.
 * You can set the configuration here.
 * Refer to SdFat examples for usage on various CPUs.
 */
SdSpiConfig g_sd_spi_config(0, DEDICATED_SPI, SD_SCK_MHZ(25));

void azplatform_set_sd_callback(sd_callback_t func, const uint8_t *buffer)
{
    /* This function can be left empty.
     * If the platform supports DMA for SD card transfers, this function
     * can be used to set a callback that is invoked while waiting for DMA
     * to finish. In that way the SD card and SCSI transfers can execute
     * simultaneously.
     */
}
#endif