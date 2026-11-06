// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one message ([status][source][checksum]).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the message to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes, and checks the checksum of every
// stream while forwarding it (a TrailerChecker holds back the last 4 bytes). When a check fails the
// trailer is NOT forwarded, so the next receiver rejects the message itself.
// The only messages it writes itself are errors ("server busy", "backend server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the message until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload][checksum: 4 bytes], then closes the connection
//
// Usage: ./load_balancer <server_ip> <server_port>     (the load balancer itself listens on port 5000)

#include <iostream>
#include <string>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";     
const int LISTEN_BACKLOG = 16;            
const int MAX_PENDING_CLIENTS = 8;       
const int STARTUP_MAX_ATTEMPTS = 50;      
const int STARTUP_POLL_INTERVAL_MS = 100; 
const int POLL_INTERVAL_MS = 200;        
const int IDLE_SLEEP_US = 10 * 1000;      
const int ACCEPTOR_WAIT_TRIES = 400;     
const int REPLY_TIMEOUT_SEC = 30;         
int lbPort = DEFAULT_LB_PORT;
string serverHost = DEFAULT_HOST;
int serverPort = DEFAULT_SERVER_PORT;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        Node *temp = front;
        while (temp != nullptr)
        {
            front = front->next;
            delete temp;
            temp = front;
        }
        rear = nullptr;
        count = 0;
    }
    void enqueue(const T &val)
    {
        Node *n = new Node();
        n->data = val;
        n->next = nullptr;
        if (rear == nullptr)
        {
            front = n;
            rear = n;
        }
        else
        {
            rear->next = n;
            rear = n;
        }
        count++;
    }
    T dequeue()
    {
        if (count == 0)
        {
            throw underflow_error("Queue is empty!");
        }
        Node *temp = front;
        front = front->next;
        if (front == nullptr)
        {
            rear = nullptr;
        }
        count--;
        T d = temp->data;
        delete temp;
        return d;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;        
    string address;   
    int port;         
    time_t arrivedAt; 
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;
int acceptorDone = 0;

bool sendError(int sock, const string &message)
{
    return sendFramedMessage(sock, STATUS_ERROR, message);
}

void closeClientSocket(int sock)
{
    shutdown(sock, SHUT_WR);
    setSocketTimeouts(sock, 1, 1);

    char buffer[IO_BUFFER_SIZE];
    time_t start = time(nullptr);
    while (time(nullptr) - start < 2)
    {
        ssize_t n = recv(sock, buffer, IO_BUFFER_SIZE, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; 
    }
    close(sock);
}
void rejectClient(int sock, const string &message)
{
    sendError(sock, message);
    closeClientSocket(sock);
}
int createListenSocket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }
    setCloseOnExec(sock);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        exit(1);
    }
    return sock;
}
int connectToServer(const char *host, int port)
{
    return connectToAddress(host, port);
}

bool startServer(int port)
{
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    int result = system(command.c_str());
    if (result == 0)
        return true;
    return false;
}
int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock != -1)
        return sock;

    cout << "[LB] server is down, starting it" << endl;
    if (!startServer(serverPort))
        return -1;

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock != -1)
            return sock;
    }
    return -1;
}

void relayOneClient(ClientInfo &c)
{
    int clientSock = c.sock;
    int serverSock = connectToServerWithWakeup();
    if (serverSock == -1)
    {
        cout << "[LB] " << c.address << ":" << c.port << " -> " << MSG_BACKEND_FAILED << endl;
        rejectClient(clientSock, MSG_BACKEND_FAILED);
        return;
    }
    setSocketTimeouts(clientSock, SOCKET_TIMEOUT_SEC, SOCKET_TIMEOUT_SEC);
    setSocketTimeouts(serverSock, REPLY_TIMEOUT_SEC, SOCKET_TIMEOUT_SEC);

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];

    TrailerChecker upload;
    bool client_failed = false;
    bool client_timed_out = false;
    bool server_failed = false;
    while (1)
    {
        ssize_t n = recv(clientSock, in, IO_BUFFER_SIZE, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            client_failed = true;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                client_timed_out = true;
            break;
        }
        if (n == 0)
            break;

        int32_t released = upload.feed(in, n, out);
        if (released > 0 && !sendAll(serverSock, out, released))
        {
            server_failed = true;
            break;
        }
    }

    if (client_failed)
    {
        cout << "[LB] " << c.address << ":" << c.port << " upload failed" << endl;
        if (client_timed_out)
            sendError(clientSock, "Upload timed out!");
        close(serverSock);
        close(clientSock);
        return;
    }
    if (server_failed)
    {
        cout << "[LB] lost the server while forwarding the upload" << endl;
        close(serverSock);
        rejectClient(clientSock, "backend server connection lost");
        return;
    }

    bool upload_good = upload.verify();
    if (upload_good)
    {
        if (!sendAll(serverSock, upload.trailer(), CHECKSUM_SIZE))
        {
            close(serverSock);
            rejectClient(clientSock, "backend server connection lost");
            return;
        }
    }

    shutdown(serverSock, SHUT_WR);

    if (upload_good == false)
    {
        cout << "[LB] " << c.address << ":" << c.port << " -> " << MSG_UPLOAD_CORRUPT << endl;
        close(serverSock);
        sendError(clientSock, MSG_UPLOAD_CORRUPT);
        close(clientSock); 
        return;
    }

    TrailerChecker reply;
    bool reply_broken = false;
    bool client_gone = false;
    while (1)
    {
        ssize_t n = recv(serverSock, in, IO_BUFFER_SIZE, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            reply_broken = true;
            break;
        }
        if (n == 0)
            break;

        int32_t released = reply.feed(in, n, out);
        if (released > 0 && !sendAll(clientSock, out, released))
        {
            client_gone = true;
            break;
        }
    }

    if (client_gone == false)
    {
        if (reply.getTotalLen() == 0)
        {
            cout << "[LB] the server closed the connection without a reply" << endl;
            sendError(clientSock, "backend server closed the connection without a reply");
        }
        else if (reply_broken == false && reply.verify())
        {
            sendAll(clientSock, reply.trailer(), CHECKSUM_SIZE);
            cout << "[LB] " << c.address << ":" << c.port << " served" << endl;
        }
        else
        {
            cout << "[LB] the reply of the server failed its checksum, trailer not sent" << endl;
        }
    }

    close(serverSock);
    close(clientSock);
}

void acceptLoop(int listenSock)
{
    while (flag)
    {
        struct pollfd pfd;
        pfd.fd = listenSock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ready = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (ready <= 0)
            continue;

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int sock = acceptClient(listenSock, (struct sockaddr *)&addr, &addr_len);
        if (sock < 0)
        {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        ClientInfo info;
        char ip_text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_text, sizeof(ip_text));
        info.sock = sock;
        info.address = ip_text;
        info.port = ntohs(addr.sin_port);
        info.arrivedAt = time(nullptr);

        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            queueMutex.unlock();
            cout << "[LB] " << info.address << ":" << info.port << " -> " << MSG_SERVER_BUSY << endl;
            rejectClient(sock, MSG_SERVER_BUSY);
        }
        else
        {
            pendingClients.enqueue(info);
            queueMutex.unlock();
        }
    }

    queueMutex.lock();
    acceptorDone = 1;
    queueMutex.unlock();
}
void dispatcherLoop()
{
    while (flag)
    {
        bool got_client = false;
        ClientInfo c;

        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {
            c = pendingClients.dequeue();
            got_client = true;
        }
        queueMutex.unlock();

        if (got_client == false)
        {
            usleep(IDLE_SLEEP_US);
            continue;
        }

        if (time(nullptr) - c.arrivedAt > CLIENT_TIMEOUT_SEC)
        {
            cout << "[LB] " << c.address << ":" << c.port << " waited too long, dropped" << endl;
            closeClientSocket(c.sock);
            continue;
        }
        relayOneClient(c);
    }
}

void handler(int sg)
{
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cout << "usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }
    serverHost = argv[1];
    serverPort = atoi(argv[2]);
    if (serverPort <= 0 || serverPort > 65535)
    {
        cout << "usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }

    signal(SIGINT, handler); 
    signal(SIGTERM, handler);
    signal(SIGPIPE, SIG_IGN); 

    int listenSock = createListenSocket(lbPort);
    cout << "[LB] listening on port " << lbPort << ", server at " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    int done = 0;
    for (int i = 0; i < ACCEPTOR_WAIT_TRIES && done == 0; i++)
    {
        queueMutex.lock();
        done = acceptorDone;
        queueMutex.unlock();
        if (done == 0)
            usleep(IDLE_SLEEP_US);
    }
    if (done == 0)
    {
        cout << "[LB] stopped" << endl;
        _exit(0);
    }
    close(listenSock);
    queueMutex.lock();
    while (!pendingClients.isEmpty())
    {
        ClientInfo waiting = pendingClients.dequeue();
        close(waiting.sock);
    }
    queueMutex.unlock();
    cout << "[LB] stopped" << endl;
    return 0;
}