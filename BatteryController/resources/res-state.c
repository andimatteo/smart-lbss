#include "contiki.h"
#include <stdio.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "sys/log.h"
#include "../../includes/utility.h"

#define LOG_MODULE "state"
#define LOG_LEVEL LOG_LEVEL_APP

extern float bat_current;
extern float bat_voltage;
extern float bat_temp;
extern float bat_soc;
extern float bat_soh;
extern coap_resource_t res_dev_state;
extern battery_state_t current_state;

static void res_state_periodic_handler(void) {
    coap_notify_observers(&res_dev_state);
}

static void res_get_state_h(coap_message_t *req, coap_message_t *res, 
                                  uint8_t *buf, uint16_t size, int32_t *off) {
    // tension and current
    float export_I = bat_current;
    float export_V = bat_voltage;

    /*
     * current state is made of:
     * V: current tension
     * I: current current
     * T: current temperature
     * S: current SoC
     * H: current SoH
     * St: current State (INI, RUN, ISO)
     * we send integers scaled by 100 in JSON format since we do not
     * care about less 10e-2 values (this is a simplification)
     * */
    int len = snprintf((char*)buf, size, 
            "{\"V\":%d,\"I\":%d,\"T\":%d,"
            "\"S\":%d,\"H\":%d,\"St\":%d}",
            (int)(export_V * 100),      // 3.95V → 395 centiV
            (int)(export_I * 100),      // 0.75A → 75 centiA  
            (int)(bat_temp * 100),      // 24.36°C → 2436 centi°C
            (int)(bat_soc * 10000),     // 0.79 → 7900 (79.00%)
            (int)(bat_soh * 10000),     // 0.91 → 9100 (91.00%)
            current_state);

    coap_set_header_content_format(res, APPLICATION_JSON);
    coap_set_payload(res, buf, len);
}

EVENT_RESOURCE(res_dev_state,
               "title=\"State\";obs",
               res_get_state_h,
               NULL,
               NULL,
               NULL,
               res_state_periodic_handler);
