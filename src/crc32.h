#ifndef MINIKV_CRC32_H
#define MINIKV_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* IEEE 802.3 CRC-32 (same polynomial as zlib/PNG), table driven. */
uint32_t crc32_compute(const void *data, size_t len);

#endif
