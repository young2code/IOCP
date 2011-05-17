#pragma once

template <typename T> class TSingleton
{
private:
	static T* m_Instance;

public:
	static void New()
	{
		if( m_Instance == 0 )
		{
			m_Instance = new T;
		}
	}


	static void Delete()
	{
		delete m_Instance;
		m_Instance = 0;
	}


	static T* Instance()
	{
		return m_Instance;
	}
};

template <typename T> T* TSingleton<T>::m_Instance = 0;
