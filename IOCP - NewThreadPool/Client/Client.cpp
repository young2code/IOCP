#include "Client.h"
#include "ClientMan.h"

#include "..\Log.h"
#include "..\Network.h"

#include <cassert>
#include <iostream>
#include <vector>
#include <boost/pool/singleton_pool.hpp>

using namespace std;

namespace
{
	class IOEvent
	{
	public:
		enum Type
		{
			CONNECT,
			RECV,
			SEND,
		};

	public:
		static IOEvent* Create(Client* clent, Type type);
		static void Destroy(IOEvent* event);

	public:
		OVERLAPPED overlapped;
		Client* client;
		Type type;

	private:
		IOEvent();
		~IOEvent();
		IOEvent(const IOEvent& rhs);
		IOEvent& operator=(IOEvent& rhs);
	};

	// use thread-safe memory pool
	typedef boost::singleton_pool<IOEvent, sizeof(IOEvent)> IOEventPool;

	/* static */ IOEvent* IOEvent::Create(Client* client, Type type)
	{
		IOEvent* event = static_cast<IOEvent*>(IOEventPool::malloc());
		ZeroMemory(event, sizeof(IOEvent));
		event->client = client;
		event->type = type;
		return event;
	}

	/* static */ void IOEvent::Destroy(IOEvent* event)
	{
		IOEventPool::free(event);
	}

	void PrintConnectionInfo(SOCKET socket)
	{
		std::string serverIP, clientIP;
		u_short serverPort = 0, clientPort = 0;
		Network::GetLocalAddress(socket, clientIP, clientPort);
		Network::GetRemoteAddress(socket, serverIP, serverPort);

		TRACE("Connection from ip[%s], port[%d] to ip[%s], port[%d] succeeded.", clientIP.c_str(), clientPort, serverIP.c_str(), serverPort);
	}
}

//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
/* static */ void CALLBACK Client::IoCompletionCallback(PTP_CALLBACK_INSTANCE /* Instance */, PVOID /* Context */,
														PVOID Overlapped, ULONG IoResult, ULONG_PTR NumberOfBytesTransferred, PTP_IO /* Io */)
{
	IOEvent* event = CONTAINING_RECORD(Overlapped, IOEvent, overlapped);
	assert(event);
	assert(event->client);

	if ( false == ClientMan::Instance()->IsAlive(event->client) )
	{
		// No client for this event.
		IOEvent::Destroy(event);
		return;
	}
 
	if(IoResult == ERROR_SUCCESS)
	{
		switch(event->type)
		{
		case IOEvent::CONNECT:
			event->client->OnConnect();
			break;

		case IOEvent::RECV:
			if(NumberOfBytesTransferred > 0)
			{
				event->client->OnRecv(NumberOfBytesTransferred);
			}
			else
			{
				event->client->OnClose();
				ClientMan::Instance()->PostRemoveClient(event->client);
			}
			break;

		case IOEvent::SEND:
			event->client->OnSend(NumberOfBytesTransferred);
			break;

		default: assert(false); break;
		}
	}
	else 
	{
		ERROR_CODE(IoResult, "I/O operation failed.");
		ClientMan::Instance()->PostRemoveClient(event->client);
	}

	IOEvent::Destroy(event);
}

//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
Client::Client()
: m_pTPIO(NULL), m_Socket(INVALID_SOCKET), m_State(WAIT)
{
}

//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
Client::~Client()
{
	Destroy();
}


//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
bool Client::Create(short port)
{	
	assert(m_Socket == INVALID_SOCKET);
	assert(m_State == WAIT);

	// Create Socket
	m_Socket = Network::CreateSocket(true, port);
	if(m_Socket == INVALID_SOCKET)
	{
		return false;
	}

	// Make the address re-usable to re-run the same client instantly.
	bool reuseAddr = true;
	if(setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr)) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "setsockopt() failed with SO_REUSEADDR.");
		return false;
	}

	// Create & Start ThreaddPool for socket IO
	m_pTPIO = CreateThreadpoolIo(reinterpret_cast<HANDLE>(m_Socket), IoCompletionCallback, NULL, NULL);
	if( m_pTPIO == NULL )
	{
		ERROR_CODE(GetLastError(), "CreateThreadpoolIo() failed.");
		return false;
	}

	m_State = CREATED;

	return true;
}

//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
void Client::Destroy()
{
	if(m_State != CLOSED)
	{
		Network::CloseSocket(m_Socket);
		CancelIoEx(reinterpret_cast<HANDLE>(m_Socket), NULL);
		m_Socket = INVALID_SOCKET;
		m_State = CLOSED;
	}

	if( m_pTPIO != NULL )
	{
		WaitForThreadpoolIoCallbacks( m_pTPIO, false );
		CloseThreadpoolIo( m_pTPIO );
		m_pTPIO = NULL;
	}
}

bool Client::PostConnect(const char* ip, short port)
{
	if(m_State != CREATED)
	{
		return false;
	}	
	
	assert(m_Socket != INVALID_SOCKET);

	// Get Address Info
	addrinfo hints;
	ZeroMemory(&hints, sizeof(addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = 0;

	char portStr[32] = "";
	if( -1 == sprintf_s(portStr, sizeof(portStr), "%d", port) )
	{
		return false;
	}

	struct addrinfo* infoList = NULL;
	if (getaddrinfo(ip, portStr, &hints, &infoList) != 0) 
	{
		ERROR_CODE(WSAGetLastError(), "getaddrinfo() failed.");
		return false;
	}

	IOEvent* event = IOEvent::Create(this, IOEvent::CONNECT);

    // loop through all the results and connect to the first we can
	struct addrinfo* info = infoList;
	for(; info != NULL; info = info->ai_next) 
	{
		StartThreadpoolIo( m_pTPIO );

		if(Network::ConnectEx(m_Socket, info->ai_addr, info->ai_addrlen, &event->overlapped) == FALSE)
		{
			int error = WSAGetLastError();

			if(error != ERROR_IO_PENDING)
			{
				CancelThreadpoolIo( m_pTPIO );
				ERROR_CODE(error, "ConnectEx() failed.");
				continue;
			}
			else
			{
				return true;
			}
		}
		else
		{
			OnConnect();
			IOEvent::Destroy(event);
			return true;
		}
    }

	IOEvent::Destroy(event);
	return false;
}


void Client::PostReceive()
{
	assert(m_State == CREATED || m_State == CONNECTED);

	WSABUF recvBufferDescriptor;
	recvBufferDescriptor.buf = reinterpret_cast<char*>(m_recvBuffer);
	recvBufferDescriptor.len = Client::MAX_RECV_BUFFER;

	DWORD numberOfBytes = 0;
	DWORD recvFlags = 0;

	IOEvent* event = IOEvent::Create(this, IOEvent::RECV);

	StartThreadpoolIo( m_pTPIO );

	if(WSARecv(m_Socket, &recvBufferDescriptor, 1, &numberOfBytes, &recvFlags, &event->overlapped, NULL) == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{			
			CancelThreadpoolIo( m_pTPIO );

			if(m_State == CREATED)
			{
				// Even though we get successful connection event, if our first call of WSARecv failed, it means we failed in connecting.
				ERROR_CODE(error, "Server cannot accept this connection.");
			}
			else
			{
				ERROR_CODE(error, "WSARecv() failed.");
			}
		
			Destroy();
		}
		else
		{
			// If this is the first call of WSARecv, we can now set the state CONNECTED.
			if(m_State == CREATED)
			{
				PrintConnectionInfo(m_Socket);
				m_State = CONNECTED;
			}
		}
	}
	else
	{
		// If this is the first call of WSARecv, we can now set the state CONNECTED.
		if(m_State == CREATED)
		{
			PrintConnectionInfo(m_Socket);
			m_State = CONNECTED;
		}

		// In this case, the completion callback will have already been scheduled to be called.
	}
}


void Client::PostSend(const char* buffer, unsigned int size)
{
	if(m_State != CONNECTED)
	{
		return;
	}

	WSABUF recvBufferDescriptor;
	recvBufferDescriptor.buf = reinterpret_cast<char*>(m_sendBuffer);
	recvBufferDescriptor.len = size;

	CopyMemory(m_sendBuffer, buffer, size);

	DWORD numberOfBytes = size;
	DWORD sendFlags = 0;

	IOEvent* event = IOEvent::Create(this, IOEvent::SEND);

	StartThreadpoolIo( m_pTPIO );

	int ret = WSASend(m_Socket, &recvBufferDescriptor, 1, &numberOfBytes, sendFlags, &event->overlapped, NULL);
	if(ret == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{
			CancelThreadpoolIo( m_pTPIO );

			ERROR_CODE(error, "WSASend() failed.");

			// Error Handling!!! //
			Destroy();
		}
	}
	else
	{
		// In this case, the completion callback will have already been scheduled to be called.
	}	
}


bool Client::Shutdown()
{
	if(m_State != CONNECTED)
	{
		return false;
	}

	assert(m_Socket != INVALID_SOCKET);
	
	if(shutdown(m_Socket, SD_SEND) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "shutdown() failed.");
		return false;
	}

	return true;
}


void Client::OnConnect()
{
	// The socket s does not enable previously set properties or options until SO_UPDATE_CONNECT_CONTEXT is set on the socket. 
	if(setsockopt(m_Socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 1) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "setsockopt() failed.");
	}
	else
	{
		PostReceive();
	}
}


void Client::OnRecv(DWORD dwNumberOfBytesTransfered)
{
	// Do not process packet received here.
	// Instead, publish event with the packet and call PostRecv()
	m_recvBuffer[dwNumberOfBytesTransfered] = '\0';
	TRACE("OnRecv() : %s", m_recvBuffer);

	// To maximize performance, post recv request ASAP. 
	PostReceive();
}


void Client::OnSend(DWORD dwNumberOfBytesTransfered)
{
	TRACE("OnSend() : %d", dwNumberOfBytesTransfered);
}


void Client::OnClose()
{
	TRACE("OnClose()");

	Destroy();
}
