#include "Server.h"
#include "Client.h"
#include "Packet.h"
#include "IOEvent.h"

#include "..\Log.h"
#include "..\Network.h"
#include <iostream>
#include <cassert>
#include <algorithm>

using namespace std;


//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
void WINAPI Server::OnIOCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
{
	IOEvent* event = CONTAINING_RECORD(lpOverlapped, IOEvent, GetOverlapped());
	assert(event);

	if(dwErrorCode != ERROR_SUCCESS)
	{
		ERROR_CODE(dwErrorCode, "I/O operation failed. type[%d]", event->GetType());

		switch(event->GetType())
		{
		case IOEvent::SEND:
			Server::Instance()->OnSend(event, dwNumberOfBytesTransfered);
			break;
		}

		Server::Instance()->OnClose(event);
	}
	else
	{	
		switch(event->GetType())
		{
		case IOEvent::ACCEPT:	
			Server::Instance()->OnAccept(event);
			break;

		case IOEvent::RECV:		
			if(dwNumberOfBytesTransfered > 0)
			{
				Server::Instance()->OnRecv(event, dwNumberOfBytesTransfered);
			}
			else
			{
				Server::Instance()->OnClose(event);
			}
			break;

		case IOEvent::SEND:
			Server::Instance()->OnSend(event, dwNumberOfBytesTransfered);
			break;

		default: assert(false); break;
		}
	}

	IOEvent::Destroy(event);
}


DWORD WINAPI Server::WorkerPostAccept(LPVOID lpParam)
{
	Server* server = static_cast<Server*>(lpParam);
	assert(server);

	bool loop = true;
	while(loop)
	{
		server->PostAccept();
	}

	return 0;
}


DWORD WINAPI Server::WorkerAddClient(LPVOID lpParam)
{
	Client* client = static_cast<Client*>(lpParam);
	assert(client);

	Server::Instance()->AddClient(client);
	return 0;
}


DWORD WINAPI Server::WorkerRemoveClient(LPVOID lpParam)
{
	Client* client = static_cast<Client*>(lpParam);
	assert(client);

	Server::Instance()->RemoveClient(client);
	return 0;
}


DWORD WINAPI Server::WorkerProcessRecvPacket(LPVOID lpParam)
{
	Packet* packet = static_cast<Packet*>(lpParam);
	assert(packet);

	Server::Instance()->Echo(packet);
	return 0;
}


//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
Server::Server(void)
: m_listenSocket(INVALID_SOCKET),
  m_MaxPostAccept(0),
  m_NumPostAccept(0)
{
}


Server::~Server(void)
{
	Destroy();
}


bool Server::Create(short port, int maxPostAccept)
{	
	assert(maxPostAccept > 0);

	m_MaxPostAccept = maxPostAccept;

	// Create Listen Socket
	m_listenSocket = Network::CreateSocket(true, port);
	if(m_listenSocket == INVALID_SOCKET)
	{
		return false;
	}

	// Make the address re-usable to re-run the same server instantly.
	bool reuseAddr = true;
	if(setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr)) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "setsockopt() failed with SO_REUSEADDR.");
		Destroy();
		return false;
	}

	// Connect the listener socket to IOCP
	if(BindIoCompletionCallback(reinterpret_cast<HANDLE>(m_listenSocket), Server::OnIOCompletion, 0) == false)
	{
		ERROR_CODE(WSAGetLastError(), "Could not assign the listen socket to the IOCP handle.");
		Destroy();
		return false;	
	}

	// Start listening
	if(listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "listen() failed.");
		return false;
	}

	// Create critical sections for m_Clients
	InitializeCriticalSection(&m_CSForClients);

	// Start posting AcceptEx requests
	if(QueueUserWorkItem(Server::WorkerPostAccept, this, WT_EXECUTEDEFAULT) == false)
	{
		ERROR_CODE(GetLastError(), "Could not start posting AcceptEx.");
		Destroy();
		return false;
	}
	
	return true;
}


void Server::Destroy()
{
	Network::CloseSocket(m_listenSocket);
	
	EnterCriticalSection(&m_CSForClients);
	for(ClientList::iterator itor = m_Clients.begin() ; itor != m_Clients.end() ; ++itor)	
	{
		Client::Destroy(*itor);
	}
	m_Clients.clear();
	LeaveCriticalSection(&m_CSForClients);

	DeleteCriticalSection(&m_CSForClients);
}


void Server::PostAccept()
{
	// If the number of clients is too big, we can just stop posting aceept.
	// That's one of the benefits from AcceptEx.
	int count = m_MaxPostAccept - m_NumPostAccept;
	if( count > 0 )
	{
		int i = 0;
		for(  ; i < count ; ++i )
		{
			Client* client = Client::Create();			
			if( !client )
			{
				break;
			}

			IOEvent* event = IOEvent::Create(IOEvent::ACCEPT, client);
			assert(event);

			if ( FALSE == Network::AcceptEx(m_listenSocket, client->GetSocket(), &event->GetOverlapped()))
			{
				int error = WSAGetLastError();

				if(error != ERROR_IO_PENDING)
				{
					ERROR_CODE(error, "AcceptEx() failed.");
					Client::Destroy(client);
					IOEvent::Destroy(event);
					break;
				}
			}
			else
			{
				OnAccept(event);
				IOEvent::Destroy(event);
			}
		}

		InterlockedExchangeAdd(&m_NumPostAccept, i);	

		TRACE("[%d] Post AcceptEx : %d", GetCurrentThreadId(), m_NumPostAccept);
	}
}


void Server::PostRecv(Client* client)
{
	assert(client);

	WSABUF recvBufferDescriptor;
	recvBufferDescriptor.buf = reinterpret_cast<char*>(client->GetRecvBuff());
	recvBufferDescriptor.len = Client::MAX_RECV_BUFFER;

	DWORD numberOfBytes = 0;
	DWORD recvFlags = 0;

	IOEvent* event = IOEvent::Create(IOEvent::RECV, client);
	assert(event);

	if(WSARecv(client->GetSocket(), &recvBufferDescriptor, 1, &numberOfBytes, &recvFlags, &event->GetOverlapped(), NULL) == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{
			ERROR_CODE(error, "WSARecv() failed.");
			
			OnClose(event);
			IOEvent::Destroy(event);
		}
	}
	else
	{
		// MSDN
		// In this case, the completion routine will have already been scheduled to be called once the calling thread is in the alertable state.
	}
}


void Server::PostSend(Client* client, Packet* packet)
{
	assert(client);
	assert(packet);

	WSABUF recvBufferDescriptor;
	recvBufferDescriptor.buf = reinterpret_cast<char*>(packet->GetData());
	recvBufferDescriptor.len = packet->GetSize();

	DWORD sendFlags = 0;

	IOEvent* event = IOEvent::Create(IOEvent::SEND, client, packet);
	assert(event);
	
	if(WSASend(client->GetSocket(), &recvBufferDescriptor, 1, NULL, sendFlags, &event->GetOverlapped(), NULL) == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{
			ERROR_CODE(error, "WSASend() failed.");

			RemoveClient(client);
		}
	}
	else
	{
		// MSDN
		// In this case, the completion routine will have already been scheduled to be called once the calling thread is in the alertable state.
	}
}


void Server::OnAccept(IOEvent* event)
{
	assert(event);

	TRACE("[%d] Enter OnAccept()", GetCurrentThreadId());
	assert(event->GetType() == IOEvent::ACCEPT);

	// Check if we need to post more accept requests.
	InterlockedDecrement(&m_NumPostAccept);

	// Add client in a different thread.
	// It is because we need to return this function ASAP so that this IO worker thread can process the other IO notifications.
	// If adding client is fast enough, we can call it here but I assume it's slow.	
	if(QueueUserWorkItem(Server::WorkerAddClient, event->GetClient(), WT_EXECUTEDEFAULT) == false)
	{
		ERROR_CODE(GetLastError(), "Could not start WorkerAddClient.");

		AddClient(event->GetClient());
	}

	TRACE("[%d] Leave OnAccept()", GetCurrentThreadId());
}


void Server::OnRecv(IOEvent* event, DWORD dwNumberOfBytesTransfered)
{
	assert(event);

	TRACE("[%d] Enter OnRecv()", GetCurrentThreadId());

	BYTE* buff = event->GetClient()->GetRecvBuff();
	buff[dwNumberOfBytesTransfered] = '\0';
	TRACE("[%d] OnRecv : %s", GetCurrentThreadId(), buff);

	// Create packet by copying recv buff.
	Packet* packet = Packet::Create(event->GetClient(), event->GetClient()->GetRecvBuff(), dwNumberOfBytesTransfered);

	// If whatever game logics relying on the packet are fast enough, we can manage them here but I assume they are slow.	
	// I think it's better to request receiving ASAP and handle packets received in another thread.
	if(QueueUserWorkItem(Server::WorkerProcessRecvPacket, packet, WT_EXECUTEDEFAULT) == false)
	{
		ERROR_CODE(GetLastError(), "Could not start WorkerProcessRecvPacket. call it directly.");

		WorkerProcessRecvPacket(packet);
	}

	PostRecv(event->GetClient());

	TRACE("[%d] Leave OnRecv()", GetCurrentThreadId());
}


void Server::OnSend(IOEvent* event, DWORD dwNumberOfBytesTransfered)
{
	assert(event);

	TRACE("[%d] OnSend : %d", GetCurrentThreadId(), dwNumberOfBytesTransfered);

	// This should be fast enough to do in this I/O thread.
	// if not, we need to queue it like what we do in OnRecv().
	Packet::Destroy(event->GetPacket());
}


void Server::OnClose(IOEvent* event)
{
	assert(event);

	TRACE("Client's socket has been closed.");

	// If whatever game logics about this event are fast enough, we can manage them here but I assume they are slow.	
	if(QueueUserWorkItem(Server::WorkerRemoveClient, event->GetClient(), WT_EXECUTEDEFAULT) == false)
	{
		ERROR_CODE(GetLastError(), "can't start WorkerRemoveClient. call it directly.");

		WorkerRemoveClient(event->GetClient());
	}
}


void Server::AddClient(Client* client)
{
	assert(client);

	// The socket sAcceptSocket does not inherit the properties of the socket associated with sListenSocket parameter until SO_UPDATE_ACCEPT_CONTEXT is set on the socket.
	if (setsockopt(client->GetSocket(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char *>(&m_listenSocket), sizeof(m_listenSocket)) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "setsockopt() for AcceptEx() failed.");

		RemoveClient(client);
	}		
	else
	{
		client->SetState(Client::ACCEPTED);

		// Connect the socket to IOCP
		if(false == BindIoCompletionCallback(reinterpret_cast<HANDLE>(client->GetSocket()), Server::OnIOCompletion, 0))
		{
			ERROR_CODE(GetLastError(), "Could not assign the socket to the IOCP handle.");

			RemoveClient(client);
		}
		else
		{
			std::string ip;
			u_short port = 0;
			Network::GetRemoteAddress(client->GetSocket(), ip, port);
			TRACE("[%d] Accept succeeded. client address : ip[%s], port[%d]", GetCurrentThreadId(), ip.c_str(), port);

			EnterCriticalSection(&m_CSForClients);
			m_Clients.push_back(client);
			LeaveCriticalSection(&m_CSForClients);

			PostRecv(client);
		}
	}
}


void Server::RemoveClient(Client* client)
{
	assert(client);

	EnterCriticalSection(&m_CSForClients);

	ClientList::iterator itor = std::remove(m_Clients.begin(), m_Clients.end(), client);

	if(itor != m_Clients.end())
	{
		TRACE("[%d] RemoveClient succeeded.", GetCurrentThreadId());

		Client::Destroy(client);

		m_Clients.erase(itor);
	}

	LeaveCriticalSection(&m_CSForClients);
}


void Server::Echo(Packet* packet)
{
	assert(packet);
	assert(packet->GetSender());

	EnterCriticalSection(&m_CSForClients);

	ClientList::iterator itor = std::find(m_Clients.begin(), m_Clients.end(), packet->GetSender());

	if( itor == m_Clients.end())
	{
		// No client to send it back.
		Packet::Destroy(packet);		
	}
	else
	{
		PostSend(packet->GetSender(), packet);
	}

	LeaveCriticalSection(&m_CSForClients);
}


size_t Server::GetNumClients()
{
	EnterCriticalSection(&m_CSForClients);

	size_t num = m_Clients.size();

	LeaveCriticalSection(&m_CSForClients);

	return num;
}

long Server::GetNumPostAccepts()
{
	return m_NumPostAccept;
}
