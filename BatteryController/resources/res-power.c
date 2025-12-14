#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

#define LOG_MODULE "state"
#define LOG_LEVEL LOG_LEVEL_APP


extern void update_leds();
extern float power_setpoint;
extern battery_state_t current_state;

static void res_power_put_handler(coap_message_t *req, coap_message_t *res, 
                                  uint8_t *buf, uint16_t size, int32_t *off) {

    // if battery not in running state then refuse command
    if (current_state != STATE_RUNNING) { 
        LOG_WARN("[CMD] Rejected - Not in RUNNING state\n");
        coap_set_status_code(res, FORBIDDEN_4_03); 
        return; 
    }

    const uint8_t *chunk;
    int len = coap_get_payload(req, &chunk);
    int param;
    float req_p;


    sscanf((char*)chunk,"{\"u\":%d}",&param);

    // CORRETTO: converti da watt a watt (nessuna divisione necessaria!)
    req_p = (float)param;

    if(len > 0 && len < 32) {

        /* Clamp a limiti fisici */
        if(req_p > BAT_MAX_POWER_W)  req_p = BAT_MAX_POWER_W;
        if(req_p < -BAT_MAX_POWER_W) req_p = -BAT_MAX_POWER_W;

        power_setpoint = req_p;

        int32_t req_w = (int32_t)req_p;

        LOG_INFO("[CMD] Power setpoint: %+ld W ", (long)req_w);
        if(req_w > 0) {
            LOG_INFO_("(Charging)\n");
        } else if(req_w < 0) {
            LOG_INFO_("(Discharging)\n");
        } else {
            LOG_INFO_("(Idle)\n");
        }

        coap_set_status_code(res, CHANGED_2_04);
        update_leds();
    } else {
        LOG_WARN("[CMD] Invalid payload length: %d\n", len);
        coap_set_status_code(res, BAD_REQUEST_4_00);
    }
}
RESOURCE(res_dev_power,
         "title=\"Power\"",
         NULL,
         NULL,
         res_power_put_handler,
         NULL);



