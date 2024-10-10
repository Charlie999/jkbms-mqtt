#ifndef CONFIG_H
#define CONFIG_H

// UART parameters
#define UART_PATH "/dev/battery"
#define UART_BAUD 115200

// Polling interval, seconds (integer)
#define POLL_INTERVAL 10

// MQTT parameters
#define MQTT_HOST "127.0.0.1"
#define MQTT_PORT 1883

// Delay between read requests, to keep the BMS happy.
#define BMS_REQ_DELAY 100000

#endif
