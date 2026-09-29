#include <stdint.h>
#include "checksum.h"

uint16_t calculateChecksum(uint8_t *data, int length)
{

    uint32_t sum = 0;

    int i = 0;

    // Process 16-bit words
    while (i + 1 < length)
    {

        uint16_t word;

        word = ((uint16_t)data[i] << 8) |
               data[i + 1];

        sum = sum + word;

        // Add carry back
        if (sum > 0xFFFF)
        {

            sum = (sum & 0xFFFF) + 1;
        }

        i = i + 2;
    }

    // If there is one byte left
    if (i < length)
    {

        uint16_t word;

        word = (uint16_t)data[i] << 8;

        sum = sum + word;

        if (sum > 0xFFFF)
        {

            sum = (sum & 0xFFFF) + 1;
        }
    }

    // One's complement
    uint16_t checksum;

    checksum = ~sum;

    return checksum;
}