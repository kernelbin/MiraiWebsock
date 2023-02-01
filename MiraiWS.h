#pragma once

#include <Windows.h>
#include <WinHttp.h>

EXTERN_C_START

#define MIRAI_WS_MAXBUF (1LL << 16)

typedef enum _MESSAGE_BLOCK_TYPE
{
    MB_AT = 1,
    MB_ATALL,
    MB_FACE,
    MB_PLAIN,
    MB_IMAGE, // FlashImage is also here
    MB_VOICE,
    MB_XML,
    MB_JSON,
    MB_APP,
    MB_POKE,
    MB_DICE,
    MB_MARKETFACE,
    MB_MUSICSHARE,
    MB_FORWARD,
    MB_FILE
} MESSAGE_BLOCK_TYPE;

typedef struct _MESSAGE_BLOCK MESSAGE_BLOCK, *PMESSAGE_BLOCK;
typedef struct _MESSAGE_BLOCK
{
    MESSAGE_BLOCK_TYPE Type;
    union
    {
        struct
        {
            INT64 Target;
            LPWSTR Display;
        } At;
        struct
        {
            INT64 FaceID;
        } Face;
        struct
        {
            LPWSTR Text;
        } Plain;
        struct
        {
            BOOL IsFlash;
            LPWSTR ImageIDStr;
            LPWSTR URL;
            
            LPWSTR ImageType; // not documented in mirai-api-http, but sent in json
            BOOL IsEmoji;     // not documented in mirai-api-http, but sent in json
        } Image;
        struct
        {
            LPWSTR VoiceIDStr;
            LPWSTR URL;
            INT64 Length;
        } Voice;
    };
} MESSAGE_BLOCK;

typedef struct
{
    INT64 ID;
    INT64 Timestamp;

    PMESSAGE_BLOCK MessageBlocks;
    int BlockCnt;
} MESSAGE_CHAIN;

// Event Types

// sent after ConnectMiraiWS is called.
// pInformation is pointer to MWS_CONNECTINFO
// when failed, dwError contains reason.
#define MWS_CONNECT 1

// some network error happened and MiraiWS have shutdown the connection
// pInformation is pointer to MWS_NWERRORINFO
// dwError contains reason.
#define MWS_NWERROR 2

// received an message and failed to parse
// pInformation is pointer to MWS_BADMSGINFO
// Message and Length contains the raw json message received
#define MWS_BADMSG  3

// received an authentication message, usually first message after connection
// ResponseCode is the code mirai sent
// may contain Session or Message.
#define MWS_AUTH    4

// received a friend's message
// MessageChain contains the message received
// Sender contains sender information
#define MWS_FRIENDMSG 5

// received a group message
// MessageChain contains the message received
// Sender contains sender and group information
#define MWS_GROUPMSG 6


typedef struct
{
    BOOL bSuccess;
    DWORD dwError;
} MWS_CONNECTINFO;

typedef struct
{
    DWORD dwError;
} MWS_NWERRORINFO;

typedef struct
{
    LPCWSTR Message;
    SIZE_T Length;
} MWS_BADMSGINFO;

typedef struct
{
    INT64 ResponseCode;
    LPCWSTR Session;
    LPCWSTR Message;
} MWS_AUTHINFO;

typedef struct
{
    struct
    {
        INT64 ID;
        LPWSTR Nick;
        LPWSTR Remark;
    } Sender;
    MESSAGE_CHAIN MessageChain;
} MWS_FRIENDMSGINFO;

typedef struct
{
    struct
    {
        INT64 ID;
        LPWSTR MemberName;
        LPWSTR SpecialTitle;
        LPWSTR Permission;
        INT64 JoinTimestamp;
        INT64 LastSpeakTimestamp;
        INT64 MuteTimeRemaining;

        struct
        {
            INT64 ID;
            LPWSTR Name;
            LPWSTR Permission;
        } Group;
    } Sender;
    MESSAGE_CHAIN MessageChain;
} MWS_GROUPMSGINFO;

typedef struct _MIRAI_WS MIRAI_WS, * PMIRAI_WS;

typedef VOID(*MWSCALLBACK)(_In_ PMIRAI_WS pMiraiWS, _In_ UINT EventType, _In_ PVOID pInformation);

typedef VOID(*SEND_MSG_CALLBACK)(_In_ PMIRAI_WS pMiraiWS, _In_ INT64 RetCode, _In_z_ LPCWSTR lpMessage, _In_ INT64 MessageCode, _In_ LPVOID Context);

typedef struct _MIRAI_WS
{
    HINTERNET hSessionHandle;
    HINTERNET hConnectionHandle;
    HINTERNET hRequestHandle;
    HINTERNET hWebSocketHandle;

    LPWSTR        lpServerName;
    INTERNET_PORT Port;
    BOOL          bSecure;

    BYTE          Buffer[MIRAI_WS_MAXBUF];
    SIZE_T        RecvLength;

    MWSCALLBACK Callback;
    BOOL bClose;
}MIRAI_WS, * PMIRAI_WS;

_Ret_maybenull_
/// <summary>
/// Create a instance of mirai websocket. Call ConnectMiraiWS to connect it.
/// </summary>
/// <param name="lpServerName">Server Name or IP Address</param>
/// <param name="Port">Server Port</param>
/// <param name="bSecure">Enable TLS 1.2 or newer.</param>
/// <returns>return a handle of mirai websock on success</returns>
PMIRAI_WS CreateMiraiWS(_In_z_ LPCWSTR lpServerName, _In_ INTERNET_PORT Port, _In_ BOOL bSecure, _In_ MWSCALLBACK Callback);

/// <summary>
/// Try connect to mirai
/// </summary>
/// <param name="pMiraiWS">handle created by CreateMiraiWS</param>
/// <param name="szVerifyKey">Verify key filled in mirai-api-http config file</param>
/// <param name="szQQ">QQ to connect to, in string</param>
/// <returns>return TRUE on success</returns>
BOOL ConnectMiraiWS(_Inout_ PMIRAI_WS pMiraiWS, _In_z_ LPCWSTR szVerifyKey, _In_z_ LPCWSTR szQQ);

/// <summary>
/// Destroy a instance of mirai websocket, asynchronouslly.
/// </summary>
/// <param name="pMiraiWS">handle created by CreateMiraiWS</param>
/// <returns>return TRUE on success</returns>
BOOL DestroyMiraiWSAsync(_In_ _Frees_ptr_ PMIRAI_WS pMiraiWS);

/// <summary>
/// Send a message to a friend
/// </summary>
/// <param name="pMiraiWS">handle created by CreateMiraiWS</param>
/// <param name="Target">target QQ id the message will be sent to</param>
/// <param name="pMessageChain">the message to send</param>
/// <param name="Callback">An optional callback to notify the sending result</param>
/// <param name="Context">user defined context to pass to Callback</param>
/// <returns>TRUE on success, callback will be called, if given, when sending finished.</returns>
BOOL SendFriendMsgAsync(
    _In_ PMIRAI_WS pMiraiWS,
    _In_ INT64 Target,
    _In_ MESSAGE_CHAIN* pMessageChain,
    _In_opt_ SEND_MSG_CALLBACK Callback,
    _In_opt_ LPVOID Context);

/// <summary>
/// Send a message to a group
/// </summary>
/// <param name="pMiraiWS">handle created by CreateMiraiWS</param>
/// <param name="Target">target Group id the message will be sent to</param>
/// <param name="pMessageChain">the message to send</param>
/// <param name="Callback">An optional callback to notify the sending result</param>
/// <param name="Context">user defined context to pass to Callback</param>
/// <returns>TRUE on success, callback will be called, if given, when sending finished.</returns>
BOOL SendGroupMsgAsync(
    _In_ PMIRAI_WS pMiraiWS,
    _In_ INT64 Target,
    _In_ MESSAGE_CHAIN* pMessageChain,
    _In_opt_ SEND_MSG_CALLBACK Callback,
    _In_opt_ LPVOID Context
);

EXTERN_C_END