#include "battery_manager.h"
#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"

#define BATTERY_PERIOD_US 5000000LL
#define BATTERY_SAMPLES 16
/* 100k / 100k: actual battery mV = ADC mV * 2. Adjust after DMM check. */
#define BATTERY_DIVIDER_NUM 2
#define BATTERY_DIVIDER_DEN 1
#define BATTERY_OFFSET_MV 0

static const char *TAG = "NC_BAT";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cal;
static adc_channel_t s_channel;
static bool s_ready;
static bool s_filter_known;
static int s_filtered_mv;
static int64_t s_next_us;

static uint8_t battery_percent(int mv)
{
    static const struct {int mv; uint8_t pct;} points[] = {
        {3300,0},{3500,5},{3600,10},{3700,20},{3800,40},
        {3900,60},{4000,80},{4100,90},{4200,100}
    };
    if (mv <= points[0].mv) return 0;
    for (unsigned i=1;i<sizeof(points)/sizeof(points[0]);++i) {
        if (mv <= points[i].mv) {
            int span=points[i].mv-points[i-1].mv;
            int delta=points[i].pct-points[i-1].pct;
            return (uint8_t)(points[i-1].pct +
                ((mv-points[i-1].mv)*delta + span/2)/span);
        }
    }
    return 100;
}

esp_err_t battery_manager_init(void)
{
    adc_unit_t unit;
    esp_err_t rc=adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO,&unit,&s_channel);
    if(rc!=ESP_OK || unit!=ADC_UNIT_1) return ESP_ERR_INVALID_ARG;
    adc_oneshot_unit_init_cfg_t cfg={.unit_id=unit,.ulp_mode=ADC_ULP_MODE_DISABLE};
    rc=adc_oneshot_new_unit(&cfg,&s_adc);
    if(rc!=ESP_OK) return rc;
    adc_oneshot_chan_cfg_t ch={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_12};
    rc=adc_oneshot_config_channel(s_adc,s_channel,&ch);
    if(rc!=ESP_OK) goto fail;
    adc_cali_curve_fitting_config_t cal={
        .unit_id=unit,.chan=s_channel,.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_12
    };
    rc=adc_cali_create_scheme_curve_fitting(&cal,&s_cal);
    if(rc!=ESP_OK) goto fail;
    s_ready=true;
    ESP_LOGI(TAG,"BAT|INIT|gpio=%d|divider=2:1",BOARD_BATTERY_ADC_GPIO);
    return ESP_OK;
fail:
    adc_oneshot_del_unit(s_adc);
    s_adc=NULL;
    ESP_LOGW(TAG,"BAT|INIT_FAILED|%s",esp_err_to_name(rc));
    return rc;
}

bool battery_manager_poll(bool *known,uint8_t *percent)
{
    if(!known || !percent || !s_ready) return false;
    int64_t now=esp_timer_get_time();
    if(now<s_next_us) return false;
    s_next_us=now+BATTERY_PERIOD_US;
    int sum=0,n=0;
    for(int i=0;i<BATTERY_SAMPLES;++i) {
        int raw,mv;
        if(adc_oneshot_read(s_adc,s_channel,&raw)==ESP_OK &&
           adc_cali_raw_to_voltage(s_cal,raw,&mv)==ESP_OK) {
            sum+=mv;
            ++n;
        }
    }
    int vb=n ? ((sum+n/2)/n)*BATTERY_DIVIDER_NUM/BATTERY_DIVIDER_DEN+BATTERY_OFFSET_MV : 0;
    /* Missing battery/wiring or unsupported cell chemistry -> unknown.
       No automatic power off; cell protection belongs in the hardware. */
    if(n<BATTERY_SAMPLES/2 || vb<2500 || vb>4350) {
        s_filter_known=false;
        *known=false;
        *percent=0;
        ESP_LOGI(TAG,"BAT|UNKNOWN|mv=%d|samples=%d",vb,n);
        return true;
    }
    s_filtered_mv=s_filter_known ? (3*s_filtered_mv+vb+2)/4 : vb;
    s_filter_known=true;
    *known=true;
    *percent=battery_percent(s_filtered_mv);
    ESP_LOGI(TAG,"BAT|LEVEL|mv=%d|filtered=%d|pct=%u",vb,s_filtered_mv,(unsigned)*percent);
    return true;
}
