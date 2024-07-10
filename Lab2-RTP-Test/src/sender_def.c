#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "sender_def.h"
#include "util.h"
#include "rtp.h"

int sock_tx = 0;
int valid_tx = 0;
struct sockaddr_in addr_tx;

volatile unsigned int in_tx;
volatile unsigned int out_tx;
struct RTP_packet* buf_tx;
int buf_size_tx;
int window_size_tx;
int window_start_tx;

#define RTP_SOCK sock_tx
#define RTP_ADDR addr_tx
#define RTP_VALID valid_tx
#define RTP_SENDER

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

int initSender(const char* receiver_ip, uint16_t receiver_port, uint32_t window_size) {
	if (valid_tx) return -1;
	if (!strlen(receiver_ip)) return -1;
 	if (receiver_port < 0 || receiver_port > 65535) return -1;

//Init resources...
	in_tx = 0, out_tx = 0;
	window_size_tx = window_size;
	buf_size_tx = is_power_of_2(window_size) ? window_size: next_pow2(window_size);
	buf_size_tx *= 64;
	buf_tx = (struct RTP_packet*)malloc(buf_size_tx * sizeof(struct RTP_packet));
	if (!buf_tx) return -1;

//Binding...
	if (sock_tx) close(sock_tx);
	sock_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in host_addr;
	host_addr.sin_port = htons(0);
	host_addr.sin_family = AF_INET;
	host_addr.sin_addr.s_addr = htonl(INADDR_ANY);
//	int optval = 1;
//	setsockopt(sock_tx, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if(bind(sock_tx, (struct sockaddr*)&host_addr, sizeof(struct sockaddr_in))) return -1;

//Perform Handshake
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	srand(ts.tv_nsec);
	window_start_tx = rand() & 63;

	addr_tx.sin_port = htons(receiver_port);
	addr_tx.sin_family = AF_INET;
	addr_tx.sin_addr.s_addr = inet_addr(receiver_ip);

	struct RTP_packet conn;
	conn.rtp.type = RTP_START;
	conn.rtp.seq_num = window_start_tx;
	conn.rtp.length = 0;

	if (socket_nonblocking(sock_tx) < 0) return -1;
	int j;
	for(int i = 0; i < RETRY_COUNT; i++) {
		printf("rtp_tx: perform handshake: count=%d\n", i);
		if (send_rtp((char*)&conn, sizeof(struct RTP_header)) < 0) return -1;
		usleep(TIMEOUT_TIME * 1000);
		struct RTP_packet ack;
		for (j = 0; j < MAX_RECV_NUM; j++) {
			printf("rtp_tx: trying: count=%d\n", j);
			if (recv_rtp((char*)&ack, sizeof(struct RTP_packet)) > 0) {
				if (ack.rtp.type == RTP_ACK && ack.rtp.seq_num == window_start_tx) {
					j = 0;
					break;
				}
			}
		}
		if (!j) break;
	}

	if (!j) {
		printf("rtp_tx: connection established.\n");
		valid_tx = 1;
		window_start_tx = 0;
		return 0;
	} else {
		printf("rtp_tx: connection reset.\n");
		struct RTP_packet end;
		end.rtp.type = RTP_END;
		end.rtp.seq_num = ++window_start_tx;
		end.rtp.length = 0;
		send_rtp((char*)&end, sizeof(struct RTP_header));
		return -1;
	}
}

struct recv_worker_t {
	pthread_cond_t* wake_sig;
	pthread_mutex_t* desleep;
	pthread_mutex_t* sleep;
	timer_t* timer;
	struct itimerspec* ts;
};

void recv_worker_tx(struct recv_worker_t* arg) {
	struct RTP_packet ack;
	int _buf_size_tx = buf_size_tx;
	int _window_size_tx = window_size_tx;
	if (socket_blocking(sock_tx) < 0) return;
	for(;;) {
		if (recv_rtp((char*)&ack, sizeof(struct RTP_packet)) <= 0) continue;
		if (ack.rtp.type == RTP_ACK) {
//			printf("rtp_tx: data_seq=%d base_seq=%d\n", ack.rtp.seq_num, buf_tx[out_tx].rtp.seq_num);
			int distance = ack.rtp.seq_num - buf_tx[out_tx].rtp.seq_num;
			if (distance <= 0 || distance > _window_size_tx) continue;

			int is_sleep = pthread_mutex_trylock(arg->sleep);
			__sync_synchronize();
			int _in_tx = in_tx;
			out_tx = (out_tx + distance) & (_buf_size_tx - 1);
			if (!is_sleep) {
				pthread_mutex_unlock(arg->sleep);
				pthread_cond_signal(arg->wake_sig);
				if (_in_tx == out_tx) {
					printf("rtp_tx: run out of FIFO, is_sleep=%d\n", is_sleep != 0);
					pthread_mutex_lock(arg->desleep);
					pthread_mutex_unlock(arg->desleep);
					pthread_mutex_lock(arg->sleep);
					pthread_mutex_unlock(arg->sleep);
				}
			} else {
				if (_in_tx == out_tx) {
					printf("rtp_tx: run out of FIFO, is_sleep=%d\n", is_sleep != 0);
					pthread_mutex_lock(arg->sleep);
					pthread_mutex_unlock(arg->sleep);
				}
			}
			if (buf_tx[out_tx].rtp.seq_num != ack.rtp.seq_num) {
				printf("rtp_tx: recv_worker exit.\n");
				return;
			}
			timer_settime(*(arg->timer), TIMER_ABSTIME, arg->ts, NULL);
		} else if (ack.rtp.type == RTP_END) {
			printf("rtp_tx: recv_worker exit.\n");
			return;
		}
	}
	return;
}

void recv_worker_opt_tx(struct recv_worker_t* arg) {
	struct RTP_packet ack;
	int _buf_size_tx = buf_size_tx;
	int _window_size_tx = window_size_tx;
	if (socket_blocking(sock_tx) < 0) return;
	for(;;) {
		if (recv_rtp((char*)&ack, sizeof(struct RTP_packet)) < 0) continue;
		if (ack.rtp.type == RTP_ACK) {
//			printf("rtp_tx: data_seq=%d base_seq=%d\n", ack.rtp.seq_num, buf_tx[out_tx].rtp.seq_num);
			int distance = ack.rtp.seq_num - buf_tx[out_tx].rtp.seq_num;
			if (distance < 0 || distance >= _window_size_tx) continue;
			buf_tx[(out_tx + distance) & (_buf_size_tx - 1)].rtp.seq_num = 0;
			if (!distance) {
				__sync_synchronize(); //because window moving, need ensure data write through
				int i = 0;
				while (++i < _window_size_tx) {
					if (buf_tx[(out_tx + i) & (_buf_size_tx - 1)].rtp.seq_num) break;
				}

				int is_sleep = pthread_mutex_trylock(arg->sleep);
				__sync_synchronize();
				int _in_tx = in_tx;
				out_tx = (out_tx + i) & (_buf_size_tx - 1);
				if (!is_sleep) {
					pthread_mutex_unlock(arg->sleep);
					pthread_cond_signal(arg->wake_sig);
					if (_in_tx == out_tx) {
						printf("rtp_tx: run out of FIFO, is_sleep=%d\n", is_sleep != 0);
						pthread_mutex_lock(arg->desleep);
						pthread_mutex_unlock(arg->desleep);
						pthread_mutex_lock(arg->sleep);
						pthread_mutex_unlock(arg->sleep);
					}
				} else {
					if (_in_tx == out_tx) {
						printf("rtp_tx: run out of FIFO, is_sleep=%d\n", is_sleep != 0);
						pthread_mutex_lock(arg->sleep);
						pthread_mutex_unlock(arg->sleep);
					}
				}
				if (!buf_tx[out_tx].rtp.seq_num) {
					printf("rtp_tx: recv_worker exit.\n");
					return;
				}
				timer_settime(*(arg->timer), TIMER_ABSTIME, arg->ts, NULL);
			}
		} else if (unlikely(ack.rtp.type == RTP_END)) {
			printf("rtp_tx: recv_worker exit.\n");
			return;
		}
	}
	return;
}

void recv_timeout_handler() {
	int j = out_tx;
	int _buf_size_tx = buf_size_tx;
	int _window_size_tx = window_size_tx;
	for (int i = 0; i < _window_size_tx; i++) {
		if (buf_tx[j].rtp.seq_num) send_rtp((char*)&buf_tx[j], sizeof(struct RTP_packet));
		j = ++j & (_buf_size_tx - 1);
		__sync_synchronize(); //ensure read correct in_tx
		if (j == in_tx) return;
	}
	return;
}

int rtp_sendMessage(const char* message, void* timeout_handler, void* worker_thread) {
	if (!valid_tx) return -1;

	pthread_t thread_recv;
	pthread_cond_t wake_sig;
	pthread_mutex_t sleep_lock, desleep_lock;
	pthread_cond_init(&wake_sig, NULL);
	pthread_mutex_init(&sleep_lock, NULL);
	pthread_mutex_lock(&sleep_lock);
	pthread_mutex_init(&desleep_lock, NULL);

	struct sigevent ev;
        struct itimerspec ts;
        timer_t timer;

        memset(&ev, 0, sizeof(struct sigevent));
        ev.sigev_notify = SIGEV_THREAD;
        ev.sigev_notify_function = timeout_handler;
        if (timer_create(CLOCK_REALTIME, &ev, &timer)) return -1;

        ts.it_value.tv_sec = 0;
        ts.it_value.tv_nsec = TIMEOUT_TIME * 1000000;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = TIMEOUT_TIME * 1000000;

        struct recv_worker_t arg;
        arg.wake_sig = &wake_sig;
	arg.sleep = &sleep_lock;
	arg.desleep = &desleep_lock;
        arg.timer = &timer;
        arg.ts = &ts;

	printf("rtp_tx: open file %s\n", message);
	struct stat stat_file;
	stat(message, &stat_file);
	printf("rtp_tx: file size:%ld\n", stat_file.st_size);
	FILE* fd = fopen(message, "rb");
	if (!fd) return -1;

	int _buf_size_tx = buf_size_tx;
	buf_tx[in_tx].rtp.seq_num = window_start_tx;
	if (pthread_create(&thread_recv, NULL, (void*)worker_thread, &arg)) return -1;

	for(;;) {
		int ret = fread(buf_tx[in_tx].payload, sizeof(char), PAYLOAD_SIZE, fd);
		buf_tx[in_tx].rtp.type = RTP_DATA;
		buf_tx[in_tx].rtp.length = ret;
		if (unlikely(ret < PAYLOAD_SIZE)) {
			printf("rtp_tx: end of file. seq_num=%d\n", window_start_tx);
			if (feof(fd)) {
				memset(buf_tx[in_tx].payload + ret, 0, (PAYLOAD_SIZE - ret) * sizeof(char));
				if (ret) {
					send_rtp((char*)&buf_tx[in_tx], sizeof(struct RTP_packet));
					__sync_synchronize(); //ensure the data is copied in before in_tx modified. used by recv_worker.
					in_tx = ++in_tx & (_buf_size_tx - 1); //safe
				}
				break;
			} else return -1;
		} else {
			send_rtp((char*)&buf_tx[in_tx], sizeof(struct RTP_packet));
		}

		__sync_synchronize(); //ensure the data is copied in before in_tx modified. used by timer.
		in_tx = ++in_tx & (_buf_size_tx - 1); //safe
		if (in_tx == out_tx) { //safe, although out_tx changed, it can only be bigger.
			pthread_mutex_lock(arg.desleep);
			pthread_cond_wait(arg.wake_sig, arg.sleep);
			pthread_mutex_unlock(arg.desleep);
		}
		buf_tx[in_tx].rtp.seq_num = ++window_start_tx;
	}
	fclose(fd);

	pthread_mutex_unlock(&sleep_lock);
	pthread_join(thread_recv, NULL);
	timer_delete(timer);
	pthread_cond_destroy(&wake_sig);
	pthread_mutex_destroy(&sleep_lock);
	pthread_mutex_destroy(&desleep_lock);

	printf("rtp_tx: message sent.\n");
	return 0;
}

int sendMessage(const char* message) {
	return rtp_sendMessage(message, recv_timeout_handler, recv_worker_tx);
}

int sendMessageOpt(const char* message) {
	return rtp_sendMessage(message, recv_timeout_handler, recv_worker_opt_tx);
}

void terminateSender() {
	printf("rtp_tx: terminating...\n");
	if (!valid_tx) {
		close(sock_tx);
		sock_tx = 0;
		return;
	}

	struct RTP_packet end, ack;
	end.rtp.type = RTP_END;
	end.rtp.seq_num = ++window_start_tx;
	end.rtp.length = 0;

        if (socket_nonblocking(sock_tx) < 0) return;

	for (int i = 0; i < RETRY_COUNT; i++) {
		send_rtp((char*)&end, sizeof(struct RTP_header));
		printf("rtp_tx: reset, count=%d\n", i);
	        usleep(TIMEOUT_TIME * 1000);

	        int j = 0;
        	for(; j < MAX_RECV_NUM; j++)
			if (recv_rtp((char*)&ack, sizeof(struct RTP_packet)) > 0) {
                	        if (ack.rtp.type == RTP_ACK && ack.rtp.seq_num == window_start_tx) {
                               		j = 0;
					break;
				}
                	}
	        if(!j) break;
	}

	printf("rtp_tx: connection closed.\n");
	close(sock_tx);
	sock_tx = 0;
	free(buf_tx);
	buf_tx = NULL;
	valid_tx = 0;
	return;
}
