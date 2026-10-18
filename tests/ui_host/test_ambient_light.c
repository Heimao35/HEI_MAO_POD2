/* Actual production sampler/filter with ADC failures/noise/time wrap injected. */
#include <assert.h>
#include <stdio.h>
#include "../../main/ambient_light.c"
static TickType_t ticks;
static unsigned reads, deleted;
static int voltage;
static bool read_fail, calibration_fail, convert_fail;
TickType_t xTaskGetTickCount(void) { return ticks; }
esp_err_t adc_oneshot_io_to_channel(int gpio,adc_unit_t *u,adc_channel_t *c)
{ assert(gpio==22); *u=1; *c=6; return ESP_OK; }
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,adc_oneshot_unit_handle_t *h)
{ assert(cfg->unit_id==1 && cfg->ulp_mode==0); *h=(void *)1; return ESP_OK; }
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h,adc_channel_t c,const adc_oneshot_chan_cfg_t *cfg)
{ assert(h==(void *)1 && c==6 && cfg->atten==12 && cfg->bitwidth==12); return ESP_OK; }
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h)
{ assert(h==(void *)1); ++deleted; return ESP_OK; }
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,adc_cali_handle_t *h)
{ assert(cfg->unit_id==1 && cfg->chan==6 && cfg->atten==12); if(calibration_fail)return ESP_ERR_NOT_SUPPORTED; *h=(void *)2; return ESP_OK; }
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h,adc_channel_t c,int *v)
{
    assert(h==(void *)1 && c==6); ++reads;
    if (read_fail) return ESP_ERR_TIMEOUT;
    *v=voltage;
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h,int raw,int *mv)
{ assert(h==(void *)2); if(convert_fail)return ESP_FAIL; *mv=raw; return ESP_OK; }
static uint8_t advance(uint8_t brightness,unsigned ms)
{
    ticks+=ms;
    uint8_t target=brightness;
    if(ambient_light_poll(brightness,&target)) {
        assert(target>=5 && target<=100);
        return target;
    }
    return brightness;
}
int main(void)
{
    uint8_t b=80, target;
    assert(!ambient_light_poll(b,&b));
    calibration_fail=true; assert(ambient_light_init()==ESP_ERR_NOT_SUPPORTED && !s_adc && deleted==1);
    calibration_fail=false; assert(ambient_light_init()==ESP_OK);
    assert(ambient_light_init()==ESP_ERR_INVALID_STATE);
    for(int i=0;i<10;++i) {
        assert(voltage_to_brightness(i*LEVEL_MV_STEP)==(i+1)*10);
        assert(voltage_to_brightness(i*LEVEL_MV_STEP+LEVEL_MV_STEP-1)==(i+1)*10);
    }
    assert(voltage_to_brightness(3300)==100);
    voltage=LEVEL_MV_STEP;assert(!ambient_light_poll(80,&target) && reads==1 && ambient_light_is_transitioning());
    b=advance(b,50);assert(b==68 && reads==1);
    b=advance(b,50);assert(b==56 && reads==2); /* Same target must not restart ramp. */
    b=advance(b,50);assert(b==44 && reads==2);
    b=advance(b,50);assert(b==32 && reads==3);
    b=advance(b,50);assert(b==20 && !ambient_light_is_transitioning());
    assert(!ambient_light_poll(b,&target));
    puts("PASS ten uniform voltage bands, single ADC read/100ms and exact250ms linear transition");

    ambient_light_reset();ticks=1000;voltage=1350;
    assert(!ambient_light_poll(b,&target));
    b=advance(b,50);assert(b==36);
    voltage=0;b=advance(b,50);assert(b==36 && s_target==10); /* New target immediately reverses. */
    b=advance(b,50);assert(b==31);
    b=advance(b,50);assert(b==26);
    b=advance(b,50);assert(b==21);
    b=advance(b,50);assert(b==16);
    b=advance(b,50);assert(b==10 && !ambient_light_is_transitioning());
    puts("PASS rapid light reversal retargets immediately from current output, no delayed confirmation");

    ambient_light_reset();ticks=2000;b=80;voltage=0;
    assert(!ambient_light_poll(b,&target));
    b=advance(b,50);assert(b==66);
    read_fail=true;b=advance(b,50);assert(b==66 && !ambient_light_is_transitioning());
    b=advance(b,100);assert(b==66);
    read_fail=false;convert_fail=true;b=advance(b,100);assert(b==66);
    convert_fail=false;voltage=1350;b=advance(b,100);assert(b==66 && ambient_light_is_transitioning());
    b=advance(b,250);assert(b==100);
    puts("PASS ADC/calibration failures hold actual brightness and cancel stale ramps; recovery restarts");

    ambient_light_reset();ticks=UINT32_MAX-100;b=80;voltage=0;
    assert(!ambient_light_poll(b,&target));
    b=advance(b,125);assert(b==45);
    ticks+=125;assert(ambient_light_poll(b,&target) && target==10 && !ambient_light_is_transitioning());
    /* Simulate failed LVGL publication at the final step: result must be retried. */
    ticks+=20;assert(ambient_light_poll(b,&target) && target==10);
    b=target;assert(!ambient_light_poll(b,&target));
    ambient_light_reset();assert(!ambient_light_is_transitioning());
    assert(!ambient_light_poll(70,&target) && s_from==70); /* Fresh mode starts from actual brightness. */
    ambient_light_reset();b=5;voltage=0;
    assert(!ambient_light_poll(b,&target));
    b=advance(b,125);assert(b==7);
    b=advance(b,125);assert(b==10);
    puts("PASS tick rollover, final UI retry, history reset and manual5% to automatic10% linear rise");
    return 0;
}
