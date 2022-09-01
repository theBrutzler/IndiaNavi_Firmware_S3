#include <stdio.h>
#include <stdint.h>

void printf_fb(uint8_t *fb, uint32_t width, uint32_t height)
{
    for (uint32_t i = 0; i < width; i++)
    {
        for (uint32_t j = 0; j < height; j++)
            printf("%d,", fb[height * i + j]);
        printf("\n");
    }
}