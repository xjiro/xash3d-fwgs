/*
net_ws.c - win network interface
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifdef _WIN32
// Winsock
#include <winsock.h>
#include <wsipx.h>
#define socklen_t int //#include <ws2tcpip.h>
#else
// BSD sockets
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
// Errors handling
#include <errno.h>
#include <fcntl.h>
#endif
#include "common.h"
#include "netchan.h"
#include "mathlib.h"

//#define NET_USE_FRAGMENTS

#define PORT_ANY			-1
#define MAX_LOOPBACK		4
#define MASK_LOOPBACK		(MAX_LOOPBACK - 1)

#define MAX_ROUTEABLE_PACKET		1400
#define SPLIT_SIZE			( MAX_ROUTEABLE_PACKET - sizeof( SPLITPACKET ))
#define NET_MAX_FRAGMENTS		( NET_MAX_FRAGMENT / SPLIT_SIZE )

#ifndef _WIN32 // it seems we need to use WS2 to support it
#define HAVE_GETADDRINFO
#endif

#ifdef _WIN32
// wsock32.dll exports
static int (_stdcall *pWSACleanup)( void );
static word (_stdcall *pNtohs)( word netshort );
static int (_stdcall *pWSAGetLastError)( void );
static int (_stdcall *pCloseSocket)( SOCKET s );
static word (_stdcall *pHtons)( word hostshort );
static dword (_stdcall *pInet_Addr)( const char* cp );
static char* (_stdcall *pInet_Ntoa)( struct in_addr in );
static SOCKET (_stdcall *pSocket)( int af, int type, int protocol );
static struct hostent *(_stdcall *pGetHostByName)( const char* name );
static int (_stdcall *pIoctlSocket)( SOCKET s, long cmd, dword* argp );
static int (_stdcall *pWSAStartup)( word wVersionRequired, LPWSADATA lpWSAData );
static int (_stdcall *pBind)( SOCKET s, const struct sockaddr* addr, int namelen );
static int (_stdcall *pSetSockopt)( SOCKET s, int level, int optname, const char* optval, int optlen );
static int (_stdcall *pRecvFrom)( SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen );
static int (_stdcall *pSendTo)( SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen );
static int (_stdcall *pSelect)( int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const struct timeval* timeout );
static int (_stdcall *pConnect)( SOCKET s, const struct sockaddr *name, int namelen );
static int (_stdcall *pGetSockName)( SOCKET s, struct sockaddr *name, int *namelen );
static int (_stdcall *pSend)( SOCKET s, const char *buf, int len, int flags );
static int (_stdcall *pRecv)( SOCKET s, char *buf, int len, int flags );
static int (_stdcall *pGetHostName)( char *name, int namelen );
static dword (_stdcall *pNtohl)( dword netlong );

static dllfunc_t winsock_funcs[] =
{
{ "bind", (void **) &pBind },
{ "send", (void **) &pSend },
{ "recv", (void **) &pRecv },
{ "ntohs", (void **) &pNtohs },
{ "htons", (void **) &pHtons },
{ "ntohl", (void **) &pNtohl },
{ "socket", (void **) &pSocket },
{ "select", (void **) &pSelect },
{ "sendto", (void **) &pSendTo },
{ "connect", (void **) &pConnect },
{ "recvfrom", (void **) &pRecvFrom },
{ "inet_addr", (void **) &pInet_Addr },
{ "inet_ntoa", (void **) &pInet_Ntoa },
{ "WSAStartup", (void **) &pWSAStartup },
{ "WSACleanup", (void **) &pWSACleanup },
{ "setsockopt", (void **) &pSetSockopt },
{ "ioctlsocket", (void **) &pIoctlSocket },
{ "closesocket", (void **) &pCloseSocket },
{ "gethostname", (void **) &pGetHostName },
{ "getsockname", (void **) &pGetSockName },
{ "gethostbyname", (void **) &pGetHostByName },
{ "WSAGetLastError", (void **) &pWSAGetLastError },
{ NULL, NULL }
};

dll_info_t winsock_dll = { "wsock32.dll", winsock_funcs, false };

static dllfunc_t kernel32_funcs[] =
{
	{ "InitializeCriticalSection", (void **) &pInitializeCriticalSection },
	{ "EnterCriticalSection", (void **) &pEnterCriticalSection },
	{ "LeaveCriticalSection", (void **) &pLeaveCriticalSection },
	{ "DeleteCriticalSection", (void **) &pDeleteCriticalSection },
	{ NULL, NULL }
};

dll_info_t kernel32_dll = { "kernel32.dll", kernel32_funcs, false };


static void NET_InitializeCriticalSections( void );

qboolean NET_OpenWinSock( void )
{
	if( Sys_LoadLibrary( &kernel32_dll ) )
		NET_InitializeCriticalSections();

	// initialize the Winsock function vectors (we do this instead of statically linking
	// so we can run on Win 3.1, where there isn't necessarily Winsock)
	return Sys_LoadLibrary( &winsock_dll );
}

void NET_FreeWinSock( void )
{
	Sys_FreeLibrary( &winsock_dll );
}
#else
#define pHtons htons
#define pConnect connect
#define pInet_Addr inet_addr
#define pRecvFrom recvfrom
#define pSendTo sendto
#define pSocket socket
#define pIoctlSocket ioctl
#define pCloseSocket close
#define pSetSockopt setsockopt
#define pBind bind
#define pGetHostName gethostname
#define pGetSockName getsockname
#define pGetHs
#define pRecv recv
#define pSend send
#define pInet_Ntoa inet_ntoa
#define pNtohs ntohs
#define pGetHostByName gethostbyname
#define pSelect select
#define pGetAddrInfo getaddrinfo
#define SOCKET int
#define INVALID_SOCKET 0
#endif

#ifdef __EMSCRIPTEN__
/* All socket operations are non-blocking already */
static int ioctl_stub( int d, unsigned long r, ...)
{
	return 0;
}
#undef pIoctlSocket
#define pIoctlSocket ioctl_stub
#endif

typedef struct
{
	byte		data[NET_MAX_MESSAGE];
	int		datalen;
} net_loopmsg_t;

typedef struct
{
	net_loopmsg_t	msgs[MAX_LOOPBACK];
	int		get, send;
} net_loopback_t;

typedef struct packetlag_s
{
	byte		*data;	// Raw stream data is stored.
	int		size;
	netadr_t		from;
	float		receivedtime;
	struct packetlag_s	*next;
	struct packetlag_s	*prev;
} packetlag_t;

// split long packets. Anything over 1460 is failing on some routers.
typedef struct
{
	int		current_sequence;
	int		split_count;
	int		total_size;
	char		buffer[NET_MAX_FRAGMENT];
} LONGPACKET;

// use this to pick apart the network stream, must be packed
#pragma pack(push, 1)
typedef struct
{
	int		net_id;
	int		sequence_number;
	short		packet_id;
} SPLITPACKET;
#pragma pack(pop)

typedef struct
{
	net_loopback_t	loopbacks[NS_COUNT];
	packetlag_t	lagdata[NS_COUNT];
	int		losscount[NS_COUNT];
	float		fakelag;			// cached fakelag value
	LONGPACKET	split;
	int		split_flags[NET_MAX_FRAGMENTS];
	long		sequence_number;
	int		ip_sockets[NS_COUNT];
	qboolean		initialized;
	qboolean		configured;
	qboolean		allow_ip;
#ifdef _WIN32
	WSADATA		winsockdata;
#endif
} net_state_t;

static net_state_t		net;
static convar_t		*net_ipname;
static convar_t		*net_hostport;
static convar_t		*net_iphostport;
static convar_t		*net_clientport;
static convar_t		*net_ipclientport;
static convar_t		*net_fakelag;
static convar_t		*net_fakeloss;
static convar_t		*net_address;
convar_t			*net_clockwindow;
netadr_t			net_local;

/*
====================
NET_ErrorString
====================
*/
char *NET_ErrorString( void )
{
#ifdef _WIN32
	int	err = WSANOTINITIALISED;

	if( net.initialized )
		err = pWSAGetLastError();

	switch( err )
	{
	case WSAEINTR: return "WSAEINTR";
	case WSAEBADF: return "WSAEBADF";
	case WSAEACCES: return "WSAEACCES";
	case WSAEDISCON: return "WSAEDISCON";
	case WSAEFAULT: return "WSAEFAULT";
	case WSAEINVAL: return "WSAEINVAL";
	case WSAEMFILE: return "WSAEMFILE";
	case WSAEWOULDBLOCK: return "WSAEWOULDBLOCK";
	case WSAEINPROGRESS: return "WSAEINPROGRESS";
	case WSAEALREADY: return "WSAEALREADY";
	case WSAENOTSOCK: return "WSAENOTSOCK";
	case WSAEDESTADDRREQ: return "WSAEDESTADDRREQ";
	case WSAEMSGSIZE: return "WSAEMSGSIZE";
	case WSAEPROTOTYPE: return "WSAEPROTOTYPE";
	case WSAENOPROTOOPT: return "WSAENOPROTOOPT";
	case WSAEPROTONOSUPPORT: return "WSAEPROTONOSUPPORT";
	case WSAESOCKTNOSUPPORT: return "WSAESOCKTNOSUPPORT";
	case WSAEOPNOTSUPP: return "WSAEOPNOTSUPP";
	case WSAEPFNOSUPPORT: return "WSAEPFNOSUPPORT";
	case WSAEAFNOSUPPORT: return "WSAEAFNOSUPPORT";
	case WSAEADDRINUSE: return "WSAEADDRINUSE";
	case WSAEADDRNOTAVAIL: return "WSAEADDRNOTAVAIL";
	case WSAENETDOWN: return "WSAENETDOWN";
	case WSAENETUNREACH: return "WSAENETUNREACH";
	case WSAENETRESET: return "WSAENETRESET";
	case WSAECONNABORTED: return "WSWSAECONNABORTEDAEINTR";
	case WSAECONNRESET: return "WSAECONNRESET";
	case WSAENOBUFS: return "WSAENOBUFS";
	case WSAEISCONN: return "WSAEISCONN";
	case WSAENOTCONN: return "WSAENOTCONN";
	case WSAESHUTDOWN: return "WSAESHUTDOWN";
	case WSAETOOMANYREFS: return "WSAETOOMANYREFS";
	case WSAETIMEDOUT: return "WSAETIMEDOUT";
	case WSAECONNREFUSED: return "WSAECONNREFUSED";
	case WSAELOOP: return "WSAELOOP";
	case WSAENAMETOOLONG: return "WSAENAMETOOLONG";
	case WSAEHOSTDOWN: return "WSAEHOSTDOWN";
	case WSASYSNOTREADY: return "WSASYSNOTREADY";
	case WSAVERNOTSUPPORTED: return "WSAVERNOTSUPPORTED";
	case WSANOTINITIALISED: return "WSANOTINITIALISED";
	case WSAHOST_NOT_FOUND: return "WSAHOST_NOT_FOUND";
	case WSATRY_AGAIN: return "WSATRY_AGAIN";
	case WSANO_RECOVERY: return "WSANO_RECOVERY";
	case WSANO_DATA: return "WSANO_DATA";
	default: return "NO ERROR";
	}
#else
	return strerror( errno );
#endif
}

_inline qboolean NET_IsSocketError( int retval )
{
#ifdef _WIN32
	return retval == SOCKET_ERROR ? true : false;
#else
	return retval < 0 ? true : false;
#endif
}

_inline qboolean NET_IsSocketValid( int socket )
{
#ifdef _WIN32
	return socket != INVALID_SOCKET;
#else
	return socket;
#endif
}

/*
====================
NET_NetadrToSockadr
====================
*/
static void NET_NetadrToSockadr( netadr_t *a, struct sockaddr *s )
{
	memset( s, 0, sizeof( *s ));

	if( a->type == NA_BROADCAST )
	{
		((struct sockaddr_in *)s)->sin_family = AF_INET;
		((struct sockaddr_in *)s)->sin_port = a->port;
		((struct sockaddr_in *)s)->sin_addr.s_addr = INADDR_BROADCAST;
	}
	else if( a->type == NA_IP )
	{
		((struct sockaddr_in *)s)->sin_family = AF_INET;
		((struct sockaddr_in *)s)->sin_addr.s_addr = *(int *)&a->ip;
		((struct sockaddr_in *)s)->sin_port = a->port;
	}
}

/*
====================
NET_SockadrToNetAdr
====================
*/
static void NET_SockadrToNetadr( struct sockaddr *s, netadr_t *a )
{
	if( s->sa_family == AF_INET )
	{
		a->type = NA_IP;
		*(int *)&a->ip = ((struct sockaddr_in *)s)->sin_addr.s_addr;
		a->port = ((struct sockaddr_in *)s)->sin_port;
	}
}

#if !defined XASH_NO_ASYNC_NS_RESOLVE && ( defined _WIN32 || !defined __EMSCRIPTEN__ )
#define CAN_ASYNC_NS_RESOLVE
#endif

#ifdef CAN_ASYNC_NS_RESOLVE
static void NET_ResolveThread( void );
#if !defined _WIN32
#include <pthread.h>
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define exit_thread( x ) pthread_exit(x)
#define create_thread( pfn ) !pthread_create( &nsthread.thread, NULL, (pfn), NULL )
#define detach_thread( x ) pthread_detach(x)
#define mutex_t  pthread_mutex_t
#define thread_t pthread_t

void *NET_ThreadStart( void *unused )
{
	NET_ResolveThread();
	return NULL;
}

#else // WIN32
struct cs {
	void* p1;
	int   i1, i2;
	void *p2, *p3;
	uint  i4;
};
#define mutex_lock pEnterCriticalSection
#define mutex_unlock pLeaveCriticalSection
#define detach_thread( x ) CloseHandle(x)
#define create_thread( pfn ) nsthread.thread = CreateThread( NULL, 0, pfn, NULL, 0, NULL )
#define mutex_t  struct cs
#define thread_t HANDLE
DWORD WINAPI NET_ThreadStart( LPVOID unused )
{
	NET_ResolveThread();
	ExitThread(0);
	return 0;
}
#endif

#ifdef DEBUG_RESOLVE
#define RESOLVE_DBG(x) Sys_PrintLog(x)
#else
#define RESOLVE_DBG(x)
#endif //  DEBUG_RESOLVE

static struct nsthread_s
{
	mutex_t mutexns;
	mutex_t mutexres;
	thread_t thread;
	int     result;
	string  hostname;
	qboolean busy;
} nsthread
#ifndef _WIN32
= { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER }
#endif
;

#ifdef _WIN32
static void NET_InitializeCriticalSections( void )
{
	pInitializeCriticalSection( &nsthread.mutexns );
	pInitializeCriticalSection( &nsthread.mutexres );
}
#endif

void NET_ResolveThread( void )
{
#ifdef HAVE_GETADDRINFO
	struct addrinfo *ai = NULL, *cur;
	struct addrinfo hints;
	int sin_addr = 0;

	RESOLVE_DBG( "[resolve thread] starting resolve for " );
	RESOLVE_DBG( nsthread.hostname );
	RESOLVE_DBG( " with getaddrinfo\n" );
	memset( &hints, 0, sizeof( hints ) );
	hints.ai_family = AF_INET;
	if( !pGetAddrInfo( nsthread.hostname, NULL, &hints, &ai ) )
	{
		for( cur = ai; cur; cur = cur->ai_next ) {
			if( cur->ai_family == AF_INET ) {
				sin_addr = *((int*)&((struct sockaddr_in *)cur->ai_addr)->sin_addr);
				freeaddrinfo( ai );
				ai = NULL;
				break;
			}
		}

		if( ai )
			freeaddrinfo( ai );
	}

	if( sin_addr )
		RESOLVE_DBG( "[resolve thread] getaddrinfo success\n" );
	else
		RESOLVE_DBG( "[resolve thread] getaddrinfo failed\n" );
	mutex_lock( &nsthread.mutexres );
	nsthread.result = sin_addr;
	nsthread.busy = false;
	RESOLVE_DBG( "[resolve thread] returning result\n" );
	mutex_unlock( &nsthread.mutexres );
	RESOLVE_DBG( "[resolve thread] exiting thread\n" );
#else
	struct hostent *res;

	RESOLVE_DBG( "[resolve thread] starting resolve for " );
	RESOLVE_DBG( nsthread.hostname );
	RESOLVE_DBG( " with gethostbyname\n" );

	mutex_lock( &nsthread.mutexns );
	RESOLVE_DBG( "[resolve thread] locked gethostbyname mutex\n" );
	res = pGetHostByName( nsthread.hostname );
	if(res)
		RESOLVE_DBG( "[resolve thread] gethostbyname success\n" );
	else
		RESOLVE_DBG( "[resolve thread] gethostbyname failed\n" );

	mutex_lock( &nsthread.mutexres );
	RESOLVE_DBG( "[resolve thread] returning result\n" );
	if( res )
		nsthread.result = *(int *)res->h_addr_list[0];
	else
		nsthread.result = 0;

	nsthread.busy = false;

	mutex_unlock( &nsthread.mutexns );

	RESOLVE_DBG( "[resolve thread] unlocked gethostbyname mutex\n" );

	mutex_unlock( &nsthread.mutexres );

	RESOLVE_DBG( "[resolve thread] exiting thread\n" );
#endif
}

#endif // CAN_ASYNC_NS_RESOLVE


/*
=============
NET_StringToAdr

localhost
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
static int NET_StringToSockaddr( const char *s, struct sockaddr *sadr, qboolean nonblocking )
{
	int ip = 0;
	char	*colon;
	char	copy[128];

	if( !net.initialized ) return false;
	
	memset( sadr, 0, sizeof( *sadr ));

	((struct sockaddr_in *)sadr)->sin_family = AF_INET;
	((struct sockaddr_in *)sadr)->sin_port = 0;

	Q_strncpy( copy, s, sizeof( copy ));

	// strip off a trailing :port if present
	for( colon = copy; *colon; colon++ )
	{
		if( *colon == ':' )
		{
			*colon = 0;
			((struct sockaddr_in *)sadr)->sin_port = pHtons((short)Q_atoi( colon + 1 ));
		}
	}

	if( copy[0] >= '0' && copy[0] <= '9' )
	{
		*(int *)&((struct sockaddr_in *)sadr)->sin_addr = pInet_Addr( copy );
	}
	else
	{
#ifdef CAN_ASYNC_NS_RESOLVE
		qboolean asyncfailed = false;
#ifdef _WIN32
		if( pInitializeCriticalSection )
#endif // _WIN32
		{
			if( !nonblocking )
			{
#ifdef HAVE_GETADDRINFO
				struct addrinfo *ai = NULL, *cur;
				struct addrinfo hints;

				memset( &hints, 0, sizeof( hints ) );
				hints.ai_family = AF_INET;
				if( !pGetAddrInfo( copy, NULL, &hints, &ai ) )
				{
					for( cur = ai; cur; cur = cur->ai_next )
					{
						if( cur->ai_family == AF_INET )
						{
							ip = *((int*)&((struct sockaddr_in *)cur->ai_addr)->sin_addr);
							freeaddrinfo(ai);
							ai = NULL;
							break;
						}
					}

					if( ai )
						freeaddrinfo(ai);
				}
#else
				struct hostent *h;

				mutex_lock( &nsthread.mutexns );
				h = pGetHostByName( copy );
				if( !h )
				{
					mutex_unlock( &nsthread.mutexns );
					return 0;
				}

				ip = *(int *)h->h_addr_list[0];
				mutex_unlock( &nsthread.mutexns );
#endif
			}
			else
			{
				mutex_lock( &nsthread.mutexres );

				if( nsthread.busy )
				{
					mutex_unlock( &nsthread.mutexres );
					return 2;
				}

				if( !Q_strcmp( copy, nsthread.hostname ) )
				{
					ip = nsthread.result;
					nsthread.hostname[0] = 0;
					detach_thread( nsthread.thread );
				}
				else
				{
					Q_strncpy( nsthread.hostname, copy, MAX_STRING );
					nsthread.busy = true;
					mutex_unlock( &nsthread.mutexres );

					if( create_thread( NET_ThreadStart ) )
						return 2;
					else // failed to create thread
					{
						MsgDev( D_ERROR, "NET_StringToSockaddr: failed to create thread!\n");
						nsthread.busy = false;
						asyncfailed = true;
					}
				}

				mutex_unlock( &nsthread.mutexres );
			}
		}
#ifdef _WIN32
		else
			asyncfailed = true;
#else
		if( asyncfailed )
#endif // _WIN32
#endif // CAN_ASYNC_NS_RESOLVE
		{
#ifdef HAVE_GETADDRINFO
			struct addrinfo *ai = NULL, *cur;
			struct addrinfo hints;

			memset( &hints, 0, sizeof( hints ) );
			hints.ai_family = AF_INET;
			if( !pGetAddrInfo( copy, NULL, &hints, &ai ) )
			{
				for( cur = ai; cur; cur = cur->ai_next )
				{
					if( cur->ai_family == AF_INET )
					{
						ip = *((int*)&((struct sockaddr_in *)cur->ai_addr)->sin_addr);
						freeaddrinfo(ai);
						ai = NULL;
						break;
					}
				}

				if( ai )
					freeaddrinfo(ai);
			}
#else
			struct hostent *h;
			if(!( h = pGetHostByName( copy )))
				return 0;
			ip = *(int *)h->h_addr_list[0];
#endif
		}

		if( !ip )
			return 0;

		*(int *)&((struct sockaddr_in *)sadr)->sin_addr = ip;
	}

	return 1;
}

/*
====================
NET_AdrToString
====================
*/
char *NET_AdrToString( const netadr_t a )
{
	if( a.type == NA_LOOPBACK )
		return "loopback";
	return va( "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], pNtohs( a.port ));
}

/*
====================
NET_BaseAdrToString
====================
*/
char *NET_BaseAdrToString( const netadr_t a )
{
	if( a.type == NA_LOOPBACK )
		return "loopback";
	return va( "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3] );
}

/*
===================
NET_CompareBaseAdr

Compares without the port
===================
*/
qboolean NET_CompareBaseAdr( const netadr_t a, const netadr_t b )
{
	if( a.type != b.type )
		return false;

	if( a.type == NA_LOOPBACK )
		return true;

	if( a.type == NA_IP )
	{
		if( !memcmp( a.ip, b.ip, 4 ))
			return true;
	}

	return false;
}

/*
====================
NET_CompareClassBAdr

Compare local masks
====================
*/
qboolean NET_CompareClassBAdr( netadr_t a, netadr_t b )
{
	if( a.type != b.type )
		return false;

	if( a.type == NA_LOOPBACK )
		return true;

	if( a.type == NA_IP )
	{
		if( a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] )
			return true;
	}
	return false;
}

/*
====================
NET_IsReservedAdr

Check for reserved ip's
====================
*/
qboolean NET_IsReservedAdr( netadr_t a )
{
	if( a.type == NA_LOOPBACK )
		return true;

	if( a.type == NA_IP )
	{
		if( a.ip[0] == 10 || a.ip[0] == 127 )
			return true;

		if( a.ip[0] == 172 && a.ip[1] >= 16 )
		{
			if( a.ip[1] >= 32 )
				return false;
			return true;
		}

		if( a.ip[0] == 192 && a.ip[1] >= 168 )
			return true;
	}

	return false;
}

/*
====================
NET_CompareAdr

Compare full address
====================
*/
qboolean NET_CompareAdr( const netadr_t a, const netadr_t b )
{
	if( a.type != b.type )
		return false;

	if( a.type == NA_LOOPBACK )
		return true;

	if( a.type == NA_IP )
	{
		if(!memcmp( a.ip, b.ip, 4 ) && a.port == b.port )
			return true;
		return false;
	}

	MsgDev( D_ERROR, "NET_CompareAdr: bad address type\n" );
	return false;
}

/*
====================
NET_IsLocalAddress
====================
*/
qboolean NET_IsLocalAddress( netadr_t adr )
{
	return (adr.type == NA_LOOPBACK) ? true : false;
}

/*
=============
NET_StringToAdr

idnewt
192.246.40.70
=============
*/
qboolean NET_StringToAdr( const char *string, netadr_t *adr )
{
	struct sockaddr s;

	memset( adr, 0, sizeof( netadr_t ));

	if( !Q_stricmp( string, "localhost" ) || !Q_stricmp( string, "loopback" ) )
	{
		adr->type = NA_LOOPBACK;
		return true;
	}

	if( !NET_StringToSockaddr( string, &s, false ))
		return false;
	NET_SockadrToNetadr( &s, adr );

	return true;
}

int NET_StringToAdrNB( const char *string, netadr_t *adr )
{
	struct sockaddr s;
	int res;

	memset( adr, 0, sizeof( netadr_t ));
	if( !Q_stricmp( string, "localhost" )  || !Q_stricmp( string, "loopback" ) )
	{
		adr->type = NA_LOOPBACK;
		return true;
	}

	res = NET_StringToSockaddr( string, &s, true );

	if( res == 0 || res == 2 )
		return res;

	NET_SockadrToNetadr( &s, adr );

	return true;
}

/*
=============================================================================

LOOPBACK BUFFERS FOR LOCAL PLAYER

=============================================================================
*/
/*
====================
NET_GetLoopPacket
====================
*/
static qboolean NET_GetLoopPacket( netsrc_t sock, netadr_t *from, byte *data, size_t *length )
{
	net_loopback_t	*loop;
	int		i;

	if( !data || !length )
		return false;

	loop = &net.loopbacks[sock];

	if( loop->send - loop->get > MAX_LOOPBACK )
		loop->get = loop->send - MAX_LOOPBACK;

	if( loop->get >= loop->send )
		return false;
	i = loop->get & MASK_LOOPBACK;
	loop->get++;

	memcpy( data, loop->msgs[i].data, loop->msgs[i].datalen );
	*length = loop->msgs[i].datalen;

	memset( from, 0, sizeof( *from ));
	from->type = NA_LOOPBACK;

	return true;
}

/*
====================
NET_SendLoopPacket
====================
*/
static void NET_SendLoopPacket( netsrc_t sock, size_t length, const void *data, netadr_t to )
{
	net_loopback_t	*loop;
	int		i;

	loop = &net.loopbacks[sock^1];

	i = loop->send & MASK_LOOPBACK;
	loop->send++;

	memcpy( loop->msgs[i].data, data, length );
	loop->msgs[i].datalen = length;
}

/*
====================
NET_ClearLoopback
====================
*/
static void NET_ClearLoopback( void )
{
	net.loopbacks[0].send = net.loopbacks[0].get = 0;
	net.loopbacks[1].send = net.loopbacks[1].get = 0;
}

/*
=============================================================================

LAG & LOSS SIMULATION SYSTEM (network debugging)

=============================================================================
*/
/*
==================
NET_RemoveFromPacketList

double linked list remove entry
==================
*/
static void NET_RemoveFromPacketList( packetlag_t *p )
{
	p->prev->next = p->next;
	p->next->prev = p->prev;
	p->prev = NULL;
	p->next = NULL;
}

/*
==================
NET_ClearLaggedList

double linked list remove queue
==================
*/
static void NET_ClearLaggedList( packetlag_t *list )
{
	packetlag_t	*p, *n;

	p = list->next;
	while( p && p != list )
	{
		n = p->next;

		NET_RemoveFromPacketList( p );

		if( p->data )
		{
			Mem_Free( p->data );
			p->data = NULL;
		}

		Mem_Free( p );
		p = n;
	}

	list->prev = list;
	list->next = list;
}

/*
==================
NET_AddToLagged

add lagged packet to stream
==================
*/
static void NET_AddToLagged( netsrc_t sock, packetlag_t *list, packetlag_t *packet, netadr_t *from, size_t length, const void *data, float timestamp )
{
	byte	*pStart;

	if( packet->prev || packet->next )
		return;

	packet->prev = list->prev;
	list->prev->next = packet;
	list->prev = packet;
	packet->next = list;

	pStart = (byte *)Z_Malloc( length );
	memcpy( pStart, data, length );
	packet->data = pStart;
	packet->size = length;
	packet->receivedtime = timestamp;
	memcpy( &packet->from, from, sizeof( netadr_t ));
}

/*
==================
NET_AdjustLag

adjust time to next fake lag
==================
*/
static void NET_AdjustLag( void )
{
	static double	lasttime = 0.0;
	float		diff, converge;
	double		dt;

	dt = host.realtime - lasttime;
	dt = bound( 0.0, dt, 0.1 );
	lasttime = host.realtime;

	if( host_developer.value || !net_fakelag->value )
	{
		if( net_fakelag->value != net.fakelag )
		{
			diff = net_fakelag->value - net.fakelag;
			converge = dt * 200.0f;
			if( fabs( diff ) < converge )
				converge = fabs( diff );
			if( diff < 0.0 )
				converge = -converge;
			net.fakelag += converge;
		}
	}
	else
	{
		Con_Printf( "Server must enable dev-mode to activate fakelag\n" );
		Cvar_SetValue( "fakelag", 0.0 );
		net.fakelag = 0.0f;
	}
}

/*
==================
NET_LagPacket

add fake lagged packet into rececived message
==================
*/
static qboolean NET_LagPacket( qboolean newdata, netsrc_t sock, netadr_t *from, size_t *length, void *data )
{
	packetlag_t	*pNewPacketLag;
	packetlag_t	*pPacket;
	int		ninterval;
	float		curtime;

	if( net.fakelag <= 0.0f )
	{
		NET_ClearLagData( true, true );
		return newdata;
	}

	curtime = host.realtime;

	if( newdata )
	{
		if( net_fakeloss->value != 0.0f )
		{
			if( host_developer.value )
			{
				net.losscount[sock]++;
				if( net_fakeloss->value <= 0.0f )
				{
					ninterval = fabs( net_fakeloss->value );
					if( ninterval < 2 ) ninterval = 2;

					if(( net.losscount[sock] % ninterval ) == 0 )
						return false;
				}
				else
				{
					if( COM_RandomLong( 0, 100 ) <= net_fakeloss->value )
						return false;
				}
			}
			else
			{
				Cvar_SetValue( "fakeloss", 0.0 );
			}
		}

		pNewPacketLag = (packetlag_t *)Z_Malloc( sizeof( packetlag_t ));
		// queue packet to simulate fake lag
		NET_AddToLagged( sock, &net.lagdata[sock], pNewPacketLag, from, *length, data, curtime );
	}

	pPacket = net.lagdata[sock].next;

	while( pPacket != &net.lagdata[sock] )
	{
		if( pPacket->receivedtime <= curtime - ( net.fakelag / 1000.0 ))
			break;

		pPacket = pPacket->next;
	}

	if( pPacket == &net.lagdata[sock] )
		return false;

	NET_RemoveFromPacketList( pPacket );

	// delivery packet from fake lag queue
	memcpy( data, pPacket->data, pPacket->size );
	memcpy( &net_from, &pPacket->from, sizeof( netadr_t ));
	*length = pPacket->size;

	if( pPacket->data )
		Mem_Free( pPacket->data );

	Mem_Free( pPacket );

	return true;
}

/*
==================
NET_GetLong

receive long packet from network
==================
*/
qboolean NET_GetLong( byte *pData, int size, int *outSize )
{
	int		i, sequence_number, offset;
	SPLITPACKET	*pHeader = (SPLITPACKET *)pData;
	int		packet_number;
	int		packet_count;
	short		packet_id;

	if( size < sizeof( SPLITPACKET ))
	{
		Con_Printf( S_ERROR "invalid split packet length %i\n", size );
		return false;
	}

	sequence_number = pHeader->sequence_number;
	packet_id = pHeader->packet_id;
	packet_count = ( packet_id & 0xFF );
	packet_number = ( packet_id >> 8 );

	if( packet_number >= NET_MAX_FRAGMENTS || packet_count > NET_MAX_FRAGMENTS )
	{
		Con_Printf( S_ERROR "malformed packet number (%i/%i)\n", packet_number + 1, packet_count );
		return false;
	}

	if( net.split.current_sequence == -1 || sequence_number != net.split.current_sequence )
	{
		net.split.current_sequence = sequence_number;
		net.split.split_count = packet_count;
		net.split.total_size = 0;

		// clear part's sequence
		for( i = 0; i < NET_MAX_FRAGMENTS; i++ )
			net.split_flags[i] = -1;

		if( net_showpackets && net_showpackets->value == 4.0f )
			Con_Printf( "<-- Split packet restart %i count %i seq\n", net.split.split_count, sequence_number );
	}

	size -= sizeof( SPLITPACKET );

	if( net.split_flags[packet_number] != sequence_number )
	{
		if( packet_number == ( packet_count - 1 ))
			net.split.total_size = size + SPLIT_SIZE * ( packet_count - 1 );

		net.split.split_count--;
		net.split_flags[packet_number] = sequence_number;

		if( net_showpackets && net_showpackets->value == 4.0f )
			Con_Printf( "<-- Split packet %i of %i, %i bytes %i seq\n", packet_number + 1, packet_count, size, sequence_number );
	}
	else
	{
		Con_DPrintf( "NET_GetLong: Ignoring duplicated split packet %i of %i ( %i bytes )\n", packet_number + 1, packet_count, size );
	}

	offset = (packet_number * SPLIT_SIZE);
	memcpy( net.split.buffer + offset, pData + sizeof( SPLITPACKET ), size );

	// have we received all of the pieces to the packet?
	if( net.split.split_count <= 0 )
	{
		net.split.current_sequence = -1; // Clear packet

		if( net.split.total_size > sizeof( net.split.buffer ))
		{
			Con_Printf( "Split packet too large! %d bytes\n", net.split.total_size );
			return false;
		}

		memcpy( pData, net.split.buffer, net.split.total_size );
		*outSize = net.split.total_size;

		return true;
	}

	return false;
}

/*
==================
NET_QueuePacket

queue normal and lagged packets
==================
*/
qboolean NET_QueuePacket( netsrc_t sock, netadr_t *from, byte *data, size_t *length )
{
	byte		buf[NET_MAX_FRAGMENT];
	int		ret;
	int		net_socket;
	int		addr_len;
	struct sockaddr	addr;

	*length = 0;

	net_socket = net.ip_sockets[sock];

	if( NET_IsSocketValid( net_socket ) )
	{
		addr_len = sizeof( addr );
		ret = pRecvFrom( net_socket, buf, sizeof( buf ), 0, (struct sockaddr *)&addr, &addr_len );

		if( !NET_IsSocketError( ret ) )
		{
			NET_SockadrToNetadr( &addr, from );

			if( ret < NET_MAX_FRAGMENT )
			{
				// Transfer data
				memcpy( data, buf, ret );
				*length = ret;

				// check for split message
				if( *(int *)data == NET_HEADER_SPLITPACKET )
				{
					return NET_GetLong( data, ret, length );
				}

				// lag the packet, if needed
				return NET_LagPacket( true, sock, from, length, data );
			}
			else
			{
				MsgDev( D_REPORT, "NET_QueuePacket: oversize packet from %s\n", NET_AdrToString( *from ));
			}
		}
		else
		{
#ifdef _WIN32
			int	err = pWSAGetLastError();

			switch( err )
			{
			case WSAEWOULDBLOCK:
			case WSAECONNRESET:
			case WSAECONNREFUSED:
			case WSAEMSGSIZE:
				break;
			default:	// let's continue even after errors
				MsgDev( D_ERROR, "NET_QueuePacket: %s from %s\n", NET_ErrorString(), NET_AdrToString( *from ));
				break;
			}
#else
			switch( errno )
			{
			case EWOULDBLOCK:
			case ECONNRESET:
			case ECONNREFUSED:
			case EMSGSIZE:
				break;
			default:	// let's continue even after errors
				MsgDev( D_ERROR, "NET_QueuePacket: %s from %s\n", NET_ErrorString(), NET_AdrToString( *from ));
				break;
			}
#endif
		}
	}

	return NET_LagPacket( false, sock, from, length, data );
}

/*
==================
NET_GetPacket

Never called by the game logic, just the system event queing
==================
*/
qboolean NET_GetPacket( netsrc_t sock, netadr_t *from, byte *data, size_t *length )
{
	if( !data || !length )
		return false;

	NET_AdjustLag();

	if( NET_GetLoopPacket( sock, from, data, length ))
	{
		return NET_LagPacket( true, sock, from, length, data );
	}
	else
	{
		return NET_QueuePacket( sock, from, data, length );
	}
}

/*
==================
NET_SendLong

Fragment long packets, send short directly
==================
*/
int NET_SendLong( netsrc_t sock, int net_socket, const char *buf, int len, int flags, const struct sockaddr *to, int tolen )
{
#ifdef NET_USE_FRAGMENTS
	// do we need to break this packet up?
	if( sock == NS_SERVER && len > MAX_ROUTEABLE_PACKET )
	{
		char		packet[MAX_ROUTEABLE_PACKET];
		int		total_sent, size, packet_count;
		int		ret, packet_number;
		SPLITPACKET	*pPacket;

		net.sequence_number++;
		if( net.sequence_number <= 0 )
			net.sequence_number = 1;

		pPacket = (SPLITPACKET *)packet;
		pPacket->sequence_number = net.sequence_number;
		pPacket->net_id = NET_HEADER_SPLITPACKET;
		packet_number = 0;
		total_sent = 0;
		packet_count = (len + SPLIT_SIZE - 1) / SPLIT_SIZE;

		while( len > 0 )
		{
			size = Q_min( SPLIT_SIZE, len );
			pPacket->packet_id = (packet_number << 8) + packet_count;
			memcpy( packet + sizeof( SPLITPACKET ), buf + ( packet_number * SPLIT_SIZE ), size );

			if( net_showpackets && net_showpackets->value == 3.0f )
			{
				netadr_t	adr;

				memset( &adr, 0, sizeof( adr ));
				NET_SockadrToNetadr((struct sockaddr *)to, &adr );

				Con_Printf( "Sending split %i of %i with %i bytes and seq %i to %s\n",
					packet_number + 1, packet_count, size, net.sequence_number, NET_AdrToString( adr ));
			}

			ret = pSendTo( net_socket, packet, size + sizeof( SPLITPACKET ), flags, to, tolen );
			if( ret < 0 ) return ret; // error

			if( ret >= size )
				total_sent += size;
			len -= size;
			packet_number++;
			Sleep( 1 );
		}

		return total_sent;
	}
	else
#endif
	{
		// no fragmenantion for client connection
		return pSendTo( net_socket, buf, len, flags, to, tolen );
	}
}

/*
==================
NET_SendPacket
==================
*/
void NET_SendPacket( netsrc_t sock, size_t length, const void *data, netadr_t to )
{
	int		ret, err;
	struct sockaddr	addr;
	SOCKET		net_socket;

	if( !net.initialized || to.type == NA_LOOPBACK )
	{
		NET_SendLoopPacket( sock, length, data, to );
		return;
	}
	else if( to.type == NA_BROADCAST )
	{
		net_socket = net.ip_sockets[sock];
		if( !NET_IsSocketValid( net_socket ) )
			return;
	}
	else if( to.type == NA_IP )
	{
		net_socket = net.ip_sockets[sock];
		if( !NET_IsSocketValid( net_socket ) )
			return;
	}
	else
	{
		Host_Error( "NET_SendPacket: bad address type %i\n", to.type );
	}

	NET_NetadrToSockadr( &to, &addr );

	ret = NET_SendLong( sock, net_socket, data, length, 0, &addr, sizeof( addr ));

	if( NET_IsSocketError( err ))
	{
		{
#ifdef _WIN32
			err = pWSAGetLastError();

			// WSAEWOULDBLOCK is silent
			if( err == WSAEWOULDBLOCK )
				return;

			// some PPP links don't allow broadcasts
			if( err == WSAEADDRNOTAVAIL && to.type == NA_BROADCAST )
				return;
#else
			// WSAEWOULDBLOCK is silent
			if( errno == EWOULDBLOCK )
				return;

			// some PPP links don't allow broadcasts
			if( errno == EADDRNOTAVAIL && to.type == NA_BROADCAST )
				return;
#endif
		}
		if( host.type == HOST_DEDICATED )
		{
			MsgDev( D_ERROR, "NET_SendPacket: %s to %s\n", NET_ErrorString(), NET_AdrToString( to ));
		}
#ifdef _WIN32
		else if( err == WSAEADDRNOTAVAIL || err == WSAENOBUFS )
#else
		else if( err == EADDRNOTAVAIL || err == ENOBUFS )
#endif
		{
			MsgDev( D_ERROR, "NET_SendPacket: %s to %s\n", NET_ErrorString(), NET_AdrToString( to ));
		}
		else
		{
			Host_Error( "NET_SendPacket: %s to %s\n", NET_ErrorString(), NET_AdrToString( to ));
		}
	}

}

/*
====================
NET_BufferToBufferCompress

generic fast compression
====================
*/
qboolean NET_BufferToBufferCompress( char *dest, uint *destLen, char *source, uint sourceLen )
{
	uint	uCompressedLen = 0;
	byte	*pbOut = NULL;

	memcpy( dest, source, sourceLen );
	pbOut = LZSS_Compress( source, sourceLen, &uCompressedLen );

	if( pbOut && uCompressedLen > 0 && uCompressedLen <= *destLen )
	{
		memcpy( dest, pbOut, uCompressedLen );
		*destLen = uCompressedLen;
		free( pbOut );
		return true;
	}
	else
	{
		if( pbOut ) free( pbOut );
		memcpy( dest, source, sourceLen );
		*destLen = sourceLen;
		return false;
	}
}

/*
====================
NET_BufferToBufferDecompress

generic fast decompression
====================
*/
qboolean NET_BufferToBufferDecompress( char *dest, uint *destLen, char *source, uint sourceLen )
{
	if( LZSS_IsCompressed( source ))
	{
		uint	uDecompressedLen = LZSS_GetActualSize( source );

		if( uDecompressedLen <= *destLen )
		{
			*destLen = LZSS_Decompress( source, dest );
		}
		else
		{
			return false;
		}
	}
	else
	{
		memcpy( dest, source, sourceLen );
		*destLen = sourceLen;
	}

	return true;
}

/*
====================
NET_IPSocket
====================
*/
static int NET_IPSocket( const char *net_interface, int port, qboolean multicast )
{
	struct sockaddr_in	addr;
	int		err, net_socket;
	uint		optval = 1;
	dword		_true = 1;

	if( NET_IsSocketError(( net_socket = pSocket( PF_INET, SOCK_DGRAM, IPPROTO_UDP )) ) )
	{
#ifdef _WIN32
		int err = pWSAGetLastError();
		if( err != WSAEAFNOSUPPORT )
#else
		if( err != EAFNOSUPPORT )
#endif
			MsgDev( D_WARN, "NET_UDPSocket: socket = %s\n", NET_ErrorString( ));
		return INVALID_SOCKET;
	}

	if( NET_IsSocketError( pIoctlSocket( net_socket, FIONBIO, &_true ) ) )
	{
		MsgDev( D_WARN, "NET_UDPSocket: ioctlsocket FIONBIO = %s\n", NET_ErrorString( ));
		pCloseSocket( net_socket );
		return INVALID_SOCKET;
	}

	// make it broadcast capable
	if( NET_IsSocketError( pSetSockopt( net_socket, SOL_SOCKET, SO_BROADCAST, (char *)&_true, sizeof( _true ) ) ) )
	{
		MsgDev( D_WARN, "NET_UDPSocket: setsockopt SO_BROADCAST = %s\n", NET_ErrorString( ));
		pCloseSocket( net_socket );
		return INVALID_SOCKET;
	}

	if( Sys_CheckParm( "-reuse" ) || multicast )
	{
		if( NET_IsSocketError( pSetSockopt( net_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof( optval )) ) )
		{
			MsgDev( D_WARN, "NET_UDPSocket: port: %d setsockopt SO_REUSEADDR: %s\n", port, NET_ErrorString( ));
			pCloseSocket( net_socket );
			return INVALID_SOCKET;
		}
	}

	if( Sys_CheckParm( "-tos" ))
	{
		optval = 16;
		Con_Printf( "Enabling LOWDELAY TOS option\n" );

		if( NET_IsSocketError( pSetSockopt( net_socket, IPPROTO_IP, IP_TOS, (const char *)&optval, sizeof( optval )) ) )
		{
#ifdef _WIN32
			err = pWSAGetLastError();
			if( err != WSAENOPROTOOPT )
#else
			if( errno != ENOPROTOOPT )
#endif
				Con_Printf( S_WARN "NET_UDPSocket: port: %d  setsockopt IP_TOS: %s\n", port, NET_ErrorString( ));
			pCloseSocket( net_socket );
			return INVALID_SOCKET;
		}
	}

	if( !net_interface[0] || !Q_stricmp( net_interface, "localhost" ))
		addr.sin_addr.s_addr = INADDR_ANY;
	else NET_StringToSockaddr( net_interface, (struct sockaddr *)&addr, false );

	if( port == PORT_ANY ) addr.sin_port = 0;
	else addr.sin_port = pHtons((short)port);

	addr.sin_family = AF_INET;

	if( NET_IsSocketError( pBind( net_socket, (void *)&addr, sizeof( addr )) ) )
	{
		MsgDev( D_WARN, "NET_UDPSocket: port: %d bind: %s\n", port, NET_ErrorString( ));
		pCloseSocket( net_socket );
		return INVALID_SOCKET;
	}

	if( Sys_CheckParm( "-loopback" ))
	{
		optval = 1;
		if( NET_IsSocketError( pSetSockopt( net_socket, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&optval, sizeof( optval )) ) )
			MsgDev( D_WARN, "NET_UDPSocket: port %d setsockopt IP_MULTICAST_LOOP: %s\n", port, NET_ErrorString( ));
	}

	return net_socket;
}

/*
====================
NET_OpenIP
====================
*/
static void NET_OpenIP( void )
{
	int	port, sv_port = 0, cl_port = 0;

	if( !NET_IsSocketValid( net.ip_sockets[NS_SERVER] ) )
	{
		port = net_iphostport->value;
		if( !port ) port = net_hostport->value;
		if( !port ) port = PORT_SERVER; // forcing to default
		net.ip_sockets[NS_SERVER] = NET_IPSocket( net_ipname->string, port, false );

		if( !NET_IsSocketValid( net.ip_sockets[NS_SERVER] ) && host.type == HOST_DEDICATED )
			Host_Error( "Couldn't allocate dedicated server IP port %d.\n", port );
		sv_port = port;
	}

	// dedicated servers don't need client ports
	if( host.type == HOST_DEDICATED ) return;

	if( !NET_IsSocketValid( net.ip_sockets[NS_CLIENT] ) )
	{
		port = net_ipclientport->value;
		if( !port ) port = net_clientport->value;
		if( !port ) port = PORT_ANY; // forcing to default
		net.ip_sockets[NS_CLIENT] = NET_IPSocket( net_ipname->string, port, false );

		if( !NET_IsSocketValid( net.ip_sockets[NS_CLIENT] ) )
			net.ip_sockets[NS_CLIENT] = NET_IPSocket( net_ipname->string, PORT_ANY, false );
		cl_port = port;
	}
}

/*
================
NET_GetLocalAddress

Returns the servers' ip address as a string.
================
*/
void NET_GetLocalAddress( void )
{
	char		buff[512];
	struct sockaddr_in	address;
	int		namelen;

	memset( &net_local, 0, sizeof( netadr_t ));
	buff[0] = '\0';

	if( net.allow_ip )
	{
		// If we have changed the ip var from the command line, use that instead.
		if( Q_strcmp( net_ipname->string, "localhost" ))
		{
			Q_strcpy( buff, net_ipname->string );
		}
		else
		{
			pGetHostName( buff, 512 );
		}

		// ensure that it doesn't overrun the buffer
		buff[511] = 0;

		if( NET_StringToAdr( buff, &net_local ))
		{
			namelen = sizeof( address );

			if( NET_IsSocketError( pGetSockName( net.ip_sockets[NS_SERVER], (struct sockaddr *)&address, &namelen ) ) )
			{
				// this may happens if multiple clients running on single machine
				MsgDev( D_ERROR, "Could not get TCP/IP address. Reason:  %s\n", NET_ErrorString( ));
//				net.allow_ip = false;
			}
			else
			{
				net_local.port = address.sin_port;
				Con_Printf( "Server IP address %s\n", NET_AdrToString( net_local ));
				Cvar_FullSet( "net_address", va( NET_AdrToString( net_local )), FCVAR_READ_ONLY );
			}
		}
		else
		{
			MsgDev( D_ERROR, "Could not get TCP/IP address, Invalid hostname: '%s'\n", buff );
		}
	}
	else
	{
		Con_Printf( "TCP/IP Disabled.\n" );
	}
}

/*
====================
NET_Config

A single player game will only use the loopback code
====================
*/
void NET_Config( qboolean multiplayer )
{
	static qboolean	bFirst = true;
	static qboolean	old_config;

	if( !net.initialized )
		return;

	if( old_config == multiplayer )
		return;

	old_config = multiplayer;

	if( multiplayer )
	{	
		// open sockets
		if( net.allow_ip ) NET_OpenIP();

		// get our local address, if possible
		if( bFirst )
		{
			NET_GetLocalAddress();
			bFirst = false;
		}
	}
	else
	{	
		int	i;

		// shut down any existing sockets
		for( i = 0; i < NS_COUNT; i++ )
		{
			if( net.ip_sockets[i] != INVALID_SOCKET )
			{
				pCloseSocket( net.ip_sockets[i] );
				net.ip_sockets[i] = INVALID_SOCKET;
			}
		}
	}

	NET_ClearLoopback ();

	net.configured = multiplayer ? true : false;
}

/*
====================
NET_IsConfigured

Is winsock ip initialized?
====================
*/
qboolean NET_IsConfigured( void )
{
	return net.configured;
}

/*
====================
NET_IsActive
====================
*/
qboolean NET_IsActive( void )
{
	return net.initialized;
}

/*
====================
NET_Sleep

sleeps msec or until net socket is ready
====================
*/
void NET_Sleep( int msec )
{
	struct timeval	timeout;
	fd_set		fdset;
	int		i = 0;

	if( !net.initialized || host.type == HOST_NORMAL )
		return; // we're not a dedicated server, just run full speed

	FD_ZERO( &fdset );

	if( net.ip_sockets[NS_SERVER] != INVALID_SOCKET )
	{
		FD_SET( net.ip_sockets[NS_SERVER], &fdset ); // network socket
		i = net.ip_sockets[NS_SERVER];
	}

	timeout.tv_sec = msec / 1000;
	timeout.tv_usec = (msec % 1000) * 1000;
	pSelect( i+1, &fdset, NULL, NULL, &timeout );
}

/*
====================
NET_ClearLagData

clear fakelag list
====================
*/
void NET_ClearLagData( qboolean bClient, qboolean bServer )
{
	if( bClient ) NET_ClearLaggedList( &net.lagdata[NS_CLIENT] );
	if( bServer ) NET_ClearLaggedList( &net.lagdata[NS_SERVER] );
}

/*
====================
NET_Init
====================
*/
void NET_Init( void )
{
	char	cmd[64];
	int	i = 1;

	if( net.initialized ) return;

	net_clockwindow = Cvar_Get( "clockwindow", "0.5", 0, "timewindow to execute client moves" );
	net_address = Cvar_Get( "net_address", "0", FCVAR_READ_ONLY, "contain local address of current client" );
	net_ipname = Cvar_Get( "ip", "localhost", FCVAR_READ_ONLY, "network ip address" );
	net_iphostport = Cvar_Get( "ip_hostport", "0", FCVAR_READ_ONLY, "network ip host port" );
	net_hostport = Cvar_Get( "hostport", va( "%i", PORT_SERVER ), FCVAR_READ_ONLY, "network default host port" );
	net_ipclientport = Cvar_Get( "ip_clientport", "0", FCVAR_READ_ONLY, "network ip client port" );
	net_clientport = Cvar_Get( "clientport", va( "%i", PORT_CLIENT ), FCVAR_READ_ONLY, "network default client port" );
	net_fakelag = Cvar_Get( "fakelag", "0", 0, "lag all incoming network data (including loopback) by xxx ms." );
	net_fakeloss = Cvar_Get( "fakeloss", "0", 0, "act like we dropped the packet this % of the time." );

	// prepare some network data
	for( i = 0; i < NS_COUNT; i++ )
	{
		net.lagdata[i].prev = &net.lagdata[i];
		net.lagdata[i].next = &net.lagdata[i];
		net.ip_sockets[i] = INVALID_SOCKET;
	}

#ifdef _WIN32
	if( !NET_OpenWinSock( ))	// loading wsock32.dll
	{
		MsgDev( D_ERROR, "network failed to load wsock32.dll.\n" );
		return;
	}

	if( pWSAStartup( MAKEWORD( 1, 1 ), &net.winsockdata ))
	{
		MsgDev( D_ERROR, "network initialization failed.\n" );
		NET_FreeWinSock();
		return;
	}
#endif

	if( Sys_CheckParm( "-noip" ))
		net.allow_ip = false;
	else net.allow_ip = true;

	// specify custom host port
	if( Sys_GetParmFromCmdLine( "-port", cmd ) && Q_isdigit( cmd ))
		Cvar_FullSet( "hostport", cmd, FCVAR_READ_ONLY );

	// adjust clockwindow
	if( Sys_GetParmFromCmdLine( "-clockwindow", cmd ))
		Cvar_SetValue( "clockwindow", Q_atof( cmd ));

	net.sequence_number = 1;
	net.initialized = true;
	MsgDev( D_REPORT, "Base networking initialized.\n" );
}


/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown( void )
{
	if( !net.initialized )
		return;

	NET_ClearLagData( true, true );

	NET_Config( false );
#ifdef _WIN32
	pWSACleanup();
	NET_FreeWinSock();
#endif
	net.initialized = false;
}
