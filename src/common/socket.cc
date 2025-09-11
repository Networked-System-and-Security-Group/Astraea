#include <arpa/inet.h>

#include <cstddef>
#include <doca_log.h>

#include "socket.h"

DOCA_LOG_REGISTER(COMMON : SOCKET)

int recvMsg(int aSock, char *buffer) {
    // receive message size
    size_t msg_len;
    if (recv(aSock, &msg_len, sizeof(msg_len), 0) <= 0) {
        perror("Failed to receive message length");
        return -1;
    }

    // receive the main message
    size_t bytes_received = 0;
    while (bytes_received < msg_len) {
        int result =
            recv(aSock, buffer + bytes_received, msg_len - bytes_received, 0);
        if (result <= 0) {
            perror("Failed to receive message body");
            return -1;
        }
        bytes_received += result;
    }

    buffer[bytes_received] = '\0';
    return bytes_received;
}

int sendMsg(int aSock, const char *aMsg, size_t len) {
    /* send msg size */
    if (send(aSock, &len, sizeof(len), 0) < 0) {
        perror("Failed to send message length");
        return -1;
    }

    /* send the main msg */
    if (send(aSock, aMsg, len, 0) < 0) {
        perror("Failed to send message body");
        return -1;
    }

    return 0;
}
