#ifndef __DATAMANAGER_H__
#define __DATAMANAGER_H__

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>
#include <inttypes.h>

struct dataSensor_st
{
    int64_t timeStamp;

    float temperature_1;
    float humidity_1;
    float temperature_2;
    float humidity_2;
};


#endif