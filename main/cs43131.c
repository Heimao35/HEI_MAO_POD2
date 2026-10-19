#include "cs43131.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESET_GPIO 52
/* HP_DETECT默认不反相，按手册标准插座电路。若实板检测相反，只改此项，
 * 不在UI中倒置状态。插拔均由DAC硬件进行250ms去抖。 */
#define HP_DETECT_INVERT 0
#define REG_POWER 0x20000
#define REG_CLOCK 0x10006
#define REG_IRQ1 0xF0000
#define REG_IRQ2 0xF0001
#define REG_MASK1 0xF0010
#define REG_PCM_CTRL 0x90003
#define REG_HP_DETECT 0xD0000
#define REG_HP_STATUS 0xD0001
static i2c_master_dev_handle_t s_dev;
static bool s_hp_on;
static const char *TAG="cs43131";

/* DS1155F2 pp.59–61：三字节MAP（高字节在前）之后必须有control byte。
 * 单寄存器访问control=0；不可沿用AXP2101的8位寄存器读写格式。 */
static esp_err_t rd(uint32_t reg, uint8_t *v)
{
    uint8_t map[4]={(uint8_t)(reg>>16),(uint8_t)(reg>>8),(uint8_t)reg,0};
    return i2c_master_transmit_receive(s_dev,map,sizeof(map),v,1,100);
}
static esp_err_t wr(uint32_t reg, uint8_t v)
{
    uint8_t data[5]={(uint8_t)(reg>>16),(uint8_t)(reg>>8),(uint8_t)reg,0,v};
    return i2c_master_transmit(s_dev,data,sizeof(data),100);
}
static esp_err_t modify(uint32_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t v; esp_err_t r=rd(reg,&v);
    return r==ESP_OK?wr(reg,(v&~mask)|(bits&mask)):r;
}
#define TRY(call) do { esp_err_t r_=(call); if(r_!=ESP_OK)return r_; } while(0)
static esp_err_t wait_irq(uint8_t ready, uint8_t error, unsigned timeout_ms)
{
    for(unsigned ms=0;ms<timeout_ms;ms+=5) {
        uint8_t v; TRY(rd(REG_IRQ1,&v)); /* sticky/read-to-clear, never W1C */
        if(v&error)return ESP_ERR_INVALID_RESPONSE;
        if(v&ready)return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t cs43131_init(void)
{
    if(s_dev)return ESP_OK;
    i2c_master_bus_handle_t bus;
    TRY(i2c_master_get_bus_handle(I2C_NUM_1,&bus));
    gpio_config_t reset={.pin_bit_mask=1ULL<<RESET_GPIO,.mode=GPIO_MODE_OUTPUT};
    TRY(gpio_config(&reset));
    TRY(gpio_set_level(RESET_GPIO,0));
    vTaskDelay(pdMS_TO_TICKS(5));
    TRY(gpio_set_level(RESET_GPIO,1));
    vTaskDelay(pdMS_TO_TICKS(5)); /* tPUD >=1.5ms, after rails ready */
    for(unsigned addr=0x30;addr<=0x33;++addr) {
        i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
            .device_address=addr,.scl_speed_hz=100000};
        TRY(i2c_master_bus_add_device(bus,&cfg,&s_dev));
        uint8_t a=0,b=0,c=0;
        if(rd(0x10000,&a)==ESP_OK && rd(0x10001,&b)==ESP_OK &&
           rd(0x10002,&c)==ESP_OK && a==0x43 && b==0x13 && (c&0xF0)==0x10) {
            ESP_LOGI(TAG,"CS43131 found at 0x%02x; external MCLK=24.576MHz",addr);
            esp_err_t ret=wr(REG_PCM_CTRL,0xEF); /* soft-ramp mute, B follows A */
            if(ret==ESP_OK)ret=wr(REG_MASK1,0x9F); /* HP plug/unplug only */
            if(ret==ESP_OK)ret=wr(REG_HP_DETECT,0x0A|(HP_DETECT_INVERT?0x20:0));
            if(ret==ESP_OK)ret=wr(REG_HP_DETECT,0xCA|(HP_DETECT_INVERT?0x20:0));
            if(ret==ESP_OK)return ESP_OK;
            i2c_master_bus_rm_device(s_dev);s_dev=NULL;return ret;
        }
        i2c_master_bus_rm_device(s_dev);s_dev=NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cs43131_prepare(uint32_t rate)
{
    if(!s_dev || s_hp_on)return ESP_ERR_INVALID_STATE;
    uint8_t code,denom;bool family44=false;
    switch(rate) {
        case 32000:code=0;denom=12;break;
        case 44100:code=1;denom=8;family44=true;break;
        case 48000:code=2;denom=8;break;
        case 88200:code=3;denom=4;family44=true;break;
        case 96000:code=4;denom=4;break;
        case 176400:code=5;denom=2;family44=true;break;
        case 192000:code=6;denom=2;break;
        default:return ESP_ERR_NOT_SUPPORTED;
    }
    TRY(wr(REG_PCM_CTRL,0xEF));
    TRY(wr(REG_CLOCK,0x02)); /* switch to live RCO before changing PLL */
    TRY(wr(REG_POWER,0xFE)); /* ports/HP/XTAL/PLL off, RCO remains available */
    TRY(wr(0x30001,0));
    if(family44) {
        /* DS1155F2 Table4-5:24.576MHz ->22.5792MHz. Internal XTAL stays off.
         * PLL_REF_PREDIV=8,INT=0x49,FRAC=0x800000,OUT_DIV=10,
         * MODE=1,CAL_RATIO=118. Preserve reserved bits in PLL_MODE. */
        TRY(modify(REG_POWER,0x04,0));
        TRY(modify(0x40002,0x03,0x03));
        TRY(wr(0x30008,0x0A));
        TRY(wr(0x30002,0));TRY(wr(0x30003,0));TRY(wr(0x30004,0x80));
        TRY(wr(0x30005,0x49));TRY(modify(0x3001B,0x02,0x02));
        TRY(wr(0x3000A,118));
        uint8_t discard;TRY(rd(REG_IRQ1,&discard));
        TRY(wr(REG_MASK1,0x99)); /* HP + PLL ready/error unmasked */
        TRY(wr(0x30001,1));TRY(wait_irq(0x04,0x02,100));
        TRY(wr(REG_CLOCK,0x05));
    } else TRY(wr(REG_CLOCK,0x00)); /* direct active external24.576MHz */
    TRY(wr(0x1000B,code));
    TRY(wr(0x1000C,0x04)); /* ASP32bit, XSP irrelevant */
    TRY(wr(0x40010,1));TRY(wr(0x40011,0));
    TRY(wr(0x40012,denom));TRY(wr(0x40013,0));
    TRY(wr(0x40014,31));TRY(wr(0x40015,0));
    TRY(wr(0x40016,63));TRY(wr(0x40017,0));
    TRY(wr(0x40018,0x1C)); /* DAC master; P4 slave, no competing BCLK driver */
    TRY(wr(0x40019,0x0A)); /* Philips:50/50,1bit delay */
    TRY(wr(0x50000,0));TRY(wr(0x50001,0));
    TRY(wr(0x5000A,0x07));TRY(wr(0x5000B,0x0F));
    TRY(wr(0x1000D,0x02)); /* enable ASP clock outputs, XSP Hi-Z */
    TRY(wr(0x90000,0x02));TRY(wr(0x90004,0));
    TRY(wr(0xB0000,0x1E));TRY(wr(0x80000,0x30));
    TRY(wr(REG_MASK1,0x98)); /* HP,PLL errors/ready,PDN_DONE */
    TRY(wr(0x10010,0x99));TRY(wr(0x80032,0x20)); /* Ex5-5 */
    TRY(modify(REG_POWER,0x40,0)); /* start ASP clocks, HP remains muted/off */
    return ESP_OK;
}

esp_err_t cs43131_start_hp(void)
{
    TRY(modify(REG_POWER,0x10,0));
    s_hp_on=true; /* ensure a later error still takes the shutdown sequence */
    vTaskDelay(pdMS_TO_TICKS(15)); /* datasheet12ms minimum */
    TRY(wr(0x80032,0));TRY(wr(0x10010,0));
    return ESP_OK;
}
esp_err_t cs43131_volume(uint8_t percent,bool mute)
{
    if(!s_dev)return ESP_ERR_INVALID_STATE;
    if(percent>100)percent=100;
    /* 百分比映射到0..-60dB；0和静音使用数字静音，保留PCM_SZC软渐变。
     * 不对PCM乘浮点系数，完整保留16/24/32bit源数据及高采样率。 */
    uint8_t attenuation=percent?(uint8_t)(((100-percent)*120+50)/100):255;
    TRY(wr(0x90002,attenuation));
    TRY(wr(0x90001,attenuation));
    return modify(REG_PCM_CTRL,3,(mute||!percent)?3:0);
}
esp_err_t cs43131_stop(void)
{
    if(!s_dev)return ESP_OK;
    esp_err_t first=modify(REG_PCM_CTRL,3,3);
    if(s_hp_on) {
        vTaskDelay(pdMS_TO_TICKS(150)); /* soft mute while clocks still run; HP down follows Ex5-3 */
        uint8_t discard;rd(REG_IRQ1,&discard);
        esp_err_t r=modify(REG_POWER,0x10,0x10);
        if(r==ESP_OK)r=wait_irq(1,0,100);
        if(first==ESP_OK)first=r;
        if(r==ESP_OK)s_hp_on=false;
    }
    if(first!=ESP_OK)return first; /* caller holds RESET on unrecoverable I2C fault */
    TRY(modify(REG_POWER,0x40,0x40));
    TRY(wr(0x10010,0));TRY(wr(0x80032,0));
    TRY(wr(REG_CLOCK,0x02)); /* idle: RCO for I2C/HP detect, before disabling PLL */
    TRY(wr(0x30001,0));
    TRY(modify(REG_POWER,0x04,0x04));
    return ESP_OK;
}
esp_err_t cs43131_poll(bool *headphone)
{
    if(!s_dev || !headphone)return ESP_ERR_INVALID_STATE;
    uint8_t irq,status,asp;
    TRY(rd(REG_IRQ1,&irq));TRY(rd(REG_IRQ2,&asp));TRY(rd(REG_HP_STATUS,&status));
    *headphone=(status&0x40)!=0;
    if(irq&0x02) { ESP_LOGW(TAG,"PLL error");return ESP_ERR_INVALID_RESPONSE; }
    if(asp&0xF8)ESP_LOGW(TAG,"ASP status=0x%02x",asp);
    return ESP_OK;
}

void cs43131_abort(void)
{
    /* I2C故障无法确认静音/下电时，使用已接的硬复位钳住输出；不循环电源轨。 */
    gpio_set_level(RESET_GPIO,0);
    if(s_dev)i2c_master_bus_rm_device(s_dev);
    s_dev=NULL;s_hp_on=false;
}
