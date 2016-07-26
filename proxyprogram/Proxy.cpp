// Proxy.cpp is a simple HTTP/HTTPS proxy server based on a simpler proxy server developed by Hu Zhonshan
// Written by Daniyal Yasin   
// e-mail daniyalyasin93@gmail.com
// 26-07-2016


#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include <iostream>
#include "Proxy.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <process.h>


#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

#define HTTP			"http://"
#define FTP				"ftp://"
#define PROXYPORT		5060		//Proxy Listen Port
#define BUFSIZE			10240		//Buffer size
#define MAXPORTSIZE		5
#define MAXCOMMANDLEN	512
#define MAXPROTOLEN		128
#define BACKLOG			5
#define PROXY_AGENT		"dan_proxy"
#define VERSION			"1.0"
#define	SERVER_NAME		"localhost"

using namespace std;

unsigned int __stdcall UpstreamCommunication(void *pParam);
unsigned int __stdcall DownstreamCommunication(void *pParam);

struct SocketPair {
	SOCKET  downstream;					//socket : client to proxy server
	SOCKET  upstream;					//socket : proxy server to remote server
	BOOL    IsDownstreamDisconnected;	// Status of connection with client
	BOOL    IsUpstreamDisconnected;		// Status of connecition with remote server
};


struct ProxyParam {
	char Address[256];    // address of remote server
	HANDLE User_SvrOK;    // flag/event to signal that connection has been established with remote server
	SocketPair *pPair;    // pointer to socket pair
	int     Port;         // port which will be used to connect to remote server
};                   //This struct is used to exchange information between threads.

SOCKET    gListen_Socket;

int StartServer()
{
	SOCKET listen_socket;
	sockaddr_in local;
#ifdef _WIN32
	WSADATA wsaData;

	if (::WSAStartup(0x202, &wsaData) != 0)
	{
		printf("\nError in Startup session.\n"); WSACleanup(); return -1;
	};
#endif // _WIN32

	local.sin_family = AF_INET; // IPv4 address family
	local.sin_addr.s_addr = INADDR_ANY; // Listen at any interface
	local.sin_port = htons(PROXYPORT); // Port number 5060

	listen_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (listen_socket == INVALID_SOCKET)
	{
#ifdef _WIN32
		printf("\nError in initialization of Socket."); WSACleanup(); return -2;
#endif
	}

	if (bind(listen_socket, (sockaddr *)&local, sizeof(local)) != 0)
	{
#ifdef _WIN32
		printf("\nError in binding socket.");	WSACleanup(); return -3;
#endif
	};

	if (listen(listen_socket, BACKLOG) != 0)
	{
#ifdef _WIN32
		printf("\nError in listening to socket"); WSACleanup(); return -4;
#endif
	}
	
	gListen_Socket = listen_socket; // Assign value to global listening socket variable
	
	unsigned thread_idn; // Receives thread identifier
	HANDLE USERTOPROXYTHREADHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); //Start another thread to listen.
	return 1;
}

int CloseServer()
{
	closesocket(gListen_Socket);
#ifdef _WIN32
	WSACleanup();
#endif
	return 1;
}

//Analyze the string, to find the remote address
int GetAddressAndPort(char * str, char *address, int * port, char *cmd, char* protocol)
{
	char buf[BUFSIZE], command[MAXCOMMANDLEN], proto[MAXPROTOLEN], *p, portBuff[MAXPORTSIZE];
	int i = 0, j;
	
	sscanf(str, "%s%s%s", command, buf, proto);
	
	if ((command == NULL) && (buf == NULL) && (proto == NULL)) // Message not according to format
		return -1;

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
				*(p + j) = ' ';						//to remove the host name: GET http://www.njust.edu.cn/ HTTP1.1  ==> GET / HTTP1.1
			*port = 80;								//default http port 
		}
	}
	else if ((p = strstr(buf, FTP))!=NULL)
	{	// FTP
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

// Setup channel and read data from client, send data to remote
unsigned int __stdcall DownstreamCommunication(void *pParam)
{
	char Buffer[BUFSIZE], command[MAXCOMMANDLEN], protocol[MAXPROTOLEN];
	char buffer_end = 0; // This is for restoring the last byte of buffer after debug statements
	int  Len;
	sockaddr_in from; // Caller address is stored in this structure
	SOCKET msg_socket;
	int fromlen, retval;
	SocketPair SocketPair_struct;
	ProxyParam proxyparam_var;
	HANDLE pChildThread;
	fromlen = sizeof(from);

	msg_socket = accept(gListen_Socket, (struct sockaddr*)&from, &fromlen); // Block until connection received

	unsigned thread_idn;

	if (msg_socket == INVALID_SOCKET)
	{
		printf("\nError  in accept "); return -5;
	}

	HANDLE DOWNSTREAMHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); // Start another thread to listen for other connections																													 

	SocketPair_struct.IsDownstreamDisconnected = FALSE; // A client has connected to the proxy
	SocketPair_struct.IsUpstreamDisconnected = TRUE;	// An upstream connected to a destination server has not yet been established
	SocketPair_struct.downstream = msg_socket;

	retval = recv(SocketPair_struct.downstream, Buffer, sizeof(Buffer), 0);	// Receive from client

	if (retval == SOCKET_ERROR)
	{
		printf("\nError Recv");
		if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
		{
			closesocket(SocketPair_struct.downstream);
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
			return -1;
		}
	}

	if (retval == 0)
	{
		printf("Client Close connection\n");
		if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
		{
			closesocket(SocketPair_struct.downstream);
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
			return -1;
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

	
		// Proxyparam structure is used to store the address and the port
	if (GetAddressAndPort(Buffer, proxyparam_var.Address, &proxyparam_var.Port, command, protocol) == -1)
	{
		printf("\nUnknown response");
		if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
		{
			closesocket(SocketPair_struct.downstream);
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
			return -1;
		}
	}

	if (strncmp(command, "CONNECT", strlen("CONNECT")) == 0) // If command is CONNECT then reply with 200 OK and wait for input.
	{
		memset(Buffer, 0, BUFSIZE);
		strncat(Buffer, protocol, BUFSIZE);
			strncat(Buffer, " ", 1);
			char response_to_connect[100] = "HTTP/1.1 200 Connection established\r\nProxy-agent: ";
			strncat(response_to_connect, PROXY_AGENT, strnlen(PROXY_AGENT, 10));
			strncat(response_to_connect, "/", 1);
			strncat(response_to_connect, VERSION, 3);
			strncat(response_to_connect, "\r\n\r\n", 4);
			response_to_connect[99] = 0;	// To guard against buffer overflows
		Len = strlen(response_to_connect);
		strncpy(Buffer, response_to_connect, Len);

#ifdef _DEBUG
		buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

		if (Len > 0 ) Buffer[Len - 1] = 0;
		printf("\n---------------------------\nSent %d bytes to client\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
		if (Len > 0 ) Buffer[Len - 1] = buffer_end;
		buffer_end = 0;
#endif

		retval = send(SocketPair_struct.downstream, Buffer, Len, 0); // Send response to client
		memset(Buffer, 0, BUFSIZ);

		if (retval == SOCKET_ERROR)
		{
			printf("\nError Send");
			if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
			{
				closesocket(SocketPair_struct.downstream);
				SocketPair_struct.IsDownstreamDisconnected = TRUE;
			}
		}
		Len = 0;

	}

	// Set flags
	SocketPair_struct.IsDownstreamDisconnected = FALSE;
	SocketPair_struct.IsUpstreamDisconnected = TRUE;
	SocketPair_struct.downstream = msg_socket;

	proxyparam_var.pPair = &SocketPair_struct;
#ifdef _WIN32
	proxyparam_var.User_SvrOK = CreateEvent(NULL, TRUE, FALSE, NULL);
#endif

	//GetAddressAndPort(Buffer, ProxyP.Address, &ProxyP.Port, command);
	unsigned threadID;
	pChildThread = (HANDLE)_beginthreadex(NULL, 0, UpstreamCommunication, (void *)&proxyparam_var, 0, &threadID);
#ifdef _WIN32
	::WaitForSingleObject(proxyparam_var.User_SvrOK, 60000);  // Wait for connection between proxy and remote server. Timeout is 60 seconds
	::CloseHandle(proxyparam_var.User_SvrOK);
#endif
	

	while (SocketPair_struct.IsUpstreamDisconnected == FALSE && SocketPair_struct.IsDownstreamDisconnected == FALSE) // Loop to forward communication from client to server and vice versa
	{
#ifdef _DEBUG
		buffer_end = (Len > 0)? Buffer[Len - 1] : 0 ;

		if (Len > 0 ) Buffer[Len - 1] = 0;
		printf("\n---------------------------\nSending %d bytes to server\nData:\n\n%s\n\n---------------------\n", retval, Buffer);
		if (Len > 0 ) Buffer[Len - 1] = buffer_end;
		buffer_end = 0;
#endif
		retval = send(SocketPair_struct.upstream, Buffer, Len, 0); // Forwards client's message to server
		if (retval == SOCKET_ERROR)
		{
#ifdef _WIN32
			printf("\n send() failed:error%d\n", WSAGetLastError());
#endif
			if (SocketPair_struct.IsUpstreamDisconnected == FALSE)
			{
				closesocket(SocketPair_struct.upstream);
				SocketPair_struct.IsUpstreamDisconnected = TRUE;
			}
			continue;
		}
		retval = recv(SocketPair_struct.downstream, Buffer, sizeof(Buffer), 0); // Receive response from client

		if (retval == SOCKET_ERROR)
		{
			printf("\nError Recv");
			if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
			{
				closesocket(SocketPair_struct.downstream);
				SocketPair_struct.IsDownstreamDisconnected = TRUE;
			}
			continue;
		}
		if (retval == 0)
		{
			printf("Client Close connection\n");
			if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
			{
				closesocket(SocketPair_struct.downstream);
				SocketPair_struct.IsDownstreamDisconnected = TRUE;
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

	if (SocketPair_struct.IsUpstreamDisconnected == FALSE)
	{
		closesocket(SocketPair_struct.upstream);
		SocketPair_struct.IsUpstreamDisconnected = TRUE;
	}
	if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
	{
		closesocket(SocketPair_struct.downstream);
		SocketPair_struct.IsDownstreamDisconnected = TRUE;
	}
#ifdef _WIN32
	::WaitForSingleObject(pChildThread, 20000);  //Should check the return value
#endif
	return 0;
}

// Read data from remote and send data to local
unsigned int __stdcall UpstreamCommunication(void *pParam)
{
	ProxyParam * proxyparam_ptr = (ProxyParam*)pParam;
	char Buffer[BUFSIZE];
	char *server_name = SERVER_NAME;
	char buffer_end = 0; // This is for restoring the last byte of buffer after debug statements
	unsigned short port;
	int retval, Len;
	unsigned int server_addr;
	int socket_type;
	struct sockaddr_in server_sockaddr_in;
	struct hostent *server_hostent_struct_ptr;
	SOCKET  conn_socket;

	socket_type = SOCK_STREAM;
	server_name = proxyparam_ptr->Address;	// Address of the remote server from the client's request
	port = proxyparam_ptr->Port;			// Port of the remote server from the client's request

	if (isalpha(server_name[0])) {   /* server address is a name */
		server_hostent_struct_ptr = gethostbyname(server_name);
	}
	else { /* Convert nnn.nnn address to a usable one */
		server_addr = inet_addr(server_name);
		server_hostent_struct_ptr = gethostbyaddr((char *)&server_addr, 4, AF_INET);
	}
	if (server_hostent_struct_ptr == NULL) {
#ifdef _WIN32
		printf("\n\n[ERROR]: Proxy server: Cannot resolve address [%s]: Error %d\n",
			server_name, WSAGetLastError());
#endif
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
#ifdef _WIN32
		printf("\n\n[ERROR]: Client: Error Opening socket: Error %d\n",
			WSAGetLastError());
#endif
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
		::SetEvent(proxyparam_ptr->User_SvrOK);
		return -1;
	}


#ifdef _DEBUG
	printf("Client connecting to: %s\n", server_hostent_struct_ptr->h_name);
#endif
	if (connect(conn_socket, (struct sockaddr*)&server_sockaddr_in, sizeof(server_sockaddr_in))
		== SOCKET_ERROR) {
#ifdef _WIN32
		printf("\n\n[ERROR]: connect() failed: %d\n", WSAGetLastError());
#endif
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
#ifdef _WIN32
		::SetEvent(proxyparam_ptr->User_SvrOK);
#endif
		return -1;
	}

	// Connected to remote server
	proxyparam_ptr->pPair->upstream = conn_socket;
	proxyparam_ptr->pPair->IsUpstreamDisconnected = FALSE;
#ifdef _WIN32
	::SetEvent(proxyparam_ptr->User_SvrOK);
#endif
	
	// Loop to forward communication between client and remote server
	while (!proxyparam_ptr->pPair->IsUpstreamDisconnected && !proxyparam_ptr->pPair->IsDownstreamDisconnected) // Both connections are open
	{
		retval = recv(conn_socket, Buffer, sizeof(Buffer), 0); // Receive from remote server
		if (retval == SOCKET_ERROR) {
#ifdef _WIN32
			printf("\n\n[ERROR]: recv() from server failed: error %d\n", WSAGetLastError());
#endif
			closesocket(conn_socket);
			proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
			break;
		}
		Len = retval;
		if (retval == 0) {
			printf("Server closed connection\n");
			closesocket(conn_socket);
			proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
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
		// Forward remote server's message to client
		retval = send(proxyparam_ptr->pPair->downstream, Buffer, Len, 0);
		if (retval == SOCKET_ERROR) {
#ifdef _WIN32
			printf("\n\n[ERROR]: send() to server failed: error %d\n", WSAGetLastError());
#endif
			closesocket(proxyparam_ptr->pPair->downstream);
			proxyparam_ptr->pPair->IsDownstreamDisconnected = TRUE;
			break;
		}
	}
	if (proxyparam_ptr->pPair->IsUpstreamDisconnected == FALSE)
	{
		closesocket(proxyparam_ptr->pPair->upstream);
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
	}
	if (proxyparam_ptr->pPair->IsDownstreamDisconnected == FALSE)
	{
		closesocket(proxyparam_ptr->pPair->downstream);
		proxyparam_ptr->pPair->IsDownstreamDisconnected = TRUE;
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


