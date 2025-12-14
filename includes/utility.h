#ifndef _UTILITY_H
#define _UTILITY_H

#include "contiki.h"
#include "random.h"
#include <stdbool.h>
#include <stdint.h>

// battery states
typedef enum {
    STATE_INIT,
    STATE_RUNNING,
    STATE_ISOLATED
} battery_state_t;

// maximum number of batteries
#define MAX_BATTERIES 5
typedef struct {
    uip_ipaddr_t ip;
    bool active;
    bool obs_requested;

    float current_soc;
    float current_voltage;
    float current_temp;
    float current_soh;
    float current_current;
    float optimal_u;
    float actual_power;
    battery_state_t state;

    bool  has_objective;
    float objective_power;
    uint32_t last_update_time;
    coap_observee_t *obs; 
} battery_node_t;

#endif
