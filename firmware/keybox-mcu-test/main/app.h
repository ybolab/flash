#pragma once

#include <stdbool.h>

typedef enum {
    NET_PROVISIONING,
    NET_CONNECTING,
    NET_CONNECTED,
} net_state_t;

typedef enum {
    LED_AUTO,
    LED_OFF,
    LED_ON,
} led_mode_t;

extern volatile net_state_t g_net_state;
extern volatile led_mode_t g_led_mode;
extern char g_hostname[32];
extern char g_service_name[32];

void web_start(void);
float app_chip_temp(void);
void app_reprovision(void);
