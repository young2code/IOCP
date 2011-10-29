#include "ClientMan.h"
#include "Client.h"

#include "..\Log.h"

#include <cassert>

namespace
{
	// Memory Pool for clients.
	typedef boost::singleton_pool<Client, sizeof(Client)> PoolClient;
}


/* static */ void CALLBACK ClientMan::WorkerRemoveClient(PTP_CALLBACK_INSTANCE /* Instance */, PVOID Context)
{
	Client* client = static_cast<Client*>(Context);
	assert(client);

	ClientMan::Instance()->RemoveClient(client);
}


ClientMan::ClientMan(void)
{
	InitializeCriticalSection(&m_CSForClients);
}

ClientMan::~ClientMan(void)
{
	RemoveClients();

	EnterCriticalSection(&m_CSForClients);
}


void ClientMan::AddClients(int numClients)
{
	EnterCriticalSection(&m_CSForClients);

	for( int i = 0; i < numClients ; ++ i )
	{
		Client* client = m_PoolClient.construct();

		if(client->Create(0))
		{
			m_listClient.push_back(client);
		}
		else
		{
			m_PoolClient.destroy(client);
		}
	}

	LeaveCriticalSection(&m_CSForClients);
}

void ClientMan::ConnectClients(const char* ip, u_short port)
{
	EnterCriticalSection(&m_CSForClients);

	for(int i = 0 ; i != static_cast<int>(m_listClient.size()) ; ++i)
	{
		m_listClient[i]->PostConnect(ip, port);
	}

	LeaveCriticalSection(&m_CSForClients);
}

void ClientMan::ShutdownClients()
{
	EnterCriticalSection(&m_CSForClients);

	for(int i = 0 ; i != static_cast<int>(m_listClient.size()) ; ++i)
	{
		m_listClient[i]->Shutdown();
	}

	LeaveCriticalSection(&m_CSForClients);
}

void ClientMan::RemoveClients()
{
	EnterCriticalSection(&m_CSForClients);

	for(int i = 0 ; i != static_cast<int>(m_listClient.size()) ; ++i)
	{
		m_PoolClient.destroy(m_listClient[i]);
	}
	m_listClient.clear();

	LeaveCriticalSection(&m_CSForClients);
}

void ClientMan::Send(const string& msg)
{
	EnterCriticalSection(&m_CSForClients);

	for(int i = 0 ; i != static_cast<int>(m_listClient.size()) ; ++i)	
	{
		m_listClient[i]->PostSend(msg.c_str(), msg.length());
	}

	LeaveCriticalSection(&m_CSForClients);
}

void ClientMan::PostRemoveClient(Client* client)
{
	if(TrySubmitThreadpoolCallback(ClientMan::WorkerRemoveClient, client, NULL) == false)
	{
		ERROR_CODE(GetLastError(), "Could not start WorkerRemoveClient.");

		// This is not good. We should remove the client in a different thread to wait until its IO operations are complete.
		// You need a fallback strategy. DO NOT JUST FOLLOW THIS EXAMPLE. YOU HAVE BEEN WARNED. 
		RemoveClient(client);
	}
}

void ClientMan::RemoveClient(Client* client)
{
	EnterCriticalSection(&m_CSForClients);

	ClientList::iterator itor = find(m_listClient.begin(), m_listClient.end(), client);

	if( itor != m_listClient.end() )
	{
		m_PoolClient.destroy(*itor);
		m_listClient.erase(itor);
	}

	LeaveCriticalSection(&m_CSForClients);
}

size_t ClientMan::GetNumClients()
{
	EnterCriticalSection(&m_CSForClients);

	size_t num = m_listClient.size();

	LeaveCriticalSection(&m_CSForClients);

	return num;
}

bool ClientMan::IsAlive(const Client* client)
{
	EnterCriticalSection(&m_CSForClients);

	ClientList::const_iterator itor = find(m_listClient.begin(), m_listClient.end(), client);
	bool result = itor != m_listClient.end();

	LeaveCriticalSection(&m_CSForClients);

	return result;
}