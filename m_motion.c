/*
  Copyright (c) 2010 - 2017, Nordic Semiconductor ASA
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form, except as embedded into a Nordic
     Semiconductor ASA integrated circuit in a product or a software update for
     such product, must reproduce the above copyright notice, this list of
     conditions and the following disclaimer in the documentation and/or other
     materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

  4. This software, with or without modification, must only be used with a
     Nordic Semiconductor ASA integrated circuit.

  5. Any software provided in binary form under this license must not be reverse
     engineered, decompiled, modified and/or disassembled.

  THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
  OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "m_motion.h"
#include "m_motion_flash.h"
#include "ble_tms.h"
#include "drv_motion.h"
#include "sdk_errors.h"
#include "pca20020.h"
#define  NRF_LOG_MODULE_NAME "m_motion      "
#include "nrf_log.h"
#include "macros_common.h"
#include "math.h"
#include "m_ui.h"


#define RAW_PARAM_NUM                 9     // Number of raw parameters (3 * acc + 3 * gyro + 3 * compass).
#define RAW_Q_FORMAT_ACC_INTEGER_BITS 6     // Number of bits used for integer part of raw data.
#define RAW_Q_FORMAT_GYR_INTEGER_BITS 11    // Number of bits used for integer part of raw data.
#define RAW_Q_FORMAT_CMP_INTEGER_BITS 12    // Number of bits used for integer part of raw data.

static ble_tms_t              m_tms;
static ble_tms_config_t     * m_config;
static const ble_tms_config_t m_default_config = MOTION_DEFAULT_CONFIG;

//*****************Your Values from Tutorial 1 here**********************

#define ACCEL_BUF_SIZE 50
#define ACCEL_HYST 0.5F

double mag_buffer[ACCEL_BUF_SIZE];

//Used for EWMA low pass filter
double ewma_alpha = 1.0;
//***********************************************************************


double prev_ewma = 0;

uint8_t acc_index = 0;

bool sign_state_pos = false;

uint32_t step_count = 0;

bool new_step = false;

/**@brief Function for applying the configuration.
 */
static uint32_t m_motion_configuration_apply(ble_tms_config_t * p_config)
{
    uint32_t err_code;
    drv_motion_cfg_t motion_cfg;

    NULL_PARAM_CHECK(p_config);

    motion_cfg.pedo_interval_ms    = p_config->pedo_interval_ms;
    motion_cfg.temp_interval_ms    = p_config->temp_interval_ms;
    motion_cfg.compass_interval_ms = p_config->compass_interval_ms;
    motion_cfg.motion_freq_hz      = p_config->motion_freq_hz;
    motion_cfg.wake_on_motion      = p_config->wake_on_motion;

    err_code = drv_motion_config(&motion_cfg);
    APP_ERROR_CHECK(err_code);

    return NRF_SUCCESS;
}


static void ble_tms_evt_handler(ble_tms_t        * p_tms,
                                ble_tms_evt_type_t evt_type,
                                uint8_t          * p_data,
                                uint16_t           length)
{
    uint32_t err_code;
    
    switch (evt_type)
    {
        case BLE_TMS_EVT_NOTIF_TAP:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_TAP - %d\r\n", p_tms->is_tap_notif_enabled);
            if (p_tms->is_tap_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_TAP);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_TAP);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_ORIENTATION:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_ORIENTATION - %d\r\n", p_tms->is_orientation_notif_enabled);
            if (p_tms->is_orientation_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_ORIENTATION);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_ORIENTATION);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_QUAT:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_QUAT - %d\r\n", p_tms->is_quat_notif_enabled);
            if (p_tms->is_quat_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_QUAT);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_QUAT);
                APP_ERROR_CHECK(err_code);
            }
            break;

        
        //Instead of using the MPU's built in Pedometer, we are going to utilize the code from Tutorial 1
        //This means, we will enable the raw accelerometer instead.
        case BLE_TMS_EVT_NOTIF_PEDOMETER:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_PEDOMETER - %d\r\n", p_tms->is_pedo_notif_enabled);
            if (p_tms->is_pedo_notif_enabled)
            {
                //err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_PEDOMETER);
                //APP_ERROR_CHECK(err_code);

                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_RAW);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_PEDOMETER);
                APP_ERROR_CHECK(err_code);

                //err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_RAW);
                //APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_RAW:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_RAW - %d\r\n", p_tms->is_raw_notif_enabled);
            if (p_tms->is_raw_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_RAW);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_RAW);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_EULER:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_EULER - %d\r\n", p_tms->is_euler_notif_enabled);
            if (p_tms->is_euler_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_EULER);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_EULER);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_ROT_MAT:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_ROT_MAT - %d\r\n", p_tms->is_rot_mat_notif_enabled);
            if (p_tms->is_rot_mat_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_ROT_MAT);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_ROT_MAT);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_HEADING:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_HEADING - %d\r\n", p_tms->is_heading_notif_enabled);
            if (p_tms->is_heading_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_HEADING);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_HEADING);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_NOTIF_GRAVITY:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_NOTIF_GRAVITY - %d\r\n", p_tms->is_gravity_notif_enabled);
            if (p_tms->is_gravity_notif_enabled)
            {
                err_code = drv_motion_enable(DRV_MOTION_FEATURE_MASK_GRAVITY_VECTOR);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK_GRAVITY_VECTOR);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_TMS_EVT_CONFIG_RECEIVED:
            NRF_LOG_INFO("ble_tms_evt_handler: BLE_TMS_EVT_CONFIG_RECEIVED - %d\r\n", length);
            APP_ERROR_CHECK_BOOL(length == sizeof(ble_tms_config_t));

            err_code = m_motion_flash_config_store((ble_tms_config_t *)p_data);
            APP_ERROR_CHECK(err_code);

            err_code = m_motion_configuration_apply((ble_tms_config_t *)p_data);
            APP_ERROR_CHECK(err_code);
            break;

        default:
            break;
    }

}


/**@brief Function for initializing the Thingy Motion Service.
 *
 * @details This callback function will be called from the ble handling module to initialize the Weather Station service.
 *
 * @retval NRF_SUCCESS If initialization was successful.
 */
static uint32_t motion_service_init(bool major_minor_fw_ver_changed)
{
    uint32_t              err_code;
    ble_tms_init_t        tms_init;

    /**@brief Load configuration from flash. */
    err_code = m_motion_flash_init(&m_default_config, &m_config);
    RETURN_IF_ERROR(err_code);
    
    if (major_minor_fw_ver_changed)
    {
        err_code = m_motion_flash_config_store(&m_default_config);
        APP_ERROR_CHECK(err_code);
    }

    err_code = m_motion_configuration_apply(m_config);
    RETURN_IF_ERROR(err_code);

    memset(&tms_init, 0, sizeof(tms_init));

    tms_init.p_init_config = m_config;
    tms_init.evt_handler = ble_tms_evt_handler;

    NRF_LOG_INFO("motion_service_init: ble_tms_init \r\n");
    err_code = ble_tms_init(&m_tms, &tms_init);
    if (err_code != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("FAILED - %d\r\n", err_code);
        return err_code;
    }
    
    return NRF_SUCCESS;
}


/**@brief Function for passing the BLE event to the Thingy Motion Service.
 *
 * @details This callback function will be called from the BLE handling module.
 *
 * @param[in] p_ble_evt    Pointer to the BLE event.
 */
static void motion_on_ble_evt(ble_evt_t * p_ble_evt)
{
    uint32_t err_code;
    ble_tms_on_ble_evt(&m_tms, p_ble_evt);

    if (p_ble_evt->header.evt_id == BLE_GAP_EVT_DISCONNECTED)
    {
        err_code = drv_motion_disable(DRV_MOTION_FEATURE_MASK);
        APP_ERROR_CHECK(err_code);
    }
}


static void drv_motion_evt_handler(drv_motion_evt_t const * p_evt, void * p_data, uint32_t size)
{
    switch (*p_evt)
    {
        case DRV_MOTION_EVT_RAW:
        {
            //NRF_LOG_INFO("DRV_MOTION_EVT_RAW:\r\n");

            APP_ERROR_CHECK_BOOL(size == sizeof(int32_t) * RAW_PARAM_NUM);

            ble_tms_raw_t data;
            int32_t     * p_raw = (int32_t *)p_data;

            double x_buf;
            double y_buf;
            double z_buf;

            char buffer[24];

            //convert fixed point from MPU to floating point
            x_buf = (double)p_raw[0];
            x_buf = x_buf/(1<<16);
              
            y_buf = (double)p_raw[1];
            y_buf = y_buf/(1<<16);

            z_buf = (double)p_raw[2];
            z_buf = z_buf/(1<<16);

            //format as string for debugging purposes and print accelerometer values to segger RTT console channel 1
            // sprintf(buffer, "ACCEL_DATA %.2f,%.2f,%.2f", x_buf,y_buf,z_buf);
            // NRF_LOG_INFO(" %s\r\n", buffer);

            process_accel_data(x_buf,y_buf,z_buf);

            (void)ble_tms_raw_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_QUAT:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(int32_t) * 4);

            ble_tms_quat_t data;
            int32_t      * p_quat = (int32_t *)p_data;

            data.w = p_quat[0];
            data.x = p_quat[1];
            data.y = p_quat[2];
            data.z = p_quat[3];

            #if NRF_LOG_ENABLED
                static const uint8_t QUAT_ELEMENTS = 4;
                double f_buf;
                char buffer[QUAT_ELEMENTS][7];

                for (uint8_t i = 0; i < QUAT_ELEMENTS; i++)
                {
                    f_buf = (double)p_quat[i];
                    f_buf = f_buf/(1<<30);
                    sprintf(buffer[i], "% 1.3f", f_buf);
                }

                NRF_LOG_DEBUG("DRV_MOTION_EVT_QUAT: w:%s x:%s y:%s z:%s\r\n", nrf_log_push(buffer[0]),
                                                                              nrf_log_push(buffer[1]),
                                                                              nrf_log_push(buffer[2]),
                                                                              nrf_log_push(buffer[3]));
            #endif

            (void)ble_tms_quat_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_EULER:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(long) * 3);

            ble_tms_euler_t data;
            int32_t      * p_euler = (int32_t *)p_data;

            data.roll   = p_euler[0];
            data.pitch  = p_euler[1];
            data.yaw    = p_euler[2];

            NRF_LOG_DEBUG("DRV_MOTION_EVT_EULER, [deg]:  roll(x):%3d   pitch(y):%3d   yaw(z):%3d  \r\n", data.roll/(1<<16), data.pitch/(1<<16), data.yaw/(1<<16));

            (void)ble_tms_euler_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_ROT_MAT:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(int32_t) * 9);

            ble_tms_rot_mat_t data;
            int32_t         * p_matrix = (int32_t *)p_data;

            data.matrix[0] = (int16_t)(p_matrix[0] >> 16);
            data.matrix[1] = (int16_t)(p_matrix[1] >> 16);
            data.matrix[2] = (int16_t)(p_matrix[2] >> 16);
            data.matrix[3] = (int16_t)(p_matrix[3] >> 16);
            data.matrix[4] = (int16_t)(p_matrix[4] >> 16);
            data.matrix[5] = (int16_t)(p_matrix[5] >> 16);
            data.matrix[6] = (int16_t)(p_matrix[6] >> 16);
            data.matrix[7] = (int16_t)(p_matrix[7] >> 16);
            data.matrix[8] = (int16_t)(p_matrix[8] >> 16);

            #if NRF_LOG_ENABLED
                static const uint8_t ROT_MAT_ELEMENTS = 9;
                char buffer[ROT_MAT_ELEMENTS][6];
                double tmp;
                for(uint8_t i = 0; i<ROT_MAT_ELEMENTS; i++)
                {
                    tmp = p_matrix[i]/(double)(1<<30);
                    sprintf(buffer[i], "% 1.2f", tmp);
                }
                
                NRF_LOG_DEBUG("DRV_MOTION_EVT_ROT_MAT:\r\n");
                NRF_LOG_DEBUG("[%s %s %s]\r\n", nrf_log_push(buffer[0]), nrf_log_push(buffer[1]), nrf_log_push(buffer[2]));
                NRF_LOG_DEBUG("[%s %s %s]\r\n", nrf_log_push(buffer[3]), nrf_log_push(buffer[4]), nrf_log_push(buffer[5]));
                NRF_LOG_DEBUG("[%s %s %s]\r\n", nrf_log_push(buffer[6]), nrf_log_push(buffer[7]), nrf_log_push(buffer[8]));
            #endif

            (void)ble_tms_rot_mat_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_HEADING:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(long));
            ble_tms_heading_t heading = *(ble_tms_heading_t *)p_data;

            NRF_LOG_DEBUG("DRV_MOTION_EVT_HEADING [deg]:  h: %d\r\n", heading/(1<<16));

            (void)ble_tms_heading_set(&m_tms, &heading);
        }
        break;

        case DRV_MOTION_EVT_GRAVITY:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(float) * 3);

            ble_tms_gravity_t data;
            float           * p_gravity = (float *)p_data;

            data.x = p_gravity[0];
            data.y = p_gravity[1];
            data.z = p_gravity[2];

            #if NRF_LOG_ENABLED
                static const uint8_t GRAVITY_ELEMENTS = 3;
                char buffer[GRAVITY_ELEMENTS][8];

                for (uint8_t i = 0; i<GRAVITY_ELEMENTS; i++)
                {
                    sprintf(buffer[i], "% 2.3f", p_gravity[i]);
                }

                NRF_LOG_DEBUG("DRV_MOTION_EVT_GRAVITY [m/s^2]:  [%s, %s, %s]\r\n", nrf_log_push(buffer[0]),
                                                                                   nrf_log_push(buffer[1]),
                                                                                   nrf_log_push(buffer[2]));
            #endif

            (void)ble_tms_gravity_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_TAP:
        {
            APP_ERROR_CHECK_BOOL(size == 2);

            ble_tms_tap_t data;
            uint8_t * p_tap = (uint8_t *)p_data;

            data.dir = p_tap[0];
            data.cnt = p_tap[1];

            NRF_LOG_DEBUG("DRV_MOTION_EVT_TAP: [%d %d]\r\n", data.cnt,
                                                             data.dir);

            (void)ble_tms_tap_set(&m_tms, &data);
        }
        break;

        case DRV_MOTION_EVT_ORIENTATION:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(uint8_t));

            NRF_LOG_DEBUG("DRV_MOTION_EVT_ORIENTATION: %d\r\n", *(ble_tms_orientation_t *)p_data);

            (void)ble_tms_orientation_set(&m_tms, (ble_tms_orientation_t *)p_data);
        }
        break;

        case DRV_MOTION_EVT_PEDOMETER:
        {
            APP_ERROR_CHECK_BOOL(size == sizeof(unsigned long) * 2);

            ble_tms_pedo_t  data;
            unsigned long * p_pedo = (unsigned long *)p_data;

            data.steps   = p_pedo[0];
            data.time_ms = p_pedo[1];

            NRF_LOG_DEBUG("DRV_MOTION_EVT_PEDOMETER: %d steps %d ms\r\n", p_pedo[0],
                                                                          p_pedo[1]);

            (void)ble_tms_pedo_set(&m_tms, &data);
        }
        break;

        default:
            NRF_LOG_WARNING("drv_motion_evt_handler: Unknown data!\r\n");
            break;
    }
}


uint32_t m_motion_sleep_prepare(bool wakeup)
{
    return drv_motion_sleep_prepare(wakeup);
}


uint32_t m_motion_init(m_ble_service_handle_t * p_handle, m_motion_init_t * p_params)
{
    uint32_t err_code;
    drv_motion_twi_init_t motion_params_mpu9250;
    drv_motion_twi_init_t motion_params_lis2dh12;

    static const nrf_drv_twi_config_t twi_config_mpu9250 =
    {
        .scl                = TWI_SCL,
        .sda                = TWI_SDA,
        .frequency          = NRF_TWI_FREQ_400K,
        .interrupt_priority = APP_IRQ_PRIORITY_LOW
    };

    static const nrf_drv_twi_config_t twi_config_lis2dh12 =
    {
        #if defined(THINGY_HW_v0_7_0)
            .scl = TWI_SCL,
            .sda = TWI_SDA,
        #elif  defined(THINGY_HW_v0_8_0)
            .scl = TWI_SCL,
            .sda = TWI_SDA,
        #elif  defined(THINGY_HW_v0_9_0)
            .scl = TWI_SCL,
            .sda = TWI_SDA,
        #else
            .scl = TWI_SCL_EXT,
            .sda = TWI_SDA_EXT,
        #endif
        .frequency          = NRF_TWI_FREQ_400K,
        .interrupt_priority = APP_IRQ_PRIORITY_LOW
    };

    NULL_PARAM_CHECK(p_handle);
    NULL_PARAM_CHECK(p_params);

    NRF_LOG_INFO("Init \r\n");

    p_handle->ble_evt_cb = motion_on_ble_evt;
    p_handle->init_cb    = motion_service_init;

    motion_params_mpu9250.p_twi_instance = p_params->p_twi_instance;
    motion_params_mpu9250.p_twi_cfg      = &twi_config_mpu9250;

    motion_params_lis2dh12.p_twi_instance = p_params->p_twi_instance;
    motion_params_lis2dh12.p_twi_cfg      = &twi_config_lis2dh12;

    err_code = drv_motion_init(drv_motion_evt_handler, &motion_params_mpu9250, &motion_params_lis2dh12);
    RETURN_IF_ERROR(err_code);

    return NRF_SUCCESS;
}

uint32_t process_accel_data(double x, double y, double z)
{
    double mean_acc = 0;
    double normalized_accel = 0;
    //int drink_buffer = 50;
    //double z_sum = 0;
    //int z_avg = 0;
    //int prev_z = 0;

    char buffer[24];

    //NRF_LOG_INFO(" sample_index = %i \r\n", acc_index);

    //do the signal processing work to compute the steps

    //remove axes
    mag_buffer[acc_index] = sqrt((x*x) + (y*y) + (z*z));

    //compute the mean acceleration across the window
    for(int i = 0; i < ACCEL_BUF_SIZE; i++)
      mean_acc = mean_acc + mag_buffer[i];

    mean_acc = mean_acc / ACCEL_BUF_SIZE;

    //subtract mean from index zero -- this removes "DC" offset
    normalized_accel = mag_buffer[acc_index] - mean_acc;

    //print out this normalized acceleration for debug
    sprintf(buffer, "NORM_DATA %.2f", normalized_accel);
    //NRF_LOG_INFO(" %s\r\n", buffer);

    //Filter noise with EWMA filter
    normalized_accel = normalized_accel*ewma_alpha + prev_ewma*(1 - ewma_alpha);
    sprintf(buffer, "EWMA_DATA %.2f", normalized_accel);
    //NRF_LOG_INFO(" %s\r\n", buffer);

    prev_ewma = normalized_accel;
    /*
    sprintf(buffer, "X %.2f", x);
    NRF_LOG_INFO(" %s\r\n", buffer);
    sprintf(buffer, "Y %.2f", y);
    NRF_LOG_INFO(" %s\r\n", buffer);
    sprintf(buffer, "Z %.2f", z);
    NRF_LOG_INFO(" %s\r\n", buffer);
    
    //Usually either negative values and very small (~1, maybe 2)
    //need to make absolute values, increase by 100 so its visible
    
    x = 100*abs(x);
    y = 100*abs(y);
    z = 100*abs(z);
    m_ui_led_constant_set(x,y,z);
    */

    // Have led respond to orientation (assumes x,y, or z never equal)
    
    if ((x > y) && (x > z))
    {
        m_ui_led_constant_set(100,0,0);
    }
    if ((y > x) && (y > z))
    {
        m_ui_led_constant_set(0,100,0);
    }
    if ((z > y) && (z > x))
    {
        m_ui_led_constant_set(0,0,100);
    }

    //for(int i = 0; i < drink_buffer; i++)
    //  z_avg = z_avg + z;

    //now, check for a zero crossing (negative to positive) and update history
    if(normalized_accel > ACCEL_HYST)
    {
      //if previous state was a negative sign below hysteresis window, step detected
      if(sign_state_pos == false)
      {
        sprintf(buffer, "STEP_DETECTED %i\r\n", step_count);
        NRF_LOG_INFO(" %s\r\n", buffer);
        step_count++;
        new_step = true;

        ble_tms_pedo_t  data;

        data.steps   = step_count;
        data.time_ms = 0;

        NRF_LOG_INFO("Transmitting step");

        (void)ble_tms_pedo_set(&m_tms, &data);

        //even, LED is on. Odd, turn it off. Useful for debugging when not tethered to the debugger.
        //if((step_count % 2) == 0)
        //    m_ui_led_constant_set(100,0,0);
        //else
        //    m_ui_led_constant_set(0,0,0);
      }
        sign_state_pos = true;
    }
    else if(normalized_accel < (0-ACCEL_HYST))
    {
      sign_state_pos = false;
    }


    //update undex of circular buffer
    acc_index++;
    if(acc_index == 99)
        acc_index = 0;


    return 0;
}
