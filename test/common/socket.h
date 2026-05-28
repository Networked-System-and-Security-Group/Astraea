#ifndef SOCKET_H_
#define SOCKET_H_

#include <cstddef>

int recvMsg(int aSock, char *buffer);
int sendMsg(int aSock, const char *aMsg, size_t len);

#endif