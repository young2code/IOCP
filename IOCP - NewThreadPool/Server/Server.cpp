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
/* static */ void CALLBACK Server::IoCompletionCallback(PTP_CALLBACK_INSTANCE /* Instance */, PVOID /* Context */,
														PVOID Overlapped, ULONG IoResult, ULONG_PTR NumberOfBytesTransferred, PTP_IO /* Io */)
{
	IOEvent* event = CONTAINING_RECORD(Overlapped, IOEvent, GetOverlapped());
	assert(event);

	if(IoResult != ERROR_SUCCESS)
	{
		ERROR_CODE(IoResult, "I/O operation failed. type[%d]", event->GetType());

		switch(event->GetType())
		{
		case IOEvent::SEND:
			Server::Instance()->OnSend(event, NumberOfBytesTransferred);
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
			if(NumberOfBytesTransferred > 0)
			{
				Server::Instance()->OnRecv(event, NumberOfBytesTransferred);
			}
			else
			{
				Server::Instance()->OnClose(event);
			}
			break;

		case IOEvent::SEND:
			Server::Instance()->OnSend(event, NumberOfBytesTransferred);
			break;

		default: assert(false); break;
		}
	}

	IOEvent::Destroy(event);
}


void CALLBACK Server::WorkerPostAccept(PTP_CALLBACK_INSTANCE /* Instance */, PVOID Context, PTP_WORK /* Work */)
{
	Server* server = static_cast<Server*>(Context);
	assert(server);

	bool loop = true;
	while(loop)
	{
		server->PostAccept();
	}
}


void CALLBACK Server::WorkerAddClient(PTP_CALLBACK_INSTANCE /* Instance */, PVOID Context)
{
	Client* client = static_cast<Client*>(Context);
	assert(client);

	Server::Instance()->AddClient(client);
}


void CALLBACK Server::WorkerRemoveClient(PTP_CALLBACK_INSTANCE /* Instance */, PVOID Context)
{
	Client* client = static_cast<Client*>(Context);
	assert(client);

	Server::Instance()->RemoveClient(client);
}


void CALLBACK Server::WorkerProcessRecvPacket(PTP_CALLBACK_INSTANCE /* Instance */, PVOID Context)
{
	Packet* packet = static_cast<Packet*>(Context);
	assert(packet);

	Server::Instance()->Echo(packet);
}


//---------------------------------------------------------------------------------//
//---------------------------------------------------------------------------------//
Server::Server(void)
: m_pTPIO(NULL),
  m_AcceptTPWORK(NULL),
  m_listenSocket(INVALID_SOCKET),
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

	// Create & Start ThreaddPool for socket IO
	m_pTPIO = CreateThreadpoolIo(reinterpret_cast<HANDLE>(m_listenSocket), Server::IoCompletionCallback, NULL, NULL);
	if( m_pTPIO == NULL )
	{
		ERROR_CODE(WSAGetLastError(), "Could not assign the listen socket to the IOCP handle.");
		Destroy();
		return false;
	}

	// Start listening
	StartThreadpoolIo( m_pTPIO );
	if(listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		ERROR_CODE(WSAGetLastError(), "listen() failed.");
		return false;
	}

	// Create critical sections for m_Clients
	InitializeCriticalSection(&m_CSForClients);

	// Create Accept worker
	m_AcceptTPWORK = CreateThreadpoolWork(Server::WorkerPostAccept, this, NULL);
	if(m_AcceptTPWORK == NULL)
	{
		ERROR_CODE(GetLastError(), "Could not create AcceptEx worker TPIO.");
		Destroy();
		return false;
	}	
	SubmitThreadpoolWork(m_AcceptTPWORK);

	return true;
}


void Server::Destroy()
{
	if( m_AcceptTPWORK != NULL )
	{
		WaitForThreadpoolWorkCallbacks( m_AcceptTPWORK, true );
		CloseThreadpoolWork( m_AcceptTPWORK );
		m_AcceptTPWORK = NULL;
	}

	if( m_listenSocket != INVALID_SOCKET )
	{
		Network::CloseSocket(m_listenSocket);
		CancelIoEx(reinterpret_cast<HANDLE>(m_listenSocket), NULL);
		m_listenSocket = INVALID_SOCKET;
	}

	if( m_pTPIO != NULL )
	{
		WaitForThreadpoolIoCallbacks( m_pTPIO, true );
		CloseThreadpoolIo( m_pTPIO );
		m_pTPIO = NULL;
	}
	
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

			StartThreadpoolIo( m_pTPIO );
			if ( FALSE == Network::AcceptEx(m_listenSocket, client->GetSocket(), &event->GetOverlapped()))
			{
				int error = WSAGetLastError();

				if(error != ERROR_IO_PENDING)
				{
					CancelThreadpoolIo( m_pTPIO );

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

	StartThreadpoolIo(client->GetTPIO());

	if(WSARecv(client->GetSocket(), &recvBufferDescriptor, 1, &numberOfBytes, &recvFlags, &event->GetOverlapped(), NULL) == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{
			CancelThreadpoolIo(client->GetTPIO());

			ERROR_CODE(error, "WSARecv() failed.");
			
			OnClose(event);
			IOEvent::Destroy(event);
		}
	}
	else
	{
		// In this case, the completion callback will have already been scheduled to be called.
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
	
	StartThreadpoolIo(client->GetTPIO());

	if(WSASend(client->GetSocket(), &recvBufferDescriptor, 1, NULL, sendFlags, &event->GetOverlapped(), NULL) == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if(error != ERROR_IO_PENDING)
		{
			CancelThreadpoolIo(client->GetTPIO());

			ERROR_CODE(error, "WSASend() failed.");

			RemoveClient(client);
		}
	}
	else
	{
		// In this case, the completion callback will have already been scheduled to be called.
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
	if(TrySubmitThreadpoolCallback(Server::WorkerAddClient, event->GetClient(), NULL) == false)
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
	if(TrySubmitThreadpoolCallback(Server::WorkerProcessRecvPacket, packet, NULL) == false)
	{
		ERROR_CODE(GetLastError(), "Could not start WorkerProcessRecvPacket. call it directly.");

		Echo(packet);
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
	if(TrySubmitThreadpoolCallback(Server::WorkerRemoveClient, event->GetClient(), NULL) == false)
	{
		ERROR_CODE(GetLastError(), "can't start WorkerRemoveClient. call it directly.");

		RemoveClient(event->GetClient());
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
		TP_IO* pTPIO = CreateThreadpoolIo(reinterpret_cast<HANDLE>(client->GetSocket()), Server::IoCompletionCallback, NULL, NULL);
		if(pTPIO == NULL)
		{
			ERROR_CODE(GetLastError(), "CreateThreadpoolIo failed for a client.");

			RemoveClient(client);
		}
		else
		{
			std::string ip;
			u_short port = 0;
			Network::GetRemoteAddress(client->GetSocket(), ip, port);
			TRACE("[%d] Accept succeeded. client address : ip[%s], port[%d]", GetCurrentThreadId(), ip.c_str(), port);

			client->SetTPIO(pTPIO);

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
