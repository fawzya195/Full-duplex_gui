#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comdlg32.lib")

static const char *CLIENT_NAMES[] = {
    "Ahmed", "Mohamed", "Sara", "Ali",
    "Fatima", "Omar", "Khaled", "Nour"};
#define NAME_COUNT 8

static const char *PickName(void)
{
    HANDLE mtx = CreateMutex(NULL, FALSE, "ChatNameCounterMutex");
    WaitForSingleObject(mtx, 5000);

    char tmp[MAX_PATH];
    GetTempPath(MAX_PATH, tmp);
    strcat(tmp, "chat_client_idx.txt");

    int idx = 0;
    FILE *f = fopen(tmp, "r");
    if (f)
    {
        fscanf(f, "%d", &idx);
        fclose(f);
    }

    f = fopen(tmp, "w");
    if (f)
    {
        fprintf(f, "%d", (idx + 1) % NAME_COUNT);
        fclose(f);
    }

    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return CLIENT_NAMES[idx % NAME_COUNT];
}

#define MSG_TEXT 0
#define MSG_FILE 1
#define MSG_NAME 2
#define MSG_JOIN 3
#define MSG_LEAVE 4
#define MSG_PRIVATE 5
#define MSG_ERROR 6

#define WM_NEW_MESSAGE (WM_USER + 1)
#define WM_NEW_FILE (WM_USER + 2)
#define WM_MSG_ERROR (WM_USER + 3)

#pragma pack(push, 1)
typedef struct
{
    uint8_t type;
    uint32_t length;
    char filename[64];
    char target[64];
} MsgHeader;
#pragma pack(pop)

static const char *MY_NAME = NULL;
HWND hChat, hInput, hTo, hwnd_global;
SOCKET sock;
HINSTANCE g_hInst;

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

void SendMsg(void)
{
    char buf[1024], toName[64];
    GetWindowText(hInput, buf, sizeof(buf));
    if (!strlen(buf))
        return;
    GetWindowText(hTo, toName, sizeof(toName));

    char full[1200];
    snprintf(full, sizeof(full), "%s: %s", MY_NAME, buf);

    MsgHeader hdr = {0};
    hdr.length = (uint32_t)strlen(full);

    if (strlen(toName) > 0)
    {
        hdr.type = MSG_PRIVATE;
        strncpy(hdr.target, toName, 63);
        send_all(sock, (char *)&hdr, sizeof(MsgHeader));
        send_all(sock, full, (int)hdr.length);
        char line[1300];
        snprintf(line, sizeof(line), "[Private -> %s] Me: %s", toName, buf);
        AppendChat(line);
    }
    else
    {
        hdr.type = MSG_TEXT;
        send_all(sock, (char *)&hdr, sizeof(MsgHeader));
        send_all(sock, full, (int)hdr.length);
        char line[1200];
        snprintf(line, sizeof(line), "Me: %s", buf);
        AppendChat(line);
    }

    SetWindowText(hInput, "");
}

DWORD WINAPI SendFile(LPVOID arg)
{
    OPENFILENAME ofn = {0};
    char path[MAX_PATH] = "";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_global;
    ofn.hInstance = g_hInst;
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileName(&ofn))
        return 0;

    HANDLE hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        MessageBox(NULL, "Cannot open file!", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    DWORD sz = GetFileSize(hf, NULL);
    if (!sz || sz == INVALID_FILE_SIZE)
    {
        MessageBox(NULL, "Invalid file size!", "Error", MB_OK | MB_ICONERROR);
        CloseHandle(hf);
        return 0;
    }

    char *data = (char *)malloc(sz);
    if (!data)
    {
        CloseHandle(hf);
        return 0;
    }

    DWORD rd = 0;
    ReadFile(hf, data, sz, &rd, NULL);
    CloseHandle(hf);

    if (rd != sz)
    {
        MessageBox(NULL, "File read incomplete!", "Error", MB_OK | MB_ICONERROR);
        free(data);
        return 0;
    }

    const char *fname = strrchr(path, '\\');
    fname = fname ? fname + 1 : path;

    char toName[64] = "";
    GetWindowText(hTo, toName, sizeof(toName));

    MsgHeader hdr = {0};
    hdr.type = MSG_FILE;
    hdr.length = sz;
    strncpy(hdr.filename, fname, 63);
    strncpy(hdr.target, toName, 63);

    int r1 = send_all(sock, (char *)&hdr, sizeof(MsgHeader));
    int r2 = send_all(sock, data, (int)sz);
    free(data);

    if (r1 < 0 || r2 < 0)
    {
        MessageBox(NULL, "Send failed!", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    char line[256];
    if (strlen(toName) > 0)
        snprintf(line, sizeof(line), "[Private -> %s] Me: [Sent file: %s]", toName, fname);
    else
        snprintf(line, sizeof(line), "Me: [Sent file: %s]", fname);

    PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(line), 0);
    return 0;
}

DWORD WINAPI ReceiveThread(LPVOID arg)
{
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
            PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(data), 0);
        }
        else if (hdr.type == MSG_PRIVATE)
        {
            char line[1400];
            snprintf(line, sizeof(line), "[Private] %s", data);
            PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(line), 0);
        }
        else if (hdr.type == MSG_ERROR)
        {
            PostMessage(hwnd_global, WM_MSG_ERROR, (WPARAM)_strdup(data), 0);
        }
        else if (hdr.type == MSG_JOIN)
        {
            char line[100];
            snprintf(line, sizeof(line), "*** %s joined ***", data);
            PostMessage(hwnd_global, WM_NEW_MESSAGE, (WPARAM)_strdup(line), 0);
        }
        else if (hdr.type == MSG_LEAVE)
        {
            char line[100];
            snprintf(line, sizeof(line), "*** %s left ***", data);
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
            PostMessage(hwnd_global, WM_NEW_FILE, (WPARAM)_strdup(fpath), 0);
        }
        free(data);
    }

    PostMessage(hwnd_global, WM_NEW_MESSAGE,
                (WPARAM)_strdup("--- Connection lost ---"), 0);
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        hChat = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                               WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                               10, 10, 420, 210, hwnd, NULL, NULL, NULL);
        CreateWindow("STATIC", "Send to:",
                     WS_VISIBLE | WS_CHILD,
                     10, 230, 130, 20, hwnd, NULL, NULL, NULL);
        hTo = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_VISIBLE | WS_CHILD,
                             145, 228, 100, 22, hwnd, (HMENU)3, NULL, NULL);
        hInput = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_VISIBLE | WS_CHILD,
                                10, 258, 255, 22, hwnd, NULL, NULL, NULL);
        CreateWindow("BUTTON", "Send",
                     WS_VISIBLE | WS_CHILD,
                     275, 257, 70, 24, hwnd, (HMENU)1, NULL, NULL);
        CreateWindow("BUTTON", "Send File",
                     WS_VISIBLE | WS_CHILD,
                     355, 257, 75, 24, hwnd, (HMENU)2, NULL, NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == 1)
            SendMsg();
        if (LOWORD(wp) == 2)
            CreateThread(NULL, 0, SendFile, NULL, 0, NULL);
        break;

    case WM_NEW_MESSAGE:
    {
        char *t = (char *)wp;
        AppendChat(t);
        free(t);
    }
    break;

    case WM_MSG_ERROR:
    {
        char *err = (char *)wp;
        MessageBox(hwnd_global, err, "Error - User Not Found", MB_OK | MB_ICONWARNING);
        free(err);
    }
    break;

    case WM_NEW_FILE:
    {
        char *path = (char *)wp;
        const char *fname = strrchr(path, '\\');
        fname = fname ? fname + 1 : path;
        char line[MAX_PATH + 30];
        snprintf(line, sizeof(line), "[File received: %s]", fname);
        AppendChat(line);
        ShellExecute(NULL, "open", path, NULL, NULL, SW_SHOW);
        free(path);
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
    g_hInst = hInst;
    MY_NAME = PickName();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(3000);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) == SOCKET_ERROR)
    {
        MessageBox(NULL, "Cannot connect to server!", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    MsgHeader nameHdr = {0};
    nameHdr.type = MSG_NAME;
    nameHdr.length = (uint32_t)strlen(MY_NAME);
    send_all(sock, (char *)&nameHdr, sizeof(MsgHeader));
    send_all(sock, MY_NAME, (int)nameHdr.length);

    char title[100];
    snprintf(title, sizeof(title), "Chat Client - %s", MY_NAME);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ChatClient";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    hwnd_global = CreateWindow("ChatClient", title,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               200, 200, 460, 330, NULL, NULL, hInst, NULL);

    CreateThread(NULL, 0, ReceiveThread, NULL, 0, NULL);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}