#include "IOEvent.h"
#include "Client.h"
#include "Packet.h"

#include <boost/pool/singleton_pool.hpp>

typedef boost::singleton_pool<IOEvent, sizeof(IOEvent)> IOEventPool;

/* static */ IOEvent* IOEvent::Create(Type type, Client* client, Packet* packet)
{
	IOEvent* event = static_cast<IOEvent*>(IOEventPool::malloc());

	ZeroMemory(event, sizeof(IOEvent));
	event->m_Client = client;
	event->m_Type = type;
	event->m_Packet = packet;

	return event;	
}

/* static */ void IOEvent::Destroy(IOEvent* event)
{
	IOEventPool::free(event);
}

