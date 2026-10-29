//
// Created by user on 29.10.26.
//

#ifndef MICROPLASTIC_APPSENSOR_SENSOR_H
#define MICROPLASTIC_APPSENSOR_SENSOR_H
#include "as7341.h"

void appSensor_Config();
void appSensorStart_Work(as7341_handle_t dev_hdl);
static void map_spectral_data_to_array(uint16_t* row_buf, const as7341_channels_spectral_data_t* data);

#endif //MICROPLASTIC_APPSENSOR_SENSOR_H
