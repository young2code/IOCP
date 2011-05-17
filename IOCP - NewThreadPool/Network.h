#pragma once

#include <winsock2.h>
#include <mswsock.h>
#include <Ws2tcpip.h>
#include <string>

struct addrinfo;

namespace Network
{
	bool Initialize();
	void Deinitialize();

	SOCKET CreateSocket(bool bind, u_short port);
	void CloseSocket(SOCKET socket);

	BOOL AcceptEx(SOCKET listenSocket, SOCKET newSocket, LPOVERLAPPED overlapped);
	BOOL ConnectEx(SOCKET socket, sockaddr* addr, int addrlen, LPOVERLAPPED overlapped);

	bool GetLocalAddress(SOCKET socket, std::string& ip, u_short& port);
	bool GetRemoteAddress(SOCKET socket, std::string& ip, u_short& port);
};
