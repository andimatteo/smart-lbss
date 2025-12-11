#ifndef PROJECT_CONF_H
#define PROJECT_CONF_H

#define LOG_LEVEL_APP LOG_LEVEL_INFO
#define NETSTACK_CONF_WITH_IPV6 1
#define COAP_MAX_CHUNK_SIZE 256

/* Potenza nominale batteria */
#define BAT_MAX_POWER_KW  10.0f         // 10 kW nominali
#define BAT_MAX_POWER_W   (BAT_MAX_POWER_KW * 1000.0f)

#define MAX_IRR   1200.0f
#define COAP_OBSERVE_CLIENT 1

// #define UGRID_EP "coap://[fd00::202:2:2:2]:5683"  // /dev/tty.usbmodemD8ACE26BFA9A1
#define UGRID_EP "coap://[fd00::f6ce:36ac:9afa:6be2]:5683" 
// #define BATTERY_EP "coap://[fd00::203:3:3:3]:5683" // /dev/tty.usbmodemD82EA79297A21
#define BATTERY_EP "coap://[fd00::f6ce:362e:a297:92a7]:5683" 
/* Increase CoAP transaction limits for better reliability */
#define COAP_MAX_OPEN_TRANSACTIONS 8
#define COAP_MAX_OBSERVERS 4
#define COAP_MAX_OBSERVEES 4

/* Enable RPL for multi-hop routing */
#define UIP_CONF_ROUTER 1

// no verbose
#define LOG_CONF_LEVEL_RPL LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_TCPIP LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_COAP LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6 LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_WARN

#endif
