/*============================================================================
 * TitanInterfaceC.stub.c
 * Dummy functions to simulate a WONnet connection that never works.
 *
 * Author:  Ted Cipicchio <ted@thereisnospork.com>
 * Created: Sat Oct 4 2003
 *==========================================================================*/
#include "TitanInterfaceC.h"
#include "lan.h"
#include "utility.h"

#ifndef _WIN32
    #include <arpa/inet.h>
#endif

#include "Debug.h"

/*----------------------------------------------------------------------------
 * Global Variables
 *--------------------------------------------------------------------------*/

wchar_t ChannelPassword[MAX_PASSWORD_LEN];
TPChannelList tpChannelList;
TPServerList tpServerList;
CaptainGameInfo tpGameCreated;
Address myAddress;
TitanGameCreationState mGameCreationState = TITANGAME_NOT_STARTED;
unsigned long DIRSERVER_PORTS[MAX_PORTS];
unsigned long PATCHSERVER_PORTS[MAX_PORTS];
ipString DIRSERVER_IPSTRINGS[MAX_IPS];
ipString PATCHSERVER_IPSTRINGS[MAX_IPS];
unsigned long HomeworldCRC[4];
wchar_t GameWereInterestedIn[MAX_TITAN_GAME_NAME_LEN];
void *GameWereInterestedInMutex = 0;


/*----------------------------------------------------------------------------
 * Functions
 *--------------------------------------------------------------------------*/

void titanGotNumUsersInRoomCB(const wchar_t *theRoomName, int theNumUsers)
{
	dbgMessagef("\ntitanGotNumUsersInRoomCB");
}


unsigned long titanStart(unsigned long isLan, unsigned long isIP)
{
#ifdef HW_ENABLE_NETWORK
	/* IPX is gone and is not coming back. The lobby tries both protocols and
	   takes whichever answers, so refusing the non-IP one here is how it ends
	   up on TCP/IP rather than reporting no network at all. */
	if (!isIP)
	{
		return 0;
	}
	if (!lanStart())
	{
		return 0;
	}

	myAddress.AddrPart.IP = lanLocalAddress();
	myAddress.Port = LAN_TCP_PORT;

	/* Internet play, such as it is: name the host and its advertisements
	   reach us and ours reach it, over exactly the same protocol a LAN uses.
	   The host needs LAN_TCP_PORT and LAN_UDP_PORT forwarded to it; nobody
	   else needs to configure anything, because the host learns our address
	   from the first packet we send. */
	if (utyMultiplayerHost[0] != 0)
	{
		udword remote = inet_addr(utyMultiplayerHost);

		if (remote == INADDR_NONE)
		{
			dbgMessagef("titanStart: MultiplayerHost '%s' is not a dotted quad, ignoring",
			            utyMultiplayerHost);
		}
		else
		{
			lanAddRemote(remote);
		}
	}

	/* Task.c gates titanPumpEngine on this, and nothing else ever set it, so
	   without this line the transport is never serviced. */
	TitanActive = TRUE;

	dbgMessagef("titanStart: up on %u.%u.%u.%u",
	            (unsigned)(myAddress.AddrPart.IP        & 0xff),
	            (unsigned)((myAddress.AddrPart.IP >>  8) & 0xff),
	            (unsigned)((myAddress.AddrPart.IP >> 16) & 0xff),
	            (unsigned)((myAddress.AddrPart.IP >> 24) & 0xff));
	return 1;
#else
	dbgMessagef("titanStart: built without networking");
	return 0;
#endif
}


unsigned long titanCheckCanNetwork(unsigned long isLan, unsigned long isIP)
{
	dbgMessagef("\ntitanCheckCanNetwork");
	if ( isLan != 0 && isIP == 1)
		return 1;
	else
		return 0;
}


// --MikeN
// Call this method to begin shutdown of titan.  Parameters specify packet to send
// to connected client(s) (a shutdown message).  The callback titanNoClientsCB() will
// be invoked when complete.
void titanStartShutdown(unsigned long titanMsgType, const void* thePacket,
                        unsigned short theLen)
{
	dbgMessagef("\ntitanStartShutdown");
	/* Probably won't ever be called, but we'll be consistent anyway... */
	titanNoClientsCB();
}


void titanLeaveGameNotify(void)
{
	dbgMessagef("\ntitanLeaveGameNotify");
	/* Matches TitanInterface::LeaveGameNotify. Without it a second game in
	   the same session would still see the state left by the first. */
	mGameCreationState = TITANGAME_NOT_STARTED;
}


void titanShutdown(void)
{
	dbgMessagef("titanShutdown");
#ifdef HW_ENABLE_NETWORK
	lanShutdown();
	TitanActive = FALSE;
#endif
}


void titanRefreshRequest(char* theDir)
{
	dbgMessagef("\ntitanRefreshRequest");
}


/* Returns whether the game may start now, and is the last gate before
   mgStartGameCB sets sigsPressedStartGame and the universe task calls
   utyNewGameStart.

   Returning 0 unconditionally, as this did, means no game can ever start by
   any route: not LAN, and not a single-player skirmish against the AI, which
   needs no transport at all.

   TitanInterface::CheckStartingGame is the original. Its first branch covers
   exactly the cases reachable here - a LAN game, or one human - and answers by
   marking the game started and returning true. The rest of that function
   negotiates a WON routing server for internet play, which has no counterpart
   in this build. InitPacketList() is not ported with it: it clears the resend
   list and sequence number belonging to that routing scheme, and there is no
   such list on this side.

   Anything else still returns 0 rather than pretending, so an internet game
   fails visibly instead of starting into a transport that cannot carry it. */
unsigned long titanReadyToStartGame(unsigned char *routingaddress)
{
	if (LANGame || tpGameCreated.numPlayers == 1)
	{
		mGameCreationState = TITANGAME_STARTED;
		dbgMessagef("titanReadyToStartGame: starting (%s, %d player(s))",
		            LANGame ? "LAN" : "local", (int)tpGameCreated.numPlayers);
		return 1;
	}

	dbgMessagef("titanReadyToStartGame: refused, no transport for a %d player "
	            "internet game", (int)tpGameCreated.numPlayers);
	return 0;
}


unsigned long titanBehindFirewall(void)
{
	dbgMessagef("\ntitanBehindFirewall");
	return 0; 
}


void titanCreateGame(wchar_t *str, DirectoryCustomInfo* myInfo)
{
	dbgMessagef("\ntitanCreateGame");

}


void titanRemoveGame(wchar_t *str)
{
	dbgMessagef("\ntitanRemoveGame");

}


void titanCreateDirectory(char *str, char* desc)
{
	dbgMessagef("\ntitanCreateDirectory");

}


void titanSendLanBroadcast(const void* thePacket, unsigned short theLen)
{
//	dbgMessagef("\ntitanSendLanBroadcast");
#ifdef HW_ENABLE_NETWORK
	/* Our own broadcast comes back to us through the UDP socket, so the
	   direct callback this used to need is gone. */
	lanSendDiscovery(thePacket, theLen);
#endif
}


void titanSendPacketTo(Address *address, unsigned char titanMsgType,
                       const void* thePacket, unsigned short theLen)
{
	dbgMessagef("\ntitanSendPacketTo");
#ifdef HW_ENABLE_NETWORK
	/* A message addressed to ourselves is dispatched directly. The lobby
	   really does send to every player including the local one, and looping
	   it through a socket would need us to be our own peer. */
	if (InternetAddressesAreEqual(*address, myAddress))
		HandleTCPMessage(address->AddrPart.IP, titanMsgType, thePacket, theLen);
	else
		lanSendTo(address->AddrPart.IP, titanMsgType, thePacket, theLen);
#endif

}


/* To every player in the game except me.

   The original branches on mGameCreationState three ways
   (TitanInterface.cpp:5607) and sends in all three; the only real difference
   is that the started case could route through a WON server, which does not
   exist here. This port kept only the GAME_NOT_STARTED branch, which was
   harmless while nothing ever moved the state off it. It stopped being
   harmless when titanReadyToStartGame started setting TITANGAME_STARTED for
   real: in-game traffic goes through here too (titanSendBroadcastMessage in
   TitanNet.c wraps it as TITANMSGTYPE_GAME), so the gate would have silently
   dropped every sync packet from the moment a game began. */
void titanBroadcastPacket(unsigned char titanMsgType, const void* thePacket, unsigned short theLen)
{
#ifdef HW_ENABLE_NETWORK
	int i;

	for (i = 0; i < tpGameCreated.numPlayers; i++)
	{
		if (!InternetAddressesAreEqual(tpGameCreated.playerInfo[i].address, myAddress))
		{
			lanSendTo(tpGameCreated.playerInfo[i].address.AddrPart.IP,
			          titanMsgType, thePacket, theLen);
		}
	}
#endif
}


void titanAnyoneSendPacketTo(Address *address, unsigned char titanMsgType,
                       const void* thePacket, unsigned short theLen)
{
	/* The "Anyone" pair exists for the window during captaincy transfer when
	   there is no captain to route through, so WON could not use its normal
	   path. Peer to peer there is no difference. Leaving them empty would make
	   host migration fail silently: the survivors would elect nobody and sit
	   waiting for a captain that never speaks. */
	titanSendPacketTo(address, titanMsgType, thePacket, theLen);
}


void titanAnyoneBroadcastPacket(unsigned char titanMsgType, const void* thePacket, unsigned short theLen)
{
	titanBroadcastPacket(titanMsgType, thePacket, theLen);
}


void titanConnectToClient(Address *address)
{
	dbgMessagef("\ntitanConnectToClient");
#ifdef HW_ENABLE_NETWORK
	/* The old implementation learned our own address from what the server
	   saw, which is why it blocked here waiting for a reply. getifaddrs told
	   us at startup, so this only has to open the link. */
	lanConnect(address->AddrPart.IP);
#endif
}


int titanStartChatServer(wchar_t *password)
{
	dbgMessagef("\ntitanStartChatServer");
	return 0;
}


void titanSendPing(Address *address,unsigned int pingsizebytes)
{
	dbgMessagef("\ntitanSendPing");
}


/* Task.c calls this once per active task per scheduler pass, so it is the
   transport's service tick. It must stay cheap when there is nothing to do. */
void titanPumpEngine()
{
#ifdef HW_ENABLE_NETWORK
	lanService();
#endif
}


void titanSetGameKey(unsigned char *key)
{
	dbgMessagef("\ntitanSetGameKey");
}


const unsigned char *titanGetGameKey(void)
{
	dbgMessagef("\ntitanGetGameKey");
	return 0; 
}


Address titanGetMyPingAddress(void)
{ 
	dbgMessagef("\ntitanGetMyPingAddress");
	return myAddress; 
}


int titanGetPatch(char *filename,char *saveFileName)
{
	dbgMessagef("\ntitanGetPatch");
	titanGetPatchFailedCB(PATCHFAIL_UNABLE_TO_CONNECT);
	return PATCHFAIL_UNABLE_TO_CONNECT;
}


void titanReplaceGameInfo(wchar_t *str, DirectoryCustomInfo* myInfo, unsigned long replaceTimeout)
{
	dbgMessagef("\ntitanReplaceGameInfo");
}


void chatConnect(wchar_t *password)
{
	dbgMessagef("\nchatConnect");
}


void chatClose(void)
{
	dbgMessagef("\nchatClose");
}


void BroadcastChatMessage(unsigned short size, const void* chatData)
{
	dbgMessagef("\nBroadcastChatMessage");
}


void SendPrivateChatMessage(unsigned long* userIDList, unsigned short numUsersInList,
                            unsigned short size, const void* chatData)
{
	dbgMessagef("\nSendPrivateChatMessage");
}


void authAuthenticate(char *loginName, char *password)
{
	dbgMessagef("\nauthAuthenticate");
}


void authCreateUser(char *loginName, char *password)
{
	dbgMessagef("\nauthCreateUser");
}


void authChangePassword(char *loginName, char *oldpassword, char *newpassword)
{
	dbgMessagef("\nauthChangePassword");
}


int titanSaveWonstuff()
{ 
	dbgMessagef("\ntitanSaveWonstuff");
	return 0; 
}


void titanWaitShutdown(void)
{
	dbgMessagef("\ntitanWaitShutdown");
}


void titanConnectingCancelHit(void)
{
	dbgMessagef("\ntitanConnectingCancelHit");
}

#ifdef HW_ENABLE_NETWORK
/* Dispatch targets, private to this file: the transport only ever calls
   HandleTCPMessage, which fans out to these. */
static void HandleJoinGame(Uint32, const void*, unsigned short);
static void HandleJoinConfirm(Uint32, const void*, unsigned short);
static void HandleJoinReject(Uint32, const void*, unsigned short);
static void HandleGameData(const void*, unsigned short);
static void HandleGameStart(const void*, unsigned short);
static void HandleGameMsg(const void*, unsigned short);

void HandleTCPMessage(Uint32 address, unsigned char msgTyp, const void* data, unsigned short len)
{
	switch(msgTyp)
	{
		case TITANMSGTYPE_JOINGAMEREQUEST :
			HandleJoinGame(address,data,len);
			break;
		case TITANMSGTYPE_JOINGAMECONFIRM :
			HandleJoinConfirm(address,data,len);
			break;
		case TITANMSGTYPE_JOINGAMEREJECT :
			HandleJoinReject(address,data,len);
			dbgMessagef("\nTITANMSGTYPE_JOINGAMEREJECT HandleTCPMessage");
			break;
		case TITANMSGTYPE_UPDATEGAMEDATA :
			HandleGameData(data,len);
			break;
		case TITANMSGTYPE_LEAVEGAMEREQUEST :
			dbgMessagef("\nTITANMSGTYPE_LEAVEGAMEREQUEST HandleTCPMessage");
			break;
		case TITANMSGTYPE_GAMEISSTARTING :
			HandleGameStart(data,len);
			break;
		case TITANMSGTYPE_PING :
			dbgMessagef("\nTITANMSGTYPE_PING HandleTCPMessage");
			break;
		case TITANMSGTYPE_PINGREPLY :
			dbgMessagef("\nTITANMSGTYPE_PINGREPLY HandleTCPMessage");
			break;
		case TITANMSGTYPE_GAME :
			HandleGameMsg(data,len);
			break;
		case TITANMSGTYPE_GAMEDISOLVED :
			dbgMessagef("\nTITANMSGTYPE_GAMEDISOLVED HandleTCPMessage");
			break;
		case TITANMSGTYPE_UPDATEPLAYER :
			dbgMessagef("\nTITANMSGTYPE_UPDATEPLAYER HandleTCPMessage");
			break;
		case TITANMSGTYPE_BEGINSTARTGAME :
			dbgMessagef("\nTITANMSGTYPE_BEGINSTARTGAME HandleTCPMessage");
			break;
		case TITANMSGTYPE_CHANGEADDRESS :
			dbgMessagef("\nTITANMSGTYPE_CHANGEADDRESS HandleTCPMessage");
			break;
		case TITANMSGTYPE_REQUESTPACKETS :
			dbgMessagef("\nTITANMSGTYPE_REQUESTPACKETS HandleTCPMessage");
			break;
		case TITANMSGTYPE_RECONNECT :
			dbgMessagef("\nTITANMSGTYPE_RECONNECT HandleTCPMessage");
			break;
		case TITANMSGTYPE_KICKPLAYER :
			dbgMessagef("\nTITANMSGTYPE_KICKPLAYER HandleTCPMessage");
			break;
	}
}

static void HandleJoinGame(Uint32 address, const void* data, unsigned short len)
{
	Address anAddress;
	long requestResult;

	anAddress.AddrPart.IP = address;
	anAddress.Port = LAN_TCP_PORT;

	/* The joiner tells us the address it calls itself, and that is what the
	   lobby has to record: every player gets a copy of it, and each of them
	   looks for its own entry by comparing against its own address. The
	   socket address is what we can reach, which is a different question and
	   one lanRouteTo answers. On a LAN the two are the same and this changes
	   nothing; behind NAT the joiner would otherwise be listed under an
	   address it has never heard of and would fail to find itself at game
	   start. A joiner that sends no address of its own still gets the old
	   behaviour rather than being refused. */
	if (data != NULL && len == sizeof(PlayerJoinInfo))
	{
		Address const claimed = ((PlayerJoinInfo const*)data)->address;

		if (claimed.AddrPart.IP != 0)
		{
			anAddress = claimed;
		}
	}

	if(mGameCreationState==TITANGAME_NOT_STARTED)
		requestResult = titanRequestReceivedCB(&anAddress, data, len);
	else
		requestResult = REQUEST_RECV_CB_JUSTDENY;
	
	if (requestResult == REQUEST_RECV_CB_ACCEPT)
        {
            titanSendPacketTo(&anAddress, TITANMSGTYPE_JOINGAMECONFIRM, NULL, 0);
            titanBroadcastPacket(TITANMSGTYPE_UPDATEGAMEDATA, &tpGameCreated, sizeof(tpGameCreated));
        }
        else
        {
            titanSendPacketTo(&anAddress, TITANMSGTYPE_JOINGAMEREJECT, NULL, 0);
        }
}

static void HandleJoinConfirm(Uint32 address, const void* data, unsigned short len)
{
	Address anAddress;
	anAddress.AddrPart.IP = address;
	anAddress.Port = LAN_TCP_PORT;

        titanConfirmReceivedCB(&anAddress, data, len);
}

static void HandleJoinReject(Uint32 address, const void* data, unsigned short len)
{
	Address anAddress;
	anAddress.AddrPart.IP = address;
	anAddress.Port = LAN_TCP_PORT;

        titanRejectReceivedCB(&anAddress, data, len);
}

static void HandleGameData(const void* data, unsigned short len)
{
        titanUpdateGameDataCB(data,len);
}

static void HandleGameStart(const void* data, unsigned short len)
{
	mgGameStartReceivedCB(data,len);
}

static void HandleGameMsg(const void* data, unsigned short len)
{
	titanGameMsgReceivedCB(data,len);
}

#endif
