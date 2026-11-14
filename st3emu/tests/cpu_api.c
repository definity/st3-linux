/* Thin C API around cpu.c for ctypes-based differential tests. */
#include "../src/cpu.h"
#include <string.h>
static uint8_t MEM[MEM_SIZE];
static CPU C;
static uint32_t io_in(void *ud, uint16_t port, int size) { (void)ud; uint32_t v = (port * 0x9E3779B1u) ^ 0x5A5A5A5Au; return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v; }
static uint32_t last_out_port, last_out_val; static int last_out_size;
static void io_out(void *ud, uint16_t port, int size, uint32_t v) { (void)ud; last_out_port = port; last_out_size = size; last_out_val = v; }
uint8_t *t_mem(void) { return MEM; }
void t_init(void) { cpu_init(&C, MEM); C.io_in = io_in; C.io_out = io_out; }
void t_set(const uint32_t *r, const uint16_t *s, uint32_t eip, uint32_t fl) {
    memcpy(C.r, r, 32); memcpy(C.seg, s, 12); C.eip = eip; C.flags = fl; C.error = 0; C.halted = 0;
}
void t_get(uint32_t *r, uint16_t *s, uint32_t *eip, uint32_t *fl) { memcpy(r, C.r, 32); memcpy(s, C.seg, 12); *eip = C.eip; *fl = C.flags; }
int t_step(void) { return cpu_step(&C); }
uint32_t t_lastout(void) { return last_out_port << 16 | (last_out_val & 0xFFFF); }
