#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "objective"
#define LOG_LEVEL LOG_LEVEL_INFO

static void
res_obj_get_handler(coap_message_t *req, coap_message_t *res,
                    uint8_t *buf, uint16_t size, int32_t *off)
{
    int len = 0;
    uint8_t first = 1;

    len += snprintf((char *)buf + len, size - len, "{ \"bats\":[");

    // add objectives for each battery
    for (int i = 0; i < battery_count && len < (int)size - 64; i++) {
        
        if(!batteries[i].active) continue;
        
        if(!first) {
            len += snprintf((char *)buf + len, size - len, ",");
        }
        first = 0;

        len += snprintf((char *)buf + len, size - len,
                        "{"
                        "\"idx\":%d,"
                        "\"obj\":%d,"
                        "\"pkw\":%d.%d"
                        "}",
                        i,
                        batteries[i].has_objective ? 1 : 0,
                        (int)(batteries[i].objective_power) * 100,
                        (int)(batteries[i].objective_power) * 100) % 100;
    }

    len += snprintf((char *)buf + len, size - len, "]}");

    coap_set_header_content_format(res, APPLICATION_JSON);
    coap_set_payload(res, buf, len);
}

void res_obj_put_handler(coap_message_t *req, coap_message_t *res,
                         uint8_t *buf, uint16_t size, int32_t *off)
{
    const uint8_t *payload;
    int plen = coap_get_payload(req, &payload);

    static char s[128];
    if(plen <= 0 || plen >= (int)sizeof(s)) {
        coap_set_status_code(res, BAD_REQUEST_4_00);
        return;
    }
    memcpy(s, payload, plen);
    s[plen] = '\0';

    int idx = -1;
    int power = 0;
    int clear = 0;

    int n = sscanf(s, "{ \"idx\" : %d , \"power_kw\" : %d , \"clear\" : %d }",
                   &idx, &power, &clear);
    if(n != 3) {
        LOG_WARN("[OBJ] Bad payload (sscanf=%d): %s\n", n, s);
        coap_set_status_code(res, BAD_REQUEST_4_00);
        return;
    }

    float power_kw = (float)power / 100.0f;

    int active = -1;
    if(idx >= 0 && idx < battery_count) active = batteries[idx].active;


    if (idx < 0 || idx >= battery_count || !batteries[idx].active) {
        LOG_WARN("[OBJ] Invalid idx=%d battery_count=%d active=%d\n", idx, battery_count, active);
        coap_set_status_code(res, BAD_REQUEST_4_00);
        return;
    }

    if (clear) {
        batteries[idx].has_objective = 0;
        batteries[idx].objective_power = 0.0f;
        LOG_INFO("[OBJ] Cleared objective for Bat #%d\n", idx);
        coap_set_status_code(res, CHANGED_2_04);
        return;
    }

    if(power_kw > BAT_MAX_POWER_KW)  power_kw = BAT_MAX_POWER_KW;
    if(power_kw < -BAT_MAX_POWER_KW) power_kw = -BAT_MAX_POWER_KW;

    batteries[idx].has_objective   = 1;
    batteries[idx].objective_power = power_kw;
    LOG_INFO("[OBJ] Set objective for Bat #%d kW\n", idx);
    coap_set_status_code(res, CHANGED_2_04);
}
RESOURCE(res_obj_ctrl,
         "title=\"Objectives\"",
         res_obj_get_handler,
         NULL,
         res_obj_put_handler,
         NULL);

