#pragma once
#include <winsock2.h>
#include <vector>
#include <string>
#include <boost\pool\object_pool.hpp>

#include "..\TSingleton.h"

class Client;

using namespace std;

class ClientMan : public TSingleton<ClientMan>
{
public:
	ClientMan(void);
	virtual ~ClientMan(void);

	void AddClients(short& port, int numClients);
	void ConnectClients(const char* ip, u_short port);
	void ShutdownClients();
	void RemoveClients();
	void Send(const string& msg);
	void RemoveClient(const Client* client);

	bool IsAlive(const Client* client);
	size_t GetNumClients();

private:
	typedef vector<Client*> ClientList;
	ClientList m_listClient;

	typedef boost::object_pool<Client> PoolTypeClient;
	PoolTypeClient m_PoolClient;

	CRITICAL_SECTION m_CSForClients;
};
