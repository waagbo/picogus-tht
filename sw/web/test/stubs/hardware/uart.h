// Host test stub: the MIDI UART is simulated by test_midi.c
#pragma once
#include "pico.h"
typedef struct uart_inst uart_inst_t;
extern uart_inst_t *uart0;
#ifdef __cplusplus
extern "C" {
#endif
void uart_write_blocking(uart_inst_t *uart, const uint8_t *src, size_t len);
void uart_tx_wait_blocking(uart_inst_t *uart);
bool uart_is_writable(uart_inst_t *uart);
#ifdef __cplusplus
}
#endif
