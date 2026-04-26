#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_CLIENTS 50
#define WM_ADD_CLIENT (WM_USER + 1)
#define WM_REMOVE_CLIENT (WM_USER + 2)
#define WM_NEW_MESSAGE (WM_USER + 3)
#define WM_NEW_FILE (WM_USER + 4)

#define MSG_TEXT 0
#define MSG_FILE 1
#define MSG_NAME 2
#define MSG_JOIN 3
#define MSG_LEAVE 4
#define MSG_PRIVATE 5
#define MSG_ERROR 6

#pragma pack(push, 1)
typedef struct
{
    uint8_t type;
    uint32_t length;
    char filename[64];
    char target[64];
    char sender[64]; // اسم المرسل
} MsgHeader;
#pragma pack(pop)

typedef struct
{
    SOCKET sock;
    char name[64];
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
SOCKET server_socket;
HWND hChat, hInput, hClients, hwnd_global;
CRITICAL_SECTION cs;

int send_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0)
            return -1;
        sent += r;
    }
    return sent;
}

int recv_all(SOCKET s, char *buf, int len)
{
    int got = 0;
    while (got < len)
    {
        int r = recv(s, buf + got, len - got, 0);
        if (r <= 0)
            return 0;
        got += r;
    }
    return got;
}

void broadcast(SOCKET from, MsgHeader *hdr, const char *data)
{
    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++)
        if (clients[i].sock != from)
        {
            send_all(clients[i].sock, (char *)hdr, sizeof(MsgHeader));
            send_all(clients[i].sock, data, (int)hdr->length);
        }
    LeaveCriticalSection(&cs);
}

void send_private(SOCKET from, MsgHeader *hdr, const char *data)
{
    EnterCriticalSection(&cs);
    int found = 0;
    for (int i = 0; i < client_count; i++)
    {
        if (_stricmp(clients[i].name, hdr->target) == 0 && clients[i].sock != from)
        {
            send_all(clients[i].sock, (char *)hdr, sizeof(MsgHeader));
            send_all(clients[i].sock, data, (int)hdr->length);
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&cs);

    if (!found)
    {
        char notice[128];
        snprintf(notice, sizeof(notice), "User '%s' is not online or does not exist.", hdr->target);
        MsgHeader nh = {0};
        nh.type = MSG_ERROR;
        nh.length = (uint32_t)strlen(notice);
        send_all(from, (char *)&nh, sizeof(MsgHeader));
        send_all(from, notice, (int)nh.length);
    }
}

void AppendChat(const char *line)
{
    char old[16384];
    GetWindowText(hChat, old, sizeof(old));
    if (strlen(old) + strlen(line) + 3 < sizeof(old))
    {
        strcat(old, line);
        strcat(old, "\r\n");
    }
    SetWindowText(hChat, old);
    SendMessage(hChat, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessage(hChat, EM_SCROLLCARET, 0, 0);
}

DWORD WINAPI ReceiveThread(LPVOID arg)
{
    SOCKET sock = *(SOCKET *)arg;
    free(arg);

    char clientName[64] = "Unknown";

    MsgHeader nameHdr;
    if (recv_all(sock, (char *)&nameHdr, sizeof(MsgHeader)) && nameHdr.type == MSG_NAME)
    {
        recv_all(sock, clientName, (int)nameHdr.length);
        clientName[nameHdr.length] = '\0';
    }

    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++)
        if (clients[i].sock == sock)
        {
            strncpy(clients[i].name, clientName, 63);
            break;
        }
    LeaveCriticalSection(&cs);

    PostMessage(hwnd_global, WM_ADD_CLIENT, (WPARAM)sock, (LPARAM)_strdup(clientName));

    MsgHeader joinHdr = {0};
    joinHdr.type = MSG_JOIN;
    joinHdr.length = (uint32_t)strlen(clientName);
    broadcast(sock, &joinHdr, clientName);

    while (1)
    {
        MsgHeader hdr;
        if (!recv_all(sock, (char *)&hdr, sizeof(MsgHeader)))
            break;

        char *data = (char *)malloc(hdr.length + 1);
        if (!data)
            break;
        if (!recv_all(sock, data, (int)hdr.length))
        {
            free(data);
            break;
        }
        data[hdr.length] = '\0';

        if (hdr.type == MSG_TEXT)
        {
            broadcast(sock, &hdr, data);
            PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(data), 0);
        }
        else if (hdr.type == MSG_PRIVATE)
        {
            send_private(sock, &hdr, data);
            char line[200];
            snprintf(line, sizeof(line), "[Private %s -> %s] %s", clientName, hdr.target, data);
            PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(line), 0);
        }
        else if (hdr.type == MSG_FILE)
        {
            char dir[MAX_PATH], fpath[MAX_PATH];
            GetTempPath(MAX_PATH, dir);
            snprintf(fpath, MAX_PATH, "%s%s", dir, hdr.filename);

            HANDLE f = CreateFile(fpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD w;
                WriteFile(f, data, hdr.length, &w, NULL);
                CloseHandle(f);
            }

            // حط اسم المرسل في الهيدر قبل الإرسال
            strncpy(hdr.sender, clientName, 63);

            if (strlen(hdr.target) > 0)
            {
                send_private(sock, &hdr, data);
                char line[200];
                snprintf(line, sizeof(line), "[Private file %s -> %s]: %s", clientName, hdr.target, hdr.filename);
                PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(line), 0);
            }
            else
            {
                broadcast(sock, &hdr, data);
                PostMessage(hwnd_global, WM_NEW_FILE, (WPARAM)_strdup(hdr.filename), 0);
            }
        }
        free(data);
    }

    MsgHeader leaveHdr = {0};
    leaveHdr.type = MSG_LEAVE;
    leaveHdr.length = (uint32_t)strlen(clientName);
    broadcast(sock, &leaveHdr, clientName);

    PostMessage(hwnd_global, WM_REMOVE_CLIENT, (WPARAM)sock, (LPARAM)_strdup(clientName));

    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++)
        if (clients[i].sock == sock)
        {
            clients[i] = clients[--client_count];
            break;
        }
    LeaveCriticalSection(&cs);

    closesocket(sock);
    return 0;
}

DWORD WINAPI AcceptThread(LPVOID arg)
{
    while (1)
    {
        SOCKET c = accept(server_socket, NULL, NULL);
        if (c == INVALID_SOCKET)
            break;

        EnterCriticalSection(&cs);
        if (client_count < MAX_CLIENTS)
        {
            clients[client_count].sock = c;
            strcpy(clients[client_count].name, "Unknown");
            client_count++;
            LeaveCriticalSection(&cs);
            SOCKET *p = (SOCKET *)malloc(sizeof(SOCKET));
            *p = c;
            CreateThread(NULL, 0, ReceiveThread, p, 0, NULL);
        }
        else
        {
            LeaveCriticalSection(&cs);
            closesocket(c);
        }
    }
    return 0;
}

void SendMsg(void)
{
    char msg[1024];
    GetWindowText(hInput, msg, sizeof(msg));
    if (!strlen(msg))
        return;

    char full[1100];
    snprintf(full, sizeof(full), "Server: %s", msg);

    MsgHeader hdr = {0};
    hdr.type = MSG_TEXT;
    hdr.length = (uint32_t)strlen(full);

    broadcast(INVALID_SOCKET, &hdr, full);
    AppendChat(full);
    SetWindowText(hInput, "");
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        hChat = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                               WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                               10, 10, 390, 210, hwnd, NULL, NULL, NULL);
        hClients = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                  WS_VISIBLE | WS_CHILD | LBS_NOTIFY | WS_VSCROLL,
                                  415, 10, 150, 210, hwnd, NULL, NULL, NULL);
        hInput = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_VISIBLE | WS_CHILD,
                                10, 230, 300, 24, hwnd, NULL, NULL, NULL);
        CreateWindow("BUTTON", "Broadcast to All",
                     WS_VISIBLE | WS_CHILD,
                     320, 229, 120, 26, hwnd, (HMENU)1, NULL, NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == 1)
            SendMsg();
        break;

    case WM_ADD_CLIENT:
    {
        char *name = (char *)lp;
        SendMessage(hClients, LB_ADDSTRING, 0, (LPARAM)name);
        char line[100];
        snprintf(line, sizeof(line), "*** %s connected ***", name);
        AppendChat(line);
        free(name);
    }
    break;

    case WM_REMOVE_CLIENT:
    {
        char *name = (char *)lp;
        int cnt = SendMessage(hClients, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < cnt; i++)
        {
            char buf[64];
            SendMessage(hClients, LB_GETTEXT, i, (LPARAM)buf);
            if (!strcmp(buf, name))
            {
                SendMessage(hClients, LB_DELETESTRING, i, 0);
                break;
            }
        }
        char line[100];
        snprintf(line, sizeof(line), "*** %s disconnected ***", name);
        AppendChat(line);
        free(name);
    }
    break;

    case WM_NEW_MESSAGE:
    {
        char *t = (char *)wp;
        AppendChat(t);
        free(t);
    }
    break;

    case WM_NEW_FILE:
    {
        char *fname = (char *)wp;
        char line[200];
        snprintf(line, sizeof(line), "[File received: %s]", fname);
        AppendChat(line);
        char dir[MAX_PATH], fpath[MAX_PATH];
        GetTempPath(MAX_PATH, dir);
        snprintf(fpath, MAX_PATH, "%s%s", dir, fname);
        ShellExecute(NULL, "open", fpath, NULL, NULL, SW_SHOW);
        free(fname);
    }
    break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR args, int ncmd)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&cs);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(3000);
    srv.sin_addr.s_addr = INADDR_ANY;

    bind(server_socket, (struct sockaddr *)&srv, sizeof(srv));
    listen(server_socket, MAX_CLIENTS);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ChatServer";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    hwnd_global = CreateWindow("ChatServer", "Chat Server",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100, 100, 590, 310, NULL, NULL, hInst, NULL);

    CreateThread(NULL, 0, AcceptThread, NULL, 0, NULL);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    for (int i = 0; i < client_count; i++)
        closesocket(clients[i].sock);
    closesocket(server_socket);
    WSACleanup();
    DeleteCriticalSection(&cs);
    return 0;
}