#ifndef CLOUD_WIRE_H
#define CLOUD_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define CLOUD_PACKET_MAX 768
#define CLOUD_TEXT_MAX 192
typedef struct {
    char ssid[33], wifi_password[65], host[65], client_id[65];
    char username[97], password[97], state_topic[65], event_topic[65];
    unsigned port, tls, baud, qos;
} cloud_config_t;
int cloud_config_parse(cloud_config_t *out, const char *text);
int cloud_quote(char *out, size_t cap, const char *text);
/* ICM-42688-P at +/-2g: magnitude deviation from 1g, in millig (not RMS/FFT). */
uint32_t cloud_vibration_mg(int16_t ax, int16_t ay, int16_t az);
int cloud_mqtt_connect(uint8_t *out, size_t cap, const cloud_config_t *cfg);
int cloud_mqtt_publish(uint8_t *out, size_t cap, const char *topic,
                       const char *payload, uint16_t id, int duplicate, unsigned qos);
int cloud_mqtt_subscribe(uint8_t *out,size_t cap,const char *topic,uint16_t id);
int cloud_ack_parse(const uint8_t *payload,unsigned length,uint32_t key[4]);
typedef void (*cloud_line_fn)(void *, const char *);
typedef void (*cloud_byte_fn)(void *, uint8_t);
/* Normal mode, CIPMUX=0, CIPDINFO=0. Binary +IPD bytes never enter AT lines. */
typedef struct {
    char line[CLOUD_TEXT_MAX];
    unsigned used, remaining, overflow, errors;
    cloud_line_fn on_line;
    cloud_byte_fn on_byte;
    void *ctx;
} cloud_at_rx_t;
void cloud_at_feed(cloud_at_rx_t *rx, uint8_t byte);
/* One QoS0 subscription receives short application ACKs after cloud DB commit. */
typedef struct {
    uint8_t type, data[CLOUD_TEXT_MAX];
    unsigned phase, size, used, shift, errors;
    void (*on_packet)(void *, uint8_t, const uint8_t *, unsigned);
    void *ctx;
} cloud_mqtt_rx_t;
void cloud_mqtt_feed(cloud_mqtt_rx_t *rx, uint8_t byte);
#endif
