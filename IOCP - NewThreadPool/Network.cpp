#include "Network.h"
#include "Log.h"
#include <cassert>
#include <sstream>
#include <string>
#include <iostream>

using namespace std;


namespace
{
	LPFN_ACCEPTEX s_AcceptEx = NULL;
	LPFN_CONNECTEX s_ConnectEx = NULL;

	bool BindSocket(SOCKET socket, addrinfo* info)
	{
		if(bind(socket, info->ai_addr, static_cast<int>(info->ai_addrlen)) == SOCKET_ERROR)
		{
			ERROR_CODE(WSAGetLastError(), "bind() failed.");
			return false;
		}

		std::string ip;
		u_short port;
		Network::GetLocalAddress(socket, ip, port);
		TRACE("Bind Address : ip[%s], port[%d]", ip.c_str(), port);

		return true;
	}
}


//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
bool Network::Initialize()
{
	WSADATA wd = {0, };
	if(WSAStartup(WINSOCK_VERSION, &wd) != 0)
	{
		ERROR_MSG("WSAStartup failed.");
		return false;
	}

	return true;
}


void Network::Deinitialize()
{
	WSACleanup();
}


SOCKET Network::CreateSocket(bool bind, u_short port)
{
	// Get Address Info
	addrinfo hints;
	ZeroMemory(&hints, sizeof(addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = bind ? AI_PASSIVE : 0;

	stringstream portBuff; 
	portBuff << port;

	struct addrinfo* infoList = NULL;
	// Passing NULL for pNodeName should return INADDR_ANY
	if (getaddrinfo(NULL, portBuff.str().c_str(), &hints, &infoList) != 0) 
	{
		ERROR_CODE(WSAGetLastError(), "getaddrinfo() failed. port : %d", port);
		return INVALID_SOCKET;
	}

	// loop through all the results and use the first we can
	struct addrinfo* info = infoList;
	SOCKET socket = INVALID_SOCKET;
	for(; info != NULL; info = info->ai_next) 
	{
		socket = WSASocket(info->ai_family, info->ai_socktype, info->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
		if(socket != INVALID_SOCKET)
		{
			if(!bind) 
				break;

			if(BindSocket(socket, info))
				break;

			CloseSocket(socket);
			socket = INVALID_SOCKET;
		}
		else
		{
			ERROR_CODE(WSAGetLastError(), "WSASocket() failed. port : %d", port);
		}
	}

	freeaddrinfo(infoList);

	return socket;
}


void Network::CloseSocket(SOCKET socket)
{
	if(closesocket(socket) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "closesocket() failed");
	}
}


BOOL Network::AcceptEx(SOCKET listenSocket, SOCKET newSocket, LPOVERLAPPED overlapped)
{
	if(s_AcceptEx == NULL)
	{
		DWORD dwBytes = 0;
		GUID guidAcceptEx = WSAID_ACCEPTEX;
		if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx), &s_AcceptEx, sizeof(s_AcceptEx), &dwBytes, 0, 0) == SOCKET_ERROR)
		{
			ERROR_CODE(WSAGetLastError(), "WSAIoctl() to get AcceptEx() failed.");
			return FALSE;
		}
	}

	// This value is for retriving first data, local and remote addresses from AcceptEx by calling GetAcceptExSockaddrs() latter.
	// Each address buffer must be at least 16 bytes more than the maximum address length for the transport protocol in use.
	// It seems we don't need to use GetAcceptExSockaddrs() as we can get the address from getsockname(), getpeername() once we set SO_UPDATE_ACCEPT_CONTEXT.
	static BYTE buffer[(sizeof(sockaddr_in6) + 16)*2];

	return s_AcceptEx(listenSocket, newSocket, &buffer, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16,  NULL, overlapped);
}


BOOL Network::ConnectEx(SOCKET socket, sockaddr* addr, int addrlen, LPOVERLAPPED overlapped)
{
	if(s_ConnectEx == NULL)
	{
		DWORD dwBytes = 0;
		GUID guidConnectEx = WSAID_CONNECTEX;
		if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx, sizeof(guidConnectEx), &s_ConnectEx, sizeof(s_ConnectEx), &dwBytes, 0, 0) == SOCKET_ERROR)
		{
			ERROR_CODE(WSAGetLastError(), "WSAIoctl() to get ConnectEx() failed");
			return FALSE;
		}
	}

	return s_ConnectEx(socket, addr, addrlen, NULL, 0, NULL, overlapped);
}


bool Network::GetLocalAddress(SOCKET socket, std::string& ip, u_short& port)
{
	sockaddr_in6 addr6;
	ZeroMemory(&addr6, sizeof(addr6)); 
	int size = sizeof(addr6);

	char buff[INET6_ADDRSTRLEN] = {0,};

	if( 0 == getsockname(socket, reinterpret_cast<sockaddr*>(&addr6), &size) )
	{
		if( size == sizeof(sockaddr_in6) )
		{
			port = ntohs(addr6.sin6_port);
			inet_ntop(AF_INET6, &addr6.sin6_addr, buff, INET6_ADDRSTRLEN);

			ip = buff;
		}
		else if ( size == sizeof(sockaddr_in) )
		{
			sockaddr_in* pAddr4 = reinterpret_cast<sockaddr_in*>(&addr6);

			port = ntohs(pAddr4->sin_port);
			inet_ntop(AF_INET, &pAddr4->sin_addr, buff, INET_ADDRSTRLEN);

			ip = buff;
		}
		else
		{
			return false;
		}

		return true;
	}

	return false;
}


bool Network::GetRemoteAddress(SOCKET socket, std::string& ip, u_short& port)
{
	char buff[INET6_ADDRSTRLEN] = {0,};

	sockaddr_in6 addr6; 
	ZeroMemory(&addr6, sizeof(addr6)); 
	int size = sizeof(addr6);

	if( 0 == getpeername(socket, reinterpret_cast<sockaddr*>(&addr6), &size) )
	{
		if( size == sizeof(sockaddr_in6) )
		{
			port = ntohs(addr6.sin6_port);
			inet_ntop(AF_INET6, &addr6.sin6_addr, buff, INET6_ADDRSTRLEN);

			ip = buff;
		}
		else if ( size == sizeof(sockaddr_in) )
		{
			sockaddr_in* pAddr4 = reinterpret_cast<sockaddr_in*>(&addr6);
			port = ntohs(pAddr4->sin_port);
			inet_ntop(AF_INET, &pAddr4->sin_addr, buff, INET_ADDRSTRLEN);

			ip = buff;
		}
		else
		{
			return false;
		}

		return true;
	}

	return false;
}
