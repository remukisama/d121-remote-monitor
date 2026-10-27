#ifndef LS_METRICS_H
#define LS_METRICS_H
#include <stdint.h>
#define LS_VALID_CPU 1
#define LS_VALID_MEMORY 2
#define LS_VALID_LOAD 4
#define LS_VALID_UPTIME 8
struct ls_metrics {
    uint64_t sampled_ms; uint32_t sequence,uptime_s,total_kib,available_kib;
    uint16_t cpu_centi,load_milli; uint8_t valid;
};
struct ls_cpu { uint64_t total,idle; int ready; };
int ls_parse_cpu(const char *,struct ls_cpu *,uint16_t *);
int ls_parse_memory(const char *,uint32_t *,uint32_t *);
int ls_parse_load(const char *,uint16_t *);
uint64_t ls_monotonic_ms(void);
void ls_collect(struct ls_metrics *,struct ls_cpu *,uint32_t);
#endif
