/**
 * @file main.c
 * @author Nguyen Nhu Hai Long ( @long27032002 )
 * @brief Main file of AirSENSE project
 * @version 0.1
 * @date 2023-01-04
 *
 * @copyright Copyright (c) 2023
 *
 */

/*------------------------------------ INCLUDE LIBRARY ------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/time.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "esp_mem.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_pm.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_eap_client.h"
#include "esp_smartconfig.h"

#include "esp_flash.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include <spi_flash_mmap.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/spi_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "sdcard.h"
#include "DS3231Time.h"
#include "datamanager.h"
#include "DeviceManager.h"
#include "sntp_sync.h"
#include "sht3x.h"
#include "lcd.h"

/*------------------------------------ DEFINE ------------------------------------ */

__attribute__((unused)) static const char *TAG = "Main";

#define PERIOD_GET_DATA_FROM_SENSOR (TickType_t)(15000 / portTICK_PERIOD_MS)
#define PERIOD_SAVE_DATA_SENSOR_TO_SDCARD (TickType_t)(7500 / portTICK_PERIOD_MS)
// #define PERIOD_SAVE_DATA_AFTER_WIFI_RECONNECT (TickType_t)(2500 / portTICK_PERIOD_MS)

#define NO_WAIT (TickType_t)(0)
#define WAIT_10_TICK (TickType_t)(10 / portTICK_PERIOD_MS)
#define WAIT_100_TICK (TickType_t)(100 / portTICK_PERIOD_MS)

#define QUEUE_SIZE 10U
#define NAME_FILE_QUEUE_SIZE 5U
#define DATA_SENSOR_MIDLEWARE_QUEUE_SIZE 20

#define WIFI_AVAIABLE_BIT BIT0
#define MQTT_CLIENT_CONNECTED WIFI_AVAIABLE_BIT
#define WIFI_DISCONNECT_BIT BIT1
#define MQTT_CLIENT_DISCONNECTED WIFI_DISCONNECT_BIT
#define FILE_RENAME_NEWDAY BIT2
#define FILE_RENAME_FROMSYNC BIT3

#if CONFIG_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

#define DEFAULT_LISTEN_INTERVAL CONFIG_WIFI_LISTEN_INTERVAL
#define DEFAULT_BEACON_TIMEOUT  CONFIG_WIFI_BEACON_TIMEOUT

TaskHandle_t getDataFromSensorTask_handle = NULL;
TaskHandle_t saveDataSensorToSDcardTask_handle = NULL;
TaskHandle_t saveDataSensorAfterReconnectWiFiTask_handle = NULL;
TaskHandle_t mqttPublishMessageTask_handle = NULL;
TaskHandle_t sntp_syncTimeTask_handle = NULL;
TaskHandle_t allocateDataForMultipleQueuesTask_handle = NULL;
TaskHandle_t smartConfigTask_handle = NULL;

SemaphoreHandle_t getDataSensor_semaphore = NULL;
SemaphoreHandle_t writeDataToSDcard_semaphore = NULL;
SemaphoreHandle_t sentDataToMQTT_semaphore = NULL;
SemaphoreHandle_t writeDataToSDcardNoWifi_semaphore = NULL;
SemaphoreHandle_t displayData_semaphore = NULL;

QueueHandle_t dataSensorSentToSD_queue;
QueueHandle_t dataSensorSentToMQTT_queue;
QueueHandle_t moduleError_queue;
QueueHandle_t nameFileSaveDataNoWiFi_queue;
QueueHandle_t dateTimeLostWiFi_queue;
QueueHandle_t dataSensorMidleware_queue;
QueueHandle_t dataSensorSentToLCD_queue;
QueueHandle_t dataSensorSentViaRS485_queue;

static EventGroupHandle_t fileStore_eventGroup;

static struct statusDevice_st statusDevice = {0};

static esp_mqtt_client_handle_t mqttClient_handle = NULL;
static char nameFileSaveData[21];

// Whether send data to MQTT queue or not (depend on WIFI connection)
bool sendToMQTTQueue = false;

const char *formatDataSensorString = "{\n\t\"station_id\":\"%x%x%x%x\",\n\t\"Time\":%lld,\n\t\"Temperature_1\":%.2f,\n\t\"Humidity_1\":%.2f,\n\t\"Temperature_2\":%.2f,\n\t\"Humidity_2\":%.2f,\n}";

//------------------------------------------------------------------

i2c_dev_t ds3231_device;
i2c_dev_t lcd;
sht3x_t sht30_sensor_1;
sht3x_t sht30_sensor_2;


bool wifi_firstConnect = true;

/*------------------------------------ WIFI ------------------------------------ */

static void mqtt_app_start(void);
static void sntp_app_start(void);
static void smartConfig_task(void * parameter);
void processDataAfterReconnectWiFi_task(void *parameters);
static void WiFi_eventHandler( void *argument,  esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
        {
            xTaskCreate(smartConfig_task, "smartconfig_task", 1024 * 4, NULL, 15, &smartConfigTask_handle);
            ESP_LOGI(__func__, "Trying to connect with Wi-Fi...\n");
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
        {
            ESP_LOGI(__func__, "Wi-Fi connected AP SSID:%s password:%s.\n", CONFIG_SSID, CONFIG_PASSWORD);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            /* When esp32 disconnect to wifi, this event become
            * active. We suspend the task mqttPublishMessageTask. */
            ESP_LOGI(__func__, "Wi-Fi disconnected: Retrying connect to AP SSID:%s password:%s", CONFIG_SSID, CONFIG_PASSWORD);
            if (mqttPublishMessageTask_handle != NULL && eTaskGetState(mqttPublishMessageTask_handle) != eSuspended)
            {
                vTaskSuspend(mqttPublishMessageTask_handle);
                statusDevice.mqttClient = DISCONNECTED;
                xEventGroupSetBits(fileStore_eventGroup, MQTT_CLIENT_DISCONNECTED);
                sendToMQTTQueue = false;
                ESP_LOGI(__func__, "set bit disconnect");
            }
            esp_wifi_connect();
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

            ESP_LOGI(__func__, "Starting MQTT Client...\n");
#ifdef CONFIG_RTC_TIME_SYNC
        if (sntp_syncTimeTask_handle == NULL)
        {
            sntp_app_start();
        }
#endif
            /* When connect/reconnect wifi, esp32 take an IP address and this
            * event become active. If it's the first-time connection, create
            * task mqttPublishMessageTask, else resume that task. */
            if (mqttPublishMessageTask_handle == NULL)
            {
                mqtt_app_start();
                statusDevice.mqttClient = CONNECTED;
            }
            else
            {
                if (eTaskGetState(mqttPublishMessageTask_handle) == eSuspended)
                {
                    vTaskResume(mqttPublishMessageTask_handle);
                    ESP_LOGI(__func__, "Resume task mqttPublishMessageTask.");
                }
                if (saveDataSensorAfterReconnectWiFiTask_handle == NULL)
                {
                    // Create task to send data from sensor read by getDataFromSensor_task() to MQTT queue after reconnect to Wifi
                    // Period 100ms
                    xTaskCreate(processDataAfterReconnectWiFi_task, "SendDataAfterReconnect", (1024 * 16), NULL, (UBaseType_t)15, &saveDataSensorAfterReconnectWiFiTask_handle);
                    if(saveDataSensorAfterReconnectWiFiTask_handle != NULL)
                    {
                        ESP_LOGW(__func__, "Create task reconnected OK.");
                    } else {
                        ESP_LOGE(__func__, "Create task reconnected failed.");
                    }
                } else if (eTaskGetState(saveDataSensorAfterReconnectWiFiTask_handle) == eSuspended) {
                    vTaskResume(saveDataSensorAfterReconnectWiFiTask_handle);
                    ESP_LOGI(__func__, "Resume task saveDataSensorAfterReconnectWiFi.");
                }
            }
        }
    } else if (event_base == SC_EVENT) {
        switch (event_id)
        {
        case SC_EVENT_SCAN_DONE:
        {
            ESP_LOGI(__func__, "Scan done.");
            break;
        }
        case SC_EVENT_FOUND_CHANNEL:
        {
            ESP_LOGI(__func__, "Found channel.");
            break;
        }
        case SC_EVENT_GOT_SSID_PSWD:
        {
            ESP_LOGI(__func__, "Got SSID and password.");

            smartconfig_event_got_ssid_pswd_t *smartconfig_event = (smartconfig_event_got_ssid_pswd_t *)event_data;
            wifi_config_t wifi_config;
            uint8_t ssid[33] = { 0 };
            uint8_t password[65] = { 0 };
            uint8_t rvd_data[33] = { 0 };

            bzero(&wifi_config, sizeof(wifi_config_t));
            memcpy(wifi_config.sta.ssid, smartconfig_event->ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, smartconfig_event->password, sizeof(wifi_config.sta.password));
            wifi_config.sta.bssid_set = smartconfig_event->bssid_set;
            if (wifi_config.sta.bssid_set == true) {
                memcpy(wifi_config.sta.bssid, smartconfig_event->bssid, sizeof(wifi_config.sta.bssid));
            }

            memcpy(ssid, smartconfig_event->ssid, sizeof(smartconfig_event->ssid));
            memcpy(password, smartconfig_event->password, sizeof(smartconfig_event->password));
            ESP_LOGI(TAG, "SSID:%s", ssid);
            ESP_LOGI(TAG, "PASSWORD:%s", password);
            if (smartconfig_event->type == SC_TYPE_ESPTOUCH_V2) {
                ESP_ERROR_CHECK_WITHOUT_ABORT( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
                ESP_LOGI(TAG, "RVD_DATA:");
                for (int i = 0; i < 33; i++) {
                    printf("%02x ", rvd_data[i]);
                }
                printf("\n");
            }

            ESP_ERROR_CHECK_WITHOUT_ABORT( esp_wifi_disconnect() );
            ESP_ERROR_CHECK_WITHOUT_ABORT( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
            esp_wifi_connect();
            break;
        }
        case SC_EVENT_SEND_ACK_DONE:
        {
            xTaskNotifyGive(smartConfigTask_handle);
            ESP_LOGI(__func__, "Send ACK done.");
            break;
        }
        default:
            break;
        }
    } else {
        ESP_LOGI(__func__, "Other event id:%" PRIi32 "", event_id);
    }

    return;
}

/**
 * @brief This function initialize wifi and create, start WiFi handle such as loop (low priority)
 *
 */
void WIFI_initSTA(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t WIFI_initConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_init(&WIFI_initConfig));

    esp_event_handler_instance_t instance_any_id_Wifi;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_any_id_SmartConfig;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WiFi_eventHandler,
                                                        NULL,
                                                        &instance_any_id_Wifi));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WiFi_eventHandler,
                                                        NULL,
                                                        &instance_got_ip));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(SC_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WiFi_eventHandler,
                                                        NULL,
                                                        &instance_any_id_SmartConfig));


    static wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_SSID,
            .password = CONFIG_PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

#ifdef CONFIG_POWER_SAVE_MODE_ENABLE
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_inactive_time(WIFI_IF_STA, DEFAULT_BEACON_TIMEOUT));
    ESP_LOGI(__func__, "Enable Power Save Mode.");
    esp_wifi_set_ps(DEFAULT_PS_MODE);
#endif // CONFIG_POWER_SAVE_MODE_ENABLE

    ESP_LOGI(__func__, "WIFI initialize STA finished.");
}

/**
 * @brief SmartConfig task
 * 
 * @param parameter 
 */
static void smartConfig_task(void * parameter)
{
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t smartConfig_config = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&smartConfig_config));
    for(;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "smartconfig over");
        esp_smartconfig_stop();
        vTaskDelete(NULL);
    }
}

/*          -------------- MQTT --------------           */

/**
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param[in] handler_args user data registered to the event.
 * @param[in] base Event base for the handler(always MQTT Base in this example).
 * @param[in] event_id The id for the received event.
 * @param[in] event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(__func__, "Event dispatched from event loop base=%s, event_id=%"PRIi32"", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(__func__, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(fileStore_eventGroup, MQTT_CLIENT_CONNECTED);
        statusDevice.mqttClient = CONNECTED;
        sendToMQTTQueue = true;

        if (eTaskGetState(mqttPublishMessageTask_handle) == eSuspended)
        {
            vTaskResume(mqttPublishMessageTask_handle);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGE(__func__, "MQTT_EVENT_DISCONNECTED");
        statusDevice.mqttClient = DISCONNECTED;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(__func__, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(__func__, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(__func__, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(__func__, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(__func__, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        }
        else
        {
            ESP_LOGW(__func__, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;

    default:
        ESP_LOGI(__func__, "Other event id:%d", event->event_id);
        break;
    }
}

/**
 * @brief Publish message dataSensor receive from dataSensorSentToMQTT_queue to MQTT
 *
 */
void mqttPublishMessage_task(void *parameters)
{
    uint8_t MAC_addressArray[6];
    WORD_ALIGNED_ATTR char MAC_address[32] = {0};
    esp_read_mac(MAC_addressArray, ESP_MAC_WIFI_STA); // Get MAC address of ESP32
    sprintf(MAC_address, "%x%x%x%x%x%x", MAC_addressArray[0], MAC_addressArray[1], MAC_addressArray[2], MAC_addressArray[3], MAC_addressArray[4], MAC_addressArray[5]);

    WORD_ALIGNED_ATTR char mqttTopic[64] = {0};
    sprintf(mqttTopic,"%s/%s", "Temp_Humi", MAC_address);

    sentDataToMQTT_semaphore = xSemaphoreCreateMutex();

    for (;;)
    {
        struct dataSensor_st dataSensorReceiveFromQueue;
        if (statusDevice.mqttClient == CONNECTED)
        {
            if (uxQueueMessagesWaiting(dataSensorSentToMQTT_queue) != 0)
            {
                if (xQueueReceive(dataSensorSentToMQTT_queue, (void *)&dataSensorReceiveFromQueue, portMAX_DELAY) == pdPASS)
                {
                    ESP_LOGI(__func__, "Receiving data from queue successfully.");
                    if (xSemaphoreTake(sentDataToMQTT_semaphore, portMAX_DELAY) == pdTRUE)
                    {
                        esp_err_t error = ESP_OK;
                        WORD_ALIGNED_ATTR char mqttMessage[256];
                        sprintf(mqttMessage, formatDataSensorString, MAC_addressArray[0],
                                MAC_addressArray[1],
                                MAC_addressArray[2],
                                MAC_addressArray[3],
                                dataSensorReceiveFromQueue.timeStamp,
                                dataSensorReceiveFromQueue.temperature_1,
                                dataSensorReceiveFromQueue.humidity_1,
                                dataSensorReceiveFromQueue.temperature_2,
                                dataSensorReceiveFromQueue.humidity_2);
                        error = esp_mqtt_client_publish(mqttClient_handle, (const char *)mqttTopic, mqttMessage, 0, 0, 0);
                        xSemaphoreGive(sentDataToMQTT_semaphore);
                        if (error == ESP_FAIL)
                        {
                            ESP_LOGE(__func__, "MQTT client publish message failed...");
                        }
                        else
                        {
                            ESP_LOGI(__func__, "MQTT client publish message success.");
                        }
                    }
                    vTaskDelay((TickType_t)(1000 / portTICK_PERIOD_MS));
                }
            }
            else
            {
                vTaskDelay(PERIOD_GET_DATA_FROM_SENSOR);
            }
        }
        else
        {
            ESP_LOGE(__func__, "MQTT Client disconnected.");
            // Suspend ourselves.
            vTaskSuspend(NULL);
        }
    }
}

/**
 * @brief This function initialize MQTT client and create, start MQTT Client handle such as loop (low priority)
 *
 */
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_Config = {
        .broker = {
            .address = {
                .uri = CONFIG_BROKER_URI,
                .hostname = CONFIG_BROKER_HOST,
                // .transport = MQTT_TRANSPORT_OVER_TCP,
                .path = CONFIG_BROKER_URI,
                .port = CONFIG_BROKER_PORT,
            },
        },
        .credentials = {
            .username = CONFIG_MQTT_USERNAME,
            .client_id = CONFIG_MQTT_CLIENT_ID,
            .set_null_client_id = false,
            .authentication = {
                .password = CONFIG_MQTT_PASSWORD,
                // .certificate = (const char *)NULL,
            },
        },
        .network = {
            .disable_auto_reconnect = false,
            .timeout_ms = CONFIG_MQTT_TIMEOUT,
        },
    };

    mqttClient_handle = esp_mqtt_client_init(&mqtt_Config);

    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(mqttClient_handle, ESP_EVENT_ANY_ID, mqtt_event_handler, mqttClient_handle);
    esp_mqtt_client_start(mqttClient_handle);

    xTaskCreate(mqttPublishMessage_task, "MQTT Publish", (1024 * 16), NULL, (UBaseType_t)10, &mqttPublishMessageTask_handle);
}

void sntp_functionCallback(struct timeval *timeValue)
{
    xEventGroupSetBits(fileStore_eventGroup, FILE_RENAME_FROMSYNC);
    return;
}

/**
 * @brief SNTP Get time task : init sntp, then get time from ntp and save time to DS3231,
 *        finally delete itself (no loop task)
 *
 * @param parameter
 */
void sntp_syncTime_task(void *parameter)
{
    do
    {
        esp_err_t errorReturn = sntp_syncTime();
        ESP_ERROR_CHECK_WITHOUT_ABORT(errorReturn);
        if (errorReturn == ESP_OK)
        {
            ds3231_getTimeString(&ds3231_device);
            sntp_setTimmeZoneToVN();
            ds3231_getTimeString(&ds3231_device);
            struct tm timeInfo = {0};
            time_t timeNow = 0;
            time(&timeNow);
            localtime_r(&timeNow, &timeInfo);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_setTime(&ds3231_device, &timeInfo));
            sntp_printServerInformation();
        }
        sntp_deinit();
        vTaskDelete(NULL);
    } while (0);
}

/**
 * @brief  This function initialize SNTP, then get time from ntp and save time to DS3231
 *
 *
 */
static void sntp_app_start(void)
{
    if (sntp_initialize((void *)(&sntp_functionCallback)) == ESP_OK)
    {
        xTaskCreate(sntp_syncTime_task, "SNTP Get Time", (1024 * 4), NULL, (UBaseType_t)15, &sntp_syncTimeTask_handle);
    }
    return;
}

/*          -------------- *** --------------           */

static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(error);
}

void getDataFromSensor_task(void *parameters)
{
    struct dataSensor_st dataSensorTemp = {0};
    struct moduleError_st moduleErrorTemp = {0};
    TickType_t task_lastWakeTime;
    esp_err_t error = ESP_OK;

    // Start periodic measurements with 1 measurement per second.
    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_start_measurement(&sht30_sensor_1, SHT3X_PERIODIC_1MPS, SHT3X_HIGH));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_start_measurement(&sht30_sensor_2, SHT3X_PERIODIC_1MPS, SHT3X_HIGH));

    // Wait until first measurement is ready (constant time of at least 30 ms
    // or the duration returned from *sht3x_get_measurement_duration*).
    vTaskDelay(sht3x_get_measurement_duration(SHT3X_HIGH));

    getDataSensor_semaphore = xSemaphoreCreateMutex();

    for (;;)
    {
        task_lastWakeTime = xTaskGetTickCount();
        if (xSemaphoreTake(getDataSensor_semaphore, portMAX_DELAY))
        {
            ds3231_getTimeString(&ds3231_device);
#if (CONFIG_USING_RTC)
            moduleErrorTemp.ds3231Error = ds3231_getEpochTime(&ds3231_device, &(dataSensorTemp.timeStamp));
            if (moduleErrorTemp.ds3231Error != ESP_OK)
            {
                dataSensorTemp.timeStamp = dataSensorTemp.timeStamp + (uint64_t)((PERIOD_GET_DATA_FROM_SENSOR * portTICK_PERIOD_MS) / 1000);
            }
            
            if (ds3231_isNewDay(&ds3231_device))
            {
                xEventGroupSetBits(fileStore_eventGroup, FILE_RENAME_NEWDAY);
            }
#endif

            if ((error = sht3x_get_results(&sht30_sensor_1, &(dataSensorTemp.temperature_1), &(dataSensorTemp.humidity_1))) == ESP_OK)
                ESP_LOGI("sht3x_sensor_1", "SHT3x Sensor: %.2f °C, %.2f %%\n", dataSensorTemp.temperature_1, dataSensorTemp.humidity_1);
            else
                ESP_LOGE("sht3x_sensor_1", "Could not get results: %d (%s)", error, esp_err_to_name(error));

            if ((error = sht3x_get_results(&sht30_sensor_2, &(dataSensorTemp.temperature_2), &(dataSensorTemp.humidity_2))) == ESP_OK)
                ESP_LOGI("sht3x_sensor_2", "SHT3x Sensor: %.2f °C, %.2f %%\n", dataSensorTemp.temperature_2, dataSensorTemp.humidity_2);
            else
                ESP_LOGE("sht3x_sensor_2", "Could not get results: %d (%s)", error, esp_err_to_name(error));

            xSemaphoreGive(getDataSensor_semaphore); // Give mutex

            ESP_LOGI(__func__, "Read data from sensors completed!");

            if (xQueueSendToBack(dataSensorMidleware_queue, (void *)&dataSensorTemp, WAIT_10_TICK * 10) != pdPASS)
            {
                ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorMidleware Queue.");
            }
            else
            {
                ESP_LOGI(__func__, "Success to post the data sensor to dataSensorMidleware Queue.");
            }

            if (moduleError_queue != NULL && (moduleErrorTemp.ds3231Error != ESP_OK))
            {
                moduleErrorTemp.timestamp = dataSensorTemp.timeStamp;
                if (xQueueSendToBack(moduleError_queue, (void *)&moduleErrorTemp, WAIT_10_TICK * 5) != pdPASS)
                {
                    ESP_LOGE(__func__, "Failed to post the moduleError to Queue.");
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the moduleError to Queue.");
                }
            }
        }
        //memset(&dataSensorTemp, 0, sizeof(struct dataSensor_st));
        memset(&moduleErrorTemp, 0, sizeof(struct moduleError_st));
        vTaskDelayUntil(&task_lastWakeTime, PERIOD_GET_DATA_FROM_SENSOR);
    }
};

void displayData_task(void *parameters)
{
    char buffer_1[40] = " 00.0*C | 00.0% ";
    char buffer_2[40] = " 00.0*C | 00.0% ";

    UBaseType_t message_stored = 0;
    struct dataSensor_st dataSensorReceiveFromQueue;
    TickType_t task_lastWakeTime;

    displayData_semaphore = xSemaphoreCreateMutex();

    for (;;)
    {
        task_lastWakeTime = xTaskGetTickCount();
        message_stored = uxQueueMessagesWaiting(dataSensorSentToLCD_queue);

        if (message_stored != 0) // Check if dataSensorSentToLCD_queue not empty
        {
            if (xQueueReceive(dataSensorSentToLCD_queue, (void *)&dataSensorReceiveFromQueue, WAIT_10_TICK) == pdPASS) // Get data sesor from queue
            {
                ESP_LOGI(__func__, "Receiving data from queue successfully.");

                xSemaphoreTake(displayData_semaphore, WAIT_100_TICK);

                lcd_clear(&lcd);
                lcd_put_cur(&lcd, 0, 0);
                vTaskDelay(10 / portTICK_PERIOD_MS);

                buffer_1[1] = ((int)dataSensorReceiveFromQueue.temperature_1 / 10) + 48;
                buffer_1[2] = ((int)dataSensorReceiveFromQueue.temperature_1 % 10) + 48;
                buffer_1[4] = ((int)(dataSensorReceiveFromQueue.temperature_1 * 10) % 10) + 48;
                buffer_1[10] = ((int)dataSensorReceiveFromQueue.humidity_1 / 10) + 48;
                buffer_1[11] = ((int)dataSensorReceiveFromQueue.humidity_1 % 10) + 48;
                buffer_1[13] = ((int)(dataSensorReceiveFromQueue.humidity_1 * 10) % 10) + 48;
                lcd_send_string(&lcd, buffer_1);

                lcd_put_cur(&lcd, 1, 0);
                vTaskDelay(10 / portTICK_PERIOD_MS);

                buffer_2[1] = ((int)dataSensorReceiveFromQueue.temperature_2 / 10) + 48;
                buffer_2[2] = ((int)dataSensorReceiveFromQueue.temperature_2 % 10) + 48;
                buffer_2[4] = ((int)(dataSensorReceiveFromQueue.temperature_2 * 10) % 10) + 48;
                buffer_2[10] = ((int)dataSensorReceiveFromQueue.humidity_2 / 10) + 48;
                buffer_2[11] = ((int)dataSensorReceiveFromQueue.humidity_2 % 10) + 48;
                buffer_2[13] = ((int)(dataSensorReceiveFromQueue.humidity_2 * 10) % 10) + 48;
                lcd_send_string(&lcd, buffer_2);

                xSemaphoreGive(displayData_semaphore);
            }
            else
            {
                ESP_LOGI(__func__, "Receiving data from queue failed.");
                continue;
            }
        }
        vTaskDelayUntil(&task_lastWakeTime, PERIOD_GET_DATA_FROM_SENSOR / 3);
    }

};

/**
 * @brief This task receive data from immediate queue and provide data to MQTT queue and SD queue
 * 
 * @param parameters 
 */
void allocateDataForMultipleQueues_task(void *parameters)
{
    TickType_t task_timeDelay = PERIOD_SAVE_DATA_SENSOR_TO_SDCARD / portTICK_PERIOD_MS;
    UBaseType_t message_stored = 0;
    struct dataSensor_st dataSensorReceiveFromQueue;

    for (;;)
    {
        message_stored = uxQueueMessagesWaiting(dataSensorMidleware_queue);

        if (message_stored != 0)
        {
            if (xQueueReceive(dataSensorMidleware_queue, (void *)&dataSensorReceiveFromQueue, portMAX_DELAY) == pdPASS)
            {
                ESP_LOGI(__func__, "Receiving data from Midleware queue successfully.");
                if (xQueueSendToBack(dataSensorSentToSD_queue, (void *)&dataSensorReceiveFromQueue, portMAX_DELAY) != pdPASS)
                {
                    ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorSentToSD Queue.");
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the data sensor to dataSensorSentToSD Queue.");
                }

                if (sendToMQTTQueue == true) 
                {
                    if (xQueueSendToBack(dataSensorSentToMQTT_queue, (void *)&dataSensorReceiveFromQueue, WAIT_100_TICK) != pdPASS)
                    {
                        ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorSentToMQTT Queue.");
                    }
                    else
                    {
                        ESP_LOGI(__func__, "Success to post the data sensor to dataSensorSentToMQTT Queue.");
                    }
                }

                if (xQueueSendToBack(dataSensorSentToLCD_queue, (void *)&dataSensorReceiveFromQueue, portMAX_DELAY) != pdPASS)
                {
                    ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorSentToLCD Queue.");
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the data sensor to dataSensorSentToLCD Queue.");
                }

                if (xQueueSendToBack(dataSensorSentViaRS485_queue, (void *)&dataSensorReceiveFromQueue, WAIT_100_TICK) != pdPASS)
                {
                    ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorSentViaRS485 Queue.");
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the data sensor to dataSensorSentViaRS485 Queue.");
                }
            }
            else
            {
                ESP_LOGI(__func__, "Receiving data from queue failed.");
            }

            // if (message_stored > (UBaseType_t)(0.5 * DATA_SENSOR_MIDLEWARE_QUEUE_SIZE))
            // {
            //     task_timeDelay = (100 * 0.5) / portTICK_PERIOD_MS;
            // } else if (message_stored > (UBaseType_t)(0.9 * DATA_SENSOR_MIDLEWARE_QUEUE_SIZE)) {
            //     task_timeDelay = (100 * (1 - 0.9)) / portTICK_PERIOD_MS;
            // } else {
            //     task_timeDelay = 100 / portTICK_PERIOD_MS;
            // }
        }

        //task_timeDelay = (PERIOD_SAVE_DATA_SENSOR_TO_SDCARD * (1 - ((float)message_stored / DATA_SENSOR_MIDLEWARE_QUEUE_SIZE))) / portTICK_PERIOD_MS;
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief This task is responsible for naming SD file
 *
 * @param parameters
 */
void fileEvent_task(void *parameters)
{
    fileStore_eventGroup = xEventGroupCreate();
    SemaphoreHandle_t file_semaphore = NULL;
    file_semaphore = xSemaphoreCreateMutex();
    ds3231_convertTimeToString(&ds3231_device, nameFileSaveData, 10);
    strcat(nameFileSaveData, "_noWiFi");

    for (;;)
    { 
        EventBits_t bits = xEventGroupWaitBits(fileStore_eventGroup,
                                               FILE_RENAME_NEWDAY | MQTT_CLIENT_CONNECTED | MQTT_CLIENT_DISCONNECTED | FILE_RENAME_FROMSYNC,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);

        if (xSemaphoreTake(file_semaphore, portMAX_DELAY) == pdTRUE)
        {
            struct tm timeInfo = {0};
            time_t timeNow = 0;
            time(&timeNow);
            localtime_r(&timeNow, &timeInfo);

            if (bits & FILE_RENAME_NEWDAY)
            {
                ds3231_convertTimeToString(&ds3231_device, nameFileSaveData, 10);
                if (statusDevice.mqttClient == DISCONNECTED)
                {
                    if (strstr(nameFileSaveData, "_noWiFi") == NULL)
                    {
                        strcat(nameFileSaveData, "_noWiFi");
                    }
                    if (xQueueSendToBack(dateTimeLostWiFi_queue, (void *)&timeInfo, WAIT_10_TICK * 5) != pdPASS)
                    {
                        ESP_LOGE(__func__, "Failed to post the data sensor to dateTimeLostWiFi_queue Queue.");
                    }
                    else
                    {
                        ESP_LOGI(__func__, "Success to post the data sensor to dateTimeLostWiFi_queue Queue.");
                    }
                }
            }
            else if (bits & MQTT_CLIENT_CONNECTED)
            {
                if (strstr(nameFileSaveData, "_noWiFi") != NULL)
                {
                    char *position = strrchr(nameFileSaveData, '_');
                    if (position != NULL)
                    {
                        *position = '\0'; // Truncate the string at the underscore position
                    }
                    ESP_LOGI(__func__, "Update file name after reconnect WiFi: %s", nameFileSaveData);
                }
            }
            else if (bits & MQTT_CLIENT_DISCONNECTED)
            {
                if (strstr(nameFileSaveData, "_noWiFi") == NULL)
                {
                    strcat(nameFileSaveData, "_noWiFi");
                    ESP_LOGI(__func__, "Update file name after disconnect: %s", nameFileSaveData);
                }
                if (xQueueSendToBack(dateTimeLostWiFi_queue, (void *)&timeInfo, WAIT_10_TICK * 5) != pdPASS)
                {
                    ESP_LOGE(__func__, "Failed to post the data sensor to dateTimeLostWiFi_queue Queue.");
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the data sensor to dateTimeLostWiFi_queue Queue.");
                }
                printf("Time: %d-%d-%d\n", timeInfo.tm_mday, timeInfo.tm_mon + 1, timeInfo.tm_year + 1900);
            }
            else if (bits & FILE_RENAME_FROMSYNC)
            {
                ds3231_convertTimeToString(&ds3231_device, nameFileSaveData, 10);
                ESP_LOGI(__func__, "File name updated from SNTP");
            }
            xSemaphoreGive(file_semaphore);
        }
    }
};

/**
 * @brief This task read data from SD card (saved when lost Wifi) and push data to dataSensorMidleware_queue queue
 *
 * @param parameters
 */
void processDataAfterReconnectWiFi_task(void *parameters)
{
    struct tm timeLostWiFi = {0};
    char nameFileSaveDataLostWiFi[21];

    for (;;)
    {
        while (uxQueueMessagesWaiting((const QueueHandle_t)dateTimeLostWiFi_queue) != 0)
        {
            memset(&timeLostWiFi, 0, sizeof(struct tm));
            memset(nameFileSaveDataLostWiFi, 0, sizeof(nameFileSaveDataLostWiFi));

            if (xQueueReceive(dateTimeLostWiFi_queue, (void *)&timeLostWiFi, portMAX_DELAY))
            {
                sprintf(nameFileSaveDataLostWiFi, "%" PRIu8 "-%" PRIu8 "-%" PRIu32 "_noWiFi", (uint8_t)timeLostWiFi.tm_mday, (uint8_t)(timeLostWiFi.tm_mon + 1), (uint32_t)(timeLostWiFi.tm_year) + 1900);
            }
            char pathFile[64];
            sprintf(pathFile, "%s/%s.txt", mount_point, nameFileSaveDataLostWiFi);
            char *dataSensorString;
            struct dataSensor_st dataSensorTemp = {0};
            dataSensorString = (char *)malloc((size_t)256);
            ESP_LOGI(__func__, "Opening file %s...", pathFile);
            FILE *file = fopen(pathFile, "r");
            if (file == NULL)
            {
                ESP_LOGE(__func__, "Failed to open file for reading.");
                continue;
            }

            while (!feof(file))
            {
                memset(dataSensorString, 0, strlen(dataSensorString));
                memset(&dataSensorTemp, 0, sizeof(struct dataSensor_st));
                fscanf(file, "%[^,],%llu,%f,%f,%f,%f\n", dataSensorString,
                       &(dataSensorTemp.timeStamp),
                       &(dataSensorTemp.temperature_1),
                       &(dataSensorTemp.humidity_1),
                       &(dataSensorTemp.temperature_2),
                       &(dataSensorTemp.humidity_2));

                BaseType_t checkSendToQueueFlag = xQueueSendToBack(dataSensorMidleware_queue, (void *)&dataSensorTemp, portMAX_DELAY);
                if (checkSendToQueueFlag != pdPASS)
                {
                    ESP_LOGW(__func__, "Failed to post the data sensor to dataSensorMidleware Queue.  Queue size: %d (max 10).", uxQueueMessagesWaiting(dataSensorSentToMQTT_queue));
                }
                else
                {
                    ESP_LOGI(__func__, "Success to post the data sensor to dataSensorMidleware Queue.");
                }

                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            free(dataSensorString);

            // After read all data from "..._noWifi" file in SD card, delete that file
            ESP_ERROR_CHECK_WITHOUT_ABORT(sdcard_removeFile(nameFileSaveDataLostWiFi));
        }
        // Suspend ourselves.
        vTaskSuspend(NULL);
    }
}

/**
 * @brief Save data from SD queue to SD card
 * 
 * @param parameters 
 */
void saveDataSensorToSDcard_task(void *parameters)
{
    TickType_t task_timeDelay = PERIOD_SAVE_DATA_SENSOR_TO_SDCARD / portTICK_PERIOD_MS;
    UBaseType_t message_stored = 0;
    struct dataSensor_st dataSensorReceiveFromQueue;

    writeDataToSDcard_semaphore = xSemaphoreCreateMutex();
    for (;;)
    {
        message_stored = uxQueueMessagesWaiting(dataSensorSentToSD_queue);

        if (message_stored != 0) // Check if dataSensorSentToSD_queue not empty
        {
            if (xQueueReceive(dataSensorSentToSD_queue, (void *)&dataSensorReceiveFromQueue, WAIT_10_TICK * 50) == pdPASS) // Get data sesor from queue
            {
                ESP_LOGI(__func__, "Receiving data from queue successfully.");

                if (xSemaphoreTake(writeDataToSDcard_semaphore, portMAX_DELAY) == pdTRUE)
                {
                    static esp_err_t errorCode_t;
                    // Create data string follow format
                    errorCode_t = sdcard_writeDataToFile(nameFileSaveData, "%llu,%.2f,%.2f,%.2f,%.2f\n",
                                                         dataSensorReceiveFromQueue.timeStamp,
                                                         dataSensorReceiveFromQueue.temperature_1,
                                                         dataSensorReceiveFromQueue.humidity_1,
                                                         dataSensorReceiveFromQueue.temperature_2,
                                                         dataSensorReceiveFromQueue.humidity_2);
                    ESP_LOGI(TAG, "Save task received mutex!");
                    xSemaphoreGive(writeDataToSDcard_semaphore);
                    if (errorCode_t != ESP_OK)
                    {
                        ESP_LOGE(__func__, "sdcard_writeDataToFile(...) function returned error: 0x%.4X", errorCode_t);
                    }
                }
            }
            else
            {
                ESP_LOGI(__func__, "Receiving data from queue failed.");
                continue;
            }
        }

        task_timeDelay = (PERIOD_SAVE_DATA_SENSOR_TO_SDCARD * (1 - ((float)message_stored / DATA_SENSOR_MIDLEWARE_QUEUE_SIZE))) / portTICK_PERIOD_MS;
        vTaskDelay(task_timeDelay);
    }
};

void rs485_logData_task(void *parameters)
{
    UBaseType_t message_stored = 0;
    TickType_t task_lastWakeTime;

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_driver_install(2, 128 * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(2, &uart_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(2, 17, 16, 4, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_mode(2, UART_MODE_RS485_HALF_DUPLEX));

    struct dataSensor_st dataSensorReceiveFromQueue;

    char message[64] = {0};

    for (;;)
    {
        // task_lastWakeTime = xTaskGetTickCount();
        message_stored = uxQueueMessagesWaiting(dataSensorSentToSD_queue);

        if (message_stored != 0) // Check if dataSensorSentViaRS485_queue not empty
        {
            if (xQueueReceive(dataSensorSentViaRS485_queue, (void *)&dataSensorReceiveFromQueue, WAIT_10_TICK * 50) == pdPASS) // Get data sesor from queue
            {
                ESP_LOGI(__func__, "Receiving data from queue successfully.");

                static esp_err_t errorCode_t = ESP_OK;
                // Create data string follow format
                sprintf(message, "{%llu,%.1f,%.1f,%.1f,%.1f}\n",
                                                        dataSensorReceiveFromQueue.timeStamp,
                                                        dataSensorReceiveFromQueue.temperature_1,
                                                        dataSensorReceiveFromQueue.humidity_1,
                                                        dataSensorReceiveFromQueue.temperature_2,
                                                        dataSensorReceiveFromQueue.humidity_2);

                errorCode_t = uart_write_bytes(2, message, strlen(message));
                if (errorCode_t != ESP_OK)
                {
                    ESP_LOGE(__func__, "uart_write_bytes(...) function returned error: 0x%.4X", errorCode_t);
                }
            }
            else
            {
                ESP_LOGI(__func__, "Receiving data from queue failed.");
                continue;
            }
        }

        vTaskDelay(PERIOD_GET_DATA_FROM_SENSOR);
    }
}

// void logErrorToSDcard_task(void *parameters)
// {
//     for (;;)
//     {
//         if (uxQueueMessagesWaiting(moduleError_queue) != 0)
//         {
//             struct errorModule_st errorModuleReceiveFromQueue;
//             if (xQueueReceive(moduleError_queue, (void *)&errorModuleReceiveFromQueue, WAIT_10_TICK * 50) == pdPASS)
//             {
//                 ESP_LOGI(__func__, "Receiving data from queue successfully.");
//                 ESP_LOGE(__func__, "Module %s error code: 0x%.4X", errorModuleReceiveFromQueue.moduleName, errorModuleReceiveFromQueue.errorCode);
//             }
//             else
//             {
//                 ESP_LOGI(__func__, "Receiving data from queue failed.");
//             }
//         }

//     }
// };


/*****************************************************************************************************/
/*-------------------------------  MAIN_APP DEFINE FUNCTIONS  ---------------------------------------*/
/*****************************************************************************************************/

void app_main(void)
{
    // Allow other core to finish initialization
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(__func__, "Starting app main.");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
        printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
            CONFIG_IDF_TARGET,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
            (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
            (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }
    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    ESP_LOGI(__func__, "Name device: %s.", CONFIG_NAME_DEVICE);
    ESP_LOGI(__func__, "Firmware version %s.", CONFIG_FIRMWARE_VERSION);

    // Initialize nvs partition
    ESP_LOGI(__func__, "Initialize nvs partition.");
    initialize_nvs();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());
    // Wait a second for memory initialization
    vTaskDelay(500 / portTICK_PERIOD_MS);

// Smartconfig
// configSmartWifi();

// Initialize SD card
#if (CONFIG_USING_SDCARD)
    // Initialize SPI Bus

    ESP_LOGI(__func__, "Initialize SD card with SPI interface.");
    esp_vfs_fat_mount_config_t mount_config_t = MOUNT_CONFIG_DEFAULT();
    spi_bus_config_t spi_bus_config_t = SPI_BUS_CONFIG_DEFAULT();
    sdmmc_host_t host_t = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_PIN_NUM_CS;
    slot_config.host_id = host_t.slot;

    sdmmc_card_t SDCARD;
    ESP_ERROR_CHECK_WITHOUT_ABORT(sdcard_initialize(&mount_config_t, &SDCARD, &host_t, &spi_bus_config_t, &slot_config));
#endif // CONFIG_USING_SDCARD

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2cdev_init());

// Initialize RTC module
#if (CONFIG_USING_RTC)
    ESP_LOGI(__func__, "Initialize DS3231 module(I2C/Wire%d).", CONFIG_RTC_I2C_PORT);

    memset(&ds3231_device, 0, sizeof(i2c_dev_t));

    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_initialize(&ds3231_device, CONFIG_RTC_I2C_PORT, CONFIG_RTC_PIN_NUM_SDA, CONFIG_RTC_PIN_NUM_SCL));
#endif // CONFIG_USING_RTC

    memset(&sht30_sensor_1, 0, sizeof(sht30_sensor_1));
    memset(&sht30_sensor_2, 0, sizeof(sht30_sensor_2));

    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_init_desc(&sht30_sensor_1, 0x44, 1, 21, 22));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_init(&sht30_sensor_1));

    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_init_desc(&sht30_sensor_2, 0x44, 0, 26, 27));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sht3x_init(&sht30_sensor_2));

    memset(&lcd, 0, sizeof(lcd));
    lcd_init(&lcd, (0x4E>>1), 1, 21, 22);

    lcd_clear(&lcd);

    lcd_put_cur(&lcd, 0, 0);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    lcd_send_string(&lcd, "Initializing...");

    xTaskCreate(fileEvent_task, "EventFile", (1024 * 8), NULL, (UBaseType_t)7, NULL);

    // Creat dataSensorMidlewareQueue
    dataSensorMidleware_queue = xQueueCreate(DATA_SENSOR_MIDLEWARE_QUEUE_SIZE, sizeof(struct dataSensor_st));
    while (dataSensorMidleware_queue == NULL)
    {
        ESP_LOGE(__func__, "Create dataSensorMidleware Queue failed.");
        ESP_LOGI(__func__, "Retry to create dataSensorMidleware Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        dataSensorMidleware_queue = xQueueCreate(DATA_SENSOR_MIDLEWARE_QUEUE_SIZE, sizeof(struct dataSensor_st));
    };
    ESP_LOGI(__func__, "Create dataSensorMidleware Queue success.");

    // Create dataSensorQueue
    dataSensorSentToSD_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    while (dataSensorSentToSD_queue == NULL)
    {
        ESP_LOGE(__func__, "Create dataSensorSentToSD Queue failed.");
        ESP_LOGI(__func__, "Retry to create dataSensorSentToSD Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        dataSensorSentToSD_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    };
    ESP_LOGI(__func__, "Create dataSensorSentToSD Queue success.");

    // Create moduleErrorQueue
    moduleError_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct moduleError_st));
    for (size_t i = 0; moduleError_queue == NULL && i < 5; i++)
    {
        ESP_LOGE(__func__, "Create moduleError Queue failed.");
        ESP_LOGI(__func__, "Retry to create moduleError Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        moduleError_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct moduleError_st));
    }

    if (moduleError_queue == NULL)
    {
        ESP_LOGE(__func__, "Create moduleError Queue failed.");
        ESP_LOGE(__func__, "ModuleErrorQueue created fail. All errors during getDataFromSensor_task() function running will not write to SD card!");
    }
    else
    {
        ESP_LOGI(__func__, "Create moduleError Queue success.");
    }

    // Create dataSensorSentToMQTT Queue
    dataSensorSentToMQTT_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    while (dataSensorSentToMQTT_queue == NULL)
    {
        ESP_LOGE(__func__, "Create dataSensorSentToMQTT Queue failed.");
        ESP_LOGI(__func__, "Retry to create dataSensorSentToMQTT Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        dataSensorSentToMQTT_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    };
    ESP_LOGI(__func__, "Create dataSensorSentToMQTT Queue success.");

    // Create dateTimeLostWiFi_queue Queue
    dateTimeLostWiFi_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct tm));
    if (dateTimeLostWiFi_queue == NULL)
    {
        ESP_LOGE("Booting", "Create nameFileSaveData Queue failed.");
    }
    ESP_LOGI(__func__, "Create dateTimeLostWiFi Queue success.");

    // Create dataSensorSentToLCD_queue
    dataSensorSentToLCD_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    for (size_t i = 0; dataSensorSentToLCD_queue == NULL && i < 5; i++)
    {
        ESP_LOGE(__func__, "Create moduleError Queue failed.");
        ESP_LOGI(__func__, "Retry to create moduleError Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        dataSensorSentToLCD_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    }

    if (dataSensorSentToLCD_queue == NULL)
    {
        ESP_LOGE(__func__, "Create dataSensorSentToLCD Queue failed.");
    }
    else
    {
        ESP_LOGI(__func__, "Create dataSensorSentToLCD Queue success.");
    }

    // Create dataSensorSentViaRS485_queue
    dataSensorSentViaRS485_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    for (size_t i = 0; dataSensorSentViaRS485_queue == NULL && i < 5; i++)
    {
        ESP_LOGE(__func__, "Create dataSensorSentViaRS485 Queue failed.");
        ESP_LOGI(__func__, "Retry to create dataSensorSentViaRS485 Queue...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        dataSensorSentViaRS485_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct dataSensor_st));
    }

    if (dataSensorSentViaRS485_queue == NULL)
    {
        ESP_LOGE(__func__, "Create dataSensorSentViaRS485 Queue failed.");
    }
    else
    {
        ESP_LOGI(__func__, "Create dataSensorSentViaRS485 Queue success.");
    }

    // Create task to get data from sensor (64Kb stack memory| priority 25(max))
    // Period 5000ms
    xTaskCreate(getDataFromSensor_task, "GetDataSensor", (1024 * 32), NULL, (UBaseType_t)25, &getDataFromSensorTask_handle);

    // Create task to save data from sensor read by getDataFromSensor_task() to SD card (16Kb stack memory| priority 10)
    // Period 5000ms
    xTaskCreate(saveDataSensorToSDcard_task, "SaveDataSensor", (1024 * 16), NULL, (UBaseType_t)10, &saveDataSensorToSDcardTask_handle);

    // Create task to display data on LCD (16Kb stack memory| priority 25)
    xTaskCreate(displayData_task, "DisplayData", (1024 * 16), NULL, (UBaseType_t)25, NULL);

    // Create task to log data to SD card (4Kb stack memory| priority 15)
    xTaskCreate(rs485_logData_task, "RS485_logData", (1024 * 4), NULL, (UBaseType_t)15, NULL);

    // Creat task to allocate data to MQTT and SD queue from dataSensorMidleware queue
    xTaskCreate(allocateDataForMultipleQueues_task, "AllocateData", (1024 * 16), NULL, (UBaseType_t)15, &allocateDataForMultipleQueuesTask_handle);
    if (allocateDataForMultipleQueuesTask_handle != NULL)
    {
        ESP_LOGI(__func__, "Create task AllocateData successfully.");
    } else {
        ESP_LOGE(__func__, "Create task AllocateData failed.");
    }

#if CONFIG_USING_WIFI
#if CONFIG_PM_ENABLE
    // Configure dynamic frequency scaling:
    // maximum and minimum frequencies are set in sdkconfig,
    // automatic light sleep is enabled if tickless idle support is enabled.
    esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_MAX_CPU_FREQ_MHZ,
            .min_freq_mhz = CONFIG_MIN_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
            .light_sleep_enable = true
#endif
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT( esp_pm_configure(&pm_config) );
#endif // CONFIG_PM_ENABLE

    WIFI_initSTA();
#endif

}
