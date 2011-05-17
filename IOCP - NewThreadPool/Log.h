#pragma once

namespace Log
{
	void Trace(const char * msg, ...);

	void Error(const char* fileName, const char* funcName, int line, const char* msg, ...);
	void Error(const char* fileName, const char* funcName, int line, int code, const char* msg, ...);

	void Setup();
	void Cleanup();

	void EnableTrace(bool enable);
}

#define TRACE(msg, ...) Log::Trace(msg, __VA_ARGS__);
#define ERROR_MSG(msg, ...) Log::Error(__FILE__, __FUNCTION__, __LINE__, msg, __VA_ARGS__);
#define ERROR_CODE(code, msg, ...) Log::Error(__FILE__, __FUNCTION__, __LINE__, code, msg, __VA_ARGS__);
