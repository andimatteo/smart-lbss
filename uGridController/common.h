#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/* Stati della Macchina a Stati Finita (FSM) del Battery Controller */
typedef enum {
    STATE_INIT,
    STATE_RUNNING,
    STATE_ISOLATED
} battery_state_t;

/* Stati operativi di potenza */
typedef enum {
    MODE_HIGH_IMPEDANCE,
    MODE_CHARGING,
    MODE_DISCHARGING
} power_mode_t;

/* Payload per la registrazione */
typedef struct {
    char id[16];
    float max_capacity;
} registration_payload_t;

#endif /* COMMON_H */
