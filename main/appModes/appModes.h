//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPMODES_H
#define MICROPLASTIC_APPMODES_H

typedef enum {
    NORMAL_MODE = 0,
    CONTEXT_BUFFERING_MODE = 1,
    LOW_ENERGY_MODE = 2,
    TEST_MODE = 3,
    CONTINUE_MODE = 4,
    LOAD_OFF_LOCAL_STORAGE_MODE = 5,
} applicationModes_t;

void setApplicationMode(applicationModes_t mode);
applicationModes_t getApplicationMode();


#endif //MICROPLASTIC_APPMODES_H
