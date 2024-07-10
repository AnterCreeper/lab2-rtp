#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "sender_def.h"
#include "rtp.h"
#include "util.h"

int sock_rx = 0;
int valid_rx = 0;
struct sockaddr_in addr_rx;

#define RTP_SOCK sock_rx
#define RTP_ADDR addr_rx
#define RTP_VALID valid_rx
#define RTP_RECEIVER

static inline int send_rtp(char* buffer, unsigned int length) {
        ((struct RTP_packet*)buffer)->rtp.checksum = 0;
        ((struct RTP_packet*)buffer)->rtp.checksum = compute_checksum(buffer, length);
        return sendto(RTP_SOCK, buffer, length, 0, (struct sockaddr*)&RTP_ADDR, sizeof(struct sockaddr_in));
}

static inline int recv_rtp(char* buffer, unsigned int length) {
#ifdef RTP_RECEIVER
        struct sockaddr_in recv_addr;
        unsigned int recv_addr_len = sizeof(struct sockaddr_in);
#endif
        int ret;
#ifdef RTP_SENDER
        while(ret = recvfrom(RTP_SOCK, buffer, length, 0, NULL, NULL)) {
#endif
#ifdef RTP_RECEIVER
        while(ret = recvfrom(RTP_SOCK, buffer, length, 0, (struct sockaddr*)&recv_addr, &recv_addr_len)) {
#endif
                if (likely(RTP_VALID)) {
//                      if (recv_addr.sin_port != RTP_ADDR.sin_port || recv_addr.sin_addr.s_addr != RTP_ADDR.sin_addr.s_addr) continue;
                } else {
#ifdef RTP_RECEIVER
                        RTP_ADDR = recv_addr;
#endif
                }
                break;
        }
        if(!ret) return 0;

        uint32_t crc = ((struct RTP_packet*)buffer)->rtp.checksum;
        ((struct RTP_packet*)buffer)->rtp.checksum = 0;
        if (crc != compute_checksum(buffer, ret)) return -1;
        return ret;
}

volatile unsigned int in_rx;
volatile unsigned int out_rx;
struct RTP_packet* buf_rx;
int buf_size_rx;
int window_size_rx;
int window_start_rx;

int initReceiver(uint16_t port, uint32_t window_size) {
	if (valid_rx) return -1;
 	if (port < 0 || port > 65535) return -1;
//Init resources...
	in_rx = 0, out_rx = 0;
	window_size_rx = window_size++; //at lease one more element
        buf_size_rx = is_power_of_2(window_size) ? window_size : next_pow2(window_size);
//	buf_size_rx *=2;
        buf_rx = (struct RTP_packet*)malloc(buf_size_rx * sizeof(struct RTP_packet));
        if (!buf_rx) return -1;

//Binding...
	if (sock_rx) close(sock_rx);
	sock_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in host_addr;
	host_addr.sin_port = htons(port);
	host_addr.sin_family = AF_INET;
	host_addr.sin_addr.s_addr = htonl(INADDR_ANY);
//	int optval = 1;
//	setsockopt(sock_rx, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (bind(sock_rx, (struct sockaddr*)&host_addr, sizeof(struct sockaddr_in))) {
		printf("rtp_rx: bind failed\n");
		return -1;
	}
	printf("rtp_rx: start listening.\n");

//Perform Handshake
	struct RTP_packet conn;
	for(;;)
		if (recv_rtp((char*)&conn, sizeof(struct RTP_packet)) > 0) {
			if (conn.rtp.type == RTP_START) {
				window_start_rx = conn.rtp.seq_num;
				break;
			}
		}

	struct RTP_packet ack;
	ack.rtp.type = RTP_ACK;
	ack.rtp.seq_num = window_start_rx;
	ack.rtp.length = 0;
	send_rtp((char*)&ack, sizeof(struct RTP_header));
	printf("rtp_rx: received a new request.\n");

	window_start_rx = 0;
	valid_rx = 1;
	return 0;
}

int recvMessage(char* filename) {
	if (!valid_rx) return -1;

	printf("rtp_rx: writing to %s\n", filename);
        FILE* fd = fopen(filename, "wb");
	if (!fd) return -1;

	struct RTP_packet data;
	int _buf_size_rx = buf_size_rx;
	int _window_size_rx = window_size_rx;
	buf_rx[in_rx].rtp.seq_num = window_start_rx;
        for(;;) {
                if (recv_rtp((char*)&data, sizeof(struct RTP_packet)) <= 0) continue;
		if (data.rtp.type == RTP_DATA) {
//			printf("rtp_rx: data_seq=%d base_seq=%d\n", data.rtp.seq_num, buf_rx[in_rx].rtp.seq_num);
			int seq_base = buf_rx[in_rx].rtp.seq_num;
			int distance = data.rtp.seq_num - buf_rx[in_rx].rtp.seq_num;
			if (distance >= 0 && distance < _window_size_rx) {
				memcpy(&buf_rx[(in_rx + distance) & (_buf_size_rx - 1)], &data, sizeof(struct RTP_packet));
				if (!distance) {
					int i = 0;
					do {
						if (buf_rx[in_rx].rtp.seq_num != seq_base) break;
						if (fwrite(buf_rx[in_rx].payload, sizeof(char), buf_rx[in_rx].rtp.length, fd) < 0) return -1;
						in_rx = ++in_rx & (_buf_size_rx - 1); ++seq_base;
					} while (i < _window_size_rx);
					buf_rx[in_rx].rtp.seq_num = seq_base;
				}
			}
			struct RTP_packet ack;
			ack.rtp.type = RTP_ACK;
			ack.rtp.seq_num = seq_base;
			ack.rtp.length = 0;
			send_rtp((char*)&ack, sizeof(struct RTP_header));
                } else if (data.rtp.type == RTP_END) {
			struct RTP_packet ack;
                        ack.rtp.type = RTP_ACK;
			ack.rtp.seq_num = data.rtp.seq_num;
			ack.rtp.length = 0;
			send_rtp((char*)&ack, sizeof(struct RTP_header));
			printf("rtp_rx: recv_worker exit.\n");
			break;
		}
        }
	fclose(fd);
	printf("rtp_rx: file saved.\n");
        return 0;
}

int recvMessageOpt(char* filename) {
	if (!valid_rx) return -1;
	printf("rtp_rx: writing to %s\n", filename);
        FILE* fd = fopen(filename, "wb");
	if (!fd) return -1;

	struct RTP_packet data;
	int _buf_size_rx = buf_size_rx;
	int _window_size_rx = window_size_rx;
	buf_rx[in_rx].rtp.seq_num = window_start_rx;
        for(;;) {
                if (recv_rtp((char*)&data, sizeof(struct RTP_packet)) <= 0) continue;
		if (data.rtp.type == RTP_DATA) {
//			printf("rtp_rx: data_seq=%d base_seq=%d\n", data.rtp.seq_num, buf_rx[in_rx].rtp.seq_num);
			int distance = data.rtp.seq_num - buf_rx[in_rx].rtp.seq_num;
			if (distance >= _window_size_rx) continue;

			struct RTP_packet ack;
			ack.rtp.type = RTP_ACK;
			ack.rtp.seq_num = data.rtp.seq_num;
			ack.rtp.length = 0;
			send_rtp((char*)&ack, sizeof(struct RTP_header));

			if (distance < 0) continue;
			memcpy(&buf_rx[(in_rx + distance) & (_buf_size_rx - 1)], &data, sizeof(struct RTP_packet));

			if (!distance) {
				int i = 0, seq_base = buf_rx[in_rx].rtp.seq_num;
				do {
					if (buf_rx[in_rx].rtp.seq_num != seq_base) break;
					if (fwrite(buf_rx[in_rx].payload, sizeof(char), buf_rx[in_rx].rtp.length, fd) < 0) return -1;
					in_rx = ++in_rx & (_buf_size_rx - 1); ++seq_base;
				} while (++i < _window_size_rx);
				buf_rx[in_rx].rtp.seq_num = seq_base;
			}
                } else if (data.rtp.type == RTP_END) {
			struct RTP_packet ack;
                        ack.rtp.type = RTP_ACK;
			ack.rtp.seq_num = data.rtp.seq_num;
			ack.rtp.length = 0;
			send_rtp((char*)&ack, sizeof(struct RTP_header));
			printf("rtp_rx: recv_worker exit.\n");
			break;
		}
        }
	fclose(fd);
	printf("rtp_rx: file saved.\n");
        return 0;
}

void terminateReceiver() {
        printf("rtp_tx: terminating...\n");
	if (sock_rx) {
		close(sock_rx);
		sock_rx = 0;
	}
	if (valid_rx) valid_rx = 0;
	return;
}
