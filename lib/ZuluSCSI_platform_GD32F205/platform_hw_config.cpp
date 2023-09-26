#include "platform_hw_config.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_config.h"

HardwareConfig g_hw_config;

S2S_CFG_TYPE hw_config_selected_device()
{
    return g_hw_config.device_type();
};

bool hw_config_is_active()
{
    return g_hw_config.is_active();
}

void hw_config_init_gpios()
{
    g_hw_config.init_gpios();
}

void hw_config_init_state()
{
    g_hw_config.init_state();
}

void HardwareConfig::init_gpios()
{
    // SCSI ID dip switch
    gpio_init(DIPSW_SCSI_ID_BIT_PORT, GPIO_MODE_IPD, 0, DIPSW_SCSI_ID_BIT_PINS);
    
    // Device select BCD rotary dip switch
    gpio_init(DIPROT_DEVICE_SEL_BIT_PORT, GPIO_MODE_IPD, 0, DIPROT_DEVICE_SEL_BIT_PINS);

    // Direct/Raw Mode Select
    gpio_init(DIPSW_DIRECT_MODE_PORT, GPIO_MODE_IPD, 0, DIPSW_DIRECT_MODE_PIN);

    LED_EJECT_OFF();
    gpio_init(LED_EJECT_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, LED_EJECT_PIN);
}

void HardwareConfig::init_state()
{
    m_is_active = RESET == gpio_input_bit_get(DIPSW_DIRECT_MODE_PORT, DIPSW_DIRECT_MODE_PIN);
    m_scsi_id = (gpio_input_port_get(DIPSW_SCSI_ID_BIT_PORT) & DIPSW_SCSI_ID_BIT_PINS) >> DIPSW_SCSI_ID_BIT_SHIFT;
    
    uint8_t rotary_select = (gpio_input_port_get(DIPROT_DEVICE_SEL_BIT_PORT) & DIPROT_DEVICE_SEL_BIT_PINS) >> DIPROT_DEVICE_SEL_BIT_SHIFT;
    switch (rotary_select)
    {
    case 0:
        m_device_type = S2S_CFG_FIXED;
    break;
    case 1:
        m_device_type = S2S_CFG_SEQUENTIAL;
    break;
    case 2:
        m_device_type = S2S_CFG_OPTICAL;
    break;
    case 3:
        m_device_type = S2S_CFG_MO;
    break;

    default:
        m_device_type = S2S_CFG_FIXED;
    }
    
    if (m_device_type == S2S_CFG_OPTICAL)
    {
        m_blocksize = DEFAULT_BLOCKSIZE_OPTICAL;
    }
    else
    {
        m_blocksize = RAW_FALLBACK_BLOCKSIZE;
    }
}

