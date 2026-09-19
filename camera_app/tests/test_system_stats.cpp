/* Exercise procfs parsing, counter resets and the MT11 sensor conversion. */
#include "../src/recording/system_stats.cpp"
#include <assert.h>

uint64_t ca_binlog_time_us(void) { return 123; }

int main(void)
{
    uint64_t total, idle;
    assert(parse_cpu("cpu 100 20 30 400 50 6 7 8 90 10\n", &total, &idle));
    assert(total == 621 && idle == 450); /* Don't double-count guest. */
    assert(parse_cpu("cpu 1 2 3 4\n", &total, &idle) && total == 10 && idle == 4);
    assert(!parse_cpu("cpu broken", &total, &idle));
    struct ca_system_stats state = {};
    assert(isnan(cpu_load(&state, 1000, 600)));
    assert(fabsf(cpu_load(&state, 1400, 900) - 25) < .001f);
    assert(isnan(cpu_load(&state, 1400, 900))); /* No elapsed ticks. */
    assert(isnan(cpu_load(&state, 10, 5))); /* Reset/wrap. */
    assert(cpu_load(&state, 110, 5) == 100);
    assert(cpu_load(&state, 210, 105) == 0);
    assert(isnan(cpu_load(&state, 220, 120))); /* Inconsistent CPU counters. */
    char text[] = "MemTotal: 65536 kB\nMemFree: 0 kB\nCached: 900 kB\nMemAvailable: 12345 kB\n";
    FILE *file = fmemopen(text, sizeof(text)-1, "r");
    assert(file);
    struct ca_log_sys sample = {};
    read_memory(file, &sample); fclose(file);
    assert(sample.valid == (CA_SYS_MEM_FREE | CA_SYS_MEM_AVAILABLE));
    assert(sample.mem_free == 0 && sample.mem_available == 12345U * 1024);
    char old_kernel[] = "MemFree: 10 kB\n";
    file = fmemopen(old_kernel, sizeof(old_kernel)-1, "r");
    assert(file); sample = (struct ca_log_sys){0};
    read_memory(file, &sample); fclose(file);
    assert(sample.valid == CA_SYS_MEM_FREE && sample.mem_free == 10240);
    uint32_t registers[1024] = {};
    assert(isnan(mt11_temperature(registers)));
    for (unsigned i=0; i<3; i++) registers[(8+i*256)/4] = 500;
    assert(fabsf(mt11_temperature(registers) - 41.350975f) < .001f);
    registers[(8+256)/4] = 901;
    assert(isnan(mt11_temperature(registers)));
    puts("PASS SYS CPU counters, memory units/availability and MT11 temperature");
    return 0;
}
