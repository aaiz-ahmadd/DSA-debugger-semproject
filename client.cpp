// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <vector>
#include <sys/stat.h>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg";
const char *TDBG_PART_FILE = "ttdb_last.tdbg.part";
const int MAX_JUMP = 1000000000;
const int MAX_ERROR_TEXT = 4096;
const int MAX_NAME_LEN = 4096;
const int MAX_RECORD_SIZE = 16 * 1024 * 1024;
string LB_HOST = DEFAULT_HOST;
int LB_PORT = DEFAULT_LB_PORT;

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    return connectToAddress(host, port);
}
bool uploadSource(int sock, const char *sourcePath)
{
    FILE *f = fopen(sourcePath, "rb");
    if (f == nullptr)
        return false;
    bool sent = sendFramedFile(sock, STATUS_OK, f);
    fclose(f);

    shutdown(sock, SHUT_WR);
    return sent;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    TrailerChecker checker;
    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    FILE *file = nullptr;
    bool got_status = false;
    bool write_failed = false;
    bool net_failed = false;
    bool timed_out = false;
    char status = STATUS_OK;
    string error_text = "";

    while (1)
    {
        ssize_t n = recv(sock, in, IO_BUFFER_SIZE, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            net_failed = true;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                timed_out = true;
            break;
        }
        if (n == 0)
            break;

        int32_t released = checker.feed(in, n, out);
        int32_t skip = 0;
        if (released > 0 && got_status == false)
        {
            status = out[0];
            got_status = true;
            skip = 1;
            if (status == STATUS_OK)
            {
                file = fopen(outTdbgPath, "wb");
                if (file == nullptr)
                    write_failed = true;
            }
        }
        int32_t len = released - skip;
        if (len == 0)
            continue;

        if (status == STATUS_OK)
        {
            if (file != nullptr)
            {
                if (fwrite(out + skip, 1, (size_t)len, file) != (size_t)len)
                    write_failed = true;
            }
        }
        else
        {
            if (error_text.size() < MAX_ERROR_TEXT)
                error_text.append(out + skip, len);
        }
    }

    bool closed_ok = true;
    if (file != nullptr)
    {
        if (fclose(file) != 0)
            closed_ok = false;
    }

    if (net_failed)
    {
        remove(outTdbgPath);
        if (timed_out)
            outError = "timed out waiting for the reply";
        else
            outError = "connection error while receiving the reply";
        return false;
    }
    if (checker.getTotalLen() == 0)
    {
        outError = "no reply (the load balancer closed the connection)";
        return false;
    }
    if (!checker.verify())
    {
        remove(outTdbgPath);
        outError = MSG_RESPONSE_CORRUPT;
        return false;
    }
    if (status == STATUS_ERROR)
    {
        if (error_text.empty())
            outError = "unknown error";
        else
            outError = error_text;
        return false;
    }
    if (status != STATUS_OK)
    {
        outError = "unknown reply status";
        return false;
    }
    if (write_failed || closed_ok == false)
    {
        remove(outTdbgPath);
        outError = "cannot write the .tdbg file";
        return false;
    }
    return true;
}
bool requestDebugFile(const char *host, int port, const string &sourcePath, const char *outTdbgPath, string &outError)
{
    FILE *probe = fopen(sourcePath.c_str(), "rb");
    if (probe == nullptr)
    {
        outError = "cannot open source file '" + sourcePath + "'";
        return false;
    }
    fclose(probe);

    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer at " + string(host) + ":" + to_string(port);
        return false;
    }
    setSocketTimeouts(sock, CLIENT_TIMEOUT_SEC, CLIENT_TIMEOUT_SEC);

    bool uploaded = uploadSource(sock, sourcePath.c_str());
    bool ok = receiveResponse(sock, outTdbgPath, outError);
    if (ok == false && uploaded == false && outError.compare(0, 8, "no reply") == 0)
        outError = "upload failed (connection lost)";
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readSnapshotInto(TdbgReader &r, int32_t step, Snapshot &out);

bool readExactAt(int fd, char *buf, int64_t n, int64_t offset)
{
    int64_t got = 0;
    while (got < n)
    {
        ssize_t r = pread(fd, buf + got, n - got, offset + got);
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
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    if (r.file_index >= 0)
        close(r.file_index);
    r.file_index = -1;
}
bool openTdbg(const char *path, TdbgReader &r)
{
    r.file_index = -1;
    r.index.clear();

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return false;
    }
    int64_t file_size = st.st_size;
    if (file_size < TDBG_HEADER_SIZE)
    {
        close(fd);
        return false;
    }

    char raw[TDBG_HEADER_SIZE];
    if (!readExactAt(fd, raw, TDBG_HEADER_SIZE, 0))
    {
        close(fd);
        return false;
    }
    TTDBHeader header;
    memcpy(header.magic, raw, 4);
    memcpy(&header.version, raw + 4, 4);
    memcpy(&header.stepCount, raw + 8, 4);
    memcpy(&header.indexOffset, raw + 12, 8);

    bool header_ok = true;
    if (memcmp(header.magic, "TTDB", 4) != 0)
        header_ok = false;
    if (header.version != TDBG_VERSION)
        header_ok = false;
    if (header.stepCount < 1)
        header_ok = false;
    if (header.indexOffset < TDBG_HEADER_SIZE || header.indexOffset > file_size)
        header_ok = false;
    if (file_size - header.indexOffset != (int64_t)header.stepCount * 8)
        header_ok = false;
    if (header_ok == false)
    {
        close(fd);
        return false;
    }

    r.index.resize(header.stepCount);
    if (!readExactAt(fd, (char *)&r.index[0], (int64_t)header.stepCount * 8, header.indexOffset))
    {
        r.index.clear();
        close(fd);
        return false;
    }
    int64_t previous = TDBG_HEADER_SIZE - 1;
    for (int i = 0; i < header.stepCount; i++)
    {
        if (r.index[i] <= previous || r.index[i] >= header.indexOffset)
        {
            r.index.clear();
            close(fd);
            return false;
        }
        previous = r.index[i];
    }

    r.file_index = fd;
    r.header = header;

    Snapshot *scratch = new Snapshot();
    bool all_good = true;
    for (int i = 0; i < header.stepCount && all_good; i++)
    {
        if (!readSnapshotInto(r, i, *scratch))
            all_good = false;
    }
    delete scratch;
    if (all_good == false)
    {
        closeTdbg(r);
        return false;
    }
    return true;
}

bool takeInt32(const char *record, int32_t size, int32_t &pos, int32_t &value)
{
    if (size - pos < 4)
        return false;
    memcpy(&value, record + pos, 4);
    pos += 4;
    return true;
}
bool takeString(const char *record, int32_t size, int32_t &pos, string &text)
{
    int32_t length;
    if (!takeInt32(record, size, pos, length))
        return false;
    if (length < 0 || length > MAX_NAME_LEN || length > size - pos)
        return false;
    text.assign(record + pos, length);
    pos += length;
    return true;
}
bool parseSnapshot(const char *record, int32_t size, Snapshot &s)
{
    int32_t pos = 0;
    if (!takeInt32(record, size, pos, s.stackDepth))
        return false;
    if (s.stackDepth < 0 || s.stackDepth > MAX_STACK_DEPTH)
        return false;

    for (int i = 0; i < s.stackDepth; i++)
    {
        Frame &frame = s.callStack[i];
        if (!takeString(record, size, pos, frame.func_name))
            return false;
        if (!takeInt32(record, size, pos, frame.argc))
            return false;
        if (frame.argc < 0 || frame.argc > MAX_VARS_PER_FRAME)
            return false;
        for (int k = 0; k < frame.argc; k++)
        {
            if (!takeString(record, size, pos, frame.argv[k].name))
                return false;
            if (!takeInt32(record, size, pos, frame.argv[k].value))
                return false;
        }
        if (!takeInt32(record, size, pos, frame.returnLine))
            return false;
        if (!takeInt32(record, size, pos, frame.localCount))
            return false;
        if (frame.localCount < 0 || frame.localCount > MAX_VARS_PER_FRAME)
            return false;
        for (int k = 0; k < frame.localCount; k++)
        {
            if (!takeString(record, size, pos, frame.locals[k].name))
                return false;
            if (!takeInt32(record, size, pos, frame.locals[k].value))
                return false;
        }
    }
    if (pos != size)
        return false;
    return true;
}
bool readSnapshotInto(TdbgReader &r, int32_t step, Snapshot &out)
{
    if (r.file_index < 0 || step < 0 || step >= r.header.stepCount)
        return false;

    int64_t begin = r.index[step];
    int64_t end = r.header.indexOffset;
    if (step + 1 < r.header.stepCount)
        end = r.index[step + 1];
    if (end <= begin || end - begin > MAX_RECORD_SIZE)
        return false;

    int32_t size = end - begin;
    char *record = new char[size];
    if (!readExactAt(r.file_index, record, size, begin))
    {
        delete[] record;
        return false;
    }
    bool ok = parseSnapshot(record, size, out);
    delete[] record;
    return ok;
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    Snapshot *s = new Snapshot();
    if (!readSnapshotInto(r, step, *s))
    {
        delete s;
        return nullptr;
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.currentStep = 0;
    s.reader.file_index = -1;
    s.reader.index.clear();
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no active session: run 'start <source file>' first" << endl;
        return false;
    }

    int64_t last = (int64_t)s.reader.header.stepCount - 1;
    int64_t target = s.currentStep + delta;
    bool clamped = false;
    if (target > last)
    {
        target = last;
        clamped = true;
    }
    if (target < 0)
    {
        target = 0;
        clamped = true;
    }
    if (target == s.currentStep)
    {
        if (delta > 0)
            cout << "already at the last step" << endl;
        else
            cout << "already at the first step" << endl;
        return false;
    }

    int64_t previous = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = previous;
        return false;
    }
    if (clamped)
    {
        if (delta > 0)
            cout << "(stopped at the last step: the jump was longer than the recording)" << endl;
        else
            cout << "(stopped at the first step: the jump was longer than the recording)" << endl;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    string err = "";
    bool downloaded = requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath, TDBG_PART_FILE, err);

    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    if (downloaded == false)
    {
        cout << err << endl;
        return false;
    }

    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    TdbgReader fresh;
    if (!openTdbg(TDBG_PART_FILE, fresh))
    {
        remove(TDBG_PART_FILE);
        cout << MSG_DAMAGED_TDBG << endl;
        return false;
    }

    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    if (s.active_flag)
        closeTdbg(s.reader);
    if (rename(TDBG_PART_FILE, TDBG_FILE) != 0)
        cout << "warning: could not save the session as " << TDBG_FILE << endl;
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;

    // 5. Show step 1 with showCurrent(s).
    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "=== step " << step << " / " << totalSteps << " ===" << endl;
    if (s.stackDepth == 0)
    {
        cout << "  (empty call stack)" << endl;
        return;
    }
    for (int i = 0; i < s.stackDepth; i++)
    {
        const Frame &frame = s.callStack[i];

        cout << "  #" << i << " " << frame.func_name << "(";
        for (int k = 0; k < frame.argc; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << frame.argv[k].name << "=" << frame.argv[k].value;
        }
        cout << ")  locals: ";
        if (frame.localCount == 0)
            cout << "-";
        for (int k = 0; k < frame.localCount; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << frame.locals[k].name << "=" << frame.locals[k].value;
        }
        if (i == 0)
            cout << "   running";
        cout << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout << "commands:" << endl;
    cout << "  start <source file>   send the program to the server and begin debugging at step 1" << endl;
    cout << "  next                  go one step forward" << endl;
    cout << "  prv                   go one step back" << endl;
    cout << "  jmp next <n>          go n steps forward" << endl;
    cout << "  jmp prv <n>           go n steps back" << endl;
    cout << "  help                  show this text" << endl;
    cout << "  quit | exit           leave" << endl;
    cout << "(a leading '" << PROGRAM_NAME << "' is accepted, e.g. '" << PROGRAM_NAME << " next')" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool parseJumpCount(const string &text, int32_t &n)
{
    if (text.size() == 0 || text.size() > 10)
        return false;
    int64_t value = 0;
    for (int i = 0; i < text.size(); i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + (text[i] - '0');
    }
    if (value < 1 || value > MAX_JUMP)
        return false;
    n = (int32_t)value;
    return true;
}

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    vector<string> words;
    istringstream iss(line);
    string word;
    while (iss >> word)
        words.push_back(word);
    if (words.size() == 0)
        return true;

    // 2. If the first word is "ttdb", drop it.
    int at = 0;
    if (words[0] == PROGRAM_NAME)
        at = 1;
    if (at >= words.size())
    {
        cout << "unknown command" << endl;
        printHelp();
        return false;
    }
    string cmd = words[at];
    int extra = words.size() - at - 1;

    // 3. Match the first word:
    if (cmd == "start" && extra == 1)
    {
        return cmdStart(s, words[at + 1]);
    }
    if (cmd == "next" && extra == 0)
    {
        return cmdNext(s);
    }
    if (cmd == "prv" && extra == 0)
    {
        return cmdPrv(s);
    }
    if (cmd == "jmp" && extra == 2 && (words[at + 1] == "next" || words[at + 1] == "prv"))
    {
        int32_t n;
        if (!parseJumpCount(words[at + 2], n))
        {
            cout << "jmp needs a positive whole number (at most " << MAX_JUMP << ")" << endl;
            return false;
        }
        if (words[at + 1] == "next")
            return cmdJmpNext(s, n);
        return cmdJmpPrv(s, n);
    }
    if (cmd == "help" && extra == 0)
    {
        printHelp();
        return true;
    }
    if ((cmd == "quit" || cmd == "exit") && extra == 0)
    {
        quitRequested = true;
        return true;
    }

    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cout << "usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    LB_HOST = argv[1];
    LB_PORT = atoi(argv[2]);
    if (LB_PORT <= 0 || LB_PORT > 65535)
    {
        cout << "usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    DebugSession session;
    initSession(session);
    // Print a short welcome and the help text.
    cout << "Time-Travel Debugger (load balancer at " << LB_HOST << ":" << LB_PORT << ")" << endl;
    printHelp();

    // Infinite loop:
    bool quitRequested = false;
    string line;
    while (1)
    {
        if (quitRequested)
            break;
        cout << PROMPT << flush;
        if (!getline(cin, line))
            break;
        runCommand(session, line, quitRequested);
        // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    }

    // After the loop: closeSession(session) and return 0.
    closeSession(session);
    return 0;
}