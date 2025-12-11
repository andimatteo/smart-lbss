#include "contiki.h"
#include "coap-engine.h"
#include "coap-blocking-api.h"
#include "coap-observe-client.h" 
#include "dev/leds.h"
#include "sys/etimer.h"
#include "sys/log.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "power_predictor_model.h"
#include "project-conf.h"

#define LOG_MODULE "uGrid"
#define LOG_LEVEL LOG_LEVEL_INFO

#define MAX_BATTERIES 5

typedef struct {
    uip_ipaddr_t ip;
    bool active;
    bool obs_requested;

    float current_soc;
    float current_voltage;
    float current_temp;
    float current_soh;
    float current_current;      /* new: corrente I [A] */
    float optimal_u;            /* setpoint ottimo [kW] */
    float actual_power;         /* potenza misurata [kW] */

    char  state[4];             /* "RUN","ISO","INI" */

    bool  has_objective;
    float objective_power;
    uint32_t last_update_time;
    coap_observee_t *obs; 
} battery_node_t;

static battery_node_t batteries[MAX_BATTERIES];
static int battery_count = 0;

/* MPC Params – modificabili da remoto via /ctrl/mpc */
static float alpha = 1.0f;
static float beta  = 1.0f;
static float gama = 20.0f;
static float price = 0.25f;

#define K_FACT          0.05f
#define SOC_REF         0.5f
#define LEARNING_RATE   0.1f
#define PGD_ITERATIONS  100


#define ML_PRED_WINDOW 20
#define N_PRED_FEAT 5 
static float pred_buffer[ML_PRED_WINDOW * N_PRED_FEAT];

static float curr_load = 2.0f;
static float curr_pv = 0.0f;
static float curr_hour = 6.0f;

static float cloud_cover = 0.3f;
static bool is_sunny_day = true;
static float base_load = 2.0f;
static bool high_demand_period = false;

extern coap_resource_t res_ugrid_state, res_register;
static struct etimer et_compute, et_observe_setup;

static void res_obj_get_handler(coap_message_t *req, coap_message_t *res,
                                uint8_t *buf, uint16_t size, int32_t *off);
static void res_obj_put_handler(coap_message_t *req, coap_message_t *res,
                                uint8_t *buf, uint16_t size, int32_t *off);

RESOURCE(res_obj_ctrl,
         "title=\"Objectives\"",
         res_obj_get_handler,
         NULL,
         res_obj_put_handler,
         NULL);

static void
res_obj_get_handler(coap_message_t *req, coap_message_t *res,
                    uint8_t *buf, uint16_t size, int32_t *off)
{
  int len = 0;
  len += snprintf((char *)buf + len, size - len, "{ \"bats\":[");
  bool first = true;

  for(int i = 0; i < battery_count && len < (int)size - 64; i++) {
    if(!batteries[i].active) continue;
    if(!first) {
      len += snprintf((char *)buf + len, size - len, ",");
    }
    first = false;

    len += snprintf((char *)buf + len, size - len,
                    "{"
                    "\"idx\":%d,"
                    "\"has_obj\":%d,"
                    "\"power_kw\":%.2f"
                    "}",
                    i,
                    batteries[i].has_objective ? 1 : 0,
                    batteries[i].objective_power);
  }

  len += snprintf((char *)buf + len, size - len, "]}");

  coap_set_header_content_format(res, APPLICATION_JSON);
  coap_set_payload(res, buf, len);
}

static void
res_obj_put_handler(coap_message_t *req, coap_message_t *res,
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

  int idx = -1;
  float power_kw = 0.0f;
  int has_power = 0;
  int clear = 0;

#define PARSE_INT_FIELD(key, var)                             \
  do {                                                        \
    char *p = strstr(json, key);                              \
    if(p) {                                                   \
      char *c = strchr(p, ':');                               \
      if(c) {                                                 \
        var = (int)strtol(c + 1, NULL, 10);                   \
      }                                                       \
    }                                                         \
  } while(0)

#define PARSE_FLOAT_FIELD(key, var, flag)                     \
  do {                                                        \
    char *p = strstr(json, key);                              \
    if(p) {                                                   \
      char *c = strchr(p, ':');                               \
      if(c) {                                                 \
        var = (float)atof(c + 1);                             \
        flag = 1;                                             \
      }                                                       \
    }                                                         \
  } while(0)

  PARSE_INT_FIELD("\"idx\"", idx);
  PARSE_INT_FIELD("\"clear\"", clear);
  PARSE_FLOAT_FIELD("\"power_kw\"", power_kw, has_power);

#undef PARSE_INT_FIELD
#undef PARSE_FLOAT_FIELD

  if(idx < 0 || idx >= battery_count || !batteries[idx].active) {
    LOG_WARN("[OBJ] Invalid idx=%d in ctrl/obj\n", idx);
    coap_set_status_code(res, BAD_REQUEST_4_00);
    return;
  }

  if(clear) {
    batteries[idx].has_objective = false;
    batteries[idx].objective_power = 0.0f;
    LOG_INFO("[OBJ] Cleared objective for Bat #%d\n", idx);
    coap_set_status_code(res, CHANGED_2_04);
    return;
  }

  if(!has_power) {
    LOG_WARN("[OBJ] Missing power_kw for Bat #%d\n", idx);
    coap_set_status_code(res, BAD_REQUEST_4_00);
    return;
  }

  /* Clamp a potenza massima fisica */
    if(power_kw > BAT_MAX_POWER_KW)  power_kw = BAT_MAX_POWER_KW;
    if(power_kw < -BAT_MAX_POWER_KW) power_kw = -BAT_MAX_POWER_KW;

  batteries[idx].has_objective   = true;
  batteries[idx].objective_power = power_kw;

  LOG_INFO("[OBJ] Set objective for Bat #%d: %.2f kW\n", idx, power_kw);

  coap_set_status_code(res, CHANGED_2_04);
}


PROCESS_NAME(ugrid_controller);

static void print_battery_status(void) {
    LOG_INFO("\n");
    LOG_INFO("╔════════════════════════════════════════════════════════════════════════╗\n");
    LOG_INFO("║                      BATTERY FLEET STATUS                             ║\n");
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Total batteries registered: %d/%d                                      ║\n", 
             battery_count, MAX_BATTERIES);
    LOG_INFO("╚════════════════════════════════════════════════════════════════════════╝\n");
    
    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) continue;
        
        uint32_t time_since_update = clock_seconds() - batteries[i].last_update_time;
        
        LOG_INFO("\n");
        LOG_INFO("┌─ Battery #%d ────────────────────────────────────────────────────────┐\n", i);
        LOG_INFO("│ IPv6 Address:    ");
        LOG_INFO_6ADDR(&batteries[i].ip);
        LOG_INFO_("                      │\n");
        LOG_INFO("│ Last Update:     %lu seconds ago                                  │\n", 
                 (unsigned long)time_since_update);
        LOG_INFO("├──────────────────────────────────────────────────────────────────────┤\n");
        LOG_INFO("│ State of Charge: %5.1f%%     Voltage: %4.2fV                       │\n", 
                 batteries[i].current_soc * 100.0f, batteries[i].current_voltage);
        LOG_INFO("│ Temperature:     %4.1f°C     SoH:     %5.1f%%                       │\n", 
                 batteries[i].current_temp, batteries[i].current_soh * 100.0f);
        LOG_INFO("├──────────────────────────────────────────────────────────────────────┤\n");
        LOG_INFO("│ Optimal Power:   %+7.2f kW                                        │\n", 
                 batteries[i].optimal_u);
        LOG_INFO("│ Actual Power:    %+7.2f kW                                        │\n", 
                 batteries[i].actual_power);
        
        float error = batteries[i].actual_power - batteries[i].optimal_u;
        LOG_INFO("│ Tracking Error:  %+7.2f kW ", error);
        if(fabs(error) < 0.5f) {
            LOG_INFO_("✓                                   │\n");
        } else if(fabs(error) < 1.0f) {
            LOG_INFO_("⚠                                   │\n");
        } else {
            LOG_INFO_("✗                                   │\n");
        }
        LOG_INFO("└──────────────────────────────────────────────────────────────────────┘\n");
    }
}

static void update_env() {
    curr_hour += 0.5f; 
    if(curr_hour >= 24.0f) {
        curr_hour = 0.0f;
        is_sunny_day = (random_rand() % 100) > 30;
    }

    /* PV Generation */
    float base_irradiance_for_ml = 0.0f;
    if (curr_hour >= 6.0f && curr_hour < 18.0f) {
        float sun_elevation = sin(3.14159f * (curr_hour - 6.0f) / 12.0f);
        base_irradiance_for_ml = 1000.0f * sun_elevation;
        
        cloud_cover += ((random_rand() % 100) / 50.0f - 1.0f) * 0.15f;
        if(cloud_cover < 0.0f) cloud_cover = 0.0f;
        if(cloud_cover > 0.95f) cloud_cover = 0.95f;
        
        if(!is_sunny_day) {
            cloud_cover = 0.5f + (cloud_cover * 0.5f);
        }
        
        float cloud_factor = 1.0f - (cloud_cover * 0.85f);
        float turbulence = 1.0f;
        if(cloud_cover > 0.3f) {
            turbulence = 0.7f + ((random_rand() % 100) / 100.0f * 0.6f);
        }
        
        float effective_irradiance = base_irradiance_for_ml * cloud_factor * turbulence;
        float pv_peak = BAT_MAX_POWER_KW;
        
        curr_pv = (pv_peak * effective_irradiance / 1000.0f);
        curr_pv += ((random_rand() % 100) / 100.0f - 0.5f) * 0.3f;
        
        if(curr_pv < 0.0f) curr_pv = 0.0f;
        if(curr_pv > pv_peak) curr_pv = pv_peak;
    } else {
        curr_pv = 0.0f;
        cloud_cover = 0.3f;
    }

    /* Load Generation */
    float hour_factor = 1.0f;
    
    if(curr_hour >= 0.0f && curr_hour < 6.0f) {
        hour_factor = 0.3f + ((random_rand() % 20) / 100.0f);
        high_demand_period = false;
    } 
    else if(curr_hour >= 6.0f && curr_hour < 9.0f) {
        float morning_ramp = (curr_hour - 6.0f) / 3.0f;
        hour_factor = 0.5f + (morning_ramp * 0.7f);
        high_demand_period = (curr_hour >= 7.0f && curr_hour <= 8.5f);
    }
    else if(curr_hour >= 9.0f && curr_hour < 12.0f) {
        hour_factor = 0.9f + ((random_rand() % 30) / 100.0f);
        high_demand_period = false;
    }
    else if(curr_hour >= 12.0f && curr_hour < 14.0f) {
        hour_factor = 1.1f + ((random_rand() % 20) / 100.0f);
        high_demand_period = true;
    }
    else if(curr_hour >= 14.0f && curr_hour < 17.0f) {
        hour_factor = 0.7f + ((random_rand() % 30) / 100.0f);
        high_demand_period = false;
    }
    else if(curr_hour >= 17.0f && curr_hour < 21.0f) {
        hour_factor = 1.3f + ((random_rand() % 40) / 100.0f);
        high_demand_period = true;
    }
    else {
        float evening_ramp = 1.0f - ((curr_hour - 21.0f) / 3.0f);
        hour_factor = 0.4f + (evening_ramp * 0.6f);
        high_demand_period = false;
    }
    
    float event_load = 0.0f;
    if((random_rand() % 100) < 15) {
        event_load = ((random_rand() % 30) / 10.0f) + 1.0f;
    }
    
    base_load = 2.5f;
    curr_load = (base_load * hour_factor) + event_load;
    curr_load += ((random_rand() % 100) / 100.0f - 0.5f) * 0.4f;
    
    if(curr_load < 0.5f) curr_load = 0.5f; 
    if(curr_load > BAT_MAX_POWER_KW * 0.8f) curr_load = BAT_MAX_POWER_KW * 0.8f;

    /* Update ML buffer */
    for(int i=0; i<(ML_PRED_WINDOW-1)*N_PRED_FEAT; i++) {
        pred_buffer[i] = pred_buffer[i+N_PRED_FEAT];
    }
    int idx = (ML_PRED_WINDOW-1)*N_PRED_FEAT;
    pred_buffer[idx] = curr_pv/BAT_MAX_POWER_KW; 
    pred_buffer[idx+1] = curr_load/BAT_MAX_POWER_KW;
    pred_buffer[idx+2] = cloud_cover; 
    pred_buffer[idx+3] = (base_irradiance_for_ml / MAX_IRR); 
    pred_buffer[idx+4] = curr_hour/24.0f;
    
    /* Logging */
    LOG_INFO("\n");
    LOG_INFO("╔════════════════════════════════════════════════════════════════════════╗\n");
    LOG_INFO("║                    MICROGRID ENVIRONMENT UPDATE                       ║\n");
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Timestamp:       %lu seconds                                          ║\n", 
             (unsigned long)clock_seconds());
    LOG_INFO("║ Time of Day:     %02d:%02d (Hour %.1f)                                     ║\n", 
             (int)curr_hour, (int)((curr_hour - (int)curr_hour) * 60), curr_hour);
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    
    LOG_INFO("║ PV Generation:   %6.2f kW ", curr_pv);
    if(curr_hour >= 6.0f && curr_hour < 18.0f) {
        if(cloud_cover < 0.3f) {
            LOG_INFO_("(Clear sky ☀)                    ║\n");
        } else if(cloud_cover < 0.6f) {
            LOG_INFO_("(Partly cloudy ⛅)               ║\n");
        } else {
            LOG_INFO_("(Overcast ☁)                    ║\n");
        }
        LOG_INFO("║   - Cloud cover: %3.0f%%                                                ║\n", 
                 cloud_cover * 100.0f);
    } else {
        LOG_INFO_("(Night 🌙)                       ║\n");
    }
    
    LOG_INFO("║ Load Demand:     %6.2f kW ", curr_load);
    if(high_demand_period) {
        LOG_INFO_("(Peak period 📈)                 ║\n");
    } else if(curr_hour >= 0.0f && curr_hour < 6.0f) {
        LOG_INFO_("(Low demand 💤)                  ║\n");
    } else {
        LOG_INFO_("(Normal 📊)                      ║\n");
    }
    
    if(event_load > 0.5f) {
        LOG_INFO("║   - Event load:  %6.2f kW (Appliance active)                       ║\n", 
                 event_load);
    }
    
    float net_power = curr_pv - curr_load;
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Net Power:       %+6.2f kW ", net_power);
    if(net_power > 1.0f) {
        LOG_INFO_("(SURPLUS ⚡ Charge batteries!)    ║\n");
    } else if(net_power < -1.0f) {
        LOG_INFO_("(DEFICIT 🔋 Discharge batteries!) ║\n");
    } else {
        LOG_INFO_("(Balanced ⚖)                      ║\n");
    }
    LOG_INFO("╚════════════════════════════════════════════════════════════════════════╝\n");
}

static void run_mpc() {
    leds_on(LEDS_BLUE);
    
    LOG_INFO("\n");
    LOG_INFO("╔════════════════════════════════════════════════════════════════════════╗\n");
    LOG_INFO("║                      MPC OPTIMIZATION CYCLE                            ║\n");
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    
    float pred_net = power_predictor_regress1(pred_buffer, ML_PRED_WINDOW * N_PRED_FEAT);
    LOG_INFO("║ ML Prediction:   Net power = %+6.2f kW                              ║\n", pred_net);
    
    float total_available_power = 0.0f;
    float avg_soc = 0.0f;
    for(int i = 0; i < battery_count; i++) {
        if(batteries[i].active) {
            total_available_power += BAT_MAX_POWER_KW;
            avg_soc += batteries[i].current_soc;
        }
    }
    if(battery_count > 0) avg_soc /= battery_count;
    
    LOG_INFO("║ Fleet Avg SoC:   %.1f%%                                                ║\n", 
             avg_soc * 100.0f);
    LOG_INFO("║ Total Capacity:  %.2f kW (%.0f%% available)                           ║\n", 
             total_available_power, (total_available_power / (MAX_BATTERIES * BAT_MAX_POWER_KW)) * 100.0f);
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Running Projected Gradient Descent (%d iterations)...                ║\n", 
             PGD_ITERATIONS);
    
    float initial_u[MAX_BATTERIES];
    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) {
            initial_u[i] = 0.0f;
            continue;
        }

        /* Se c'è un obiettivo, l'ottimo logico è già quello */
        if(batteries[i].has_objective) {
            batteries[i].optimal_u = batteries[i].objective_power;
        }

        initial_u[i] = batteries[i].optimal_u;
    }
    
    /* PGD: aggiorna SOLO le batterie senza obiettivo manuale */
    for(int iter = 0; iter < PGD_ITERATIONS; iter++) {
        for(int i = 0; i < battery_count; i++) {
            if(!batteries[i].active) continue;
            if(strcmp(batteries[i].state, "ISO") == 0) continue;
            if(batteries[i].has_objective) continue;   /* non toccare se c'è un obj */

            float u = batteries[i].optimal_u;
            float soc_term = batteries[i].current_soc + (K_FACT * u) - SOC_REF;
            float grad = (alpha * price) + (2.0f * beta  * u) + (2.0f * gama * K_FACT * soc_term);

            u = u - (LEARNING_RATE * grad);
            if(u > BAT_MAX_POWER_KW)  u = BAT_MAX_POWER_KW;
            if(u < -BAT_MAX_POWER_KW) u = -BAT_MAX_POWER_KW;
            
            batteries[i].optimal_u = u;
        }
    }
    
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Optimization Results:                                                  ║\n");
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    
    float total_command = 0.0f;
    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) continue;
        
        float cmd_kw = batteries[i].has_objective
                       ? batteries[i].objective_power
                       : batteries[i].optimal_u;

        float delta_u = cmd_kw - initial_u[i];
        total_command += cmd_kw;
        
        LOG_INFO("║ Bat #%d: %+7.2f kW  (Δ = %+6.2f kW, SoC = %5.1f%%)  [%s]         ║\n", 
                 i,
                 cmd_kw,
                 delta_u,
                 batteries[i].current_soc * 100.0f,
                 batteries[i].has_objective ? "OBJ" : "MPC");
    }
    
    LOG_INFO("╠════════════════════════════════════════════════════════════════════════╣\n");
    LOG_INFO("║ Total Command:   %+7.2f kW                                           ║\n", 
             total_command);
    
    float expected_grid = curr_load - curr_pv + total_command;
    LOG_INFO("║ Expected Grid:   %+7.2f kW ", expected_grid);
    if(fabs(expected_grid) < 0.5f) {
        LOG_INFO_("(Grid balanced ✓)               ║\n");
    } else if(expected_grid > 0) {
        LOG_INFO_("(Import from grid)              ║\n");
    } else {
        LOG_INFO_("(Export to grid)                ║\n");
    }
    LOG_INFO("╚════════════════════════════════════════════════════════════════════════╝\n");
    
    leds_off(LEDS_BLUE);
}


static void
battery_notification_handler(coap_observee_t *obs,
                             void *notification,
                             coap_notification_flag_t flag)
{
  if(notification) {
    const uint8_t *payload = NULL;
    int len = coap_get_payload(notification, &payload);

    if(len > 0) {
      char buf[128];
      if(len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
      }
      memcpy(buf, payload, len);
      buf[len] = '\0';

      /* JSON esempio: {"V":3.70,"I":-2.50,"T":28.5,"S":0.45,"H":0.98,"St":"RUN"} */
      float voltage = 0.0f, current = 0.0f, temp = 0.0f, soc = 0.0f, soh = 0.0f;

      char *v_ptr = strstr(buf, "\"V\":");
      char *i_ptr = strstr(buf, "\"I\":");
      char *t_ptr = strstr(buf, "\"T\":");
      char *s_ptr = strstr(buf, "\"S\":");
      char *h_ptr = strstr(buf, "\"H\":");
      char *st_ptr = strstr(buf, "\"St\":\"");

      if(v_ptr) voltage = (float)atof(v_ptr + 4);
      if(i_ptr) current = (float)atof(i_ptr + 4);
      if(t_ptr) temp    = (float)atof(t_ptr + 4);
      if(s_ptr) soc     = (float)atof(s_ptr + 4);
      if(h_ptr) soh     = (float)atof(h_ptr + 4);

      for(int i = 0; i < battery_count; i++) {
        if(uip_ipaddr_cmp(&batteries[i].ip, &obs->endpoint.ipaddr)) {
          batteries[i].current_soc     = soc;
          batteries[i].current_voltage = voltage;
          batteries[i].current_temp    = temp;
          batteries[i].current_soh     = soh;
          batteries[i].current_current = current;

          /* Potenza in kW da V * I in Watt */
          batteries[i].actual_power = (voltage * current) / 1000.0f;
          batteries[i].last_update_time = clock_seconds();

          if(st_ptr) {
            /* Copia "RUN","ISO","INI" (max 3 caratteri) */
            char *start = st_ptr + strlen("\"St\":\"");
            int j = 0;
            while(start[j] != '"' && start[j] != '\0' && j < 3) {
              batteries[i].state[j] = start[j];
              j++;
            }
            batteries[i].state[j] = '\0';
          }

          LOG_INFO("[OBSERVE] Bat #%d updated: SoC=%.1f%%, V=%.2fV, I=%+.2fA,"
                   " P=%+.2fkW, st=%s\n",
                   i, soc * 100.0f, voltage, current,
                   batteries[i].actual_power,
                   batteries[i].state);

          break;
        }
      }
    }
  } else {
    LOG_WARN("[OBSERVE] Notification handler called with NULL notification (flag=%d)\n",
             flag);
  }
}


static void res_reg_h(coap_message_t *req, coap_message_t *res, uint8_t *buf, uint16_t size, int32_t *off) {
    LOG_INFO(">>> [REGISTRY] Received registration from ");
    LOG_INFO_6ADDR(&req->src_ep->ipaddr);
    LOG_INFO_("\n");
    
    if(battery_count < MAX_BATTERIES) {
        uip_ipaddr_copy(&batteries[battery_count].ip, &req->src_ep->ipaddr);
        batteries[battery_count].current_soc = 0.5f;
        batteries[battery_count].current_voltage = 0.0f;
        batteries[battery_count].current_temp = 25.0f;
        batteries[battery_count].current_soh = 1.0f;
        batteries[battery_count].current_current = 0.0f;
        batteries[battery_count].optimal_u = 0.0f;
        batteries[battery_count].actual_power = 0.0f;
        strcpy(batteries[battery_count].state, "INI");

        batteries[battery_count].active = true;
        batteries[battery_count].obs_requested = false;
        batteries[battery_count].current_soc = 0.5f;
        batteries[battery_count].optimal_u = 0.0f;
        batteries[battery_count].actual_power = 0.0f;  /* Initialize to 0 */
        batteries[battery_count].last_update_time = clock_seconds();  /* Set initial time */
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
RESOURCE(res_register, "title=\"Reg\"", NULL, res_reg_h, NULL, NULL);

static void res_mpc_get_handler(coap_message_t *req, coap_message_t *res,
                                uint8_t *buf, uint16_t size, int32_t *off);
static void res_mpc_put_handler(coap_message_t *req, coap_message_t *res,
                                uint8_t *buf, uint16_t size, int32_t *off);

// change mpc params dynamically
RESOURCE(res_mpc_params,
         "title=\"MPC params\"",
         res_mpc_get_handler,
         NULL,
         res_mpc_put_handler,
         NULL);

static void
res_mpc_get_handler(coap_message_t *req, coap_message_t *res,
                    uint8_t *buf, uint16_t size, int32_t *off)
{
  int len = snprintf((char *)buf, size,
                     "{"
                     "\"alpha\":%.3f,"
                     "\"beta\":%.3f,"
                     "\"gama\":%.3f,"
                     "\"price\":%.3f"
                     "}",
                     alpha, beta, gama, price);

  coap_set_header_content_format(res, APPLICATION_JSON);
  coap_set_payload(res, buf, len);
}

static void
res_mpc_put_handler(coap_message_t *req, coap_message_t *res,
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

  #define PARSE_FLOAT(_key, _var)                              \
    do {                                                       \
      char *p = strstr(json, _key);                            \
      if(p) {                                                  \
        char *c = strchr(p, ':');                              \
        if(c) {                                                \
          float v = (float)atof(c + 1);                        \
          _var = v;                                            \
        }                                                      \
      }                                                        \
    } while(0)

  PARSE_FLOAT("\"alpha\"", alpha);
  PARSE_FLOAT("\"beta\"",  beta);
  PARSE_FLOAT("\"gama\"", gama);
  PARSE_FLOAT("\"price\"", price);

  #undef PARSE_FLOAT

  LOG_INFO("[MPC] Updated params: alpha=%.3f beta=%.3f gama=%.3f price=%.3f\n",
           alpha, beta, gama, price);

  coap_set_status_code(res, CHANGED_2_04);
}

static void
res_state_h(coap_message_t *req, coap_message_t *res,
            uint8_t *buf, uint16_t size, int32_t *off)
{
  int len = 0;

  /* curr_load e curr_pv sono già in kW */
  float load_kw = curr_load;
  float pv_kw   = curr_pv;

  len += snprintf((char *)buf + len, size - len,
                  "{"
                  "\"cnt\":%d,"
                  "\"load_kw\":%.2f,"
                  "\"pv_kw\":%.2f,"
                  "\"bats\":[",
                  battery_count, load_kw, pv_kw);

  for(int i = 0; i < battery_count && len < (int)size - 64; i++) {
    if(!batteries[i].active) continue;   /* meglio saltare quelle inattive */

    if(i > 0) {
      len += snprintf((char *)buf + len, size - len, ",");
    }

    char ipbuf[UIPLIB_IPV6_MAX_STR_LEN];
    uiplib_ipaddr_snprint(ipbuf, sizeof(ipbuf), &batteries[i].ip);

    len += snprintf((char *)buf + len, size - len,
                    "{"
                    "\"idx\":%d,"
                    "\"ip\":\"%s\","
                    "\"u\":%.2f,"
                    "\"obj\":%.2f,"
                    "\"has_obj\":%d,"
                    "\"soc\":%.2f,"
                    "\"p\":%.2f,"
                    "\"V\":%.2f,"
                    "\"I\":%.2f,"
                    "\"temp\":%.1f,"
                    "\"soh\":%.2f,"
                    "\"state\":\"%s\""
                    "}",
                    i,
                    ipbuf,
                    batteries[i].optimal_u,
                    batteries[i].objective_power,
                    batteries[i].has_objective ? 1 : 0,
                    batteries[i].current_soc,
                    batteries[i].actual_power,
                    batteries[i].current_voltage,
                    batteries[i].current_current,
                    batteries[i].current_temp,
                    batteries[i].current_soh,
                    batteries[i].state);
  }

  len += snprintf((char *)buf + len, size - len, "]}");

  coap_set_header_content_format(res, APPLICATION_JSON);
  coap_set_payload(res, buf, len);
}
RESOURCE(res_ugrid_state, "title=\"State\"", res_state_h, NULL, NULL, NULL);

static void empty_cb(coap_message_t *response) {
    /* Silent callback */
}

PROCESS(ugrid_controller, "uGrid");
AUTOSTART_PROCESSES(&ugrid_controller);

PROCESS_THREAD(ugrid_controller, ev, data) {
    static coap_endpoint_t ep;
    static coap_message_t req[1];
    static char pl[16];
    static int i; 

    PROCESS_BEGIN();
    
    printf("\n*** UGRID CONTROLLER STARTED ***\n");
    
    LOG_INFO("[INIT] Waiting for network stack initialization...\n");
    etimer_set(&et_compute, CLOCK_SECOND * 3);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_compute));
    
    leds_on(LEDS_GREEN);
    
    coap_activate_resource(&res_register, "dev/register");
    coap_activate_resource(&res_ugrid_state, "dev/state");
    coap_activate_resource(&res_mpc_params, "ctrl/mpc");
    coap_activate_resource(&res_obj_ctrl, "ctrl/obj");

    
    LOG_INFO("[INIT] CoAP resources activated\n");
    LOG_INFO("[INIT] Ready to accept battery registrations\n");
    LOG_INFO("\n");
    
    etimer_set(&et_compute, CLOCK_SECOND * 5);
    etimer_set(&et_observe_setup, CLOCK_SECOND * 2);
    
    while(1) {
        PROCESS_WAIT_EVENT();
        
        if(ev == PROCESS_EVENT_TIMER && data == &et_compute) {
            update_env(); 
            run_mpc(); 
            
            LOG_INFO("\n");
            LOG_INFO("╔════════════════════════════════════════════════════════════════════════╗\n");
            LOG_INFO("║                      ACTUATION COMMANDS                                ║\n");
            LOG_INFO("╚════════════════════════════════════════════════════════════════════════╝\n");
            for(i = 0; i < battery_count; i++) {
                if(!batteries[i].active) continue;
                
                if(strcmp(batteries[i].state, "ISO") == 0) {
                    LOG_INFO("  → Bat #%d: state=ISO, skipping command\n", i);
                    continue;
                }

                memset(&ep, 0, sizeof(ep));
                memset(req, 0, sizeof(coap_message_t));

                uip_ipaddr_copy(&ep.ipaddr, &batteries[i].ip);
                ep.port = UIP_HTONS(COAP_DEFAULT_PORT);
                
                coap_init_message(req, COAP_TYPE_CON, COAP_PUT, 0);
                coap_set_header_uri_path(req, "dev/power");
                
                float cmd_kw = batteries[i].has_objective
                   ? batteries[i].objective_power
                   : batteries[i].optimal_u;

                /* CONVERSIONE CORRETTA: kW → W per il BatteryController */
                float power_watts = cmd_kw * 1000.0f;

                snprintf(pl, sizeof(pl), "%.2f", power_watts);
                coap_set_payload(req, (uint8_t*)pl, strlen(pl));

                LOG_INFO("  → Bat #%d: [%s] Commanding %+7.2f kW (%+.0f W)\n",
                         i,
                         batteries[i].has_objective ? "OBJ" : "MPC",
                         cmd_kw,
                         power_watts);
                
                COAP_BLOCKING_REQUEST(&ep, req, empty_cb);
            }


            print_battery_status();

            etimer_reset(&et_compute);
        }
        
        /* Setup observe */
        if((ev == PROCESS_EVENT_TIMER && data == &et_observe_setup) || 
           ev == PROCESS_EVENT_MSG) {
            
            for(i = 0; i < battery_count; i++) {
                if(batteries[i].active && !batteries[i].obs_requested) {
                    memset(&ep, 0, sizeof(ep));
                    uip_ipaddr_copy(&ep.ipaddr, &batteries[i].ip);
                    ep.port = UIP_HTONS(COAP_DEFAULT_PORT);

                    LOG_INFO("[OBSERVE] Setting up observation for Battery #%d: ", i);
                    LOG_INFO_6ADDR(&batteries[i].ip);
                    LOG_INFO_("\n");
                    
                    batteries[i].obs = coap_obs_request_registration(
                        &ep, 
                        "dev/state", 
                        battery_notification_handler, 
                        NULL
                    );
                    
                    if(batteries[i].obs != NULL) {
                        LOG_INFO("[OBSERVE] ✓ Observation registered successfully for Battery #%d\n", i);
                    } else {
                        LOG_WARN("[OBSERVE] ✗ Failed to register observation for Battery #%d\n", i);
                    }
                    
                    batteries[i].obs_requested = true;
                }
            }
            
            etimer_reset(&et_observe_setup);
        }
    }
    PROCESS_END();
}
