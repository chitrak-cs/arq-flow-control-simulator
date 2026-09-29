#include "crc.h"

uint32_t calculateCRC8(uint8_t *data, int length)
{

    uint8_t crc = 0x00;

    for (int i = 0; i < length; i++)
    {

        crc = crc ^ data[i];

        for (int j = 0; j < 8; j++)
        {

            if (crc & 0x80)
            {

                crc = (crc << 1) ^ 0x07;
            }
            else
            {

                crc = crc << 1;
            }
        }
    }

    return crc;
}

uint32_t calculateCRC10(uint8_t *data, int length)
{
    uint32_t crc = 0;

    for (int i = 0; i < length; i++)
    {

        crc = crc ^ ((uint32_t)data[i] << 2);

        for (int j = 0; j < 8; j++)
        {

            if (crc & 0x200)
            {

                crc = (crc << 1) ^ 0x233;
            }
            else
            {

                crc = crc << 1;
            }

            crc = crc & 0x3FF;
        }
    }

    return crc;
}

uint32_t calculateCRC16(uint8_t *data, int length)
{
    uint32_t crc = 0;

    for (int i = 0; i < length; i++)
    {

        crc = crc ^ ((uint32_t)data[i] << 8);

        for (int j = 0; j < 8; j++)
        {

            if (crc & 0x8000)
            {

                crc = (crc << 1) ^ 0x8005;
            }
            else
            {

                crc = crc << 1;
            }

            crc = crc & 0xFFFF;
        }
    }

    return crc;
}

uint32_t calculateCRC32(uint8_t *data, int length)
{
    uint32_t crc = 0;

    for (int i = 0; i < length; i++)
    {

        crc = crc ^ ((uint32_t)data[i] << 24);

        for (int j = 0; j < 8; j++)
        {

            if (crc & 0x80000000)
            {

                crc = (crc << 1) ^ 0x04C11DB7;
            }
            else
            {

                crc = crc << 1;
            }
        }
    }

    return crc;
}