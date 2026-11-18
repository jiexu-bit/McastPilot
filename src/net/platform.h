#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <string>

namespace mcast::net {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
#endif

// Windows requires WSAStartup before any socket call. No-op on POSIX.
bool InitSockets(std::string& err);
void ShutdownSockets();

std::string SocketErrorText(int code);

}  // namespace mcast::net
