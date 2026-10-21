/* Production adapter + unmodified Bosch SensorAPI against a paged register
 * model. Covers actual config upload, feature bits, die temperature, daily
 * accounting, confirmed IRQs and the SMP pause gate. No physical hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/motion_sensor.c"

static uint8_t registers[256], pages[8][16], uploaded[8192];
static unsigned uploaded_bytes, upload_chunks, transaction_count, notifications, wake_requests;
static unsigned steps_publications;
static TickType_t ticks;
static bool locked, in_isr, dark, powering_off, fail_i2c, drop_init;
static bool task_failure, irq_failure;
static int failed_reg=-1;
static bool displayed_steps_valid, displayed_temp_valid;
static uint32_t displayed_steps;
static int16_t displayed_temperature;
static rtc_clock_snapshot_t clock_state;
static void (*gpio_handler)(void *);

void vTaskDelay(unsigned delay) { assert(!locked && !in_isr); ticks+=delay; }
void esp_rom_delay_us(uint32_t us) { assert(us<1000 && !locked && !in_isr); }
TickType_t xTaskGetTickCount(void) { return ticks; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task==(void *)2 && !in_isr); ++notifications; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{ assert(in_isr && task==(void *)2); ++notifications; *woken=pdTRUE; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; return 0; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *out)
{
    assert(fn==sensor_worker && !strcmp(name,"motion_sensor") && stack==6144 && !arg && priority==2);
    if(task_failure) return 0;
    *out=(void *)2; return pdPASS;
}
esp_err_t i2c_master_get_bus_handle(int port,i2c_master_bus_handle_t *bus)
{ assert(port==I2C_NUM_1); *bus=(void *)1; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,const i2c_device_config_t *cfg,i2c_master_dev_handle_t *dev)
{ assert(bus==(void *)1 && cfg->device_address==0x68 && cfg->scl_speed_hz==100000); *dev=(void *)3; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{ assert(dev==(void *)3); return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *cfg)
{ assert(cfg->pin_bit_mask==(1ULL<<12) && cfg->intr_type==GPIO_INTR_NEGEDGE && cfg->pull_up_en); return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void (*fn)(void *),void *arg)
{ assert(pin==12 && fn==imu_irq && !arg); if(irq_failure) return ESP_FAIL; gpio_handler=fn; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ assert(pin==12); gpio_handler=NULL; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin==12); return registers[BMI2_INT_STATUS_0_ADDR]?0:1; }
bool screen_power_is_dark(void) { return dark; }
bool system_power_is_active(void) { return powering_off; }
void screen_power_request_wake(void) { assert(!locked && !in_isr); ++wake_requests; }
void rtc_clock_get_snapshot(rtc_clock_snapshot_t *out) { *out=clock_state; }
bool lvgl_port_lock(uint32_t wait) { (void)wait; assert(!locked && !in_isr); locked=true; return true; }
void lvgl_port_unlock(void) { assert(locked); locked=false; }
void ui_lockscreen_set_steps(bool valid,uint32_t steps)
{ assert(locked); displayed_steps_valid=valid; displayed_steps=steps; ++steps_publications; }
void ui_lockscreen_set_temperature(bool valid,int16_t temperature)
{ assert(locked); displayed_temp_valid=valid; displayed_temperature=temperature; }
static void reset_registers(void)
{
    memset(registers,0,sizeof(registers)); memset(pages,0,sizeof(pages));
    registers[BMI2_CHIP_ID_ADDR]=BMI270_CHIP_ID;
    registers[BMI2_PWR_CONF_ADDR]=1;
    registers[BMI2_TEMPERATURE_1_ADDR]=0x80;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *bytes,size_t len,int timeout)
{
    assert(dev==(void *)3 && !locked && !in_isr && len>=2 && len<=65 && timeout==100);
    assert(atomic_load(&s_gate)&WORK_BUSY);
    ++transaction_count;
    uint8_t reg=bytes[0];
    if(fail_i2c || reg==failed_reg) return ESP_FAIL;
    if(reg==BMI2_CMD_REG_ADDR && bytes[1]==BMI2_SOFT_RESET_CMD) { reset_registers(); uploaded_bytes=0; return ESP_OK; }
    if(reg==BMI2_INIT_DATA_ADDR) {
        unsigned address=((unsigned)registers[BMI2_INIT_ADDR_1]<<4 | (registers[BMI2_INIT_ADDR_0]&15))*2;
        assert(address+len-1<=sizeof(uploaded));
        memcpy(uploaded+address,bytes+1,len-1); uploaded_bytes+=len-1; ++upload_chunks;
        return ESP_OK;
    }
    for(size_t n=1;n<len;n++) {
        uint8_t addr=reg+n-1;
        if(addr>=BMI2_FEATURES_REG_ADDR && addr<BMI2_FEATURES_REG_ADDR+16)
            pages[registers[BMI2_FEAT_PAGE_ADDR]&7][addr-BMI2_FEATURES_REG_ADDR]=bytes[n];
        else registers[addr]=bytes[n];
    }
    if(reg==BMI2_INIT_CTRL_ADDR && bytes[1]==1 && !drop_init)
        registers[BMI2_INTERNAL_STATUS_ADDR]=1;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const uint8_t *bytes,size_t nw,uint8_t *out,size_t len,int timeout)
{
    assert(dev==(void *)3 && nw==1 && len<=64 && timeout==100 && !locked && !in_isr);
    assert(atomic_load(&s_gate)&WORK_BUSY);
    ++transaction_count;
    if(fail_i2c || bytes[0]==failed_reg) return ESP_FAIL;
    for(size_t n=0;n<len;n++) {
        uint8_t addr=bytes[0]+n;
        out[n]=(addr>=BMI2_FEATURES_REG_ADDR && addr<BMI2_FEATURES_REG_ADDR+16)?
            pages[registers[BMI2_FEAT_PAGE_ADDR]&7][addr-BMI2_FEATURES_REG_ADDR]:registers[addr];
        if(addr==BMI2_INT_STATUS_0_ADDR || addr==BMI2_INT_STATUS_1_ADDR) registers[addr]=0;
    }
    return ESP_OK;
}
static void raw_count(uint32_t n) { for(unsigned i=0;i<4;i++) pages[0][i]=n>>(i*8); }
static void raw_temperature(int16_t n) { registers[0x22]=(uint16_t)n; registers[0x23]=(uint16_t)n>>8; }
static void poll(void) { ticks+=1000; sensor_step(); }
static void wrist_irq(unsigned status)
{
    registers[BMI2_INT_STATUS_0_ADDR]=status;
    unsigned transactions=transaction_count;
    in_isr=true; gpio_handler(NULL); in_isr=false;
    assert(transaction_count==transactions);
    sensor_step();
}
int main(void)
{
    reset_registers();
    irq_failure=true; assert(motion_sensor_init()==ESP_FAIL && !s_device && !s_worker);
    irq_failure=false; task_failure=true; assert(motion_sensor_init()==ESP_ERR_NO_MEM && !s_device && !gpio_handler);
    task_failure=false; assert(motion_sensor_init()==ESP_OK);
    /* Wrong identity never uploads; failed load never enables wake. */
    registers[0]=0; sensor_step(); assert(!s_configured && !uploaded_bytes && !displayed_temp_valid);
    registers[0]=BMI270_CHIP_ID; drop_init=true; ticks+=5000; sensor_step();
    assert(!s_configured && !motion_sensor_wake_enabled());
    drop_init=false; ticks+=5000; sensor_step();
    assert(s_configured && uploaded_bytes==8192 && upload_chunks>=128);
    assert(!memcmp(uploaded,s_bmi.config_file_ptr,8192));
    assert(registers[BMI2_PWR_CTRL_ADDR]==8 && (registers[BMI2_PWR_CONF_ADDR]&1));
    assert((registers[BMI2_ACC_CONF_ADDR]&15)==BMI2_ACC_ODR_50HZ);
    assert(registers[BMI2_INT1_IO_CTRL_ADDR]==0x0c && registers[BMI2_INT_LATCH_ADDR]==1);
    assert(!displayed_temp_valid && !displayed_steps_valid);
    puts("PASS real Bosch configuration upload, wrong-ID/load failures and low-active latched pin");

    motion_sensor_set_features(true,true); sensor_step();
    assert(s_applied==3 && motion_sensor_wake_enabled());
    assert(registers[BMI2_INT1_MAP_FEAT_ADDR]==BMI270_INT_WRIST_WEAR_WAKEUP_MASK);
    assert((registers[BMI2_PWR_CTRL_ADDR]&0x0e)==0x0c); /* accel+temp, not gyro */
    clock_state=(rtc_clock_snapshot_t){.valid=true,.online=true,.time={.year=2026,.month=10,.day=21}};
    raw_temperature(512); raw_count(23); poll();
    assert(displayed_steps_valid && displayed_steps==23 && displayed_temperature==240 && displayed_temp_valid);
    raw_temperature(-512*24); poll(); assert(displayed_temperature==-10);
    raw_temperature(INT16_MIN); poll(); assert(!displayed_temp_valid);
    puts("PASS actual feature enable, independent die temperature and invalid-temperature sentinel");

    clock_state.time.day=22; raw_count(30); poll(); assert(displayed_steps==0 && s_day==20261022);
    raw_count(35); poll(); assert(displayed_steps==5);
    clock_state.valid=false; raw_count(38); poll(); assert(!displayed_steps_valid && s_daily==8);
    clock_state.valid=true; raw_count(40); poll(); assert(displayed_steps_valid && displayed_steps==10);
    motion_sensor_set_features(false,true); sensor_step(); assert(!displayed_steps_valid);
    clock_state.time.day=23; poll(); assert(s_daily==0);
    motion_sensor_set_features(true,true); sensor_step(); raw_count(42); poll(); assert(displayed_steps==2);
    s_last_raw=UINT32_MAX-1; raw_count(2); poll(); assert(displayed_steps==6);
    puts("PASS midnight and disabled midnight reset, invalid RTC recovery and counter rollover");

    dark=true; wrist_irq(0x01); assert(!wake_requests);
    wrist_irq(BMI270_WRIST_WAKE_UP_STATUS_MASK); assert(wake_requests==1);
    dark=false; wrist_irq(BMI270_WRIST_WAKE_UP_STATUS_MASK); assert(wake_requests==1);
    dark=true; powering_off=true; wrist_irq(BMI270_WRIST_WAKE_UP_STATUS_MASK); assert(wake_requests==1);
    powering_off=false; motion_sensor_set_features(true,false); sensor_step();
    wrist_irq(BMI270_WRIST_WAKE_UP_STATUS_MASK); assert(wake_requests==1 && !motion_sensor_wake_enabled());
    puts("PASS wrist status confirmation, no wake from unrelated/disabled/awake/shutdown IRQs");

    unsigned tx=transaction_count;
    assert(motion_sensor_sleep(true)); sensor_step(); assert(transaction_count==tx);
    assert(motion_sensor_shutdown(true)); motion_sensor_sleep(false); sensor_step(); assert(transaction_count==tx);
    motion_sensor_shutdown(false);
    atomic_store(&s_gate,WORK_BUSY); assert(!motion_sensor_sleep(true));
    atomic_fetch_and(&s_gate,~WORK_BUSY); assert(motion_sensor_sleep(true)); motion_sensor_sleep(false);
    fail_i2c=true; poll(); assert(!displayed_steps_valid && !displayed_temp_valid && !motion_sensor_wake_enabled());
    tx=transaction_count;
    for(unsigned i=0;i<5;i++) { motion_sensor_notify_wake(); sensor_step(); }
    assert(transaction_count==tx); /* stuck IRQ cannot flood the shared bus */
    fail_i2c=false; poll(); assert(displayed_steps_valid);
    motion_sensor_set_features(true,true); sensor_step(); assert(motion_sensor_wake_enabled());
    fail_i2c=true; poll(); assert(!motion_sensor_wake_enabled());
    fail_i2c=false; poll(); assert(motion_sensor_wake_enabled());
    puts("PASS independent sleep/shutdown BUSY barriers and I2C failure/recovery");
    return 0;
}

