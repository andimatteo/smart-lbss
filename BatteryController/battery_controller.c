#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "os/net/linkaddr.h"
#include "dev/leds.h"
#include "dev/button-hal.h"
#include "sys/etimer.h"
#include "sys/log.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "coap-engine.h"
#include "coap.h"

#include "../includes/utility.h"
#include "../includes/constants.h"
#include "../includes/battery_soh_model.h"
#include "project-conf.h"

#define LOG_MODULE "BatCtrl"
#define LOG_LEVEL LOG_LEVEL_INFO

// utility functions
float get_random_noise(float magnitude) {
    return ((random_rand() % 100) / 50.0f - 1.0f) * magnitude;
}


// scaling configuration and current battery state
#define POWER_SCALE_FACTOR 1000.0f  /* Scale factor: 1kW per unit power */
#define SCALED_CAPACITY_AH 200.0f   /* Scaled capacity: 200Ah (100x cells in parallel) */
battery_state_t current_state = STATE_INIT;
float bat_voltage = 3.7f;
float bat_current = 0.0f;
float bat_temp = 25.0f;
float bat_soc = 0.8f;
float bat_soh = 1.0f;
float bat_capacity_ah = SCALED_CAPACITY_AH;  /* Scaled: 200Ah */
float power_setpoint = 0.0f;   /* Power in Watts (will be scaled) */
int battery_id = 1;            /* Application level device ID */

// safety configuration
float soh_critical = 0.65f;      /* sotto questo → CRITICAL  */
float soh_warning  = 0.75f;      /* sotto questo → WARNING   */
float temp_critical = 60.0f;     /* sopra questo → CRITICAL  */
float temp_warning  = 50.0f;     /* sopra questo → WARNING   */
uint32_t cycles_warning = 100;   /* oltre questo → WARNING   */

// ML configuration
float ml_buffer[ML_WINDOW * N_FEATURES];
float output[1];

// realistic battery modeling
uint32_t charge_cycles = 0;       /* Numero di cicli carica/scarica */
float total_ah_throughput = 0.0f; /* Ah totali trasferiti */
float peak_temp_reached = 25.0f;  /* Temperatura massima raggiunta */
uint8_t was_charging = false;        /* Per contare cicli */

// coap resource declaration
extern coap_resource_t
    res_dev_state, 
    res_dev_power;

// timer definition
struct etimer et_loop, et_init_wait, et_notify;
struct ctimer ct_led_blink;

// utility functions
static void print_battery_status(void) {
    int v = (int)lroundf(bat_voltage*100.0f);

    LOG_INFO("V:%d.%d, I:%d.%d, T:%d.%d, SoC: %d.%d, SoH: %d.%d\n",
           v/100, abs(v)%100,
           (int)(bat_current) / 100,(int)(bat_current) % 100,
           (int)(bat_temp) / 100,(int)(bat_temp) % 100,
           (int)(bat_soc*100.0f) / 100,(int)(bat_soc*100.0f) % 100,
           (int)(bat_soh*100.0f) / 100,(int)(bat_soh*100.0f) % 100);
}

void led_blink(void * pt) {
    if (current_state == STATE_INIT) {
        leds_toggle(LEDS_YELLOW);
        ctimer_reset(&ct_led_blink);
    } else if (current_state == STATE_ISOLATED) {
        leds_toggle(LEDS_RED);
        ctimer_reset(&ct_led_blink);
    }
}


void update_leds() {
    leds_off(~0);
    if (current_state == STATE_RUNNING ) {
        if (power_setpoint > 0.5f) {
            leds_on(LEDS_GREEN);  /* Charging */
        } else if (power_setpoint < -0.5f) {
            leds_on(LEDS_RED);    /* Discharging */
        } else {
            leds_on(LEDS_BLUE);   /* Idle */
        }
    }
}

static void update_sensors_and_buffer() {
    if (current_state == STATE_RUNNING) {
        /* Parametri fisici della batteria Li-ion SCALATA (Pacco Domestico 13.5kWh) */
        const float BATTERY_CAPACITY_AH = SCALED_CAPACITY_AH;  /* Capacità scalata: 200Ah */
        const float NOMINAL_VOLTAGE = 3.7f;                     /* Tensione nominale [V] - manteniamo bassa per ML */
        const float V_MIN = 3.0f;                               /* Tensione minima scarica [V] */
        const float V_MAX = 4.2f;                               /* Tensione massima carica [V] */
        const float INTERNAL_RESISTANCE = 0.0008f;              /* Resistenza interna [Ω] - scalata (0.08/100) */
        const float THERMAL_MASS = 5000.0f;                     /* Massa termica scalata [J/°C] - pacco grande */
        const float HEAT_DISSIPATION = 200.0f;                  /* Coefficiente dissipazione [W/°C] - scalato */
        const float AMBIENT_TEMP = 25.0f;                       /* Temperatura ambiente [°C] */
        const float EFFICIENCY = 0.92f;                         /* Efficienza conversione - più realistica */
        
        /* Timestep di aggiornamento (1 secondo) */
        const float dt = 1.0f; /* [s] */

        /*
         * 0. LEGA LA POTENZA EROGABILE ALLO STATE OF CHARGE
         */
        const float SOC_EMPTY_CUTOFF      = 0.02f;  /* sotto 2% vietata scarica */
        const float SOC_DERATE_DISCHARGE  = 0.10f;  /* sotto 10% scarica deratata */
        const float SOC_FULL_CUTOFF       = 0.98f;  /* sopra 98% vietata carica */
        const float SOC_DERATE_CHARGE     = 0.90f;  /* sopra 90% carica deratata */

        float effective_power = power_setpoint;

        int32_t soc_permil = (int32_t)(bat_soc * 1000.0f);
        int32_t soc_pct_tenths = soc_permil;
        int32_t soc_pct_int = soc_pct_tenths / 10;
        int32_t soc_pct_dec = (soc_pct_tenths >= 0 ? soc_pct_tenths : -soc_pct_tenths) % 10;

        /* Comando di SCARICA (potenza negativa) */
        if (effective_power < -0.5f) {
            if (bat_soc <= SOC_EMPTY_CUTOFF) {
                effective_power = 0.0f;
            } else if (bat_soc < SOC_DERATE_DISCHARGE) {
                float scale = (bat_soc - SOC_EMPTY_CUTOFF) /
                              (SOC_DERATE_DISCHARGE - SOC_EMPTY_CUTOFF);
                if (scale < 0.0f) scale = 0.0f;
                effective_power *= scale;
            }
        }

        /* Comando di CARICA (potenza positiva) */
        if(effective_power > 0.5f) {
            if(bat_soc >= SOC_FULL_CUTOFF) {
                LOG_WARN("[LIMIT] SoC=%ld.%01ld%% -> carica vietata, forzo 0W\n",
                         (long)soc_pct_int, (long)soc_pct_dec);
                effective_power = 0.0f;
            } else if(bat_soc > SOC_DERATE_CHARGE) {
                float scale = (SOC_FULL_CUTOFF - bat_soc) /
                              (SOC_FULL_CUTOFF - SOC_DERATE_CHARGE);
                if(scale < 0.0f) scale = 0.0f;
                float old = effective_power;
                effective_power *= scale;

                int32_t old_w = (int32_t)(old);
                int32_t new_w = (int32_t)(effective_power);

                LOG_INFO("[LIMIT] SoC=%ld.%01ld%% -> derating carica: %ldW -> %ldW\n",
                         (long)soc_pct_int, (long)soc_pct_dec,
                         (long)old_w, (long)new_w);
            }
        }

        power_setpoint = effective_power;
        
        /* 1. CALCOLA CORRENTE dalla potenza richiesta */
        float ocv = V_MIN + (V_MAX - V_MIN) * bat_soc;
        float requested_current = (ocv > 0.1f) ? (power_setpoint / ocv) : 0.0f;
        float current_noise = get_random_noise(0.02f * fabs(requested_current));
        bat_current = requested_current + current_noise;
        
        /* Limita corrente in base a C-rate */
        float max_current = BATTERY_CAPACITY_AH * 15.0f;
        if(bat_current > max_current) bat_current = max_current;
        if(bat_current < -max_current) bat_current = -max_current;
        
        /* 2. AGGIORNA TENSIONE */
        bat_voltage = ocv - (bat_current * INTERNAL_RESISTANCE);
        
        if(bat_soc < 0.1f) {
            bat_voltage -= (0.1f - bat_soc) * 2.0f;
        }
        if(bat_soc > 0.9f) {
            bat_voltage += (bat_soc - 0.9f) * 0.5f;
        }
        
        if(bat_voltage > V_MAX) bat_voltage = V_MAX;
        if(bat_voltage < V_MIN) bat_voltage = V_MIN;
        bat_voltage += get_random_noise(0.01f);
        
        /* 3. AGGIORNA STATE OF CHARGE */
        float efficiency = (bat_current > 0) ? EFFICIENCY : (1.0f / EFFICIENCY);
        float energy_joules = power_setpoint * efficiency * dt;
        float current_capacity_ah = BATTERY_CAPACITY_AH * bat_soh;
        float capacity_joules = current_capacity_ah * NOMINAL_VOLTAGE * 3600.0f;
        float delta_soc = energy_joules / capacity_joules;
        bat_soc += delta_soc;
        
        float ah_transferred = fabs(bat_current) * (dt / 3600.0f);
        total_ah_throughput += ah_transferred;
        
        bool is_charging = (bat_current > 0.5f);
        if(is_charging && !was_charging && bat_soc < 0.5f) {
            charge_cycles++;
        }
        was_charging = is_charging;
        
        if(bat_soc > 1.0f) bat_soc = 1.0f;
        if(bat_soc < 0.0f) bat_soc = 0.0f;
        
        /* 4. AGGIORNA TEMPERATURA */
        float power_loss = bat_current * bat_current * INTERNAL_RESISTANCE;
        float heat_generated = power_loss * dt;
        float heat_dissipated = HEAT_DISSIPATION * (bat_temp - AMBIENT_TEMP) * dt;
        float delta_temp = (heat_generated - heat_dissipated) / THERMAL_MASS;
        bat_temp += delta_temp;
        bat_temp += get_random_noise(0.5f);
        
        if(bat_temp > peak_temp_reached) {
            peak_temp_reached = bat_temp;
        }
        
        if(bat_temp < 0.0f) bat_temp = 0.0f;
        if(bat_temp > 80.0f) bat_temp = 80.0f;
        
        /* 5. AGGIORNA CAPACITÀ (SoH) */
        float cycle_degradation = charge_cycles * 0.0008f;
        float throughput_degradation = total_ah_throughput * 0.00005f;
        
        float temp_degradation = 0.0f;
        if(bat_temp > 40.0f) {
            temp_degradation = (bat_temp - 40.0f) * 0.0001f;
        }
        if(bat_temp > 55.0f) {
            temp_degradation += (bat_temp - 55.0f) * 0.0005f;
        }
        
        float soc_stress_degradation = 0.0f;
        if(bat_soc < 0.15f) {
            soc_stress_degradation = (0.15f - bat_soc) * 0.0002f;
        }
        if(bat_soc > 0.95f) {
            soc_stress_degradation = (bat_soc - 0.95f) * 0.0001f;
        }
        
        float c_rate = fabs(bat_current) / BATTERY_CAPACITY_AH;
        float c_rate_degradation = 0.0f;
        if(c_rate > 3.0f) {
            c_rate_degradation = (c_rate - 3.0f) * 0.00003f;
        }
        
        float total_degradation = cycle_degradation + throughput_degradation + 
                                 temp_degradation + soc_stress_degradation + 
                                 c_rate_degradation;
        
        bat_soh -= total_degradation * dt;
        
        if(bat_soh > 1.0f) bat_soh = 1.0f;
        if(bat_soh < 0.5f) bat_soh = 0.5f;
        
        bat_capacity_ah = BATTERY_CAPACITY_AH * bat_soh;
    }

    /* Aggiorna buffer ML (ancora float) */
    for(int i=0; i<(ML_WINDOW-1)*N_FEATURES; i++) {
        ml_buffer[i] = ml_buffer[i+N_FEATURES];
    }
    int idx = (ML_WINDOW-1)*N_FEATURES;
    ml_buffer[idx] = bat_voltage / 4.2f;
    ml_buffer[idx+1] = ((bat_current + 10.0f) / 20.0f);
    ml_buffer[idx+2] = bat_temp / 80.0f;
    ml_buffer[idx+3] = bat_soc;
}

static void check_safety() {

    battery_soh_regress(ml_buffer, ML_WINDOW*N_FEATURES, output, 1);

    // clamp output to acceptable values
    output[0] = output[0] < 0 ? 0 : output[0];
    output[0] = output[0] > 100 ? 100 : output[0];

    float ml_soh = output[0] / 100.0f;  // 0..1
    float combined_soh = (ml_soh*0.7f) + (bat_soh*0.3f);
    
    // thermal effect
    if(bat_temp > 45.0f) {
        combined_soh -= (bat_temp - 45.0f) * 0.001f;
    }
    
    // low state of charge effect
    if(bat_soc < 0.1f) {
        combined_soh -= (0.1f - bat_soc) * 0.02f;
    }
    
    // to high or too low combined soh
    if(combined_soh > 1.0f) combined_soh = 1.0f;
    if(combined_soh < 0.5f) combined_soh = 0.5f;
    
    // final battery soh and capacity updated
    bat_soh = (bat_soh * 0.95f) + (combined_soh * 0.05f);
    bat_capacity_ah = SCALED_CAPACITY_AH * bat_soh;

    int32_t soh_permil = (int32_t)(bat_soh * 1000.0f);
    int32_t soh_pct_int = soh_permil / 10;
    int32_t soh_pct_dec = (soh_permil >= 0 ? soh_permil : -soh_permil) % 10;

    int32_t cap_milliah = (int32_t)(bat_capacity_ah * 1000.0f);
    int32_t cap_int = cap_milliah / 1000;
    int32_t cap_dec3 = (cap_milliah >= 0 ? cap_milliah : -cap_milliah) % 1000;

    int32_t t_deci = (int32_t)(bat_temp * 10.0f);
    int32_t t_int = t_deci / 10;
    int32_t t_dec = (t_deci >= 0 ? t_deci : -t_deci) % 10;

    bool critical_soh = (bat_soh < soh_critical);
    bool critical_temp = (bat_temp > temp_critical);
    bool warning_soh = (bat_soh < soh_warning);
    bool warning_temp = (bat_temp > temp_warning);
    bool warning_cycles = (charge_cycles > cycles_warning);
    
    if(critical_soh || critical_temp) {
        LOG_INFO_("CRITICAL ✗\n");
        LOG_ERR("!!! SAFETY CRITICAL !!! Isolating battery\n");
        LOG_ERR("    Reason: ");
        if(critical_soh) {
          LOG_ERR_("SoH=%ld.%01ld%% (min 75%%) ",
                   (long)soh_pct_int, (long)soh_pct_dec);
        }
        if(critical_temp) {
          LOG_ERR_("Temp=%ld.%01ld°C (max 60°C)", (long)t_int, (long)t_dec);
        }
        LOG_ERR_("\n");
        LOG_ERR("Press button to reset battery to factory conditions\n");
        
        current_state = STATE_ISOLATED;
        power_setpoint = 0.0f;
        bat_current = 0.0f;
        coap_notify_observers(&res_dev_state);
        
        ctimer_reset(&ct_led_blink);

        leds_off(LEDS_ALL);
        leds_toggle(LEDS_RED);
        
    } else if (warning_soh || warning_temp || warning_cycles) {
        LOG_INFO_("WARNING ⚠\n");
        if(warning_soh) {
            LOG_WARN("Battery degradation: SoH=%ld.%01ld%% (%ld.%03ld Ah remaining)\n", 
                     (long)soh_pct_int, (long)soh_pct_dec,
                     (long)cap_int, (long)cap_dec3);
        }
        if (warning_temp) {
            int32_t peak_deci = (int32_t)(peak_temp_reached * 10.0f);
            int32_t peak_int = peak_deci / 10;
            int32_t peak_dec = (peak_deci >= 0 ? peak_deci : -peak_deci) % 10;
            LOG_WARN("High temperature: %ld.%01ld°C (peak: %ld.%01ld°C)\n", 
                     (long)t_int, (long)t_dec,
                     (long)peak_int, (long)peak_dec);
        }
        if(warning_cycles) {
            LOG_WARN("High cycle count: %lu cycles completed\n", 
                     (unsigned long)charge_cycles);
        }
    } else {
        LOG_INFO("OK\n");
    }
}

// registration callback
static void reg_callback(coap_message_t *response) {

    if(response) {
        LOG_INFO("[INIT] Registration ACK received");
        if(response->code == CREATED_2_01 || response->code == CHANGED_2_04) {
            LOG_INFO("[INIT] Registration SUCCESS\n");
            current_state = STATE_RUNNING;
            update_leds();
            print_battery_status();
        } else {
            LOG_WARN("[INIT] Unexpected response code: %d\n", response->code);
        }
    } else {
        LOG_WARN("[INIT] Registration TIMEOUT - will retry\n");
    }
}

PROCESS(battery_controller, "Battery Controller");
AUTOSTART_PROCESSES(&battery_controller);

PROCESS_THREAD(battery_controller, ev, data) {
    static coap_endpoint_t server_ep;
    static coap_message_t request[1];
    static int retry_count = 0;
    
    PROCESS_BEGIN();
    

    // avoid errors with emlearn
    printf("%p\n", eml_error_str);
    printf("%p\n", eml_net_activation_function_strs);
    

    // start timer
    ctimer_set(
            &ct_led_blink,
            CLOCK_SECOND,
            led_blink,
            NULL);

    // activate coap resources
    coap_activate_resource(&res_dev_state, "dev/state");
    coap_activate_resource(&res_dev_power, "dev/power");
    LOG_INFO("[INIT] CoAP resources activated (dev/state is OBSERVABLE)\n");

    // registate to CoAP endpoint
    LOG_INFO("[INIT] Starting registration to µGrid controller...\n");
    LOG_INFO("[INIT] Target endpoint: %s\n", UGRID_EP);
    

    // registration phase 
    while(current_state == STATE_INIT) {

        LOG_INFO("[INIT] Registration attempt #%d\n", retry_count++);
        
        if (retry_count > 0) {
            etimer_set(&et_init_wait, CLOCK_SECOND * 1);
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_init_wait));
        }
        update_leds();
    
        // build request
        coap_endpoint_parse(UGRID_EP, strlen(UGRID_EP), &server_ep);
        memset(request, 0, sizeof(coap_message_t));
        coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
        coap_set_header_uri_path(request, "dev/register");
        static char payload[64];
        snprintf(payload, sizeof(payload), "%d", battery_id);
        coap_set_payload(request, (uint8_t*)payload, strlen(payload));

        // send request
        COAP_BLOCKING_REQUEST(&server_ep, request, reg_callback);
        
        if(current_state == STATE_RUNNING) {
            LOG_INFO("[INIT] Entering main control loop\n");
            break;
        }
    }

    // notify state 
    etimer_set(&et_loop, CLOCK_SECOND * 5);
    
    while(1) {
        PROCESS_WAIT_EVENT();
        
        if(ev == PROCESS_EVENT_TIMER && data == &et_loop) {
            if(current_state != STATE_ISOLATED) {
                update_sensors_and_buffer();
            }
            
            if(current_state == STATE_RUNNING) { 
                check_safety();
                
                static int status_counter = 0;
                if(++status_counter >= 10) {
                    print_battery_status();
                    status_counter = 0;
                }
            }
            
            // notify uGridController
            coap_notify_observers(&res_dev_state);

            etimer_reset(&et_loop);
        }
        
        
        // can only be triggered when in isolated state
        if(ev == button_hal_release_event && current_state == STATE_ISOLATED) {
            LOG_INFO("[INFO] Factory Reset Triggered\n");

            current_state = STATE_RUNNING; 
            bat_soh = 1.0f;
            bat_capacity_ah = SCALED_CAPACITY_AH;
            bat_temp = 25.0f;
            power_setpoint = 0.0f;
            charge_cycles = 0;
            total_ah_throughput = 0.0f;
            peak_temp_reached = 25.0f;
            was_charging = false;
            
            update_leds();
            print_battery_status();
        }
    }
    
    PROCESS_END();
}
