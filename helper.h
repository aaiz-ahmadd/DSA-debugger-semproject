#pragma once


// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

using namespace std;

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64 * 1024; 
const int32_t CHECKSUM_SIZE = 4;
const int32_t SOCKET_TIMEOUT_SEC = 5; 
#ifndef TTDB_CLIENT_TIMEOUT_SEC
#define TTDB_CLIENT_TIMEOUT_SEC 30 
#endif
const int32_t CLIENT_TIMEOUT_SEC = TTDB_CLIENT_TIMEOUT_SEC; 
const int32_t DEFAULT_LB_PORT = 5000;
const int32_t DEFAULT_SERVER_PORT = 5001;
const char *DEFAULT_HOST = "127.0.0.1";
 
const char STATUS_OK = 0;
const char STATUS_ERROR = 1;
 
const int32_t TDBG_VERSION = 1;
const int32_t TDBG_HEADER_SIZE = 20;
 
const char *MSG_SERVER_BUSY = "server busy, try again later";
const char *MSG_BACKEND_FAILED = "backend server failed to start";
const char *MSG_UPLOAD_CORRUPT = "upload checksum mismatch, request corrupted";
const char *MSG_RESPONSE_CORRUPT = "checksum mismatch, response corrupted";
const char *MSG_DAMAGED_TDBG = "server sent a damaged .tdbg file";

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH]; 
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

const uint32_t CRC_START = 0xFFFFFFFF;
  
uint32_t crc32Update(uint32_t crc, const char *data, int32_t n)
{
    for (int i = 0; i < n; i++)
    {
        crc = crc ^ (unsigned char)data[i];
        for (int k = 0; k < 8; k++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
    }
    return crc;
}

void u32ToBytes(uint32_t value, char out[4])
{
    out[0] = (char)(value >> 24);
    out[1] = (char)(value >> 16);
    out[2] = (char)(value >> 8);
    out[3] = (char)value;
}
uint32_t bytesToU32(const char in[4])
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
    {
        value = (value << 8) | (unsigned char)in[i];
    }
    return value;
}
 
class TrailerChecker
{
    uint32_t crc;
    char tail[4];
    int32_t tailLen;
    int64_t bodyLen; 
    int64_t totalLen; 
 
public:
    TrailerChecker()
    {
        crc = CRC_START;
        tailLen = 0;
        bodyLen = 0;
        totalLen = 0;
    }
    int32_t feed(const char *in, int32_t n, char *out)
    {
        int32_t released = 0;
        for (int i = 0; i < n; i++)
        {
            if (tailLen == 4)
            {
                out[released] = tail[0];
                released++;
                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = tail[3];
                tail[3] = in[i];
            }
            else
            {
                tail[tailLen] = in[i];
                tailLen++;
            }
        }
        crc = crc32Update(crc, out, released);
        bodyLen += released;
        totalLen += n;
        return released;
    }
    bool verify()
    {
        if (tailLen != 4 || bodyLen < 1)
            return false;
        return bytesToU32(tail) == (crc ^ 0xFFFFFFFF);
    }
    const char *trailer() 
    {
        return tail;
    }
    int64_t getTotalLen()
    {
        return totalLen;
    }
};
 
bool recvAll(int sock, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        got = got + r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        sent = sent + r;
    }
    return true;
}
void setSocketTimeouts(int sock, int recvSec, int sendSec)
{
    struct timeval tv;
    tv.tv_usec = 0;
    tv.tv_sec = recvSec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = sendSec;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void setCloseOnExec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int acceptClient(int listenSock, struct sockaddr *addr, socklen_t *addrLen)
{
#ifdef __linux__
    return accept4(listenSock, addr, addrLen, SOCK_CLOEXEC);
#else
    int sock = accept(listenSock, addr, addrLen);
    if (sock >= 0)
        setCloseOnExec(sock);
    return sock;
#endif
}
int connectToAddress(const char *host, int port)
{
    struct addrinfo hints;
    struct addrinfo *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
 
    string port_text = to_string(port);
    if (getaddrinfo(host, port_text.c_str(), &hints, &result) != 0)
        return -1;
 
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        freeaddrinfo(result);
        return -1;
    }
    setCloseOnExec(sock);
    if (connect(sock, result->ai_addr, result->ai_addrlen) != 0)
    {
        close(sock);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
    return sock;
}
 
bool sendFramedMessage(int sock, char status, const string &body)
{
    string message = "";
    message += status;
    message += body;
 
    uint32_t crc = crc32Update(CRC_START, message.data(), message.size());
    char trailer[4];
    u32ToBytes(crc ^ 0xFFFFFFFF, trailer);
    message.append(trailer, 4);
 
    return sendAll(sock, message.data(), message.size());
}

bool sendFramedFile(int sock, char status, FILE *f)
{
    uint32_t crc = crc32Update(CRC_START, &status, 1);
    if (!sendAll(sock, &status, 1))
        return false;
 
    char buffer[IO_BUFFER_SIZE];
    while (1)
    {
        size_t n = fread(buffer, 1, IO_BUFFER_SIZE, f);
        if (n > 0)
        {
            crc = crc32Update(crc, buffer, n);
            if (!sendAll(sock, buffer, n))
                return false;
        }
        if (n < IO_BUFFER_SIZE)
        {
            if (ferror(f))
                return false; 
            break;            
        }
    }
 
    char trailer[4];
    u32ToBytes(crc ^ 0xFFFFFFFF, trailer);
    return sendAll(sock, trailer, 4);
}