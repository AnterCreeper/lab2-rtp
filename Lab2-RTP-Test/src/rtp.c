#include <fcntl.h>

int socket_blocking(int sock) {
        if (!sock) return -1;
        int flag = fcntl(sock, F_GETFL, 0);
        if (flag < 0) return -1;
        return fcntl(sock, F_GETFL, flag & (~O_NONBLOCK));
}

int socket_nonblocking(int sock) {
        if (!sock) return -1;
        int flag = fcntl(sock, F_GETFL, 0);
        if (flag < 0) return -1;
        return fcntl(sock, F_GETFL, flag | O_NONBLOCK);
}
