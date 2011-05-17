
#include <string>
#include <iostream>
#include <vector>
using namespace std;

#include "..\\Log.h"
#include "..\\Network.h"
#include "ClientMan.h"

//The IANA suggests 49152 to 65535 as "dynamic and/or private ports".
const short PORT_START = 50000;

void main(int argc, char* argv[])
{
	Log::Setup();

	if(argc != 4)
	{
		TRACE("Please add server IP, port and max number of clients in command line.");
		TRACE("(ex) 127.0.0.1 1234 1000");
		return;
	}

	const char* serverIP = argv[1];
	u_short serverPort = static_cast<u_short>(atoi(argv[2]));
	int maxClients = atoi(argv[3]);

	TRACE("Input : Server IP: %s, Port : %d, Max Clients : %i", serverIP, serverPort, maxClients);

	if(Network::Initialize() == false)
	{
		return;
	}

	ClientMan::New();

	short port = PORT_START;

	string input;
	bool loop = true;
	while(loop)
	{
		std::getline(cin, input);

		if(input == "`size")
		{
			TRACE(" Number of Clients : %d", ClientMan::Instance()->GetNumClients());
		}
		else if(input =="`create")
		{
			ClientMan::Instance()->AddClients(port, maxClients);
		}
		else if(input == "`connect")
		{
			ClientMan::Instance()->ConnectClients(serverIP, serverPort);
		}
		else if(input == "`shutdown")
		{
			ClientMan::Instance()->ShutdownClients();
		}
		else if(input == "`remove")
		{
			ClientMan::Instance()->RemoveClients();
		}
		else if(input == "`enable_trace")
		{
			Log::EnableTrace(true);
		}
		else if(input == "`disable_trace")
		{
			Log::EnableTrace(false);
		}
		else if(!input.empty() && input.at(0) == '`')
		{
			TRACE("wrong command.");
		}
		else if(input.length() != 0)
		{
			ClientMan::Instance()->Send(input);
		}
	}
	
	ClientMan::Delete();

	Network::Deinitialize();

	Log::Cleanup();

	return;
}
