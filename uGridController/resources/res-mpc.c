#include "contiki.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "coap-engine.h"
#include "coap.h"
#include "../../includes/constants.h"
#include "../../includes/utility.h"
#include "sys/log.h"

extern battery_node_t batteries[];
extern int battery_count;

#define LOG_MODULE "mcp"
#define LOG_LEVEL LOG_LEVEL_INFO

extern float alpha;
extern float beta;
extern float gama;
extern float price;

// change mpc params dynamically
static void
res_mpc_get_handler(coap_message_t *req, coap_message_t *res,
        uint8_t *buf, uint16_t size, int32_t *off)
{
    int len = snprintf((char *)buf, size,
            "{"
            "\"a\":%d.%d,"
            "\"b\":%d.%d,"
            "\"g\":%d.%d,"
            "\"p\":%d.%d"
            "}",
            (int)alpha, ((int)(alpha) * 100) % 100,
            (int)beta, ((int)(beta) * 100) % 100,
            (int)gama, ((int)(gama) * 100) % 100,
            (int)price, ((int)(price) * 100) % 100);

    coap_set_header_content_format(res, APPLICATION_JSON);
    coap_set_payload(res, buf, len);
}

static void
res_mpc_put_handler(coap_message_t *req, coap_message_t *res,
        uint8_t *buf, uint16_t size, int32_t *off)
{
    const uint8_t *payload;
    coap_get_payload(req, &payload);

    static int a,b,c,p;

    sscanf((char*)payload, "{\"a\":%d, \"b\":%d, \"g\":%d, \"p\":%d}", &a, &b, &c, &p);

    alpha = (float)a / 100.0f;
    beta = (float)b / 100.0f;
    gama = (float)c / 100.0f;
    price = (float)p / 100.0f;

    LOG_INFO("[MPC] Updated params: alpha=%d.%d beta=%d.%d gama=%d.%d price=%d.%d\n",
            (int)(alpha), ((int)(alpha) * 100) % 100,
            (int)(beta), ((int)(beta) * 100) % 100,
            (int)(gama), ((int)(gama) * 100) % 100,
            (int)(price), ((int)(price) * 100) % 100 );

    coap_set_status_code(res, CHANGED_2_04);
}
RESOURCE(res_mpc_params,
        "title=\"MPC params\"",
        res_mpc_get_handler,
        NULL,
        res_mpc_put_handler,
        NULL);

