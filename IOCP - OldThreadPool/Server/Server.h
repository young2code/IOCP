#pragma once

#include <winsock2.h>
#include <vector>

#include "..\TSingleton.h"

class Client;
class Packet;
class IOEvent;

class Server :  public TSingleton<Server>
{
private:
	// Callback Routine
	static void WINAPI OnIOCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

	// Worker Thread Functions
	static DWORD WINAPI WorkerPostAccept(LPVOID lpParam);
	static DWORD WINAPI WorkerAddClient(LPVOID lpParam);
	static DWORD WINAPI WorkerRemoveClient(LPVOID lpParam);
	static DWORD WINAPI WorkerProcessRecvPacket(LPVOID lpParam);
	static DWORD WINAPI WorkerProcessSentPacket(LPVOID lpParam);

public:
	Server();
	virtual ~Server();

	bool Create(short port, int maxPostAccept);
	void Destroy();

	size_t GetNumClients();
	long GetNumPostAccepts();

private:
	void PostAccept();
	void PostRecv(Client* client);
	void PostSend(Client* client, Packet* packet);

	void OnAccept(IOEvent* event);
	void OnRecv(IOEvent* event, DWORD dwNumberOfBytesTransfered);
	void OnSend(IOEvent* event, DWORD dwNumberOfBytesTransfered);
	void OnClose(IOEvent* event);

	void AddClient(Client* client);
	void RemoveClient(Client* client);

	void Echo(Packet* packet);

private:
	Server& operator=(Server& rhs);
	Server(const Server& rhs);

private:
	SOCKET m_listenSocket;

	typedef std::vector<Client*> ClientList;
	ClientList m_Clients;

	int	m_MaxPostAccept;
	volatile long m_NumPostAccept;

	CRITICAL_SECTION m_CSForClients;
};
