#include "cmSystemTools.h"

#ifdef _WIN32
#include <WinSock2.h>
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/ip.h>
typedef int SOCKET;
static void closesocket(SOCKET socket) { close(socket); }
#endif

/*
	This is a very basic abstraction of the UNIX socket with blocking all-or-nothing I/O.
	Long-term, we may want to switch it to using libuv as the rest of CMake does, but the current basic implementation should be good enough for both Windows and Linux.
*/
class BasicIncomingSocket
{
private:
	SOCKET m_Socket, m_AcceptedSocket;

public:
	BasicIncomingSocket(int tcpPort) : m_Socket(0), m_AcceptedSocket(0)
	{
		sockaddr_in addr = {
			AF_INET,
		};
		addr.sin_port = htons(tcpPort);

		m_Socket = socket(AF_INET, SOCK_STREAM, 0);

		if (m_Socket < 0)
		{
			cmSystemTools::Error("Failed to create a listening socket for the debug server.");
			return;
		}

		if (bind(m_Socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		{
			closesocket(m_Socket);
			m_Socket = -1;
			cmSystemTools::Error("Failed to bind the listening socket for the debug server.");
			return;
		}

		if (listen(m_Socket, 1) < 0)
		{
			closesocket(m_Socket);
			m_Socket = -1;
			cmSystemTools::Error("Failed to start the listening socket for the debug server.");
			return;
		}
	}

	bool Accept()
	{
		if (m_Socket < 0)
			return false;
		sockaddr_in addr;
		socklen_t len = sizeof(addr);
		m_AcceptedSocket = accept(m_Socket, (struct sockaddr *)&addr, &len);
		if (m_AcceptedSocket < 0)
		{
			cmSystemTools::Error("Failed to accept a debugging connection.");
			return false;
		}

		return true;
	}

	bool HasIncomingData()
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(m_AcceptedSocket, &fds);

		timeval timeout = {
			0,
		};
		int result = select(m_AcceptedSocket + 1, &fds, NULL, NULL, &timeout);
		return result > 0;
	}

	bool Write(const void *pData, size_t size)
	{
		if (m_AcceptedSocket <= 0)
			return false;
		int done = send(m_AcceptedSocket, (const char *)pData, size, 0);
		return done == size;
	}

	bool ReadAll(void *pData, size_t size)
	{
		if (m_AcceptedSocket <= 0)
			return false;
		int done = recv(m_AcceptedSocket, (char *)pData, size, MSG_WAITALL);
		return done == size;
	}

	~BasicIncomingSocket()
	{
		if (m_AcceptedSocket > 0)
			closesocket(m_AcceptedSocket);
		if (m_Socket > 0)
			closesocket(m_Socket);
	}
};
