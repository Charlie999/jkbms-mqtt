#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <modbus/modbus.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <asm/ioctls.h>
#include <mosquitto.h>
#include <stdlib.h>

#include "config.h"

#define VERSION_STR "1.0"
#if defined(GIT_BRANCH) && defined(GIT_VERSION)
#undef VERSION_STR
#define xstr(s) str(s)
#define str(s) #s
#define VERSION_STR "git-" xstr(GIT_BRANCH) "_" xstr(GIT_VERSION)
#endif

#define PUBLISH_VALUE(topic, fmtstr, ...) { char* payload; asprintf(&payload, fmtstr, __VA_ARGS__); \
        mosq_rc = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 2, false);\
        if(mosq_rc != MOSQ_ERR_SUCCESS){ \
		free(payload); \
                fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(mosq_rc)); \
                return -1; \
        } \
 free(payload); }

static volatile int mqtt_connected = 0;

void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
int main() {
    int ret;
    modbus_t *ctx;

    fprintf(stderr, "JK BMS Poller %s, (C) Charlie Camilleri, Oct '24\n", VERSION_STR);

    struct mosquitto *mosq;

    ctx = modbus_new_rtu(UART_PATH, UART_BAUD, 'N', 8, 1);
    if (ctx == NULL) {
        perror("Unable to create the libmodbus context\n");
        return -1;
    }

    modbus_set_response_timeout(ctx, 1, 0);

    if(modbus_set_slave(ctx,1) < 0){
        perror("modbus_set_slave error\n");
        return -1;
    }

    if(modbus_connect(ctx) < 0){
        perror("modbus_connect error\n");
        return -1;
    }

    fprintf(stderr, "Connecting to device...\n");

    // read out device info
    char hwinfo[17];
    hwinfo[16] = 0;
    if (modbus_read_registers(ctx, 0x1400, 8, (uint16_t*)hwinfo) < 0) {
	perror("modbus_read_regs");
	return -1;
    }
    fprintf(stderr, "Device ID = %s\n", hwinfo);

    hwinfo[8] = 0;
    usleep(50000);
    if (modbus_read_registers(ctx, 0x1410, 4, (uint16_t*)hwinfo) < 0) {
        perror("modbus_read_regs");
        return -1;
    }
    fprintf(stderr, "HW Version = %s\n", hwinfo);

    usleep(50000);
    if (modbus_read_registers(ctx, 0x1418, 4, (uint16_t*)hwinfo) < 0) {
        perror("modbus_read_regs");
        return -1;
    }
    fprintf(stderr, "SW Version = %s\n", hwinfo);

    usleep(50000);
    uint16_t cell_count[2];
    if(modbus_read_registers(ctx, 0x106C, 4, cell_count) < 0) {
	perror("modbus_read_regs");
	return -1;
    }

    uint32_t n_cells = (cell_count[0]<<16) | cell_count[1];
    fprintf(stderr, "Device has %d cells\n", n_cells);

    usleep(BMS_REQ_DELAY); // to keep the BMS happy
    //

    mosquitto_lib_init();

    mosq = mosquitto_new(NULL, true, NULL);
    if (mosq == NULL) {
     perror("mosquitto_new");
     return -1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);

    int mosq_rc;
    if((mosq_rc=mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60)) != MOSQ_ERR_SUCCESS){
      mosquitto_destroy(mosq);
      fprintf(stderr, "Error: %s\n", mosquitto_strerror(mosq_rc));
      return 1;
    }

    mosq_rc = mosquitto_loop_start(mosq);
    if(mosq_rc != MOSQ_ERR_SUCCESS){
     mosquitto_destroy(mosq);
     fprintf(stderr, "Error: %s\n", mosquitto_strerror(mosq_rc));
     return 1;
    }

    int mc = 0;
    while(!mqtt_connected) {
	fprintf(stderr, "Connecting to MQTT [%s:%hu]..\n", MQTT_HOST, MQTT_PORT);
	usleep(500000);
	if (mc++ > 25) {
		fprintf(stderr, "Timed out.\n");

		modbus_close(ctx);
		modbus_free(ctx);

		mosquitto_lib_cleanup();
	}
    }

    fprintf(stderr,"Polling every %d seconds\n", POLL_INTERVAL);

    int first = 1;
    for(;;){
	if (first==0) sleep(POLL_INTERVAL);
        first = 0; // so we start polling immediately

	uint16_t *cell_mv = (uint16_t*)malloc(sizeof(uint16_t) * n_cells);
        if(modbus_read_registers(ctx, 0x1200, n_cells, cell_mv) < 0){
		free(cell_mv);
        	perror("modbus_read_regs");
        	return -1;
        }

	for (int i=0;i<n_cells;i++)
		PUBLISH_VALUE("battery", "battery cell%d_voltage=%.3f", i, cell_mv[i]/1000.f);

	free(cell_mv);

	usleep(BMS_REQ_DELAY);
	uint16_t inb;
	if (modbus_read_registers(ctx, 0x1246, 1, &inb) < 0) {
		perror("modbus_read_regs");
		return -1;
	}

	PUBLISH_VALUE("battery", "battery vdiff_max=%.3f", inb/1000.f);

	usleep(BMS_REQ_DELAY);
	uint16_t temps[2];
	if (modbus_read_registers(ctx, 0x129C, 2, temps) < 0) {
		perror("modbus_read_regs");
		return -1;
	}

	PUBLISH_VALUE("battery", "battery temp0=%.1f", temps[0]/10.0f);
	PUBLISH_VALUE("battery", "battery temp1=%.1f", temps[1]/10.0f);

	/*usleep(50000);
	uint16_t bal_info[2];
	if (modbus_read_registers(ctx, 0x12A4, 2, bal_info) < 0) {
                perror("modbus_read_regs");
                return -1;
        }*/

	usleep(BMS_REQ_DELAY);
	uint16_t bat_info[6];
	if (modbus_read_registers(ctx, 0x1290, 6, bat_info) < 0) {
		perror("modbus_read_regs");
		return -1;
	}

	float bat_voltage = ((bat_info[0]<<16) + bat_info[1])/1000.0;
	float bat_power = ((bat_info[2]<<16) + bat_info[3])/1000.0;
	float bat_current = ((bat_info[4]<<16) + bat_info[5])/1000.0;

	PUBLISH_VALUE("battery", "battery bat_voltage=%.3f", bat_voltage);
	PUBLISH_VALUE("battery", "battery bat_power=%.3f", bat_power);
	PUBLISH_VALUE("battery", "battery bat_current=%.3f", bat_current);

	usleep(BMS_REQ_DELAY);
	int16_t bal_current;
	if (modbus_read_registers(ctx, 0x12A4, 1, &bal_current) < 0) {
		perror("modbus_read_regs");
		return -1;
	}

	PUBLISH_VALUE("battery", "battery bal_current=%.3f", bal_current/1000.f);

	usleep(BMS_REQ_DELAY);
	uint16_t bal_state;
	if (modbus_read_registers(ctx, 0x12A6, 1, &bal_state) < 0) {
		perror("modbus_read_regs");
		return -1;
	}

	PUBLISH_VALUE("battery", "battery bal_state=%hhu", (bal_state&0xFF00)>>8);
	PUBLISH_VALUE("battery", "battery soc=%hhu", bal_state&0xFF);
    }

    modbus_close(ctx);
    modbus_free(ctx);

    mosquitto_lib_cleanup();
}

void on_connect(struct mosquitto *mosq, void *obj, int reason_code) {
	fprintf(stderr, "MQTT status: %s\n", mosquitto_connack_string(reason_code));
	if(reason_code != 0)
		mosquitto_disconnect(mosq);
	else
		mqtt_connected=1;
}
