#pragma once
#include "adc_cali.h"
#include "adc_oneshot.h"
typedef struct { adc_unit_t unit_id; adc_channel_t chan; int atten,bitwidth; } adc_cali_curve_fitting_config_t;
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *,adc_cali_handle_t *);
