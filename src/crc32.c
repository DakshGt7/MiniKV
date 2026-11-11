#include "crc32.h"

static uint32_t table[256];
static int table_ready = 0;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    table_ready = 1;
}

uint32_t crc32_compute(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;

    if (!table_ready)
        build_table();
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
