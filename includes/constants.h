#ifndef _CONSTANTS_H
#define _CONSTANTS_H

#define ROSSO   "\033[31m"
#define VERDE   "\033[32m"
#define RESET   "\033[0m"

#define BATTERY_TIMEOUT_SEC 30
#define BAT_MAX_POWER_KW  10.0f         // 10 kW nominali
#define BAT_MAX_POWER_W   (BAT_MAX_POWER_KW * 1000.0f)
#define MAX_IRR   1200.0f

// ML configuration
#define ML_WINDOW 10
#define N_FEATURES 4

#endif
