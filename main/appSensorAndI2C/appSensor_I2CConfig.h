//
// Created by user on 28.10.26.
//

#ifndef MICROPLASTIC_APPSENSOR_H
#define MICROPLASTIC_APPSENSOR_H
#include "driver/i2c_master.h"

void appI2C_Init(void);
i2c_master_bus_handle_t get_i2c_bus_handle(void);
i2c_master_dev_handle_t get_dev_handle(void);

#endif //MICROPLASTIC_APPSENSOR_H
