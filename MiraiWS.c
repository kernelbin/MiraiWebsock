#include <Windows.h>
#include <strsafe.h>
#include "MiraiWS.h"
#include "yyjson.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Normaliz.lib")

#define MAX_ASYNC_PENDING 1024
SRWLOCK AsyncCallListLock = SRWLOCK_INIT;

typedef enum _ASYNC_CALL_TYPE
{
    ASYNC_FRIENDMSG = 1,
    ASYNC_GROUPMSG
}ASYNC_CALL_TYPE;

typedef struct
{
    BOOL bUsed;
    INT64 ID;
    ASYNC_CALL_TYPE Type;
    LPVOID Callback;
    LPVOID Context;
} ASYNC_CALL;
// It's really stupid to use an array to store and search for pending async calls. it takes O(n) to search, insert, and remove
// I hope there should not have too much pending calls at a same time.
// TODO: consider use some other K-V containers like a balanced binary tree, or 01-trie.
ASYNC_CALL AsyncCalls[MAX_ASYNC_PENDING] = { 0 };
INT64 AsyncCallIDAlloc = 0;

typedef BOOL(*EVENTHANDLER)(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField);

#define RESERVED_SYNC_ID -1 // set in setting.yml of mirai.

/// <summary>
/// Allocate space and copy a zero-terminated ANSI string.
/// Free the allocated string using HeapFree with GetProcessHeap
/// </summary>
/// <param name="Source">The string to copy</param>
/// <returns>return the allocated string or NULL when failure.</returns>
static LPSTR StrAllocCopyA(_In_ LPCSTR Source)
{
    SIZE_T len = strlen(Source);
    LPSTR lpCopyStr = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(CHAR));
    if (!lpCopyStr) return NULL;

    if (StringCchCopyA(lpCopyStr, len + 1, Source) != S_OK)
    {
        HeapFree(GetProcessHeap(), 0, lpCopyStr);
        return NULL;
    }
    lpCopyStr[len] = '\0';
    return lpCopyStr;
}

/// <summary>
/// Allocate space and copy a zero-terminated Wide Char string.
/// </summary>
/// <param name="Source">The string to copy</param>
/// <returns>return the allocated string or NULL when failure.</returns>
static LPWSTR StrAllocCopyW(_In_ LPCWSTR Source)
{
    SIZE_T len = wcslen(Source);
    LPWSTR lpCopyStr = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!lpCopyStr) return NULL;

    if (StringCchCopyW(lpCopyStr, len + 1, Source) != S_OK)
    {
        HeapFree(GetProcessHeap(), 0, lpCopyStr);
        return NULL;
    }
    lpCopyStr[len] = L'\0';
    return lpCopyStr;
}

/// <summary>
/// Allocate space and convert a wide-char string to utf8 string.
/// </summary>
/// <param name="Source">wide-char string</param>
/// <param name="cchLen">length in char, or -1 to get zero-terminated length automatically</param>
/// <param name="cbConvLen">optional, pass out converted length in byte</param>
/// <returns>converted string, free it with HeapFree</returns>
static LPSTR StrWideToUtf8(_In_ LPCWSTR Source, _In_ int cchLen, _Out_opt_ int* cbConvLen)
{
    if (cbConvLen) *cbConvLen = 0;
    SIZE_T cbLen = WideCharToMultiByte(CP_UTF8, 0, Source, cchLen, NULL, 0, 0, 0);
    LPSTR lpBuffer = (LPSTR)HeapAlloc(GetProcessHeap(), 0, cbLen + 1);
    if (!lpBuffer)
        return NULL;

    WideCharToMultiByte(CP_UTF8, 0, Source, cchLen, lpBuffer, cbLen, 0, 0);
    lpBuffer[cbLen] = '\0';
    if (cbConvLen) *cbConvLen = cbLen;
    return lpBuffer;
}

/// <summary>
/// Allocate space and convert utf8 string to a wide-char string.
/// </summary>
/// <param name="Source">utf8 string</param>
/// <param name="cbLen">length in byte, or -1 to get zero-terminated length automatically</param>
/// <param name="cchConvLen">optional, pass out converted length in char</param>
/// <returns>converted string, free it with HeapFree</returns>
static LPWSTR StrUtf8ToWide(_In_ LPCSTR Source, _In_ int cbLen, _Out_opt_ int* cchConvLen)
{
    if (cchConvLen) *cchConvLen = 0;

    SIZE_T cchLen = MultiByteToWideChar(CP_UTF8, 0, Source, cbLen, NULL, 0);
    LPWSTR lpBuffer = (LPWSTR)HeapAlloc(GetProcessHeap(), 0, (cchLen + 1) * sizeof(WCHAR));
    if (!lpBuffer)
        return NULL;

    MultiByteToWideChar(CP_UTF8, 0, Source, cbLen, lpBuffer, cchLen);
    lpBuffer[cchLen] = L'\0';
    if (cchConvLen) *cchConvLen = cchLen;
    return lpBuffer;
}

/// <summary>
/// Stores a information about an async call, and allocate ID for it.
/// </summary>
/// <param name="Type">the type of async call</param>
/// <param name="Callback">callback address provided by user</param>
/// <param name="Context">context provided by user</param>
/// <returns>the allocated ID when success, 0 when failed.</returns>
static INT64 GetAsyncCallID(_In_ ASYNC_CALL_TYPE Type, _In_opt_ LPVOID Callback, _In_opt_ LPVOID Context)
{
    INT64 AllocID = 0;
    AcquireSRWLockExclusive(&AsyncCallListLock);
    __try
    {
        for (int i = 0; i < _countof(AsyncCalls); i++)
        {
            if (!AsyncCalls[i].bUsed)
            {
                AllocID = AsyncCalls[i].ID = ++AsyncCallIDAlloc;
                AsyncCalls[i].Type = Type;
                AsyncCalls[i].Callback = Callback;
                AsyncCalls[i].Context = Context;
                AsyncCalls[i].bUsed = TRUE;
                break;
            }
        }
    }
    __finally
    {
        ReleaseSRWLockExclusive(&AsyncCallListLock);
    }
    return AllocID;
}

/// <summary>
/// Find and remove informations about a async call
/// </summary>
/// <param name="ID">async call ID</param>
/// <param name="pType">returns the type of that async call</param>
/// <param name="pCallback">returns the callback address</param>
/// <param name="pContext">returns the context</param>
/// <returns>return TRUE when success</returns>
static BOOL RemoveAsyncCallID(_In_ INT64 ID, _Out_opt_ ASYNC_CALL_TYPE* pType, _Out_opt_ LPVOID* pCallback, _Out_opt_ LPVOID* pContext)
{
    BOOL bSuccess = FALSE;
    AcquireSRWLockExclusive(&AsyncCallListLock);
    __try
    {
        for (int i = 0; i < _countof(AsyncCalls); i++)
        {
            if (AsyncCalls[i].ID == ID)
            {
                if (pType) *pType = AsyncCalls[i].Type;
                if (pCallback) *pCallback = AsyncCalls[i].Callback;
                if (pContext) *pContext = AsyncCalls[i].Context;

                AsyncCalls[i].bUsed = FALSE;
                bSuccess = TRUE;
                __leave;
            }
        }

        if (pType) *pType = 0;
        if (pCallback) *pCallback = NULL;
        if (pContext) *pContext = NULL;
    }
    __finally
    {
        ReleaseSRWLockExclusive(&AsyncCallListLock);
    }

    return bSuccess;
}

static BOOL DestructMessageBlock(_In_ MESSAGE_BLOCK* pBlock)
{
    switch (pBlock->Type)
    {
    case MB_AT:
    {
        if (pBlock->At.Display)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->At.Display);
        }
        break;
    }
    case MB_ATALL:
    {
        break;
    }
    case MB_FACE:
    {
        break;
    }
    case MB_PLAIN:
    {
        if (pBlock->Plain.Text)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Plain.Text);
        }
        break;
    }
    case MB_IMAGE:
    {
        if (pBlock->Image.ImageIDStr)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Image.ImageIDStr);
        }
        if (pBlock->Image.URL)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Image.URL);
        }
        if (pBlock->Image.ImageType)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Image.ImageType);
        }
        break;
    }
    case MB_VOICE:
    {
        if (pBlock->Voice.VoiceIDStr)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Voice.VoiceIDStr);
        }
        if (pBlock->Voice.URL)
        {
            HeapFree(GetProcessHeap(), 0, pBlock->Voice.URL);
        }
        break;
    }
    case MB_XML:
    {
        break;
    }
    case MB_JSON:
    {
        break;
    }
    case MB_APP:
    {
        break;
    }
    case MB_POKE:
    {
        break;
    }
    case MB_DICE:
    {
        break;
    }
    case MB_MARKETFACE:
    {
        break;
    }
    case MB_MUSICSHARE:
    {
        break;
    }
    case MB_FORWARD:
    {
        break;
    }
    case MB_FILE:
    {
        break;
    }
    default:
        return FALSE;
    }
    return TRUE;
}

static BOOL ReleaseMessageChain(_In_ MESSAGE_CHAIN* pMessageChain)
{
    BOOL bSuccess = TRUE;
    if (pMessageChain->MessageBlocks)
    {
        for (int i = 0; i < pMessageChain->BlockCnt; i++)
        {
            bSuccess &= DestructMessageBlock(pMessageChain->MessageBlocks + i);
        }
        bSuccess &= HeapFree(GetProcessHeap(), 0, pMessageChain->MessageBlocks);
        pMessageChain->MessageBlocks = NULL;
    }
    return bSuccess;
}

static BOOL ConstructMessageBlock(_Out_ MESSAGE_BLOCK *pBlock, _In_ LPCSTR lpType, _In_ yyjson_val *Node)
{
    if (strcmp(lpType, "At") == 0)
    {
        pBlock->Type = MB_AT;
        yyjson_val* TargetField = yyjson_obj_get(Node, "target");
        yyjson_val* DisplayField = yyjson_obj_get(Node, "display");
        if (!TargetField || !yyjson_is_int(TargetField) || !DisplayField || !yyjson_is_str(DisplayField))
            return FALSE;


        LPWSTR CopiedDisplay = StrUtf8ToWide(yyjson_get_str(DisplayField), -1, NULL);
        if (!CopiedDisplay)
            return FALSE;

        pBlock->At.Target = yyjson_get_sint(TargetField);
        pBlock->At.Display = CopiedDisplay;
    }
    else if (strcmp(lpType, "AtAll") == 0)
    {
        pBlock->Type = MB_ATALL;
    }
    else if (strcmp(lpType, "Face") == 0)
    {
        pBlock->Type = MB_FACE;
        yyjson_val* FaceIDField = yyjson_obj_get(Node, "faceId");
        if (!FaceIDField || !yyjson_is_int(FaceIDField))
            return FALSE;

        pBlock->Face.FaceID = yyjson_get_sint(FaceIDField);
    }
    else if (strcmp(lpType, "Plain") == 0)
    {
        pBlock->Type = MB_PLAIN;
        yyjson_val* TextField = yyjson_obj_get(Node, "text");
        if (!TextField || !yyjson_is_str(TextField))
            return FALSE;

        LPWSTR CopiedText = StrUtf8ToWide(yyjson_get_str(TextField), -1, NULL);
        if (!CopiedText)
            return FALSE;

        pBlock->Plain.Text = CopiedText;
    }
    else if (strcmp(lpType, "Image") == 0)
    {
        pBlock->Type = MB_IMAGE;
        yyjson_val* ImageIDField = yyjson_obj_get(Node, "imageId");
        yyjson_val* UrlField = yyjson_obj_get(Node, "url");
        yyjson_val* ImageTypeField = yyjson_obj_get(Node, "imageType");
        yyjson_val* IsEmojiField = yyjson_obj_get(Node, "isEmoji");

        if (!ImageIDField || !yyjson_is_str(ImageIDField) ||
            !UrlField || !yyjson_is_str(UrlField) ||
            !ImageTypeField || !yyjson_is_str(ImageTypeField) ||
            !IsEmojiField || !yyjson_is_bool(IsEmojiField))
            return FALSE;

        LPWSTR CopiedImageID = StrUtf8ToWide(yyjson_get_str(ImageIDField), -1, NULL);
        LPWSTR CopiedUrl = StrUtf8ToWide(yyjson_get_str(UrlField), -1, NULL);
        LPWSTR CopiedImageType = StrUtf8ToWide(yyjson_get_str(ImageTypeField), -1, NULL);
        if (!CopiedImageID || !CopiedUrl || !CopiedImageType)
            return FALSE;

        pBlock->Image.IsFlash = FALSE;
        pBlock->Image.ImageIDStr = CopiedImageID;
        pBlock->Image.URL = CopiedUrl;
        pBlock->Image.ImageType = CopiedImageType;
        pBlock->Image.IsEmoji = (BOOL)yyjson_get_bool(IsEmojiField);
    }
    else if (strcmp(lpType, "FlashImage") == 0)
    {
        pBlock->Type = MB_IMAGE;
        yyjson_val* UrlField = yyjson_obj_get(Node, "url");
        if (!UrlField || !yyjson_is_str(UrlField))
            return FALSE;

        LPWSTR CopiedUrl = StrUtf8ToWide(yyjson_get_str(UrlField), -1, NULL);
        if (!CopiedUrl)
            return FALSE;

        pBlock->Image.IsFlash = TRUE;
        pBlock->Image.URL = CopiedUrl;
    }
    else if (strcmp(lpType, "Voice") == 0)
    {
        pBlock->Type = MB_VOICE;
        yyjson_val* VoiceIDField = yyjson_obj_get(Node, "voiceId");
        yyjson_val* UrlField = yyjson_obj_get(Node, "url");
        yyjson_val* LengthField = yyjson_obj_get(Node, "length");

        if (!VoiceIDField || !yyjson_is_str(VoiceIDField) ||
            !UrlField || !yyjson_is_str(UrlField) ||
            !LengthField || !yyjson_is_int(LengthField))
            return FALSE;

        LPWSTR CopiedVoiceID = StrUtf8ToWide(yyjson_get_str(VoiceIDField), -1, NULL);
        LPWSTR CopiedUrl = StrUtf8ToWide(yyjson_get_str(UrlField), -1, NULL);

        if (!CopiedVoiceID || !CopiedUrl)
            return FALSE;
        
        pBlock->Voice.VoiceIDStr = CopiedVoiceID;
        pBlock->Voice.URL = CopiedUrl;
        pBlock->Voice.Length = yyjson_get_sint(LengthField);
    }
    else if (strcmp(lpType, "Xml") == 0)
    {
        pBlock->Type = MB_XML;
        // TODO: WIP
    }
    else if (strcmp(lpType, "Json") == 0)
    {
        pBlock->Type = MB_JSON;
        // TODO: WIP
    }
    else if (strcmp(lpType, "App") == 0)
    {
        pBlock->Type = MB_APP;
        // TODO: WIP
    }
    else if (strcmp(lpType, "Poke") == 0)
    {
        pBlock->Type = MB_POKE;
        // TODO: WIP
    }
    else if (strcmp(lpType, "Dice") == 0)
    {
        pBlock->Type = MB_DICE;
        // TODO: WIP
    }
    else if (strcmp(lpType, "MarketFace") == 0)
    {
        pBlock->Type = MB_MARKETFACE;
        // TODO: WIP
    }
    else if (strcmp(lpType, "MusicShare") == 0)
    {
        pBlock->Type = MB_MUSICSHARE;
        // TODO: WIP
    }
    else if (strcmp(lpType, "Forward") == 0)
    {
        pBlock->Type = MB_FORWARD;
        // TODO: WIP
    }
    else if (strcmp(lpType, "File") == 0)
    {
        pBlock->Type = MB_FILE;
        // TODO: WIP
    }
    else
    {
        pBlock->Type = 0;
        return FALSE;
    }
    return TRUE;
}

static BOOL UnpackMessageChain(_Out_ MESSAGE_CHAIN* pMessageChain, _In_ yyjson_val *MessageChainNode)
{
    BOOL bSuccess = FALSE;
    pMessageChain->BlockCnt = 0;
    __try
    {
        size_t EnumIndex, MaxNode = yyjson_arr_size(MessageChainNode);
        yyjson_val* EnumNode;

        // atleast one "Source" node.
        if (MaxNode < 1)
            __leave;
        pMessageChain->MessageBlocks = (PMESSAGE_BLOCK)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MESSAGE_BLOCK) * MaxNode);

        if (!pMessageChain->MessageBlocks)
            __leave;

        BOOL bHaveSource = FALSE; // we have to check if source node exists.
        yyjson_arr_foreach(MessageChainNode, EnumIndex, MaxNode, EnumNode) {
            yyjson_val* TypeField = yyjson_obj_get(EnumNode, "type");
            if (!TypeField || !yyjson_is_str(TypeField))
            {
                __leave;
            }
            LPCSTR lpType = yyjson_get_str(TypeField);

            // Handle different type of message block
            // well... I don't think "Source" and "Quote" should be treated as a message block.
            if (strcmp(lpType, "Source") == 0)
            {
                yyjson_val* IDField = yyjson_obj_get(EnumNode, "id");
                yyjson_val* TimeField = yyjson_obj_get(EnumNode, "time");
                if (!IDField || !yyjson_is_int(IDField) || !TimeField || !yyjson_is_int(TimeField))
                {
                    __leave;
                }
                pMessageChain->ID = yyjson_get_sint(IDField);
                pMessageChain->Timestamp = yyjson_get_sint(TimeField);

                bHaveSource = TRUE;
            }
            else if (strcmp(lpType, "Quote") == 0)
            {
                // TODO: WIP
            }
            else
            {
                if (!ConstructMessageBlock(pMessageChain->MessageBlocks + pMessageChain->BlockCnt, lpType, EnumNode))
                {
                    __leave;
                }
                pMessageChain->BlockCnt++;
            }
        }
        if (!bHaveSource)
            __leave;
        bSuccess = TRUE;
    }
    __finally
    {
        if (!bSuccess)
        {
            ReleaseMessageChain(pMessageChain);
        }
    }
    return bSuccess;
}

static BOOL FriendMessageUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    MWS_FRIENDMSGINFO Info = { 0 };
    BOOL bSuccess = FALSE;
    __try
    {
        yyjson_val* MessageChainField = yyjson_obj_get(DataField, "messageChain");
        yyjson_val* SenderField = yyjson_obj_get(DataField, "sender");
        if (!MessageChainField || !yyjson_is_arr(MessageChainField) || !SenderField || !yyjson_is_obj(SenderField))
            __leave;

        yyjson_val* SenderIDField = yyjson_obj_get(SenderField, "id");
        yyjson_val* SenderNickField = yyjson_obj_get(SenderField, "nickname");
        yyjson_val* SenderRemarkField = yyjson_obj_get(SenderField, "remark");

        if (!SenderIDField || !yyjson_is_int(SenderIDField) ||
            !SenderNickField || !yyjson_is_str(SenderNickField) ||
            !SenderRemarkField || !yyjson_is_str(SenderRemarkField))
            __leave;


        if (!UnpackMessageChain(&Info.MessageChain, MessageChainField))
            __leave;

        Info.Sender.ID = yyjson_get_sint(SenderIDField);
        Info.Sender.Nick = StrUtf8ToWide(yyjson_get_str(SenderNickField), -1, NULL);
        Info.Sender.Remark = StrUtf8ToWide(yyjson_get_str(SenderRemarkField), -1, NULL);

        if (!Info.Sender.Nick || !Info.Sender.Remark)
            __leave;

        pMiraiWS->Callback(pMiraiWS, MWS_FRIENDMSG, &Info);
        bSuccess = TRUE;
    }
    __finally
    {
        if (Info.Sender.Nick) HeapFree(GetProcessHeap(), 0, Info.Sender.Nick);
        if (Info.Sender.Remark) HeapFree(GetProcessHeap(), 0, Info.Sender.Remark);
        ReleaseMessageChain(&Info.MessageChain);
    }

    return bSuccess;
}

static BOOL GroupMessageUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    MWS_GROUPMSGINFO Info = { 0 };
    BOOL bSuccess = FALSE;
    __try
    {
        yyjson_val* MessageChainField = yyjson_obj_get(DataField, "messageChain");
        yyjson_val* SenderField = yyjson_obj_get(DataField, "sender");
        if (!MessageChainField || !yyjson_is_arr(MessageChainField) || !SenderField || !yyjson_is_obj(SenderField))
            __leave;

        yyjson_val* SenderIDField = yyjson_obj_get(SenderField, "id");
        yyjson_val* SenderMemberNameField = yyjson_obj_get(SenderField, "memberName");
        yyjson_val* SenderSpecialTitleField = yyjson_obj_get(SenderField, "specialTitle");
        yyjson_val* SenderPermissionField = yyjson_obj_get(SenderField, "permission");
        yyjson_val* SenderJoinTimeField = yyjson_obj_get(SenderField, "joinTimestamp");
        yyjson_val* SenderLastSpeakTimeField = yyjson_obj_get(SenderField, "lastSpeakTimestamp");
        yyjson_val* SenderMuteTimeRemainField = yyjson_obj_get(SenderField, "muteTimeRemaining");
        yyjson_val* SenderGroupField = yyjson_obj_get(SenderField, "group");

        if (!SenderIDField || !yyjson_is_int(SenderIDField) ||
            !SenderMemberNameField || !yyjson_is_str(SenderMemberNameField) ||
            !SenderSpecialTitleField || !yyjson_is_str(SenderSpecialTitleField) ||
            !SenderPermissionField || !yyjson_is_str(SenderPermissionField) ||
            !SenderJoinTimeField || !yyjson_is_int(SenderJoinTimeField) ||
            !SenderLastSpeakTimeField || !yyjson_is_int(SenderLastSpeakTimeField) ||
            !SenderMuteTimeRemainField || !yyjson_is_int(SenderMuteTimeRemainField) ||
            !SenderGroupField || !yyjson_is_obj(SenderGroupField))
            __leave;

        yyjson_val* GroupIDField = yyjson_obj_get(SenderGroupField, "id");
        yyjson_val* GroupNameField = yyjson_obj_get(SenderGroupField, "name");
        yyjson_val* GroupPermissionField = yyjson_obj_get(SenderGroupField, "permission");

        if (!GroupIDField || !yyjson_is_int(GroupIDField) ||
            !GroupNameField || !yyjson_is_str(GroupNameField) ||
            !GroupPermissionField || !yyjson_is_str(GroupPermissionField))
            __leave;


        if (!UnpackMessageChain(&Info.MessageChain, MessageChainField))
            __leave;

        Info.Sender.ID = yyjson_get_sint(SenderIDField);
        Info.Sender.MemberName = StrUtf8ToWide(yyjson_get_str(SenderMemberNameField), -1, NULL);
        Info.Sender.SpecialTitle = StrUtf8ToWide(yyjson_get_str(SenderSpecialTitleField), -1, NULL);
        Info.Sender.Permission = StrUtf8ToWide(yyjson_get_str(SenderPermissionField), -1, NULL);
        Info.Sender.JoinTimestamp = yyjson_get_sint(SenderJoinTimeField);
        Info.Sender.LastSpeakTimestamp = yyjson_get_sint(SenderLastSpeakTimeField);
        Info.Sender.MuteTimeRemaining = yyjson_get_sint(SenderMuteTimeRemainField);
        Info.Sender.Group.ID = yyjson_get_sint(GroupIDField);
        Info.Sender.Group.Name = StrUtf8ToWide(yyjson_get_str(GroupNameField), -1, NULL);
        Info.Sender.Group.Permission = StrUtf8ToWide(yyjson_get_str(GroupPermissionField), -1, NULL);

        if (!Info.Sender.MemberName ||
            !Info.Sender.SpecialTitle ||
            !Info.Sender.Permission ||
            !Info.Sender.Group.Name ||
            !Info.Sender.Group.Permission)
            __leave;

        pMiraiWS->Callback(pMiraiWS, MWS_GROUPMSG, &Info);
        bSuccess = TRUE;
    }
    __finally
    {
        if (Info.Sender.MemberName)       HeapFree(GetProcessHeap(), 0, Info.Sender.MemberName);
        if (Info.Sender.SpecialTitle)     HeapFree(GetProcessHeap(), 0, Info.Sender.SpecialTitle);
        if (Info.Sender.Permission)       HeapFree(GetProcessHeap(), 0, Info.Sender.Permission);
        if (Info.Sender.Group.Name)       HeapFree(GetProcessHeap(), 0, Info.Sender.Group.Name);
        if (Info.Sender.Group.Permission) HeapFree(GetProcessHeap(), 0, Info.Sender.Group.Permission);
        ReleaseMessageChain(&Info.MessageChain);
    }
    return bSuccess;
}

static BOOL TempMessageUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    return TRUE;
}

static BOOL StrangerMessageUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    return TRUE;
}

static BOOL OtherClientMessageUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    return TRUE;
}

static void CallBadMsgCallback(_In_ PMIRAI_WS pMiraiWS)
{
    int cchLen;
    LPWSTR wMessage = StrUtf8ToWide(pMiraiWS->Buffer, pMiraiWS->RecvLength, &cchLen);
    if (!wMessage)
        return;

    MWS_BADMSGINFO Info = { wMessage, cchLen };
    pMiraiWS->Callback(pMiraiWS, MWS_BADMSG, &Info);
    HeapFree(GetProcessHeap(), 0, wMessage);
}

static void CallAuthCallback(_In_ PMIRAI_WS pMiraiWS, _In_ INT64 ResponseCode, _In_opt_z_ LPCSTR lpSession, _In_opt_z_ LPCSTR lpMessage)
{
    LPWSTR wSession = lpSession ? StrUtf8ToWide(lpSession, -1, NULL) : NULL;
    LPWSTR wMessage = lpMessage ? StrUtf8ToWide(lpMessage, -1, NULL) : NULL;
    MWS_AUTHINFO Info = { ResponseCode, wSession, wMessage };
    
    pMiraiWS->Callback(pMiraiWS, MWS_AUTH, &Info);

    if (wSession) HeapFree(GetProcessHeap(), 0, wSession);
    if (wMessage) HeapFree(GetProcessHeap(), 0, wMessage);
}

static BOOL EventsUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ yyjson_val* DataField)
{
    yyjson_val* TypeField = yyjson_obj_get(DataField, "type");
    if (!TypeField || !yyjson_is_str(TypeField))
    {
        CallBadMsgCallback(pMiraiWS);
        return FALSE;
    }

    // TODO: Use something to optimize this, perhaps trie tree?
    LPCSTR TypeList[] = 
    { "FriendMessage",       "GroupMessage",       "TempMessage",       "StrangerMessage",       "OtherClientMessage" };
    EVENTHANDLER EventPackerList[] = 
    { FriendMessageUnpacker, GroupMessageUnpacker, TempMessageUnpacker, StrangerMessageUnpacker, OtherClientMessageUnpacker };

    const LPCSTR szType = unsafe_yyjson_get_str(TypeField);
    for (SIZE_T i = 0; i < _countof(TypeList); i++)
    {
        if (strcmp(szType, TypeList[i]) == 0)
        {
            if (!EventPackerList[i](pMiraiWS, DataField))
            {
                CallBadMsgCallback(pMiraiWS);
                return FALSE;
            }

            return TRUE;
        }
    }
    return TRUE;
}

static BOOL CallbacksUnpacker(_In_ PMIRAI_WS pMiraiWS, _In_ INT64 ID, _In_ yyjson_val* DataField)
{
    ASYNC_CALL_TYPE Type = 0;
    LPVOID Callback;
    LPVOID Context;

    if (!RemoveAsyncCallID(ID, &Type, &Callback, &Context))
    {
        CallBadMsgCallback(pMiraiWS);
        return FALSE;
    }

    switch (Type)
    {
    case ASYNC_FRIENDMSG:
    case ASYNC_GROUPMSG:
    {
        yyjson_val* CodeField = yyjson_obj_get(DataField, "code");
        yyjson_val* MsgField = yyjson_obj_get(DataField, "msg");
        yyjson_val* MsgIDField = yyjson_obj_get(DataField, "messageId");

        if (!CodeField || !yyjson_is_int(CodeField) ||
            !MsgField || !yyjson_is_str(MsgField))
            return FALSE;

        if(MsgIDField && !yyjson_is_int(MsgIDField))
            return FALSE;

        INT64 Code = yyjson_get_sint(CodeField);
        LPWSTR lpMsg = StrUtf8ToWide(yyjson_get_str(MsgField), -1, NULL);
        INT64 MsgID = MsgIDField ? yyjson_get_sint(MsgIDField) : 0;

        if (Callback) ((SEND_MSG_CALLBACK)Callback)(pMiraiWS, Code, lpMsg, MsgID, Context);

        HeapFree(GetProcessHeap(), 0, lpMsg);
        return TRUE;
    }
    default:
    {
        DebugBreak();
        return FALSE;
    }
    }
}

static void HandleJsonMessage(_In_ PMIRAI_WS pMiraiWS)
{
    yyjson_doc* JsonDoc = yyjson_read(pMiraiWS->Buffer, pMiraiWS->RecvLength, 0);
    if (!JsonDoc)
    {
        CallBadMsgCallback(pMiraiWS);
        return;
    }
    __try
    {
        yyjson_val* JsonRoot = yyjson_doc_get_root(JsonDoc);
        yyjson_val* SyncIDField = yyjson_obj_get(JsonRoot, "syncId");
        yyjson_val* DataField = yyjson_obj_get(JsonRoot, "data");

        if (!SyncIDField || !DataField)
        {
            CallBadMsgCallback(pMiraiWS);
            __leave;
        }
        const char* szSyncID = yyjson_get_str(SyncIDField);

        if (!szSyncID)
        {
            CallBadMsgCallback(pMiraiWS);
            __leave;
        }

        int FieldLen = strlen(szSyncID);
        if (FieldLen == 0)
        {
            // Seems to be the first message passing session key. check the code.
            yyjson_val* CodeField = yyjson_obj_get(DataField, "code");
            yyjson_val* SessionField = yyjson_obj_get(DataField, "session");
            yyjson_val* MessageField = yyjson_obj_get(DataField, "msg");
            if (!CodeField || !yyjson_is_int(CodeField))
            {
                CallBadMsgCallback(pMiraiWS);
                __leave;
            }
            INT64 ResponseCode = unsafe_yyjson_get_sint(CodeField);

            CallAuthCallback(pMiraiWS, ResponseCode, SessionField ? yyjson_get_str(SessionField) : NULL, MessageField ? yyjson_get_str(MessageField) : NULL);
        }
        else
        {
            INT64 ID = atoll(szSyncID);
            if (ID == RESERVED_SYNC_ID)
            {
                // events sent by server
                if (!EventsUnpacker(pMiraiWS, DataField))
                {
                    CallBadMsgCallback(pMiraiWS);
                    __leave;
                }
            }
            else
            {
                // responding requests sent by client

                if (!CallbacksUnpacker(pMiraiWS, ID, DataField))
                {
                    CallBadMsgCallback(pMiraiWS);
                    __leave;
                }
            }
        }
    }
    __finally
    {
        yyjson_doc_free(JsonDoc);
    }
}

static void CleanUpMiraiWSAsync(_In_ PMIRAI_WS pMiraiWS)
{
    if (pMiraiWS->hRequestHandle != NULL)
    {
        WinHttpCloseHandle(pMiraiWS->hRequestHandle); // will be set to NULL when WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING callback is called
    }

    if (pMiraiWS->hWebSocketHandle != NULL)
    {
        WinHttpCloseHandle(pMiraiWS->hWebSocketHandle); // will be set to NULL when WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING callback is called
    }

    if (pMiraiWS->hConnectionHandle != NULL)
    {
        WinHttpCloseHandle(pMiraiWS->hConnectionHandle);
        pMiraiWS->hConnectionHandle = NULL;
    }

    if (pMiraiWS->hSessionHandle != NULL)
    {
        WinHttpCloseHandle(pMiraiWS->hSessionHandle);
        pMiraiWS->hSessionHandle = NULL;
    }
}

static void CALLBACK WinHttpStatusCallback(
    _In_ HINTERNET hInternet,
    _In_ DWORD_PTR dwContext,
    _In_ DWORD dwInternetStatus,
    _In_ LPVOID lpvStatusInformation,
    _In_ DWORD dwStatusInformationLength
)
{
    PMIRAI_WS pMiraiWS = (PMIRAI_WS)dwContext;
    switch (dwInternetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        // WinHttpSendRequest successed.
        if (!WinHttpReceiveResponse(pMiraiWS->hRequestHandle, NULL))
        {
            MWS_CONNECTINFO Info = { FALSE, GetLastError() };
            pMiraiWS->Callback(pMiraiWS, MWS_CONNECT, &Info);
        }
        break;

    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        // WinHttpReceiveResponse successed.
        pMiraiWS->hWebSocketHandle = WinHttpWebSocketCompleteUpgrade(pMiraiWS->hRequestHandle, (DWORD_PTR)pMiraiWS);
        if (!pMiraiWS->hWebSocketHandle)
        {
            MWS_CONNECTINFO Info = { FALSE, GetLastError() };
            pMiraiWS->Callback(pMiraiWS, MWS_CONNECT, &Info);
        }
        else
        {
            // connection established.
            MWS_CONNECTINFO Info = { TRUE, NO_ERROR };
            pMiraiWS->Callback(pMiraiWS, MWS_CONNECT, &Info);

            // start receiving data.
            DWORD RecvLen;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType;
            DWORD dwRet = WinHttpWebSocketReceive(
                pMiraiWS->hWebSocketHandle,
                pMiraiWS->Buffer,
                sizeof(pMiraiWS->Buffer),
                &RecvLen,
                &eBufferType);
            if (dwRet != NO_ERROR)
            {
                MWS_NWERRORINFO Info = { dwRet };
                pMiraiWS->Callback(pMiraiWS, MWS_NWERROR, &Info);
                CleanUpMiraiWSAsync(pMiraiWS);
            }
        }
        break;
    
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    {
        WINHTTP_WEB_SOCKET_STATUS* pWebSockData = (WINHTTP_WEB_SOCKET_STATUS*)lpvStatusInformation;
        if (pWebSockData->eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            pWebSockData->eBufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
        {
            pMiraiWS->RecvLength += pWebSockData->dwBytesTransferred;

            if (pWebSockData->eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            {
                HandleJsonMessage(pMiraiWS);
                pMiraiWS->RecvLength = 0;
            }

            // start a new recv
            DWORD RecvLen;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType;
            if (pMiraiWS->RecvLength >= sizeof(pMiraiWS->Buffer))
            {
                // TODO: assert here
                DebugBreak();
            }
            DWORD dwRet = WinHttpWebSocketReceive(
                pMiraiWS->hWebSocketHandle,
                pMiraiWS->Buffer + pMiraiWS->RecvLength,
                sizeof(pMiraiWS->Buffer) - pMiraiWS->RecvLength,
                &RecvLen,
                &eBufferType);
            if (dwRet != NO_ERROR)
            {
                MWS_NWERRORINFO Info = { dwRet };
                pMiraiWS->Callback(pMiraiWS, MWS_NWERROR, &Info);
                CleanUpMiraiWSAsync(pMiraiWS);
            }
        }
        Sleep(0);
        break;
    }

    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        WINHTTP_ASYNC_RESULT* pResult = lpvStatusInformation;
        switch (pResult->dwResult)
        {
        case API_SEND_REQUEST:
        case API_RECEIVE_RESPONSE:
        {
            MWS_CONNECTINFO Info = { FALSE, pResult->dwError };
            pMiraiWS->Callback(pMiraiWS, MWS_CONNECT, &Info);
            break;
        }
        default:
        {
            MWS_NWERRORINFO Info = { pResult->dwError };
            pMiraiWS->Callback(pMiraiWS, MWS_NWERROR, &Info);
            break;
        }
        }
        CleanUpMiraiWSAsync(pMiraiWS);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
    {
        if (pMiraiWS && pMiraiWS->bClose)
        {
            if (pMiraiWS->hRequestHandle == hInternet)
            {
                pMiraiWS->hRequestHandle = 0;
            }
            if (pMiraiWS->hWebSocketHandle == hInternet)
            {
                pMiraiWS->hWebSocketHandle = 0;
            }
            if (pMiraiWS->hRequestHandle == 0 && pMiraiWS->hWebSocketHandle == 0)
            {
                // all clear, no one should have pMiraiWS in hand now.
                if (pMiraiWS->lpServerName)
                {
                    HeapFree(GetProcessHeap(), 0, pMiraiWS->lpServerName);
                }
                HeapFree(GetProcessHeap(), 0, pMiraiWS);
            }
        }
        break;
    }
    }
}

_Ret_maybenull_
PMIRAI_WS CreateMiraiWS(_In_z_ LPCWSTR lpServerName, _In_ INTERNET_PORT Port, _In_ BOOL bSecure, _In_ MWSCALLBACK Callback)
{
    BOOL bSuccess = FALSE;
    PMIRAI_WS pMiraiWS = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MIRAI_WS));
    if (!pMiraiWS)
        return FALSE;

    __try
    {
        int cchLen = (int)wcslen(lpServerName);
        int cchConvertLen = IdnToAscii(0, lpServerName, cchLen, NULL, 0);

        // WinHttpConnect needs to convert hostname into punny code, we convert it here.
        pMiraiWS->lpServerName = (LPWSTR)HeapAlloc(GetProcessHeap(), 0, (cchConvertLen + 1) * sizeof(WCHAR));

        if (!pMiraiWS->lpServerName)
            __leave;
        IdnToAscii(0, lpServerName, cchLen, pMiraiWS->lpServerName, cchConvertLen);
        pMiraiWS->lpServerName[cchConvertLen] = L'\0';

        pMiraiWS->Port     = Port;
        pMiraiWS->bSecure  = bSecure;
        pMiraiWS->Callback = Callback;
        bSuccess = TRUE;
    }
    __finally
    {
        if (!bSuccess)
        {
            if (pMiraiWS->lpServerName)
                HeapFree(GetProcessHeap(), 0, pMiraiWS->lpServerName);

            HeapFree(GetProcessHeap(), 0, pMiraiWS);
            pMiraiWS = NULL;
        }
    }

    return pMiraiWS;
}

BOOL ConnectMiraiWS(_Inout_ PMIRAI_WS pMiraiWS, _In_z_ LPCWSTR szVerifyKey, _In_z_ LPCWSTR szQQ)
{
    BOOL bSuccess = FALSE;
    LPWSTR szHeaderStr = NULL;
    __try
    {
        DWORD dwHttpOpenFlag = pMiraiWS->bSecure ? WINHTTP_FLAG_SECURE_DEFAULTS : WINHTTP_FLAG_ASYNC; // WINHTTP_FLAG_SECURE_DEFAULTS also forces WINHTTP_FLAG_ASYNC

        pMiraiWS->hSessionHandle = WinHttpOpen(
            L"Mirai WS",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            dwHttpOpenFlag);
        if (!pMiraiWS->hSessionHandle)
            __leave;
        
        // We're going to use WinHttp Async mode, so we need to set a callback.
        // request will inherit the callback from session.
        WinHttpSetStatusCallback(pMiraiWS->hSessionHandle, WinHttpStatusCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);

        pMiraiWS->hConnectionHandle = WinHttpConnect(
            pMiraiWS->hSessionHandle,
            pMiraiWS->lpServerName,
            pMiraiWS->Port, 0);
        if (!pMiraiWS->hConnectionHandle)
            __leave;

        pMiraiWS->hRequestHandle = WinHttpOpenRequest(
            pMiraiWS->hConnectionHandle,
            L"GET",
            L"/all", // for pushing messages and events. see mirai-api-http websocket adapter docs for more detail.
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            pMiraiWS->bSecure ? WINHTTP_FLAG_SECURE : 0);
        if (!pMiraiWS->hRequestHandle)
            __leave;

        if (!WinHttpSetOption(pMiraiWS->hRequestHandle,
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
            NULL,
            0))
            __leave;

        // format the header string.

        DWORD_PTR FormatArgs[2] = { (DWORD_PTR)szVerifyKey, (DWORD_PTR)szQQ };
        DWORD cchHeaderStr = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
            L"verifyKey: %1\r\nqq: %2\r\n",
            0, 0, (LPWSTR)&szHeaderStr, 0, (va_list *)FormatArgs);
        if (cchHeaderStr == 0)
            __leave;

        if (!WinHttpSendRequest(pMiraiWS->hRequestHandle, szHeaderStr, cchHeaderStr, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR)pMiraiWS))
            __leave;

        bSuccess = TRUE;
    }
    __finally
    {
        if(szHeaderStr)
            LocalFree(szHeaderStr);

        if (!bSuccess)
        {
            CleanUpMiraiWSAsync(pMiraiWS);
        }
    }

    return bSuccess;
}

BOOL DestroyMiraiWSAsync(_In_ _Frees_ptr_ PMIRAI_WS pMiraiWS)
{
    pMiraiWS->bClose = TRUE;

    CleanUpMiraiWSAsync(pMiraiWS);

    return TRUE;
}

_Success_(return)
static BOOL CreateWebsockAdapterJson(_In_ INT64 SyncID, _In_z_ LPCSTR Command, _In_opt_z_ LPCSTR SubCommand, _Outptr_result_nullonfailure_ yyjson_mut_doc **pDoc, _Outptr_result_nullonfailure_ yyjson_mut_val **pContent)
{
    BOOL bSuccess = FALSE;
    yyjson_mut_doc* Doc = NULL;
    yyjson_mut_val* Root = NULL;
    yyjson_mut_val* Content = NULL;

    __try
    {
        Doc = yyjson_mut_doc_new(NULL);
        if (!Doc) __leave;
        Root = yyjson_mut_obj(Doc);
        if (!Root) __leave;
        yyjson_mut_doc_set_root(Doc, Root);
        yyjson_mut_obj_add_int(Doc, Root, "syncId", SyncID);
        yyjson_mut_obj_add_str(Doc, Root, "command", Command);

        if (SubCommand)
            yyjson_mut_obj_add_str(Doc, Root, "subCommand", Command);
        else
            yyjson_mut_obj_add_null(Doc, Root, "subCommand");

        Content = yyjson_mut_obj(Doc);
        if (!Content)
            __leave;
        yyjson_mut_obj_add_val(Doc, Root, "content", Content);
        bSuccess = TRUE;
    }
    __finally
    {
        if (!bSuccess)
        {
            yyjson_mut_doc_free(Doc);
            Doc = NULL;
            Content = NULL;
        }
    }

    *pDoc = Doc;
    *pContent = Content;

    return bSuccess;
}

static yyjson_mut_val* GetMessageChainJson(_In_ yyjson_mut_doc *Doc, _In_ MESSAGE_CHAIN* pMessageChain)
{
    yyjson_mut_val* MsgChain = yyjson_mut_arr(Doc);
    if (!MsgChain)
        return NULL;
    for (int i = 0; i < pMessageChain->BlockCnt; i++)
    {
        yyjson_mut_val* MsgBlockNode = yyjson_mut_obj(Doc);

        if (!MsgBlockNode)
            return NULL;

        switch (pMessageChain->MessageBlocks[i].Type)
        {
        case MB_AT:
        {
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", "At");
            yyjson_mut_obj_add_int(Doc, MsgBlockNode, "target", pMessageChain->MessageBlocks[i].At.Target);
            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }
        case MB_ATALL:
        {
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", "AtAll");

            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }
        case MB_FACE:
        {
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", "Face");
            yyjson_mut_obj_add_int(Doc, MsgBlockNode, "faceId", pMessageChain->MessageBlocks[i].Face.FaceID);

            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }
        case MB_PLAIN:
        {
            LPSTR lpText = StrWideToUtf8(pMessageChain->MessageBlocks[i].Plain.Text, -1, NULL);
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", "Plain");
            yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "text", lpText);
            HeapFree(GetProcessHeap(), 0, lpText);

            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }
        case MB_IMAGE:
        {
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", pMessageChain->MessageBlocks[i].Image.IsFlash ? "FlashImage" : "Image");
            if (pMessageChain->MessageBlocks[i].Image.ImageIDStr)
            {
                LPSTR lpImageID = StrWideToUtf8(pMessageChain->MessageBlocks[i].Image.ImageIDStr, -1, NULL);
                yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "imageId", lpImageID);
                HeapFree(GetProcessHeap(), 0, lpImageID);
            }
            if (pMessageChain->MessageBlocks[i].Image.URL)
            {
                LPSTR lpURL = StrWideToUtf8(pMessageChain->MessageBlocks[i].Image.URL, -1, NULL);
                yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "url", lpURL);
                HeapFree(GetProcessHeap(), 0, lpURL);
            }
            if (pMessageChain->MessageBlocks[i].Image.ImageType)
            {
                LPSTR lpImageType = StrWideToUtf8(pMessageChain->MessageBlocks[i].Image.ImageType, -1, NULL);
                yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "imageType", lpImageType);
                HeapFree(GetProcessHeap(), 0, lpImageType);
            }

            yyjson_mut_obj_add_bool(Doc, MsgBlockNode, "isEmoji", (bool)pMessageChain->MessageBlocks[i].Image.IsEmoji);
            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }    
        case MB_VOICE:
        {
            yyjson_mut_obj_add_str(Doc, MsgBlockNode, "type", "Voice");
            if (pMessageChain->MessageBlocks[i].Voice.VoiceIDStr)
            {
                LPSTR lpVoiceID = StrWideToUtf8(pMessageChain->MessageBlocks[i].Voice.VoiceIDStr, -1, NULL);
                yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "voiceId", lpVoiceID);
                HeapFree(GetProcessHeap(), 0, lpVoiceID);
            }
            if (pMessageChain->MessageBlocks[i].Voice.URL)
            {
                LPSTR lpURL = StrWideToUtf8(pMessageChain->MessageBlocks[i].Voice.URL, -1, NULL);
                yyjson_mut_obj_add_strcpy(Doc, MsgBlockNode, "url", lpURL);
                HeapFree(GetProcessHeap(), 0, lpURL);
            }
            yyjson_mut_arr_append(MsgChain, MsgBlockNode);
            break;
        }
        case MB_XML:
        {
            break;
        }
        case MB_JSON:
        {
            break;
        }
        case MB_APP:
        {
            break;
        }
        case MB_POKE:
        {
            break;
        }
        case MB_DICE:
        {
            break;
        }
        case MB_MARKETFACE:
        {
            break;
        }
        case MB_MUSICSHARE:
        {
            break;
        }
        case MB_FORWARD:
        {
            break;
        }
        case MB_FILE:
        {
            break;
        }
        default:
        {
            // Assert here
            DebugBreak();
            break;
        }
        }

    }
    return MsgChain;
}

BOOL SendFriendMsgAsync(
    _In_ PMIRAI_WS pMiraiWS,
    _In_ INT64 Target,
    _In_ MESSAGE_CHAIN* pMessageChain,
    _In_opt_ SEND_MSG_CALLBACK Callback,
    _In_opt_ LPVOID Context)
{
    BOOL bSuccess = FALSE;
    INT64 AsyncID = GetAsyncCallID(ASYNC_FRIENDMSG, Callback, Context);
    if (!AsyncID)
        return FALSE;

    yyjson_mut_doc* Doc = NULL;
    yyjson_mut_val* Content = NULL;
    LPSTR lpJsonText = NULL;
    __try
    {
        if (!CreateWebsockAdapterJson(AsyncID, "sendFriendMessage", NULL, &Doc, &Content))
            __leave;

        yyjson_mut_obj_add_int(Doc, Content, "target", Target);

        yyjson_mut_val* MsgChain = GetMessageChainJson(Doc, pMessageChain);

        yyjson_mut_obj_add_val(Doc, Content, "messageChain", MsgChain);

        SIZE_T JsonLen;
        lpJsonText = yyjson_mut_write(Doc, 0, &JsonLen);
        if (!lpJsonText)
            __leave;

        if (WinHttpWebSocketSend(pMiraiWS->hWebSocketHandle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)lpJsonText, (DWORD)JsonLen) != NO_ERROR)
            __leave;

        bSuccess = TRUE;
    }
    __finally
    {
        if (lpJsonText)
        {
            free(lpJsonText);
        }
        if (!bSuccess)
        {
            RemoveAsyncCallID(AsyncID, NULL, NULL, NULL);
        }
        if (Doc) yyjson_mut_doc_free(Doc);
    }
    return bSuccess;
}

BOOL SendGroupMsgAsync(
    _In_ PMIRAI_WS pMiraiWS,
    _In_ INT64 Target,
    _In_ MESSAGE_CHAIN* pMessageChain,
    _In_opt_ SEND_MSG_CALLBACK Callback,
    _In_opt_ LPVOID Context
)
{
    BOOL bSuccess = FALSE;
    INT64 AsyncID = GetAsyncCallID(ASYNC_GROUPMSG, Callback, Context);
    if (!AsyncID)
        return FALSE;

    yyjson_mut_doc* Doc = NULL;
    yyjson_mut_val* Content = NULL;
    LPSTR lpJsonText = NULL;
    __try
    {
        if (!CreateWebsockAdapterJson(AsyncID, "sendGroupMessage", NULL, &Doc, &Content))
            __leave;

        yyjson_mut_obj_add_int(Doc, Content, "target", Target);

        yyjson_mut_val* MsgChain = GetMessageChainJson(Doc, pMessageChain);

        yyjson_mut_obj_add_val(Doc, Content, "messageChain", MsgChain);

        SIZE_T JsonLen;
        lpJsonText = yyjson_mut_write(Doc, 0, &JsonLen);
        if (!lpJsonText)
            __leave;

        if (WinHttpWebSocketSend(pMiraiWS->hWebSocketHandle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)lpJsonText, (DWORD)JsonLen) != NO_ERROR)
            __leave;

        bSuccess = TRUE;
    }
    __finally
    {
        if (lpJsonText)
        {
            free(lpJsonText);
        }
        if (!bSuccess)
        {
            RemoveAsyncCallID(AsyncID, NULL, NULL, NULL);
        }
        if (Doc) yyjson_mut_doc_free(Doc);
    }
    return bSuccess;
}
