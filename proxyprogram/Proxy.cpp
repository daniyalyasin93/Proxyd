// Proxy.cpp is a simple HTTP/HTTPS proxy server based on a simpler proxy server developed by Hu Zhonshan
// Written by Daniyal Yasin   
// e-mail daniyalyasin93@gmail.com
// 26-07-2016
#define _DEBUG

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#elif defined __linux__
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <netdb.h>
#endif

#include <iostream>
#include "Proxy.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>



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

#ifdef _WIN32
unsigned int __stdcall UpstreamCommunication(void *pParam);
unsigned int __stdcall DownstreamCommunication(void *pParam);
#elif defined __linux__
#define __stdcall 
typedef int SOCKET;
typedef bool BOOL;
typedef char TCHAR;
#define FALSE false
#define TRUE true
#define INVALID_SOCKET -1
#define SOCKET_ERROR	-1
void* UpstreamCommunication(void *pParam);
void* DownstreamCommunication(void *pParam);
#endif



struct SocketPair {
	SOCKET  downstream;					//socket : client to proxy server
	SOCKET  upstream;					//socket : proxy server to remote server
	BOOL    IsDownstreamDisconnected;	// Status of connection with client
	BOOL    IsUpstreamDisconnected;		// Status of connecition with remote server
};


struct ProxyParam {
	char Address[256];    // address of remote server
	SocketPair *pPair;    // pointer to socket pair
	int     Port;         // port which will be used to connect to remote server
#ifdef _WIN32
	HANDLE User_SvrOK;    // flag/event to signal that connection has been established with remote server	
#elif defined __linux__
	pthread_mutex_t lock;
    pthread_cond_t cond;
    BOOL User_SvrOK;
    BOOL* childThreadExit;
    pthread_cond_t ThreadExitCond;
#endif
};                   //This struct is used to exchange information between threads.

#ifdef _WIN32
SOCKET    gListen_Socket;
#elif defined __linux__
int    gListen_Socket;
#endif

int StartServer()
{
	
	sockaddr_in local;
#ifdef _WIN32
	SOCKET listen_socket;
	WSADATA wsaData;

	if (::WSAStartup(0x202, &wsaData) != 0)
	{
		printf("\nError in Startup session.\n"); WSACleanup(); return -1;
	};
#elif defined __linux__
	int listen_socket;

#endif // _WIN32

	local.sin_family = AF_INET; // IPv4 address family
	local.sin_addr.s_addr = INADDR_ANY; // Listen at any interface
	local.sin_port = htons(PROXYPORT); // Port number 5060

	listen_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (listen_socket == INVALID_SOCKET)
	{
#ifdef _WIN32
		printf("\nError in initialization of Socket."); WSACleanup(); return -2;
#elif defined __linux__
		printf("\nError in initialization of Socket."); 
#endif
	}

	if (bind(listen_socket, (sockaddr *)&local, sizeof(local)) != 0)
	{
#ifdef _WIN32
		printf("\nError in binding socket.");	WSACleanup(); return -3;
#elif defined __linux__
		printf("\nError in binding socket."); 
#endif
	};

	if (listen(listen_socket, BACKLOG) != 0)
	{
#ifdef _WIN32
		printf("\nError in listening to socket"); WSACleanup(); return -4;
#elif defined __linux__
		printf("\nError in listening to socket"); 
#endif
	}
	
	gListen_Socket = listen_socket; // Assign value to global listening socket variable
	
	unsigned thread_idn; // Receives thread identifier
	#ifdef _WIN32
	HANDLE USERTOPROXYTHREADHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); //Start another thread to listen.
	#elif defined __linux__
	pthread_t USERTOPROXYTHREADHANDLE;
	pthread_create(&USERTOPROXYTHREADHANDLE, NULL, DownstreamCommunication, NULL);
	#endif

	return 1;
}

int CloseServer()
{
#ifdef _WIN32
	closesocket(gListen_Socket);
	WSACleanup();
#elif defined __linux__
	close(gListen_Socket);
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
#ifdef _WIN32
unsigned int __stdcall DownstreamCommunication(void *pParam)
#elif defined __linux__
void* DownstreamCommunication(void *pParam)
#endif
{
	char Buffer[BUFSIZE], command[MAXCOMMANDLEN], protocol[MAXPROTOLEN];
	char buffer_end = 0; // This is for restoring the last byte of buffer after debug statements
	int  Len;
	sockaddr_in from; // Caller address is stored in this structure
	SOCKET msg_socket;
	unsigned int retval;
	SocketPair SocketPair_struct;
	ProxyParam proxyparam_var;
#ifdef _WIN32	
	HANDLE pChildThread;
	int fromlen;

#elif defined __linux__
	unsigned int fromlen;

#endif

	fromlen = sizeof(from);

	msg_socket = accept(gListen_Socket, (struct sockaddr*)&from, &fromlen); // Block until connection received

	unsigned thread_idn;

	if (msg_socket == INVALID_SOCKET)
	{
		printf("\nError  in accept ");
	}
	#ifdef _WIN32
	HANDLE DOWNSTREAMHANDLE = (HANDLE)_beginthreadex(NULL, 0, DownstreamCommunication, NULL, 0, &thread_idn); // Start another thread to listen for other connections																													 
	#elif defined __linux__
	pthread_t DOWNSTREAMHANDLE;
	pthread_create(&DOWNSTREAMHANDLE, NULL, DownstreamCommunication, NULL);
	#endif

	SocketPair_struct.IsDownstreamDisconnected = FALSE; // A client has connected to the proxy
	SocketPair_struct.IsUpstreamDisconnected = TRUE;	// An upstream connected to a destination server has not yet been established
	SocketPair_struct.downstream = msg_socket;

	retval = recv(SocketPair_struct.downstream, Buffer, sizeof(Buffer), 0);	// Receive from client

	if (retval == SOCKET_ERROR)
	{
		printf("\nError Recv");
		if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
		{
			#ifdef	_WIN32
			closesocket(SocketPair_struct.downstream);
			#elif defined	__linux__
			close(SocketPair_struct.downstream);
			#endif
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
//			return -1;
		}
	}

	if (retval == 0)
	{
		printf("Client Close connection\n");
		if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
		{
			#ifdef	_WIN32
			closesocket(SocketPair_struct.downstream);
			#elif defined	__linux__
			close(SocketPair_struct.downstream);
			#endif
			
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
//			return -1;
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
			#ifdef	_WIN32
			closesocket(SocketPair_struct.downstream);
			#elif defined	__linux__
			close(SocketPair_struct.downstream);
			#endif
			SocketPair_struct.IsDownstreamDisconnected = TRUE;
//			return -1;
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
				#ifdef	_WIN32
				closesocket(SocketPair_struct.downstream);
				#elif defined	__linux__
				close(SocketPair_struct.downstream);
				#endif
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
#elif defined __linux__
	pthread_mutex_init(&proxyparam_var.lock, NULL);
	pthread_cond_init(&proxyparam_var.cond, NULL);
	pthread_cond_init(&proxyparam_var.ThreadExitCond, NULL);
	pthread_mutex_lock(&proxyparam_var.lock);
	proxyparam_var.User_SvrOK = FALSE;
	proxyparam_var.childThreadExit = (BOOL*)malloc(sizeof(BOOL));
	*(proxyparam_var.childThreadExit) = FALSE;
	pthread_mutex_unlock(&proxyparam_var.lock);
#endif

	//GetAddressAndPort(Buffer, ProxyP.Address, &ProxyP.Port, command);
#ifdef _WIN32
	unsigned threadID;
	pChildThread = (HANDLE)_beginthreadex(NULL, 0, UpstreamCommunication, (void *)&proxyparam_var, 0, &threadID);

	::WaitForSingleObject(proxyparam_var.User_SvrOK, 60000);  // Wait for connection between proxy and remote server. Timeout is 60 seconds
	::CloseHandle(proxyparam_var.User_SvrOK);
#elif defined __linux__
	struct timespec timeToWait;
    struct timeval now;
    int rt = 0;
    gettimeofday(&now,NULL);


    timeToWait.tv_sec = now.tv_sec+60;
    timeToWait.tv_nsec = (now.tv_usec+1000UL*60000)*1000UL;

	pthread_t pChildThread;
	pthread_create(&pChildThread, NULL, UpstreamCommunication, (void *)&proxyparam_var);



	pthread_mutex_lock(&proxyparam_var.lock);

	while ((proxyparam_var.User_SvrOK == FALSE)&&(rt!=ETIMEDOUT))
		rt = pthread_cond_timedwait(&proxyparam_var.cond, &proxyparam_var.lock, &timeToWait);
	pthread_mutex_unlock(&proxyparam_var.lock);
	pthread_cond_destroy(&proxyparam_var.cond);
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
#elif defined __linux__
			printf("\n send() failed:\n");
#endif
			if (SocketPair_struct.IsUpstreamDisconnected == FALSE)
			{
				#ifdef	_WIN32
				closesocket(SocketPair_struct.upstream);
				#elif defined	__linux__
				close(SocketPair_struct.upstream);
				#endif
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
				#ifdef	_WIN32
				closesocket(SocketPair_struct.downstream);
				#elif defined	__linux__
				close(SocketPair_struct.downstream);
				#endif
				SocketPair_struct.IsDownstreamDisconnected = TRUE;
			}
			continue;
		}
		if (retval == 0)
		{
			printf("Client Close connection\n");
			if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
			{
				#ifdef	_WIN32
				closesocket(SocketPair_struct.downstream);
				#elif defined	__linux__
				close(SocketPair_struct.downstream);
				#endif
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
		#ifdef	_WIN32
		closesocket(SocketPair_struct.upstream);
		#elif defined	__linux__
		close(SocketPair_struct.upstream);
		#endif
		SocketPair_struct.IsUpstreamDisconnected = TRUE;
	}
	if (SocketPair_struct.IsDownstreamDisconnected == FALSE)
	{
		#ifdef	_WIN32
		closesocket(SocketPair_struct.downstream);
		#elif defined	__linux__
		close(SocketPair_struct.downstream);
		#endif
		SocketPair_struct.IsDownstreamDisconnected = TRUE;
	}
#ifdef _WIN32
	::WaitForSingleObject(pChildThread, 20000);  //Should check the return value
#elif defined __linux__
	rt = 0;
    gettimeofday(&now,NULL);


    timeToWait.tv_sec = now.tv_sec+60;
    timeToWait.tv_nsec = (now.tv_usec+1000UL*60000)*1000UL;

	pthread_mutex_lock(&proxyparam_var.lock);

	while ((*(proxyparam_var.childThreadExit) == FALSE)&&(rt!=ETIMEDOUT))
		rt = pthread_cond_timedwait(&proxyparam_var.ThreadExitCond, &proxyparam_var.lock, &timeToWait);
	pthread_mutex_unlock(&proxyparam_var.lock);
	pthread_cond_destroy(&proxyparam_var.ThreadExitCond);
#endif
//	return 0;
}

// Read data from remote and send data to local
#ifdef _WIN32
unsigned int __stdcall UpstreamCommunication(void *pParam)
#elif defined __linux__
void* UpstreamCommunication(void *pParam)
#endif
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
		::SetEvent(proxyparam_ptr->User_SvrOK);
#elif defined __linux__
		printf("\n\n[ERROR]: Proxy server: Cannot resolve address [%s]\n",
			server_name);
		pthread_mutex_lock(&proxyparam_ptr->lock);
		proxyparam_ptr->User_SvrOK = TRUE;
		pthread_mutex_unlock(&proxyparam_ptr->lock);
		pthread_cond_signal(&proxyparam_ptr->cond);
#endif
		
		////return 0;
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
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
#ifdef _WIN32
		printf("\n\n[ERROR]: Client: Error Opening socket: Error %d\n",
			WSAGetLastError());
		
		::SetEvent(proxyparam_ptr->User_SvrOK);
#elif defined __linux__
		printf("\n\n[ERROR]: Client: Error Opening socket\n");
#endif
		
		////return -1;
	}


#ifdef _DEBUG
	printf("Client connecting to: %s\n", server_hostent_struct_ptr->h_name);
#endif
	if (connect(conn_socket, (struct sockaddr*)&server_sockaddr_in, sizeof(server_sockaddr_in))
		== SOCKET_ERROR) {
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
#ifdef _WIN32
		printf("\n\n[ERROR]: connect() failed: %d\n", WSAGetLastError());
		::SetEvent(proxyparam_ptr->User_SvrOK);
#elif defined __linux__
		printf("\n\n[ERROR]: connect() failed\n");
		pthread_mutex_lock(&proxyparam_ptr->lock);
		proxyparam_ptr->User_SvrOK = TRUE;
		pthread_mutex_unlock(&proxyparam_ptr->lock);
		pthread_cond_signal(&proxyparam_ptr->cond);
#endif
		////return -1;
	}

	// Connected to remote server
	proxyparam_ptr->pPair->upstream = conn_socket;
	proxyparam_ptr->pPair->IsUpstreamDisconnected = FALSE;
#ifdef _WIN32
	::SetEvent(proxyparam_ptr->User_SvrOK);
#elif defined __linux__
	pthread_mutex_lock(&proxyparam_ptr->lock);
	proxyparam_ptr->User_SvrOK = TRUE;
	pthread_mutex_unlock(&proxyparam_ptr->lock);
	pthread_cond_signal(&proxyparam_ptr->cond);
#endif
	
	// Loop to forward communication between client and remote server
	while (!proxyparam_ptr->pPair->IsUpstreamDisconnected && !proxyparam_ptr->pPair->IsDownstreamDisconnected) // Both connections are open
	{
		retval = recv(conn_socket, Buffer, sizeof(Buffer), 0); // Receive from remote server
		if (retval == SOCKET_ERROR) {
#ifdef _WIN32
			printf("\n\n[ERROR]: recv() from server failed: error %d\n", WSAGetLastError());
			closesocket(conn_socket);
#elif defined __linux__
			printf("\n\n[ERROR]: recv() from server failed\n");
			close(conn_socket);
#endif
			proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
			break;
		}
		Len = retval;
		if (retval == 0) {
			printf("Server closed connection\n");
#ifdef _WIN32
			closesocket(conn_socket);
#elif defined __linux__
			close(conn_socket);
#endif
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
			closesocket(proxyparam_ptr->pPair->downstream);
#elif defined __linux__
			printf("\n\n[ERROR]: send() to server failed\n");
			close(proxyparam_ptr->pPair->downstream);
#endif
			
			proxyparam_ptr->pPair->IsDownstreamDisconnected = TRUE;
			break;
		}
	}
	if (proxyparam_ptr->pPair->IsUpstreamDisconnected == FALSE)
	{
#ifdef _WIN32		
		closesocket(proxyparam_ptr->pPair->upstream);
#elif defined __linux__
		close(proxyparam_ptr->pPair->upstream);
#endif
		proxyparam_ptr->pPair->IsUpstreamDisconnected = TRUE;
	}
	if (proxyparam_ptr->pPair->IsDownstreamDisconnected == FALSE)
	{
#ifdef _WIN32		
		closesocket(proxyparam_ptr->pPair->downstream);
#elif defined __linux__
		close(proxyparam_ptr->pPair->downstream);
#endif
		proxyparam_ptr->pPair->IsDownstreamDisconnected = TRUE;
	}
	//return 1;
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


