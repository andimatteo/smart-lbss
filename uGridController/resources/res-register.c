#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"
#include "net/ipv6/uiplib.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "register"
#define LOG_LEVEL LOG_LEVEL_INFO

PROCESS_NAME(ugrid_controller);

static void res_reg_h(coap_message_t *req, coap_message_t *res, uint8_t *buf, uint16_t size, int32_t *off) {
    LOG_INFO(">>> [REGISTRY] Received registration from ");
    LOG_INFO_6ADDR(&req->src_ep->ipaddr);
    LOG_INFO_("\n");

    if (battery_count < MAX_BATTERIES) {
        uip_ipaddr_copy(&batteries[battery_count].ip, &req->src_ep->ipaddr);
        batteries[battery_count].current_soc = 0.5f;
        batteries[battery_count].current_voltage = 0.0f;
        batteries[battery_count].current_temp = 25.0f;
        batteries[battery_count].current_soh = 1.0f;
        batteries[battery_count].current_current = 0.0f;
        batteries[battery_count].optimal_u = 0.0f;
        batteries[battery_count].actual_power = 0.0f;
        batteries[battery_count].state = 0;

        batteries[battery_count].active = 1;
        batteries[battery_count].obs_requested = 0;
        batteries[battery_count].last_update_time = clock_seconds();
        batteries[battery_count].obs = NULL;
        batteries[battery_count].has_objective = false;
        batteries[battery_count].objective_power = 0.0f;

        LOG_INFO(">>> [REGISTRY] Registered Battery #%d: ", battery_count); 
        LOG_INFO_6ADDR(&batteries[battery_count].ip); 
        LOG_INFO_("\n");

        battery_count++;

        coap_set_status_code(res, CREATED_2_01);

        process_post(&ugrid_controller, PROCESS_EVENT_MSG, NULL);
    } else {
        LOG_WARN(">>> [REGISTRY] Max batteries reached\n");
        coap_set_status_code(res, SERVICE_UNAVAILABLE_5_03);
    }
}

RESOURCE(res_register,
        "title=\"Reg\"",
        NULL,
        res_reg_h,
        NULL,
        NULL);

