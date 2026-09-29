#ifndef CRC_H
#define CRC_H

#include <stdint.h>

uint32_t calculateCRC8(uint8_t *data, int length);
uint32_t calculateCRC10(uint8_t *data, int length);
uint32_t calculateCRC16(uint8_t *data, int length);
uint32_t calculateCRC32(uint8_t *data, int length);

#endif