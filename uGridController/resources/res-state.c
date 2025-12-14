#include "contiki.h"
#include "coap.h"
#include "coap-engine.h"
#include "cbor.h"
#include <stdint.h>
#include <math.h>

#include "../../includes/utility.h"

extern int battery_count;
extern float curr_load;
extern float curr_pv;
extern battery_node_t batteries[];

    static void
res_get_state_h(coap_message_t *req, coap_message_t *res,
        uint8_t *buf, uint16_t size, int32_t *off)
{
    (void)req; (void)off;

    cbor_writer_state_t ws;
    cbor_init_writer(&ws, buf, size);

    const int load_c = (int)lroundf(curr_load * 100.0f);
    const int pv_c   = (int)lroundf(curr_pv   * 100.0f);

    /* conta solo attive (coerente con bats[]) */
    int active_cnt = 0;
    for(int i = 0; i < battery_count; i++) {
        if(batteries[i].active) active_cnt++;
    }

    cbor_open_map(&ws);

    cbor_write_unsigned(&ws, 0); cbor_write_unsigned(&ws, (uint64_t)active_cnt);
    cbor_write_unsigned(&ws, 1); cbor_write_signed(&ws,  (int64_t)load_c);
    cbor_write_unsigned(&ws, 2); cbor_write_signed(&ws,  (int64_t)pv_c);

    cbor_write_unsigned(&ws, 3);
    cbor_open_array(&ws);

    for(int i = 0; i < battery_count; i++) {
        if(!batteries[i].active) continue;

        const int u_c = (int)lroundf(batteries[i].optimal_u       * 100.0f);
        const int S_c = (int)lroundf(batteries[i].current_soc     * 100.0f); /* 0..1 -> 2 dec */
        const int p_c = (int)lroundf(batteries[i].actual_power    * 100.0f); /* kW */
        const int V_c = (int)lroundf(batteries[i].current_voltage * 100.0f);
        const int I_c = (int)lroundf(batteries[i].current_current * 100.0f);
        const int T_c = (int)lroundf(batteries[i].current_temp    * 100.0f);
        const int H_c = (int)lroundf(batteries[i].current_soh     * 100.0f);

        /* st: 0/1/2 */
        const uint64_t st = (uint64_t)batteries[i].state;

        cbor_open_array(&ws);
        cbor_write_unsigned(&ws, (uint64_t)i);
        cbor_write_signed(&ws,  (int64_t)u_c);
        cbor_write_signed(&ws,  (int64_t)S_c);
        cbor_write_signed(&ws,  (int64_t)p_c);
        cbor_write_signed(&ws,  (int64_t)V_c);
        cbor_write_signed(&ws,  (int64_t)I_c);
        cbor_write_signed(&ws,  (int64_t)T_c);
        cbor_write_signed(&ws,  (int64_t)H_c);
        cbor_write_unsigned(&ws, st);
        cbor_close_array(&ws);
    }

    cbor_close_array(&ws);
    cbor_close_map(&ws);

    const size_t out_len = cbor_end_writer(&ws);
    if(out_len == 0) {
        coap_set_status_code(res, INTERNAL_SERVER_ERROR_5_00);
        return;
    }

    coap_set_header_content_format(res, APPLICATION_CBOR);
    coap_set_payload(res, buf, (uint16_t)out_len);
}

RESOURCE(res_ugrid_state, "title=\"State\"", res_get_state_h, NULL, NULL, NULL);

