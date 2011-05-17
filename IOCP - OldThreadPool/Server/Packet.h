#pragma once
#include <Windows.h>

class Client;
class Packet
{
private:
	enum
	{
		MAX_BUFF_SIZE = 1024,
	};
	
public:
	static Packet* Create(Client* sender, const BYTE* buff, DWORD size);
	static void Destroy(Packet* packet);

public:
	Client* GetSender() { return m_Sender; }
	DWORD GetSize() { return m_Size; }
	BYTE* GetData() { return m_Data; }

private:
	Packet();
	~Packet();
	Packet(const Packet& rhw);
	Packet& operator=(const Packet& input);

private:
	Client* m_Sender;
	DWORD m_Size;
	BYTE m_Data[MAX_BUFF_SIZE];
};
