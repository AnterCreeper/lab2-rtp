#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_START 0
#define RTP_END   1
#define RTP_DATA  2
#define RTP_ACK   3

#define PAYLOAD_SIZE 1461

typedef struct __attribute__ ((__packed__)) RTP_header {
    uint8_t type;       // 0: START; 1: END; 2: DATA; 3: ACK
    uint16_t length;    // Length of data; 0 for ACK, START and END packets
    volatile uint32_t seq_num;
    uint32_t checksum;  // 32-bit CRC
} rtp_header_t;

typedef struct __attribute__ ((__packed__)) RTP_packet {
    rtp_header_t rtp;
    char payload[PAYLOAD_SIZE];
} rtp_packet_t;

#define TIMEOUT_TIME    100
#define MAX_RECV_NUM    64
#define RETRY_COUNT     3

#define max(x, y) (x) > (y) ? (x) : (y)
#define min(x, y) (x) < (y) ? (x) : (y)

static inline int is_power_of_2(unsigned x) {
        return x > 0 && !(x & (x - 1));
}

static inline unsigned int next_pow2(unsigned int x) {
        x -= 1;
        x |= (x >> 1);
        x |= (x >> 2);
        x |= (x >> 4);
        x |= (x >> 8);
        x |= (x >> 16);
        return x + 1;
}

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

int socket_blocking(int sock);
int socket_nonblocking(int sock);

#ifdef __cplusplus
}
#endif

#endif //RTP_H
