#pragma once

#include "Log.h"
#include <windows.h>
#include <iostream>
#include <iomanip>
using namespace std;

namespace Log
{
	const int BUFFER_SIZE = 256;

	static bool s_Enable = true;

	HMODULE libModule;
	CRITICAL_SECTION logCS;

	void Error(const char * fileName, const char * funcName, int line, const char * msg, ...)
	{
		EnterCriticalSection(&logCS);

		static char buffer[BUFFER_SIZE] = {0,};
		va_list args;
		va_start(args, msg);
		vsnprintf_s(buffer, BUFFER_SIZE, BUFFER_SIZE-1, msg, args);
		va_end(args);

		cout << "File: " << fileName << "\nFunction: " << funcName << "\nLine: " << line \
			 << "\nError: " << buffer << endl;

		LeaveCriticalSection(&logCS);
	}

	void Error(const char * fileName, const char * funcName, int line, int code, const char * msg, ...)
	{
		EnterCriticalSection(&logCS);

		char* lpMessageBuffer;

		FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | 
			FORMAT_MESSAGE_FROM_SYSTEM | 
			FORMAT_MESSAGE_FROM_HMODULE,
			libModule, 
			code,  
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPSTR) &lpMessageBuffer,  
			0,  
			NULL );

		static char buffer[BUFFER_SIZE] = {0,};
		va_list args;
		va_start(args, msg);
		vsnprintf_s(buffer, BUFFER_SIZE, BUFFER_SIZE-1, msg, args);
		va_end(args);		

		cout << "File: " << fileName << "\nFunction: " << funcName << "\nLine: " << line \
			<< "\nError: " << buffer << "\nMsg: " << lpMessageBuffer << "Code: " << code << " 0x" << hex << code << endl;

		cout << dec;

		// Free the buffer allocated by the system.
		LocalFree( lpMessageBuffer ); 

		LeaveCriticalSection(&logCS);
	}

	void Trace(const char * msg, ...)
	{
		if( s_Enable )
		{
			EnterCriticalSection(&logCS);

			static char buffer[BUFFER_SIZE] = {0,};
			va_list args;
			va_start(args, msg);
			vsnprintf_s(buffer, BUFFER_SIZE, BUFFER_SIZE-1, msg, args);
			va_end(args);
			
			cout << buffer << endl;

			LeaveCriticalSection(&logCS);
		}
	}

	void Setup()
	{
		InitializeCriticalSection(&logCS);

		libModule = LoadLibraryA("NTDLL.DLL");
	}

	void Cleanup()
	{
		EnterCriticalSection(&logCS);

		if(false == FreeLibrary(libModule))
		{
			ERROR_CODE(GetLastError(), "Log::CleanUp() - FreeLibrary() failed.");
		}

		DeleteCriticalSection(&logCS);
	}

	void EnableTrace(bool enable)
	{
		s_Enable = enable;
	}
}