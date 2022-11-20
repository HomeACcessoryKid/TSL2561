/*  (c) 2022 HomeAccessoryKid
 *  This is a ambient light sensor based on a TSL2561 chip
 *  It uses any ESP8266 with as little as 1MB flash. 
 *  GPIO pin 2 (SCL) and GPIO pin 0 (SDA) are used as I2C bus
 *  UDPlogger is used to have remote logging
 *  LCM is enabled in case you want remote updates
 *  MQTT is used to send measurements upstream
 */

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
//#include <espressif/esp_system.h> //for timestamp report only
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>
#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <string.h>
#include "lwip/api.h"
#include <udplogger.h>
#include "ds18b20/ds18b20.h"
#include "math.h"
#include "mqtt-client.h"
#include <sysparam.h>
#include "i2c/i2c.h"
#include "tsl2561/tsl2561.h"

#ifndef VERSION
 #error You must set VERSION=x.y.z to match github version tag x.y.z
#endif

#ifndef SCL_PIN
 #error SCL_PIN is not specified
#endif
#ifndef SDA_PIN
 #error SDA_PIN is not specified
#endif
#ifndef I2C_BUS
 #error I2C_BUS is not specified
#endif


/* ============== BEGIN HOMEKIT CHARACTERISTIC DECLARATIONS =============================================================== */
// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include "ota-api.h"
homekit_characteristic_t ota_trigger  = API_OTA_TRIGGER;
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  "X");
homekit_characteristic_t serial       = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, "1");
homekit_characteristic_t model        = HOMEKIT_CHARACTERISTIC_(MODEL,         "Z");
homekit_characteristic_t revision     = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  "0.0.0");

// next use these two lines before calling homekit_server_init(&config);
//    int c_hash=ota_read_sysparam(&manufacturer.value.string_value,&serial.value.string_value,
//                                      &model.value.string_value,&revision.value.string_value);
//    config.accessories[0]->config_number=c_hash;
// end of OTA add-in instructions

homekit_characteristic_t lux = HOMEKIT_CHARACTERISTIC_(CURRENT_AMBIENT_LIGHT_LEVEL, 0.1);

// void identify_task(void *_args) {
//     vTaskDelete(NULL);
// }

void identify(homekit_value_t _value) {
    UDPLUS("Identify\n");
//    xTaskCreate(identify_task, "identify", 256, NULL, 2, NULL);
}

/* ============== END HOMEKIT CHARACTERISTIC DECLARATIONS ================================================================= */

#define N 60 //samples in the ringbuffer and seconds of the sliding window
uint32_t ring[N];
uint32_t sort[N];
tsl2561_t lightSensor;
char *dmtczidx=NULL;
TimerHandle_t xTimer;
void vTimerCallback( TimerHandle_t xTimer ) {
    uint32_t seconds = ( uint32_t ) pvTimerGetTimerID( xTimer );
    vTimerSetTimerID( xTimer, (void*)seconds+1); //136 year to loop
    int i,idx,in,io;
    uint32_t old,new=0;
    idx=seconds%N;
    if (tsl2561_read_lux(&lightSensor, &new)) {
        old=ring[idx]; ring[idx]=new;
        if (new<old) {  //search where to insert new measurement into sorted array
            i=0;   while (new>sort[i]) i++; in=i;
                   while (old>sort[i]) i++; io=i;
            for (i=io;i>in;i--) sort[i]=sort[i-1];
            sort[i]=new;
        }
        // if (new==old) do_nothing;
        if (new>old) {
            i=N-1; while (new<sort[i]) i--; in=i;
                   while (old<sort[i]) i--; io=i;
            for (i=io;i<in;i++) sort[i]=sort[i+1];
            sort[i]=new;
        }
        //for (i=0;i<N;i++) printf("%d ",sort[i]); printf("\n");
        printf("time:%3ds  60sMedian: %ulx   now: %ulx\n", seconds, sort[N/2], new);

        if (!idx) { //only publish at the full minute
            float old_lux=lux.value.float_value;
            lux.value.float_value=(float)sort[N/2]?sort[N/2]:0.001; //zero is not a valid value!
            if (old_lux!=lux.value.float_value) \
                homekit_characteristic_notify(&lux, HOMEKIT_FLOAT(lux.value.float_value));

            int n=mqtt_client_publish("{\"idx\":%s,\"nvalue\":0,\"svalue\":\"%u\"}", dmtczidx, sort[N/2]);
            if (n<0) printf("MQTT publish failed because %s\n",MQTT_CLIENT_ERROR(n));
        }
    } else printf("Could not read data from TSL2561\n");
}

mqtt_config_t mqttconf=MQTT_DEFAULT_CONFIG;
char error[]="error";
static void ota_string() {
    char *otas;
    if (sysparam_get_string("ota_string", &otas) == SYSPARAM_OK) {
        mqttconf.host=strtok(otas,";");
        mqttconf.user=strtok(NULL,";");
        mqttconf.pass=strtok(NULL,";");
        dmtczidx=strtok(NULL,";");
    }
    if (mqttconf.host==NULL) mqttconf.host=error;
    if (mqttconf.user==NULL) mqttconf.user=error;
    if (mqttconf.pass==NULL) mqttconf.pass=error;
    if (dmtczidx==NULL) dmtczidx=error;
}

void device_init() {
    uint32_t lux;
    i2c_init(I2C_BUS, SCL_PIN, SDA_PIN, I2C_FREQ_100K);
    // TSL2561_I2C_ADDR_VCC(0x49) TSL2561_I2C_ADDR_GND(0x29) TSL2561_I2C_ADDR_FLOAT(0x39) Default
    lightSensor.i2c_dev.addr = TSL2561_I2C_ADDR_GND;
    lightSensor.i2c_dev.bus = I2C_BUS;
    tsl2561_init(&lightSensor);
    // TSL2561_INTEGRATION_13MS(0x00) TSL2561_INTEGRATION_101MS(0x01) TSL2561_INTEGRATION_402MS(0x02) Default
    tsl2561_set_integration_time(&lightSensor, TSL2561_INTEGRATION_402MS);
    // TSL2561_GAIN_16X(0x10) TSL2561_GAIN_1X(0x00) Default
    tsl2561_set_gain(&lightSensor, TSL2561_GAIN_1X);
    if (tsl2561_read_lux(&lightSensor, &lux)) for (int i=0;i<N;i++) {ring[i]=lux;sort[i]=lux;}
    //sysparam_set_string("ota_string", "192.168.178.5;LuxFront;fakepassword;91"); //can be used if not using LCM
    ota_string();
    mqtt_client_init(&mqttconf);
    xTimer=xTimerCreate( "Timer", 1000/portTICK_PERIOD_MS, pdTRUE, (void*)0, vTimerCallback);
    xTimerStart(xTimer, 0);
}

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(
        .id=1,
        .category=homekit_accessory_category_sensor,
        .services=(homekit_service_t*[]){
            HOMEKIT_SERVICE(ACCESSORY_INFORMATION,
                .characteristics=(homekit_characteristic_t*[]){
                    HOMEKIT_CHARACTERISTIC(NAME, "TSL2561"),
                    &manufacturer,
                    &serial,
                    &model,
                    &revision,
                    HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
                    NULL
                }),
            HOMEKIT_SERVICE(LIGHT_SENSOR, .primary=true,
                .characteristics=(homekit_characteristic_t*[]){
                    HOMEKIT_CHARACTERISTIC(NAME, "LightLevel"),
                    &lux,
                    &ota_trigger,
                    NULL
                }),
            NULL
        }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};


void user_init(void) {
    uart_set_baud(0, 115200);
    udplog_init(3);
    UDPLUS("\n\n\nTSL2561 " VERSION "\n");

    device_init();
    
    int c_hash=ota_read_sysparam(&manufacturer.value.string_value,&serial.value.string_value,
                                      &model.value.string_value,&revision.value.string_value);
    //c_hash=1; revision.value.string_value="0.0.1"; //cheat line
    config.accessories[0]->config_number=c_hash;
    
    homekit_server_init(&config);
}
