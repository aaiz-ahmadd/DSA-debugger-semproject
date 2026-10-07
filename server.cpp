// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;              // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                  // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            throw overflow_error("Stack is full!");
        }
        Node *n = new Node();
        n->next = top;
        top = n;
        n->data = val;
        count++;
    }
    T pop()
    {
        if (count == 0)
        {
            throw underflow_error("Stack is empty!");
        }
        Node *temp = top;
        top = top->next;
        count--;
        T d = temp->data;
        delete temp;
        return d;
    }
    T &peek()
    {
        if (count == 0)
        {
            throw underflow_error("Stack is empty!");
        }
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int _count = 0;
        Node *temp = top;
        while (temp != nullptr && _count < maxLen)
        {
            out[_count++] = temp->data;
            temp = temp->next;
        }
        return _count;
    }
    ~Stack()
    {
        Node *temp = top;
        while (temp != nullptr)
        {
            top = top->next;
            delete temp;
            temp = top;
        }
    }
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode *temp = new TimelineNode();
        temp->data = s;
        if (head == nullptr)
        {
            head = tail = temp;
            head->next = nullptr;
            tail->prev = nullptr;
            stepCount++;
        }
        else
        {
            tail->next = temp;
            temp->prev = tail;
            tail = temp;
            tail->next = nullptr;
            stepCount++;
        }
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
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

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out))
    {
        if (out == "")
            continue;
        else
            return true;
    }
    return false;
}
string firstWord(const string &line)
{
    int i = 0;
    while (i < line.size() && line[i] == ' ')
        i++;

    string temp;

    while (i < line.size() && line[i] != ' ')
    {
        temp += line[i];
        i++;
    }
    return temp;
}
string secondWord(const string &line)
{
    int i = 0;
    while (i < line.size() && line[i] == ' ')
        i++;

    while (i < line.size() && line[i] != ' ')
        i++;

    while (i < line.size() && line[i] == ' ')
        i++;

    string temp;

    while (i < line.size() && line[i] != ' ')
    {
        temp += line[i];
        i++;
    }

    return temp;
}
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    if (!in)
    {
        cout << "File not opened!" << endl;
        return false;
    }
    string str;
    bool in_func = false;
    while (readSourceLine(in, str))
    {
        string first = firstWord(str);
        if (first == "func")
        {
            if (in_func)
                return false;
            in_func = true;
        }
        if (first == "func_end")
        {
            if (!in_func)
                return false;
            in_func = false;
        }
    }
    if (in_func)
        return false;
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t current = ftell(f);
    int32_t text_size = text.size();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&text_size, sizeof(int32_t), 1, f);
    fwrite(text.data(), 1, text_size, f);

    return current;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offset;
    int32_t size;

    if (fread(&offset, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return -1;

    if (size <= 0)
        return -1; // corrupt size

    string temp(size, '\0');

    if (fread(&temp[0], 1, size, f) != size)
        return -1;
    outText = temp;

    return offset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);
    if (!in)
    {
        cout << "Source file not opened!" << endl;
        return -1;
    }
    FILE *dest = fopen(resolveBinPath, "wb");
    if (dest == nullptr)
    {
        cout << "Dest file not opened!" << endl;
        return -1;
    }

    string temp;
    int64_t mainOffset = -1;

    while (readSourceLine(in, temp))
    {
        int64_t record_position = writeResolveRecord(dest, 0, temp);
        string first = firstWord(temp);
        string second = secondWord(temp);

        if (first == "func" && second == "main")
        {
            mainOffset = record_position;
        }
        if (first == "func")
        {
            if (funcCount < MAX_FUNCS)
            {
                funcArray[funcCount].funcName = second;
                funcArray[funcCount].byteOffsetInResolveBin = record_position;
                funcCount++;
            }
            else
            {
                cout << "Maximum function limit reached!" << endl;
                in.close();
                fclose(dest);
                return -1;
            }
        }
        else if (first == "call")
        {
            if (patchCount < MAX_PATCHES)
            {
                patches[patchCount].targetFuncName = second;
                patches[patchCount].byteOffsetOfOffsetField = record_position;
                patchCount++;
            }
            else
            {
                cout << "Maximum patch limit reached!" << endl;
                in.close();
                fclose(dest);
                return -1;
            }
        }
    }

    in.close();

    for (int i = 0; i < patchCount; i++)
    {
        bool found = false;
        for (int j = 0; j < funcCount; j++)
        {
            if (patches[i].targetFuncName == funcArray[j].funcName)
            {
                found = true;
                if (fseek(dest, patches[i].byteOffsetOfOffsetField, SEEK_SET) != 0)
                {
                    cout << "fseek failed!" << endl;
                    fclose(dest);
                    return -1;
                }
                fwrite(&funcArray[j].byteOffsetInResolveBin, sizeof(funcArray[j].byteOffsetInResolveBin), 1, dest);
                break;
            }
        }
        if (!found)
        {
            cout << "Func not found & you are calling!" << endl;
            fclose(dest);
            return -1;
        }
    }

    fclose(dest);

    if (mainOffset == -1)
    {
        cout << "main function not found!" << endl;
        return mainOffset;
    }

    return mainOffset;

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int32_t count = 0;
    string first = firstWord(line);
    if (count >= maxTokens)
    {
        return -1;
    }
    if (!first.empty())
    {
        tokens[count].type = KEYWORD;
        tokens[count++].text = first;
    }
    else
        return -1;
    string second = secondWord(line);
    if (!second.empty())
    {
        if (count >= maxTokens)
            return -1;
        tokens[count].type = IDENTIFIER;
        tokens[count++].text = second;
    }

    int i = 0;
    while (i < line.size() && line[i] == ' ')
        i++;

    while (i < line.size() && line[i] != ' ')
        i++;

    while (i < line.size() && line[i] == ' ')
        i++;

    while (i < line.size() && line[i] != ' ')
        i++;

    while (i < line.size() && line[i] == ' ')
        i++;

    while (i < line.size())
    {
        string temp = "";
        while (i < line.size() && line[i] != ' ')
        {
            temp += line[i];
            i++;
        }
        if (count < maxTokens)
        {
            tokens[count].type = PARAM;
            tokens[count++].text = temp;
        }
        else
            return -1;
        while (i < line.size() && line[i] == ' ')
        {
            i++;
        }
    }

    return count;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot *s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);

    return s;
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *f = fopen(resolveBinPath, "rb");
    if (f == nullptr)
    {
        cout << "File not opened!" << endl;
        return;
    }
    fseek(f, mainOffset, SEEK_SET);
    Frame main_frame;
    main_frame.func_name = "main";
    main_frame.argc = 0;
    main_frame.returnLine = -1; // bcz main has no caller to return to
    main_frame.localCount = 0;

    Stack<Frame> frameStack;
    frameStack.push(main_frame);

    while (1)
    {
        string data = "";
        int64_t offset = readResolveRecord(f, data);

        if (offset == -1)
        {
            break;
        }

        Token tokens[MAX_TOKENS];

        Frame &current = frameStack.peek(); // & to update properly

        int32_t token_count = tokenizeLine(data, tokens, MAX_TOKENS);
        if (tokens[0].text == "func") {
            if(token_count < 2) {
                cout << "Invalid function heaer!" << endl;
                fclose(f);
                return;
            }

            if(current.func_name != tokens[1].text) {
                cout << "Func not matched!" << endl;
                fclose(f);
                return;
            }
        }
        else if (tokens[0].text == "func_end")
        {
            if (frameStack.depth() == 1)
            {
                Snapshot* s = buildSnapshot(frameStack);
                timeline.record(s);

                frameStack.pop();
                break;
            }
            Frame finished_func = frameStack.pop();

            Frame &caller = frameStack.peek();

            if (fseek(f, 0, SEEK_SET) != 0)
            {
                cout << "Fseek is failed!" << endl;
                fclose(f);
                return;
            }

            bool call_found = false;
            string call_line;

            while (1)
            {
                int64_t call_offset = readResolveRecord(f, call_line);

                if (call_offset == -1)
                {
                    break;
                }

                int64_t current_pos = ftell(f);

                if (current_pos == finished_func.returnLine)
                {
                    Token call_tokens[MAX_TOKENS];

                    int32_t call_token_count = tokenizeLine(call_line, call_tokens, MAX_TOKENS);

                    if (call_token_count < 2)
                    {
                        cout << "Invalid call!" << endl;
                        fclose(f);
                        return;
                    }

                    for (int i = 0; i < finished_func.argc; i++)
                    {
                        bool found = false;

                        for (int j = 0; j < caller.localCount; j++)
                        {
                            if (caller.locals[j].name == call_tokens[i + 2].text)
                            {
                                caller.locals[j].value = finished_func.argv[i].value;
                                found = true;
                                break;
                            }
                        }

                        if (!found)
                        {
                            for (int j = 0; j < caller.argc; j++)
                            {
                                if (caller.argv[j].name == call_tokens[i + 2].text)
                                {
                                    caller.argv[j].value = finished_func.argv[i].value;
                                    found = true;
                                    break;
                                }
                            }
                        }

                        if (!found)
                        {
                            cout << "Arguments not found!" << endl;
                            fclose(f);
                            return;
                        }
                    }
                    call_found = true;
                    break;
                }
            }

            if (!call_found)
            {
                cout << "Call not found!" << endl;
                fclose(f);
                return;
            }

            if (fseek(f, finished_func.returnLine, SEEK_SET) != 0)
            {
                cout << "Fseek is failed!" << endl;
                fclose(f);
                return;
            }
        }
        else if (tokens[0].text == "call")
        {
            int64_t return_pos = ftell(f);
            Frame called_func;
            called_func.func_name = tokens[1].text;
            called_func.localCount = 0;
            called_func.returnLine = return_pos;
            called_func.argc = 0;

            if (fseek(f, offset, SEEK_SET) != 0)
            {
                cout << "Fseek is failed!" << endl;
                fclose(f);
                return;
            }

            string out;
            int64_t header_offset = readResolveRecord(f, out);

            Token header_tokens[MAX_TOKENS];

            int32_t header_token_count = tokenizeLine(out, header_tokens, MAX_TOKENS);

            if (header_token_count < 2)
            {
                cout << "Invalid func header!" << endl;
                fclose(f);
                return;
            }

            if (header_token_count - 2 != token_count - 2)
            {
                cout << "Arguments not matched!" << endl;
                fclose(f);
                return;
            }

            if (header_tokens[1].text == tokens[1].text)
            {
                called_func.argc = token_count - 2;
                for (int i = 0; i < called_func.argc; i++)
                {
                    called_func.argv[i].name = header_tokens[i + 2].text;

                    bool found = false;
                    for (int j = 0; j < current.localCount; j++)
                    {
                        if (current.locals[j].name == tokens[i + 2].text)
                        {
                            found = true;
                            called_func.argv[i].value = current.locals[j].value;
                            break;
                        }
                    }
                    if (!found)
                    {
                        for (int j = 0; j < current.argc; j++)
                        {
                            if (current.argv[j].name == tokens[i + 2].text)
                            {
                                found = true;
                                called_func.argv[i].value = current.argv[j].value;
                                break;
                            }
                        }
                    }
                    if (!found)
                    {
                        cout << "Arguments not found!" << endl;
                        fclose(f);
                        return;
                    }
                }
                frameStack.push(called_func);
            }
            else
            {
                cout << "Func header not matched!" << endl;
                fclose(f);
                return;
            }
        }
        else if (tokens[0].text == "set")
        {
            bool found = false;
            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[1].text)
                {
                    found = true;
                    current.locals[i].value = stoi(tokens[2].text);
                    break;
                }
            }
            if (!found)
            {
                if (current.localCount < MAX_VARS_PER_FRAME)
                {
                    current.locals[current.localCount].name = tokens[1].text;
                    current.locals[current.localCount].value = stoi(tokens[2].text);
                    current.localCount++;
                }
                else
                {
                    cout << "Vars per frames reached!" << endl;
                    fclose(f);
                    return;
                }
            }
        }
        else if (tokens[0].text == "add")
        {
            Variable *var1 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[1].text)
                {
                    var1 = &current.locals[i];
                }
            }
            if (var1 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[1].text)
                    {
                        var1 = &current.argv[i];
                    }
                }
            }

            if (var1 == nullptr)
            {
                cout << "Var 1 not exists!" << endl;
                fclose(f);
                return;
            }

            Variable *var2 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[2].text)
                {
                    var2 = &current.locals[i];
                }
            }
            if (var2 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[2].text)
                    {
                        var2 = &current.argv[i];
                    }
                }
            }

            if (var2 == nullptr)
            {
                cout << "Var 2 not exists!" << endl;
                fclose(f);
                return;
            }

            var1->value += var2->value;
        }
        else if (tokens[0].text == "sub")
        {
            Variable *var1 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[1].text)
                {
                    var1 = &current.locals[i];
                }
            }
            if (var1 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[1].text)
                    {
                        var1 = &current.argv[i];
                    }
                }
            }

            if (var1 == nullptr)
            {
                cout << "Var 1 not exists!" << endl;
                fclose(f);
                return;
            }

            Variable *var2 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[2].text)
                {
                    var2 = &current.locals[i];
                }
            }
            if (var2 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[2].text)
                    {
                        var2 = &current.argv[i];
                    }
                }
            }

            if (var2 == nullptr)
            {
                cout << "Var 2 not exists!" << endl;
                fclose(f);
                return;
            }

            var1->value -= var2->value;
        }
        else if (tokens[0].text == "mul")
        {
            Variable *var1 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[1].text)
                {
                    var1 = &current.locals[i];
                }
            }
            if (var1 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[1].text)
                    {
                        var1 = &current.argv[i];
                    }
                }
            }

            if (var1 == nullptr)
            {
                cout << "Var 1 not exists!" << endl;
                fclose(f);
                return;
            }

            Variable *var2 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[2].text)
                {
                    var2 = &current.locals[i];
                }
            }
            if (var2 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[2].text)
                    {
                        var2 = &current.argv[i];
                    }
                }
            }

            if (var2 == nullptr)
            {
                cout << "Var 2 not exists!" << endl;
                fclose(f);
                return;
            }

            var1->value *= var2->value;
        }
        else if (tokens[0].text == "div")
        {
            Variable *var1 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[1].text)
                {
                    var1 = &current.locals[i];
                }
            }
            if (var1 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[1].text)
                    {
                        var1 = &current.argv[i];
                    }
                }
            }

            if (var1 == nullptr)
            {
                cout << "Var 1 not exists!" << endl;
                fclose(f);
                return;
            }

            Variable *var2 = nullptr;

            for (int i = 0; i < current.localCount; i++)
            {
                if (current.locals[i].name == tokens[2].text)
                {
                    var2 = &current.locals[i];
                }
            }
            if (var2 == nullptr)
            {
                for (int i = 0; i < current.argc; i++)
                {
                    if (current.argv[i].name == tokens[2].text)
                    {
                        var2 = &current.argv[i];
                    }
                }
            }

            if (var2 == nullptr)
            {
                cout << "Var 2 not exists!" << endl;
                fclose(f);
                return;
            }
            if (var2->value == 0)
            {
                cout << "Err: Dividing by 0" << endl;
                fclose(f);
                return;
            }
            var1->value /= var2->value;
        }
        Snapshot* s = buildSnapshot(frameStack);
        timeline.record(s);
    }
    fclose(f);
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}