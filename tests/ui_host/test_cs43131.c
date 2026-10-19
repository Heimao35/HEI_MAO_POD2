#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/cs43131.c"
static uint8_t registers[0x100000];
static uint8_t address;
static bool reset_high,write_failed;
static unsigned writes;
static uint32_t logs[512];static uint8_t values[512];
esp_err_t i2c_master_get_bus_handle(int port,i2c_master_bus_handle_t *bus) {assert(port==1);*bus=(void*)1;return ESP_OK;}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,const i2c_device_config_t *c,i2c_master_dev_handle_t *dev)
{assert(bus==(void*)1 && c->scl_speed_hz==100000);address=c->device_address;*dev=(void*)2;return ESP_OK;}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) {(void)d;return ESP_OK;}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,const uint8_t *p,size_t n,uint8_t *out,size_t m,int timeout)
{
    assert(d==(void*)2 && n==4 && p[3]==0 && m==1 && timeout==100);
    if(address!=0x32)return ESP_FAIL;
    uint32_t reg=(uint32_t)p[0]<<16|p[1]<<8|p[2];*out=registers[reg];
    if(reg==REG_IRQ1 || reg==REG_IRQ2)registers[reg]=0;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const uint8_t *p,size_t n,int timeout)
{
    assert(d==(void*)2 && n==5 && p[3]==0 && timeout==100);
    if(write_failed)return ESP_FAIL;
    uint32_t reg=(uint32_t)p[0]<<16|p[1]<<8|p[2];
    assert(writes<512);logs[writes]=reg;values[writes++]=p[4];
    /* Enforce the datasheet's clock-source availability rule. */
    if(reg==REG_POWER && (p[4]&4))assert((registers[REG_CLOCK]&3)!=1);
    if(reg==REG_POWER && (p[4]&0x10) && !(registers[REG_POWER]&0x10))registers[REG_IRQ1]|=1;
    registers[reg]=p[4];
    if(reg==0x30001 && p[4])registers[REG_IRQ1]|=4;
    return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *c) {assert(c->pin_bit_mask==(1ULL<<52));return ESP_OK;}
esp_err_t gpio_set_level(gpio_num_t pin,int level) {assert(pin==52);reset_high=level;return ESP_OK;}
void vTaskDelay(unsigned ms) {(void)ms;}
int main(void)
{
    registers[0x10000]=0x43;registers[0x10001]=0x13;registers[0x10002]=0x10;
    registers[REG_POWER]=0xFE;registers[REG_CLOCK]=0x06;registers[0x3001B]=0x13;
    assert(cs43131_init()==ESP_OK && reset_high && address==0x32);
    assert(registers[REG_HP_DETECT]==0xCA && registers[REG_MASK1]==0x9F);
    assert((registers[REG_PCM_CTRL]&3)==3);
    assert(cs43131_prepare(192000)==ESP_OK);
    assert(registers[REG_CLOCK]==0 && registers[0x40012]==2 && registers[0x1000B]==6);
    assert(registers[0x40018]==0x1C && registers[0x5000A]==7 && registers[0x5000B]==15);
    assert(registers[REG_POWER]&8); /* active MCLK: XTAL driver stays down */
    assert(cs43131_start_hp()==ESP_OK);
    assert(cs43131_volume(50,false)==ESP_OK && registers[0x90002]==60 && !(registers[REG_PCM_CTRL]&3));
    assert(cs43131_volume(0,false)==ESP_OK && registers[0x90002]==255 && (registers[REG_PCM_CTRL]&3)==3);
    assert(cs43131_stop()==ESP_OK);
    assert(cs43131_prepare(44100)==ESP_OK);
    assert(registers[REG_CLOCK]==5 && registers[0x30005]==0x49 && registers[0x30004]==0x80);
    assert(registers[0x30008]==10 && registers[0x3000A]==118 && registers[0x40012]==8);
    assert(registers[0x3001B]==0x13 && registers[REG_POWER]&8);
    assert(cs43131_start_hp()==ESP_OK && cs43131_stop()==ESP_OK);
    assert(cs43131_prepare(48000)==ESP_OK); /* safely leave PLL before powerdown */
    assert(registers[REG_CLOCK]==0 && registers[REG_POWER]&4);
    assert(cs43131_prepare(22050)==ESP_ERR_NOT_SUPPORTED);
    bool hp;registers[REG_HP_STATUS]=0x40;assert(cs43131_poll(&hp)==ESP_OK && hp);
    registers[REG_HP_STATUS]=0x20;assert(cs43131_poll(&hp)==ESP_OK && !hp);
    cs43131_abort();assert(!reset_high && !s_dev);
    puts("PASS3byte MAP/control-byte, address+identity verification, active24.576MHz and44.1k PLL");
    puts("PASS192k32bit I2S master format, volume/mute, HP status, pop-free sequencing and safe clock change");
    return 0;
}
