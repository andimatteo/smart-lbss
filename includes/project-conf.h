#ifndef PROJECT_CONF_H
#define PROJECT_CONF_H

#define LOG_LEVEL_APP LOG_LEVEL_INFO

#define COAP_OBSERVE_CLIENT 1

#undef COAP_MAX_CHUNK_SIZE
#define COAP_MAX_CHUNK_SIZE 256


// #define UGRID_EP "coap://[fd00::202:2:2:2]:5683"  // /dev/tty.usbmodemD8ACE26BFA9A1
#define UGRID_EP "coap://[fd00::f6ce:36ac:9afa:6be2]:5683" 
// #define BATTERY_EP "coap://[fd00::203:3:3:3]:5683" // /dev/tty.usbmodemD82EA79297A21
#define BATTERY_EP "coap://[fd00::f6ce:362e:a297:92a7]:5683" 

#endif
