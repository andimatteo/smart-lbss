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

#include "battery_soh_model.h"
#include "project-conf.h"

#define LOG_MODULE "BatCtrl"
#define LOG_LEVEL LOG_LEVEL_INFO

typedef enum {
    STATE_INIT,
    STATE_RUNNING,
    STATE_ISOLATED
} battery_state_t;

static battery_state_t current_state = STATE_INIT;
/* --- Safety thresholds remotamente modificabili via /dev/params --- */
static float soh_critical = 0.75f;      /* sotto questo → CRITICAL  */
static float soh_warning  = 0.85f;      /* sotto questo → WARNING   */
static float temp_critical = 60.0f;     /* sopra questo → CRITICAL  */
static float temp_warning  = 50.0f;     /* sopra questo → WARNING   */
static uint32_t cycles_warning = 100;   /* oltre questo → WARNING   */


#define ML_WINDOW 20
#define N_FEATURES 4
static float ml_buffer[ML_WINDOW * N_FEATURES];

/* ========== SCALING CONFIGURATION ========== */
#define POWER_SCALE_FACTOR 1000.0f  /* Scale factor: 1kW per unit power */
#define SCALED_CAPACITY_AH 200.0f   /* Scaled capacity: 200Ah (100x cells in parallel) */

static float bat_voltage = 3.7f;      /* Keep voltage realistic for ML model */
static float bat_current = 0.0f;      /* Scaled current (can reach ±2700A at 10kW) */
static float bat_temp = 25.0f;
static float bat_soc = 0.8f;          /* Inizia all'80% SoC */
static float bat_soh = 1.0f;
static float bat_capacity_ah = SCALED_CAPACITY_AH;  /* Scaled: 200Ah */
static float power_setpoint = 0.0f;   /* Power in Watts (will be scaled) */
static int battery_id = 1;            /* Application level device ID */

/* Contatori per degrado realistico */
static uint32_t charge_cycles = 0;       /* Numero di cicli carica/scarica */
static float total_ah_throughput = 0.0f; /* Ah totali trasferiti */
static float peak_temp_reached = 25.0f;  /* Temperatura massima raggiunta */
static bool was_charging = false;        /* Per contare cicli */

/* Forward declarations */
extern coap_resource_t res_dev_state, res_dev_power;
static struct etimer et_loop, et_init_wait, et_notify;

static float get_random_noise(float magnitude) {
    return ((random_rand() % 100) / 50.0f - 1.0f) * magnitude;
}

static void print_battery_status(void) {
    LOG_INFO("\n");
    LOG_INFO("┌─ Battery Status ─────────────────────────────────────────────────┐\n");
    LOG_INFO("│ ID: %d                                State: ", battery_id);
    
    if(current_state == STATE_RUNNING) {
        LOG_INFO_("RUNNING ✓");
    } else if(current_state == STATE_ISOLATED) {
        LOG_INFO_("ISOLATED ✗");
    } else {
        LOG_INFO_("INIT...");
    }
    LOG_INFO_("                        │\n");
    LOG_INFO("├──────────────────────────────────────────────────────────────────┤\n");
    LOG_INFO("│ Voltage:     %4.2f V      Current:    %+6.2f A                 │\n", 
             bat_voltage, bat_current);
    LOG_INFO("│ Temperature: %4.1f°C     SoC:        %5.1f%%                  │\n", 
             bat_temp, bat_soc * 100.0f);
    LOG_INFO("│ SoH:         %5.1f%%     Capacity:   %4.2f Ah                 │\n", 
             bat_soh * 100.0f, bat_capacity_ah);
    LOG_INFO("│ Power:       %+6.2f W    Cycles:     %lu                       │\n", 
             power_setpoint, (unsigned long)charge_cycles);
    LOG_INFO("│ Throughput:  %6.1f Ah   Peak Temp:  %4.1f°C                  │\n",
             total_ah_throughput, peak_temp_reached);
    LOG_INFO("└──────────────────────────────────────────────────────────────────┘\n");
}

static void update_leds() {
    leds_off(LEDS_ALL);
    if(current_state == STATE_INIT) {
        leds_toggle(LEDS_YELLOW);
    } else if(current_state == STATE_ISOLATED) {
        leds_toggle(LEDS_RED);
    } else {
        if(power_setpoint > 0.5f) {
            leds_on(LEDS_GREEN);  /* Charging */
        } else if(power_setpoint < -0.5f) {
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
         *
         * - Per SoC molto basso: niente scarica (solo carica o idle)
         * - Per SoC basso: scarica deratata (ridotta linearmente)
         * - Per SoC molto alto: niente carica
         * - Per SoC alto: carica deratata
         *
         * NB: power_setpoint > 0  ⇒ carica
         *     power_setpoint < 0  ⇒ scarica
         */
        const float SOC_EMPTY_CUTOFF      = 0.02f;  /* sotto 2% vietata scarica */
        const float SOC_DERATE_DISCHARGE  = 0.10f;  /* sotto 10% scarica deratata */
        const float SOC_FULL_CUTOFF       = 0.98f;  /* sopra 98% vietata carica */
        const float SOC_DERATE_CHARGE     = 0.90f;  /* sopra 90% carica deratata */

        float effective_power = power_setpoint;

        /* Comando di SCARICA (potenza negativa) */
        if(effective_power < -0.5f) {
            if(bat_soc <= SOC_EMPTY_CUTOFF) {
                /* Batteria considerata "vuota" → niente più scarica */
                LOG_WARN("[LIMIT] SoC=%.1f%% -> scarica vietata, forzo 0W\n",
                         bat_soc * 100.0f);
                effective_power = 0.0f;
            } else if(bat_soc < SOC_DERATE_DISCHARGE) {
                /* Derating lineare tra SOC_DERATE_DISCHARGE e SOC_EMPTY_CUTOFF */
                float scale = (bat_soc - SOC_EMPTY_CUTOFF) /
                              (SOC_DERATE_DISCHARGE - SOC_EMPTY_CUTOFF);
                if(scale < 0.0f) scale = 0.0f;
                float old = effective_power;
                effective_power *= scale;
                LOG_INFO("[LIMIT] SoC=%.1f%% -> derating scarica: %.1fW -> %.1fW\n",
                         bat_soc * 100.0f, old, effective_power);
            }
        }

        /* Comando di CARICA (potenza positiva) */
        if(effective_power > 0.5f) {
            if(bat_soc >= SOC_FULL_CUTOFF) {
                /* Batteria praticamente piena → niente più carica */
                LOG_WARN("[LIMIT] SoC=%.1f%% -> carica vietata, forzo 0W\n",
                         bat_soc * 100.0f);
                effective_power = 0.0f;
            } else if(bat_soc > SOC_DERATE_CHARGE) {
                /* Derating lineare tra SOC_DERATE_CHARGE e SOC_FULL_CUTOFF */
                float scale = (SOC_FULL_CUTOFF - bat_soc) /
                              (SOC_FULL_CUTOFF - SOC_DERATE_CHARGE);
                if(scale < 0.0f) scale = 0.0f;
                float old = effective_power;
                effective_power *= scale;
                LOG_INFO("[LIMIT] SoC=%.1f%% -> derating carica: %.1fW -> %.1fW\n",
                         bat_soc * 100.0f, old, effective_power);
            }
        }

        /* Aggiorna il setpoint effettivo usato dal modello fisico */
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
            LOG_INFO("[CYCLE] Charge cycle #%lu completed\n", (unsigned long)charge_cycles);
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

    /* Aggiorna buffer ML */
    for(int i=0; i<(ML_WINDOW-1)*N_FEATURES; i++) {
        ml_buffer[i] = ml_buffer[i+N_FEATURES];
    }
    int idx = (ML_WINDOW-1)*N_FEATURES;
    ml_buffer[idx] = bat_voltage / 4.2f;
    ml_buffer[idx+1] = ((bat_current / 100.0f) + 10.0f) / 20.0f;
    ml_buffer[idx+2] = bat_temp / 80.0f;
    ml_buffer[idx+3] = bat_soc;
}

static void check_safety() {
    float predicted_soh = battery_soh_regress1(ml_buffer, ML_WINDOW*N_FEATURES);
    float combined_soh = (predicted_soh * 0.7f) + (bat_soh * 0.3f);
    
    if(bat_temp > 45.0f) {
        combined_soh -= (bat_temp - 45.0f) * 0.001f;
    }
    
    if(bat_soc < 0.1f) {
        combined_soh -= (0.1f - bat_soc) * 0.02f;
    }
    
    if(combined_soh > 1.0f) combined_soh = 1.0f;
    if(combined_soh < 0.5f) combined_soh = 0.5f;
    
    bat_soh = (bat_soh * 0.95f) + (combined_soh * 0.05f);
    bat_capacity_ah = SCALED_CAPACITY_AH * bat_soh;

    LOG_INFO("[SAFETY] SoH: %.1f%% (%.3f Ah) | Temp: %.1f°C | Cycles: %lu | ", 
             bat_soh * 100.0f, bat_capacity_ah, bat_temp, (unsigned long)charge_cycles);

    bool critical_soh = (bat_soh < soh_critical);
    bool critical_temp = (bat_temp > temp_critical);
    bool warning_soh = (bat_soh < soh_warning);
    bool warning_temp = (bat_temp > temp_warning);
    bool warning_cycles = (charge_cycles > cycles_warning);
    
    if(critical_soh || critical_temp) {
        LOG_INFO_("CRITICAL ✗\n");
        LOG_ERR("!!! SAFETY CRITICAL !!! Isolating battery\n");
        LOG_ERR("    Reason: ");
        if(critical_soh) LOG_ERR_("SoH=%.1f%% (min 75%%) ", bat_soh * 100.0f);
        if(critical_temp) LOG_ERR_("Temp=%.1f°C (max 60°C)", bat_temp);
        LOG_ERR_("\n");
        LOG_ERR("    Press button to reset battery to factory conditions\n");
        
        current_state = STATE_ISOLATED;
        power_setpoint = 0.0f;
        bat_current = 0.0f;
        coap_notify_observers(&res_dev_state);
        
        leds_off(LEDS_ALL);
        leds_toggle(LEDS_RED);
        
    } else if(warning_soh || warning_temp || warning_cycles) {
        LOG_INFO_("WARNING ⚠\n");
        if(warning_soh) {
            LOG_WARN("    Battery degradation: SoH=%.1f%% (%.3f Ah remaining)\n", 
                     bat_soh * 100.0f, bat_capacity_ah);
        }
        if(warning_temp) {
            LOG_WARN("    High temperature: %.1f°C (peak: %.1f°C)\n", 
                     bat_temp, peak_temp_reached);
        }
        if(warning_cycles) {
            LOG_WARN("    High cycle count: %lu cycles completed\n", 
                     (unsigned long)charge_cycles);
        }
    } else {
        LOG_INFO_("OK ✓\n");
    }
}

/* --- Risorse CoAP CON OBSERVE --- */
static void res_state_get_handler(coap_message_t *req, coap_message_t *res, 
                                   uint8_t *buf, uint16_t size, int32_t *off) {
    char s[5] = "INI";
    if(current_state == STATE_RUNNING)      strcpy(s, "RUN"); 
    else if(current_state == STATE_ISOLATED) strcpy(s, "ISO");

    if(size < 128) {
        LOG_WARN("[CoAP] Buffer too small for response\n");
        return;
    }

    /* Corrente e tensione da esportare */
    float export_I = bat_current;
    float export_V = bat_voltage;

    if(current_state == STATE_ISOLATED) {
        /* Isolata: NESSUNO SCAMBIO DI POTENZA */
        export_I = 0.0f;

        /* opzionale: metti la V all’OCV corrispondente al SoC (open-circuit) */
        const float V_MIN = 3.0f;
        const float V_MAX = 4.2f;
        float ocv = V_MIN + (V_MAX - V_MIN) * bat_soc;
        export_V = ocv;
    }

    int len = snprintf((char*)buf, size, 
                      "{\"V\":%.2f,\"I\":%.2f,\"T\":%.1f,"
                      "\"S\":%.2f,\"H\":%.2f,\"St\":\"%s\"}",
                      export_V, export_I, bat_temp, bat_soc, bat_soh, s);
    
    coap_set_header_content_format(res, APPLICATION_JSON);
    coap_set_payload(res, buf, len);
}

/* CRITICAL: Periodic notify event handler */
static void res_state_periodic_handler(void) {
    /* This function is called by CoAP engine periodically for observable resources */
    coap_notify_observers(&res_dev_state);
}

/* Define resource WITH obs flag and periodic handler */
EVENT_RESOURCE(res_dev_state,
               "title=\"State\";obs",
               res_state_get_handler,
               NULL,
               NULL,
               NULL,
               res_state_periodic_handler);

static void res_power_put_handler(coap_message_t *req, coap_message_t *res, 
                                   uint8_t *buf, uint16_t size, int32_t *off) {
    if(current_state != STATE_RUNNING) { 
        LOG_WARN("[CMD] Rejected - Not in RUNNING state\n");
        coap_set_status_code(res, FORBIDDEN_4_03); 
        return; 
    }
    
    const uint8_t *chunk;
    int len = coap_get_payload(req, &chunk);
    
    if(len > 0 && len < 32) {
        char val[32]; 
        memcpy(val, chunk, len); 
        val[len] = '\0';
        
        float req_p = atof(val);
        
        /* Clamp a limiti fisici */
        if(req_p > BAT_MAX_POWER_W)  req_p = BAT_MAX_POWER_W;
        if(req_p < -BAT_MAX_POWER_W) req_p = -BAT_MAX_POWER_W;
        
        power_setpoint = req_p;
        
        LOG_INFO("[CMD] Power setpoint: %+6.2f W ", power_setpoint);
        if(power_setpoint > 0.5f) {
            LOG_INFO_("(Charging)\n");
        } else if(power_setpoint < -0.5f) {
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
RESOURCE(res_dev_power, "title=\"Power\"", NULL, NULL, res_power_put_handler, NULL);

/* --- /dev/params: lettura e modifica parametri batteria --- */
static void res_params_get_handler(coap_message_t *req, coap_message_t *res,
                                   uint8_t *buf, uint16_t size, int32_t *off);
static void res_params_put_handler(coap_message_t *req, coap_message_t *res,
                                   uint8_t *buf, uint16_t size, int32_t *off);

RESOURCE(res_dev_params,
         "title=\"Params\"",
         res_params_get_handler,
         NULL,
         res_params_put_handler,
         NULL);

static void
res_params_get_handler(coap_message_t *req, coap_message_t *res,
                       uint8_t *buf, uint16_t size, int32_t *off)
{
  /* JSON con stato attuale e soglie di sicurezza */
  int len = snprintf((char *)buf, size,
                     "{"
                     "\"id\":%d,"
                     "\"soc\":%.3f,"
                     "\"soh\":%.3f,"
                     "\"temp\":%.1f,"
                     "\"capacity_ah\":%.2f,"
                     "\"state\":\"%s\","
                     "\"soh_critical\":%.3f,"
                     "\"soh_warning\":%.3f,"
                     "\"temp_critical\":%.1f,"
                     "\"temp_warning\":%.1f,"
                     "\"cycles_warning\":%lu"
                     "}",
                     battery_id,
                     bat_soc,
                     bat_soh,
                     bat_temp,
                     bat_capacity_ah,
                     (current_state == STATE_RUNNING)  ? "RUN" :
                     (current_state == STATE_ISOLATED) ? "ISO" : "INI",
                     soh_critical,
                     soh_warning,
                     temp_critical,
                     temp_warning,
                     (unsigned long)cycles_warning);

  coap_set_header_content_format(res, APPLICATION_JSON);
  coap_set_payload(res, buf, len);
}

static void
res_params_put_handler(coap_message_t *req, coap_message_t *res,
                       uint8_t *buf, uint16_t size, int32_t *off)
{
  const uint8_t *payload;
  int len = coap_get_payload(req, &payload);
  if(len <= 0 || len >= 128) {
    coap_set_status_code(res, BAD_REQUEST_4_00);
    return;
  }

  char json[128];
  memcpy(json, payload, len);
  json[len] = '\0';

  /* helper per estrarre float dopo ":" */
  #define PARSE_FLOAT(_key, _var)                              \
    do {                                                       \
      char *p = strstr(json, _key);                            \
      if(p) {                                                  \
        char *c = strchr(p, ':');                              \
        if(c) {                                                \
          float v = atof(c + 1);                               \
          _var = v;                                            \
        }                                                      \
      }                                                        \
    } while(0)

  #define PARSE_UINT(_key, _var)                               \
    do {                                                       \
      char *p = strstr(json, _key);                            \
      if(p) {                                                  \
        char *c = strchr(p, ':');                              \
        if(c) {                                                \
          unsigned long v = strtoul(c + 1, NULL, 10);          \
          _var = (uint32_t)v;                                  \
        }                                                      \
      }                                                        \
    } while(0)

  /* Soglie di sicurezza */
  PARSE_FLOAT("\"soh_critical\"", soh_critical);
  PARSE_FLOAT("\"soh_warning\"",  soh_warning);
  PARSE_FLOAT("\"temp_critical\"", temp_critical);
  PARSE_FLOAT("\"temp_warning\"",  temp_warning);
  PARSE_UINT("\"cycles_warning\"", cycles_warning);

  /* Parametri di stato – opzionali (es. per testing) */
  PARSE_FLOAT("\"soc\"", bat_soc);
  PARSE_FLOAT("\"soh\"", bat_soh);
  PARSE_FLOAT("\"temp\"", bat_temp);
  PARSE_FLOAT("\"capacity_ah\"", bat_capacity_ah);

  /* Cambiare stato remoto (solo verso ISO per "staccare" la batteria) */
  char *st = strstr(json, "\"state\"");
  if(st) {
    char *c = strchr(st, ':');
    if(c) {
      /* ci aspettiamo "RUN","ISO","INI" */
      char *q = strchr(c, '"');
      if(q) {
        q++;
        if(strncmp(q, "ISO", 3) == 0) {
          current_state = STATE_ISOLATED;
          power_setpoint = 0.0f;
          bat_current = 0.0f;
          update_leds();
          coap_notify_observers(&res_dev_state);
        }

        if(strncmp(q, "ISO", 3) == 0) {
          current_state = STATE_ISOLATED;
          power_setpoint = 0.0f;
          update_leds();
          coap_notify_observers(&res_dev_state);
        }
        /* per sicurezza NON permettiamo di uscire da ISO via rete:
           il reset resta tramite bottone fisico */
      }
    }
  }

  #undef PARSE_FLOAT
  #undef PARSE_UINT

  coap_set_status_code(res, CHANGED_2_04);
}
 

/* Callback Registrazione */
static void reg_callback(coap_message_t *response) {
    if(response) {
        LOG_INFO("[INIT] Registration ACK received (Code: %d)\n", response->code);
        if(response->code == CREATED_2_01 || response->code == CHANGED_2_04) {
            LOG_INFO("[INIT] ✓ Registration SUCCESS\n");
            LOG_INFO("\n");
            current_state = STATE_RUNNING;
            leds_off(LEDS_YELLOW);
            leds_on(LEDS_BLUE);
            
            print_battery_status();
        } else {
            LOG_WARN("[INIT] Unexpected response code: %d\n", response->code);
        }
    } else {
        LOG_WARN("[INIT] ✗ Registration TIMEOUT - will retry\n");
    }
}

PROCESS(battery_controller, "Battery Controller");
AUTOSTART_PROCESSES(&battery_controller);

PROCESS_THREAD(battery_controller, ev, data) {
    static coap_endpoint_t server_ep;
    static coap_message_t request[1];
    static int retry_count = 0;
    
    PROCESS_BEGIN();
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║        HOME BATTERY ENERGY STORAGE SYSTEM v2.0                ║\n");
    printf("║        Li-ion Battery Pack: %.0f Ah @ 3.7V = %.1f kWh        ║\n",
           SCALED_CAPACITY_AH, SCALED_CAPACITY_AH * 3.7f / 1000.0f);
    printf("║        ML-based SoH Monitoring (NASA Dataset Scaled)          ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    LOG_INFO("[INIT] Initializing network stack...\n");
    etimer_set(&et_init_wait, CLOCK_SECOND * 3);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_init_wait));

    LOG_INFO("[INIT] Link-layer address: ");
    LOG_INFO_LLADDR(&linkaddr_node_addr);
    LOG_INFO_("\n");

    coap_activate_resource(&res_dev_state, "dev/state");
    coap_activate_resource(&res_dev_power, "dev/power");
    coap_activate_resource(&res_dev_params, "dev/params");

    LOG_INFO("[INIT] CoAP resources activated (dev/state is OBSERVABLE)\n");
    
    for(int i=0; i<ML_WINDOW*N_FEATURES; i++) {
        ml_buffer[i] = 0.5f;
    }

    LOG_INFO("[INIT] Starting registration to µGrid controller...\n");
    LOG_INFO("[INIT] Target endpoint: %s\n", UGRID_EP);
    
    LOG_INFO("[INIT] Waiting for RPL network to stabilize...\n");
    etimer_set(&et_init_wait, CLOCK_SECOND * 10);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_init_wait));
    
    while(current_state == STATE_INIT && retry_count < 10) {
        LOG_INFO("[INIT] Registration attempt #%d/%d\n", retry_count + 1, 10);
        
        if(retry_count > 0) {
            etimer_set(&et_init_wait, CLOCK_SECOND * 5);
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_init_wait));
        }
        update_leds();

        coap_endpoint_parse(UGRID_EP, strlen(UGRID_EP), &server_ep);

        memset(request, 0, sizeof(coap_message_t));
        coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
        coap_set_header_uri_path(request, "dev/register");
        
        static char payload[64];
        snprintf(payload, 64, "%d", battery_id);
        coap_set_payload(request, (uint8_t*)payload, strlen(payload));
        
        COAP_BLOCKING_REQUEST(&server_ep, request, reg_callback);
        
        retry_count++;
        
        if(current_state == STATE_RUNNING) {
            LOG_INFO("[INIT] Entering main control loop\n");
            break;
        }
    }
    
    if(current_state != STATE_RUNNING) {
        LOG_ERR("[INIT] ✗ Failed to register after %d attempts\n", retry_count);
        LOG_ERR("[INIT] System halted - check network connectivity\n");
        leds_on(LEDS_RED);
        PROCESS_EXIT();
    }

    /* LOOP PRINCIPALE CON NOTIFY PERIODICI */
    etimer_set(&et_loop, CLOCK_SECOND);
    etimer_set(&et_notify, CLOCK_SECOND * 5);  /* Notify ogni 5 secondi */
    
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
            
            etimer_reset(&et_loop);
        }
        
        /* PERIODIC NOTIFY - CRITICAL FOR OBSERVE */
        if(ev == PROCESS_EVENT_TIMER && data == &et_notify) {
            if(current_state == STATE_RUNNING) {
                res_state_periodic_handler();
            }
            etimer_reset(&et_notify);
        }
        
        if(ev == button_hal_release_event && current_state == STATE_ISOLATED) {
            LOG_INFO("\n");
            LOG_INFO("╔═══════════════════════════════════════════════════════════════╗\n");
            LOG_INFO("║              FACTORY RESET TRIGGERED                          ║\n");
            LOG_INFO("╠═══════════════════════════════════════════════════════════════╣\n");
            LOG_INFO("║ Resetting battery to factory conditions...                   ║\n");
            LOG_INFO("╚═══════════════════════════════════════════════════════════════╝\n");
            LOG_INFO("\n");
            
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
