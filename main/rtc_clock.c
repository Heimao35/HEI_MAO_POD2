/** PCF85063TP Rev.4 (2015-05-06), sections 8.1..8.4 and 9.
 * All I2C runs in the existing PMIC task. Short RAM critical sections protect
 * snapshots/commands only; neither LVGL nor I2C runs inside the spinlock.
 */
#include "rtc_clock.h"
#include "pmic_axp2101.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define RTC_ADDRESS 0x51 /* Datasheet A2h/A3h are 8-bit addresses. */
#define RTC_IRQ_GPIO 11
#define RTC_TIMEOUT_MS 100
#define CTRL_STOP 0x20
#define CTRL_12H 0x02
#define SECONDS_OS 0x80
static const char *TAG="rtc_clock";
static i2c_master_dev_handle_t s_device;
static portMUX_TYPE s_snapshot_lock=portMUX_INITIALIZER_UNLOCKED;
static rtc_clock_snapshot_t s_snapshot;
static rtc_datetime_t s_requested;
static bool s_queued, s_configured;
static uint32_t s_request_id;
static TickType_t s_last_read, s_last_config;
static bool s_read_once, s_config_once, s_fault_logged, s_irq_installed;
static bool s_write_uncertain;

uint8_t rtc_clock_days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(month<1 || month>12) return 0;
    if(month==2 && year%4==0 && (year%100!=0 || year%400==0)) return 29;
    return days[month-1];
}
bool rtc_clock_datetime_valid(const rtc_datetime_t *t)
{
    return t && t->year>=2000 && t->year<=2099 && t->month>=1 && t->month<=12 &&
        t->day>=1 && t->day<=rtc_clock_days_in_month(t->year,t->month) &&
        t->weekday<=6 && t->hour<24 && t->minute<60 && t->second<60;
}
static bool unbcd(uint8_t raw, uint8_t *value)
{
    if((raw&15)>9 || (raw>>4)>9) return false;
    *value=(raw>>4)*10+(raw&15);
    return true;
}
static uint8_t bcd(unsigned value) { return (value/10)*16+value%10; }

/* One burst freezes all counters during the read (DS 8.4), avoiding mixed
 * dates/minutes around rollover. Access is bounded far below the 1s limit.
 * OS is never cleared by reading/configuring: only an explicit user time set
 * can establish a valid clock after loss of oscillator/backup power. */
static bool decode(const uint8_t r[11], rtc_datetime_t *t)
{
    uint8_t year;
    if((r[0]&(0x80|CTRL_STOP)) || (r[4]&SECONDS_OS) ||
       !unbcd(r[4]&0x7f,&t->second) || !unbcd(r[5]&0x7f,&t->minute) ||
       !unbcd(r[6]&((r[0]&CTRL_12H)?0x1f:0x3f),&t->hour) ||
       !unbcd(r[7]&0x3f,&t->day) || !unbcd(r[9]&0x1f,&t->month) ||
       !unbcd(r[10],&year)) return false;
    if(r[0]&CTRL_12H) {
        if(t->hour<1 || t->hour>12) return false;
        t->hour=t->hour%12+((r[6]&0x20)?12:0);
    }
    t->year=2000+year;
    t->weekday=r[8]&7;
    return rtc_clock_datetime_valid(t);
}
static esp_err_t read_regs(uint8_t reg, uint8_t *out, size_t size)
{ return i2c_master_transmit_receive(s_device,&reg,1,out,size,RTC_TIMEOUT_MS); }
static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[]={reg,value};
    return i2c_master_transmit(s_device,data,sizeof(data),RTC_TIMEOUT_MS);
}
static esp_err_t read_clock(rtc_datetime_t *time, bool *valid)
{
    uint8_t regs[11];
    esp_err_t ret=read_regs(0,regs,sizeof(regs));
    *valid=ret==ESP_OK && decode(regs,time);
    return ret;
}
static esp_err_t configure(void)
{
    uint8_t ctrl2,offset;
    esp_err_t ret=read_regs(1,&ctrl2,1);
    if(ret==ESP_OK) ret=read_regs(2,&offset,1);
    /* MI is usable only with offset MODE=0 (DS 8.2.2). Preserve calibration,
     * oscillator capacitance and CLKOUT. In MODE=1 use polling instead; do not
     * silently change a calibrated chip. INT is only a refresh hint, no wake. */
    if(ret==ESP_OK) ret=write_reg(1,(ctrl2&7)|((offset&0x80)?0:0x20));
    return ret;
}
static void next_second(rtc_datetime_t *t)
{
    if(++t->second<60) return;
    t->second=0;
    if(++t->minute<60) return;
    t->minute=0;
    if(++t->hour<24) return;
    t->hour=0;t->weekday=(t->weekday+1)%7;
    if(++t->day<=rtc_clock_days_in_month(t->year,t->month)) return;
    t->day=1;
    if(++t->month<=12) return;
    t->month=1;t->year=t->year==2099?2000:t->year+1;
}
static bool equal_time(const rtc_datetime_t *a,const rtc_datetime_t *b)
{
    return a->year==b->year && a->month==b->month && a->day==b->day &&
        a->weekday==b->weekday && a->hour==b->hour && a->minute==b->minute &&
        a->second==b->second;
}
static esp_err_t set_clock(const rtc_datetime_t *t,rtc_datetime_t *actual,bool *valid)
{
    uint8_t ctrl;
    esp_err_t ret=read_regs(0,&ctrl,1);
    if(ret!=ESP_OK) return ret;
    /* Preserve CAP_SEL/CIE and unrelated fields; never issue SR=0x58. Use
     * STOP while changing all seven counters, convert to normal 24h mode,
     * clear OS only in this explicit set. Always attempt to resume after a
     * failed calendar write, so an I2C glitch does not knowingly freeze RTC. */
    uint8_t old_ctrl=ctrl;
    ctrl&=(uint8_t)~(0x80|CTRL_STOP|0x10|CTRL_12H);
    s_write_uncertain=true;
    ret=write_reg(0,ctrl|CTRL_STOP);
    if(ret!=ESP_OK) {
        (void)write_reg(0,ctrl); /* A timed-out STOP write may have reached RTC. */
        return ret;
    }
    uint8_t data[]={4,bcd(t->second),bcd(t->minute),bcd(t->hour),bcd(t->day),
                    t->weekday,bcd(t->month),bcd(t->year-2000)};
    ret=i2c_master_transmit(s_device,data,sizeof(data),RTC_TIMEOUT_MS);
    esp_err_t run_ret=write_reg(0,ret==ESP_OK?ctrl:
        old_ctrl&(uint8_t)~(0x80|CTRL_STOP|0x10));
    if(ret!=ESP_OK) return ret;
    if(run_ret!=ESP_OK) return run_ret;
    ret=read_clock(actual,valid);
    rtc_datetime_t one_later=*t;
    next_second(&one_later);
    if(ret==ESP_OK && (!*valid || (!equal_time(t,actual) && !equal_time(&one_later,actual))))
        ret=ESP_ERR_INVALID_RESPONSE;
    if(ret==ESP_OK) s_write_uncertain=false;
    return ret;
}

void rtc_clock_get_snapshot(rtc_clock_snapshot_t *out)
{
    if(!out) return;
    portENTER_CRITICAL(&s_snapshot_lock);
    *out=s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}
esp_err_t rtc_clock_request_set(const rtc_datetime_t *time,uint32_t *id)
{
    if(!rtc_clock_datetime_valid(time) || !id) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_snapshot_lock);
    if(!s_device || s_snapshot.set_pending) {
        portEXIT_CRITICAL(&s_snapshot_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_requested=*time;s_queued=true;s_snapshot.set_pending=true;
    if(++s_request_id==0) ++s_request_id;
    *id=s_request_id;
    portEXIT_CRITICAL(&s_snapshot_lock);
    pmic_axp2101_notify_service();
    return ESP_OK;
}
static void IRAM_ATTR rtc_irq_handler(void *arg)
{
    (void)arg;
    pmic_axp2101_notify_service_from_isr();
}
esp_err_t rtc_clock_init(void)
{
    if(s_device) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_handle_t bus;
    esp_err_t ret=i2c_master_get_bus_handle(I2C_NUM_1,&bus);
    if(ret!=ESP_OK) return ret;
    const i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=RTC_ADDRESS,.scl_speed_hz=100000};
    ret=i2c_master_bus_add_device(bus,&cfg,&s_device);
    if(ret!=ESP_OK) return ret;
    const gpio_config_t irq={.pin_bit_mask=1ULL<<RTC_IRQ_GPIO,.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_NEGEDGE};
    ret=gpio_config(&irq);
    if(ret==ESP_OK) {
        ret=gpio_isr_handler_add(RTC_IRQ_GPIO,rtc_irq_handler,NULL);
        s_irq_installed=ret==ESP_OK;
    }
    if(ret!=ESP_OK) ESP_LOGW(TAG,"RTC IRQ unavailable; using 1s polling: %s",esp_err_to_name(ret));
    ESP_LOGI(TAG,"PCF85063TP: I2C1 SDA48/SCL47 address=0x51 INT11 (not a wake source)");
    return ESP_OK;
}
void rtc_clock_deinit(void)
{
    if(s_irq_installed) gpio_isr_handler_remove(RTC_IRQ_GPIO);
    if(s_device) i2c_master_bus_rm_device(s_device);
    s_device=NULL;s_irq_installed=false;
}
void rtc_clock_service(void)
{
    if(!s_device) return;
    TickType_t now=xTaskGetTickCount();
    rtc_datetime_t requested={0},actual={0};
    uint32_t id=0;
    portENTER_CRITICAL(&s_snapshot_lock);
    if(s_queued) { requested=s_requested;id=s_request_id;s_queued=false; }
    portEXIT_CRITICAL(&s_snapshot_lock);
    if(!s_configured && (!s_config_once || id ||
        (TickType_t)(now-s_last_config)>=pdMS_TO_TICKS(5000))) {
        s_config_once=true;s_last_config=now;
        s_configured=configure()==ESP_OK;
    }
    if(!id && s_read_once && (TickType_t)(now-s_last_read)<pdMS_TO_TICKS(1000)) return;
    s_read_once=true;s_last_read=now;
    bool valid=false;
    esp_err_t ret;
    if(id) ret=set_clock(&requested,&actual,&valid);
    else ret=read_clock(&actual,&valid);
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.online=ret==ESP_OK;
    /* A possibly partial failed set cannot silently become trusted on the
     * next poll; require another explicitly verified set for this boot. */
    s_snapshot.valid=ret==ESP_OK && valid && !s_write_uncertain;
    if(s_snapshot.valid) s_snapshot.time=actual;
    if(id) {
        s_snapshot.set_result=ret;s_snapshot.completed_id=id;
        s_snapshot.set_pending=false;
    }
    portEXIT_CRITICAL(&s_snapshot_lock);
    if(ret!=ESP_OK && !s_fault_logged)
        ESP_LOGW(TAG,"RTC access failed: %s (time shown as unknown)",esp_err_to_name(ret));
    if(ret==ESP_OK && id) ESP_LOGI(TAG,"RTC time saved and verified: %04u-%02u-%02u %02u:%02u:%02u weekday=%u",
        actual.year,actual.month,actual.day,actual.hour,actual.minute,actual.second,actual.weekday);
    s_fault_logged=ret!=ESP_OK;
}
