
// Proxy.cpp is very simple program to implement the HTTP proxy function.
// To make it short and clear , some error tolerant codes are omitted.
// Written by HU Zhongshan   
// e-mail huzhongshan@hotmail.com OR yangjy@mail.njust.edu.cn
// 1999-4-18
#include <iostream>
#include "Proxy.h"
#include <winsock2.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <process.h>


#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// The one and only application object

#define HTTP			"http://"
#define FTP				"ftp://"
#define PROXYPORT		5060    //Proxy Listen Port
#define BUFSIZE			10240      //Buffer size
#define MAXPORTSIZE		5
#define MAXCOMMANDLEN	512
#define MAXPROTOLEN		128
#define BACKLOG			5

using namespace std;

unsigned int __stdcall UpstreamCommunication(void *pParam);
unsigned int __stdcall DownstreamCommunication(void *pParam);

struct SocketPair {
	SOCKET  user_proxy;      //socket : local machine to proxy server
	SOCKET  proxy_server;    //socket : proxy server to remote server
	BOOL    IsUser_ProxyClosed; // status of local machine to proxy server
	BOOL    IsProxy_ServerClosed; // status of proxy server to remote server
};


struct ProxyParam {
	char Address[256];    // address of remote server
	HANDLE User_SvrOK;    // status of setup connection between proxy server and remote server
	SocketPair *pPair;    // pointer to socket pair
	int     Port;         // port which will be used to connect to remote server
};                   //This struct is used to exchange information between threads.

SOCKET    gListen_Socket;

int StartServer()
{
	WSADATA wsaData;
	sockaddr_in local;
	SOCKET listen_socket;

	if (::WSAStartup(0x202, &wsaData) != 0)
	{
		printf("\nError in Startup session.\n"); WSACleanup(); return -1;
	};

	local.sin_family = AF_INET; // IPv4 address family
	local.sin_addr.s_addr = INADDR_ANY; // Listen at any interface
	local.sin_port = htons(PROXYPORT); // Port number 5060

	listen_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (listen_socket == INVALID_SOCKET)
	{
		printf("\nError in initialization of Socket."); WSACleanup(); return -2;
	}

	if (bind(listen_socket, (sockaddr *)&local, sizeof(local)) != 0)
	{
		printf("\nError in binding socket.");	WSACleanup(); return -3;
	};

	if (listen(listen_socket, BACKLOG) != 0)
	{
		printf("\nError in listening to socket"); WSACleanup(); return -4;
	}
	gListen_Socket = listen_socket;
	//_beginthreadex(UserToProxyThread,0, NULL);   //Start accept function
	unsigned thread_idn;
	HANDLE USERTOPROXYTHREADHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); //Start another thread to listen.
	return 1;
}

int CloseServer()
{
	closesocket(gListen_Socket);
	WSACleanup();
	return 1;
}

//Analisys the string, to find the remote address
int GetAddressAndPort(char * str, char *address, int * port, char *cmd, char* protocol)
{
	char buf[BUFSIZE], command[MAXCOMMANDLEN], proto[MAXPROTOLEN], *p, portBuff[MAXPORTSIZE];
	int i = 0, j;
	sscanf(str, "%s%s%s", command, buf, proto);
	p = strstr(buf, HTTP);

	strncpy(protocol, proto, MAXPROTOLEN);

	//HTTP
	if (p)
	{
		p += strlen(HTTP);
		for (i = 0; i < strlen(p); i++)
			if ((*(p + i) == '/') || (*(p + i) == ':')) break;
		if (p[i] == ':')
		{
			j = i + 1;
			while (p[i] != ' ')
				i++;
			memcpy(portBuff, p + j, i - j);
			portBuff[i - j] = 0;
			*port = atoi(portBuff);
		}
		else
		{
			*(p + i) = 0;
			strcpy(address, p);
			p = strstr(str, HTTP);
			for (j = 0; j < i + strlen(HTTP); j++)
				*(p + j) = ' ';  //to remove the host name: GET http://www.njust.edu.cn/ HTTP1.1  ==> GET / HTTP1.1
			*port = 80;      //default http port 
		}
	}
	else if ((p = strstr(buf, FTP))!=NULL)
	{//FTP, Not supported, and following code can be omitted.
		p = strstr(buf, FTP);
		if (!p) return 0;
		p += strlen(FTP);
		for (i = 0; i < strlen(p); i++)
			if (*(p + i) == '/') break;      //Get The Remote Host
		*(p + i) = 0;
		for (j = 0; j < strlen(p); j++)
			if (*(p + j) == ':')
			{
				*port = atoi(p + j + 1);    //Get The Port
				*(p + j) = 0;
			}
			else *port = 21;

			strcpy(address, p);
			p = strstr(str, FTP);
			for (j = 0; j < i + strlen(FTP); j++)
				*(p + j) = ' ';
	}
	else
	{
		p = buf;
		if (!p) return 0;
		//p += strlen(FTP);
		for (j = 0; j < strlen(p); j++)
			if (*(p + j) == ':')
			{
				*port = atoi(p + j + 1);    //Get The Port
				*(p + j) = 0;
				break;
			}
			else *port = 80;


		for (i = 0; i < strlen(p); i++)
			if (*(p + i) == '/') break;      //Get The Remote Host
		if (i != (strlen(p)-1)) // A '/' character was found
			*(p + i) = 0;
		else
			*(p + j) = 0;


		strcpy(address, p);
		
	}
	memcpy(cmd, command, 512);
	return 1;
}

// Setup channel and read data from local , send data to remote
unsigned int __stdcall DownstreamCommunication(void *pParam)
{
	char Buffer[BUFSIZE], command[MAXCOMMANDLEN], protocol[MAXPROTOLEN];
	char buffer_end = 0; // This is for restoring the last byte of buffer after debug statements
	int  Len;
	sockaddr_in from;
	SOCKET msg_socket;
	int fromlen, retval;
	SocketPair SocketPair_struct;
	ProxyParam proxyparam_var;
	HANDLE pChildThread;
	fromlen = sizeof(from);
	msg_socket = accept(gListen_Socket, (struct sockaddr*)&from, &fromlen);
	unsigned thread_idn;

	if (msg_socket == INVALID_SOCKET)
	{
		printf("\nError  in accept "); return -5;
	}

	HANDLE USERTOPROXYTHREADHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); //Start another thread to listen.
   //recieve the first line of client

	SocketPair_struct.IsUser_ProxyClosed = FALSE;
	SocketPair_struct.IsProxy_ServerClosed = TRUE;
	SocketPair_struct.user_proxy = msg_socket;

	retval = recv(SocketPair_struct.user_proxy, Buffer, sizeof(Buffer), 0);

	if (retval == SOCKET_ERROR)
	{
		printf("\nError Recv");
		if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
		{
			closesocket(SocketPair_struct.user_proxy);
			SocketPair_struct.IsUser_ProxyClosed = TRUE;
		}
	}

	if (retval == 0)
	{
		printf("Client Close connection\n");
		if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
		{
			closesocket(SocketPair_struct.user_proxy);
			SocketPair_struct.IsUser_ProxyClosed = TRUE;
		}
	}
	else
	{

		Len = retval;
#ifdef _DEBUG
		buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

		if (Len > 0) Buffer[Len - 1] = 0;
		printf("\n---------------------------\nReceived %d bytes from client\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
		if (Len > 0 ) Buffer[Len - 1] = buffer_end;
		buffer_end = 0;
#endif
	}
	Len = retval;
	GetAddressAndPort(Buffer, proxyparam_var.Address, &proxyparam_var.Port, command, protocol);

	if (strncmp(command, "CONNECT", strlen("CONNECT")) == 0) // If command is CONNECT then reply with 200 OK and wait for input.
	{
		memset(Buffer, 0, BUFSIZE);
		strncat(Buffer, protocol, BUFSIZE);
			strncat(Buffer, " ", 1);
			char response_to_connect[] = "HTTP/1.1 200 Connection established\r\nProxy-agent: dan_proxy/1.0\r\n\r\n";
		Len = strlen(response_to_connect);
		strncpy(Buffer, response_to_connect, Len);

#ifdef _DEBUG
		buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

		if (Len > 0 ) Buffer[Len - 1] = 0;
		printf("\n---------------------------\nSent %d bytes to client\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
		if (Len > 0 ) Buffer[Len - 1] = buffer_end;
		buffer_end = 0;
#endif

		retval = send(SocketPair_struct.user_proxy, Buffer, Len, 0);
		memset(Buffer, 0, BUFSIZ);

		if (retval == SOCKET_ERROR)
		{
			printf("\nError Send");
			if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
			{
				closesocket(SocketPair_struct.user_proxy);
				SocketPair_struct.IsUser_ProxyClosed = TRUE;
			}
		}
		Len = 0;

	}

	//
	SocketPair_struct.IsUser_ProxyClosed = FALSE;
	SocketPair_struct.IsProxy_ServerClosed = TRUE;
	SocketPair_struct.user_proxy = msg_socket;

	proxyparam_var.pPair = &SocketPair_struct;
	proxyparam_var.User_SvrOK = CreateEvent(NULL, TRUE, FALSE, NULL);

	//GetAddressAndPort(Buffer, ProxyP.Address, &ProxyP.Port, command);
	unsigned threadID;
	pChildThread = (HANDLE)_beginthreadex(NULL, 0, UpstreamCommunication, (void *)&proxyparam_var, 0, &threadID);
	::WaitForSingleObject(proxyparam_var.User_SvrOK, 60000);  //Wait for connection between proxy and remote server
	::CloseHandle(proxyparam_var.User_SvrOK);

	while (SocketPair_struct.IsProxy_ServerClosed == FALSE && SocketPair_struct.IsUser_ProxyClosed == FALSE)
	{
#ifdef _DEBUG
		buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

		if (Len > 0 ) Buffer[Len - 1] = 0;
		printf("\n---------------------------\nSending %d bytes to server\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
		if (Len > 0 ) Buffer[Len - 1] = buffer_end;
		buffer_end = 0;
#endif
		retval = send(SocketPair_struct.proxy_server, Buffer, Len, 0);
		if (retval == SOCKET_ERROR)
		{
			printf("\n send() failed:error%d\n", WSAGetLastError());
			if (SocketPair_struct.IsProxy_ServerClosed == FALSE)
			{
				closesocket(SocketPair_struct.proxy_server);
				SocketPair_struct.IsProxy_ServerClosed = TRUE;
			}
			continue;
		}
		retval = recv(SocketPair_struct.user_proxy, Buffer, sizeof(Buffer), 0);

		if (retval == SOCKET_ERROR)
		{
			printf("\nError Recv");
			if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
			{
				closesocket(SocketPair_struct.user_proxy);
				SocketPair_struct.IsUser_ProxyClosed = TRUE;
			}
			continue;
		}
		if (retval == 0)
		{
			printf("Client Close connection\n");
			if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
			{
				closesocket(SocketPair_struct.user_proxy);
				SocketPair_struct.IsUser_ProxyClosed = TRUE;
			}
			break;
		}
		else
		{

			Len = retval;
#ifdef _DEBUG
			buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

			if (Len > 0) Buffer[Len - 1] = 0;
			printf("\n---------------------------\nReceived %d bytes from client\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
			if (Len > 0 ) Buffer[Len - 1] = buffer_end;
			buffer_end = 0;
#endif
		}
		Len = retval;

	} //End While

	if (SocketPair_struct.IsProxy_ServerClosed == FALSE)
	{
		closesocket(SocketPair_struct.proxy_server);
		SocketPair_struct.IsProxy_ServerClosed = TRUE;
	}
	if (SocketPair_struct.IsUser_ProxyClosed == FALSE)
	{
		closesocket(SocketPair_struct.user_proxy);
		SocketPair_struct.IsUser_ProxyClosed = TRUE;
	}
	::WaitForSingleObject(pChildThread, 20000);  //Should check the return value
	return 0;
}

// Read data from remote and send data to local
unsigned int __stdcall UpstreamCommunication(void *pParam)
{
	ProxyParam * proxyparam_ptr = (ProxyParam*)pParam;
	char Buffer[BUFSIZE];
	char *server_name = "localhost";
	char buffer_end = 0; // This is for restoring the last byte of buffer after debug statements
	unsigned short port;
	int retval, Len;
	unsigned int server_addr;
	int socket_type;
	struct sockaddr_in server_sockaddr_in;
	struct hostent *server_hostent_struct_ptr;
	SOCKET  conn_socket;

	socket_type = SOCK_STREAM;
	server_name = proxyparam_ptr->Address;
	port = proxyparam_ptr->Port;

	if (isalpha(server_name[0])) {   /* server address is a name */
		server_hostent_struct_ptr = gethostbyname(server_name);
	}
	else { /* Convert nnn.nnn address to a usable one */
		server_addr = inet_addr(server_name);
		server_hostent_struct_ptr = gethostbyaddr((char *)&server_addr, 4, AF_INET);
	}
	if (server_hostent_struct_ptr == NULL) {
		printf("\n\n[ERROR]: Client: Cannot resolve address [%s]: Error %d\n",
			server_name, WSAGetLastError());
		::SetEvent(proxyparam_ptr->User_SvrOK);
		return 0;
	}

	//
	// Copy the resolved information into the sockaddr_in structure
	//
	memset(&server_sockaddr_in, 0, sizeof(server_sockaddr_in));
	memcpy(&(server_sockaddr_in.sin_addr), server_hostent_struct_ptr->h_addr, server_hostent_struct_ptr->h_length);
	server_sockaddr_in.sin_family = server_hostent_struct_ptr->h_addrtype;
	server_sockaddr_in.sin_port = htons(port);

	conn_socket = socket(AF_INET, socket_type, 0); /* Open a socket */
	
	if (conn_socket < 0) {
		printf("\n\n[ERROR]: Client: Error Opening socket: Error %d\n",
			WSAGetLastError());
		proxyparam_ptr->pPair->IsProxy_ServerClosed = TRUE;
		::SetEvent(proxyparam_ptr->User_SvrOK);
		return -1;
	}


#ifdef _DEBUG
	printf("Client connecting to: %s\n", server_hostent_struct_ptr->h_name);
#endif
	if (connect(conn_socket, (struct sockaddr*)&server_sockaddr_in, sizeof(server_sockaddr_in))
		== SOCKET_ERROR) {
		printf("\n\n[ERROR]: connect() failed: %d\n", WSAGetLastError());
		proxyparam_ptr->pPair->IsProxy_ServerClosed = TRUE;
		::SetEvent(proxyparam_ptr->User_SvrOK);
		return -1;
	}
	proxyparam_ptr->pPair->proxy_server = conn_socket;
	proxyparam_ptr->pPair->IsProxy_ServerClosed = FALSE;
	::SetEvent(proxyparam_ptr->User_SvrOK);
	// cook up a string to send
	while (!proxyparam_ptr->pPair->IsProxy_ServerClosed && !proxyparam_ptr->pPair->IsUser_ProxyClosed) // Both connections are open
	{
		retval = recv(conn_socket, Buffer, sizeof(Buffer), 0);
		if (retval == SOCKET_ERROR) {
			printf("\n\n[ERROR]: recv() from server failed: error %d\n", WSAGetLastError());
			closesocket(conn_socket);
			proxyparam_ptr->pPair->IsProxy_ServerClosed = TRUE;
			break;
		}
		Len = retval;
		if (retval == 0) {
			printf("Server closed connection\n");
			closesocket(conn_socket);
			proxyparam_ptr->pPair->IsProxy_ServerClosed = TRUE;
			break;
		}
		else
		{

#ifdef _DEBUG
			buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

			if (Len > 0 ) Buffer[Len - 1] = 0;
			printf("\n---------------------------Received from server and Sending %d bytes to user\nData:\n\n%s\n\n---------------------\n", Len, Buffer);
			if (Len > 0 ) Buffer[Len - 1] = buffer_end;
			buffer_end = 0;
#endif
		}
		retval = send(proxyparam_ptr->pPair->user_proxy, Buffer, Len, 0);
		if (retval == SOCKET_ERROR) {
			printf("\n\n[ERROR]: send() to server failed: error %d\n", WSAGetLastError());
			closesocket(proxyparam_ptr->pPair->user_proxy);
			proxyparam_ptr->pPair->IsUser_ProxyClosed = TRUE;
			break;
		}
	}
	if (proxyparam_ptr->pPair->IsProxy_ServerClosed == FALSE)
	{
		closesocket(proxyparam_ptr->pPair->proxy_server);
		proxyparam_ptr->pPair->IsProxy_ServerClosed = TRUE;
	}
	if (proxyparam_ptr->pPair->IsUser_ProxyClosed == FALSE)
	{
		closesocket(proxyparam_ptr->pPair->user_proxy);
		proxyparam_ptr->pPair->IsUser_ProxyClosed = TRUE;
	}
	return 1;
}



int main(int argc, TCHAR* argv[], TCHAR* envp[])
{
	int nRetCode = 0; // Error code

	try
	{
		printf("Starting server\n\n");
		StartServer();

		while (1)
			;
	}
	catch (const std::exception&)
	{
		CloseServer();
	}
	


	return nRetCode;
}


