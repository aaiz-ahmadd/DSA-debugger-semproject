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
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <netinet/in.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length

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
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
    // placeholder for other two data members
}

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
                if(frameStack.depth() >= MAX_STACK_DEPTH) {
                    cout << "Max stack depth limit reached!" <<endl;
                    fclose(f);
                    return;
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

void writeString(FILE* f, const string& str) {
    int32_t size = str.size();
    fwrite(&size, sizeof(int32_t), 1, f);

    if(size > 0) {
        fwrite(str.data(), sizeof(char), size, f);
    }
}

void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb");
    
    if(!f) {
        cout << "File not opened!" << endl;
        return;
    }

    int32_t step_count = timeline.getStepCount();

    TTDBHeader header;

    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';

    header.version = 1;
    header.stepCount = step_count;
    header.indexOffset = 0;
    
    fwrite(header.magic, sizeof(char), 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    int64_t* idx = new int64_t[step_count];
    TimelineNode* temp = timeline.begin();

    for(int i = 0; i < step_count; i++) {
        idx[i] = ftell(f);
        Snapshot* snap = temp->data;

        fwrite(&snap->stackDepth, sizeof(int32_t), 1, f);

        for(int j = 0; j < snap->stackDepth; j++) {
            Frame& frame = snap->callStack[j];

            writeString(f, frame.func_name);

            fwrite(&frame.argc, sizeof(int32_t), 1, f);

            for(int k = 0; k < frame.argc; k++) {
                writeString(f, frame.argv[k].name);

                fwrite(&frame.argv[k].value, sizeof(int32_t), 1, f);
            }
            fwrite(&frame.returnLine, sizeof(int32_t), 1, f);
            fwrite(&frame.localCount, sizeof(int32_t), 1, f);

            for(int k = 0; k < frame.localCount; k++) {
                writeString(f, frame.locals[k].name);
                fwrite(&frame.locals[k].value, sizeof(int32_t), 1, f);
            }
        }
        temp = temp->next;
    }

    header.indexOffset = ftell(f);

    fwrite(idx, sizeof(int64_t), step_count, f);

    if(fseek(f, 0, SEEK_SET) != 0) {
        cout << "Fseek is failed!" << endl;
        delete [] idx;
        fclose(f);
        return;
    }

    fwrite(header.magic, sizeof(char), 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    delete [] idx;

    fclose(f);
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}

const int32_t LISTEN_BACKLOG = 16;
const char *SOURCE_PATH = "source.bin";
const char *RESOLVE_PATH = "resolve.bin";
const char *TDBG_PATH = "session.tdbg";
sig_atomic_t flag = 1; // set to 0 by ctrl+c, the server loop ends and closes things safely

// Runs pass 0x0 - 0x3 on source.bin. On a failure it returns false and err holds the text for the client.
bool buildDebugFile(string &err)
{
    // The Phase 1 passes report their errors with cout. While they run, cout is pointed at a string, so whatever
    // they print is the error text for the client (when everything is fine they print nothing).
    ostringstream captured;
    streambuf *old_buffer = cout.rdbuf(captured.rdbuf());

    bool ok = true;
    bool invalid_structure = false;
    try
    {
        if (!validateProgram(SOURCE_PATH))
        {
            ok = false;
            invalid_structure = true;
        }

        int64_t mainOffset = -1;
        if (ok)
        {
            mainOffset = resolveProgram(SOURCE_PATH, RESOLVE_PATH);
            if (mainOffset == -1)
                ok = false;
        }

        Timeline timeline;
        if (ok)
        {
            executeProgram(RESOLVE_PATH, mainOffset, timeline);
            if (captured.str().size() > 0)
                ok = false;
        }
        if (ok)
        {
            writeTdbg(timeline, TDBG_PATH);
            if (captured.str().size() > 0)
                ok = false;
        }
    }
    catch (...)
    {
        ok = false;
        captured << "Internal server error!";
    }
    cout.rdbuf(old_buffer); // cout prints on the screen again

    if (ok)
        return true;

    err = captured.str();
    while (err.size() > 0 && (err[err.size() - 1] == '\n' || err[err.size() - 1] == '\r'))
        err.erase(err.size() - 1);
    if (err.size() == 0)
    {
        if (invalid_structure)
            err = "Invalid program: func and func_end do not match!";
        else
            err = "Invalid program!";
    }
    return false;
}

// STAGE 0: RECEIVE
// Reads [status][source][checksum] until the client closes its write side (recv returns 0) and writes
// the source straight to source.bin. It always reads to the end, even when the upload is already known
// to be bad, so closing the socket later can not reset the connection while the reply is on its way.
bool receiveUpload(int sock, const char *path, string &err)
{
    FILE *dest = fopen(path, "wb");
    if (dest == nullptr)
    {
        err = "Server could not create source.bin!";
        return false;
    }

    TrailerChecker checker;
    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    bool got_status = false;
    bool too_big = false;
    bool write_failed = false;
    bool net_failed = false;
    bool timed_out = false;
    char status = STATUS_OK;
    uint64_t source_bytes = 0;

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
            break; // the client finished sending

        int32_t released = checker.feed(in, n, out);
        int32_t skip = 0;
        if (released > 0 && got_status == false)
        {
            status = out[0]; // the first byte of the message is the status
            got_status = true;
            skip = 1;
        }
        int32_t len = released - skip;
        if (len == 0)
            continue;

        source_bytes += len;
        if (source_bytes > MAX_SOURCE_BYTES)
            too_big = true; // keep reading, but stop writing
        if (too_big == false)
        {
            if (fwrite(out + skip, 1, (size_t)len, dest) != (size_t)len)
                write_failed = true;
        }
    }
    if (fclose(dest) != 0)
        write_failed = true;

    if (net_failed)
    {
        if (timed_out)
            err = "Upload timed out!";
        else
            err = "Connection error while receiving the upload!";
        return false;
    }
    if (too_big)
    {
        err = "Source file too large (limit is " + to_string(MAX_SOURCE_BYTES) + " bytes)!";
        return false;
    }
    if (!checker.verify())
    {
        err = MSG_UPLOAD_CORRUPT;
        return false;
    }
    if (status != STATUS_OK)
    {
        err = "Unexpected status byte in the upload!";
        return false;
    }
    if (write_failed)
    {
        err = "Server could not write source.bin!";
        return false;
    }
    return true;
}

// REPLY: [status][payload][checksum]
void replyError(int sock, const string &message)
{
    sendFramedMessage(sock, STATUS_ERROR, message);
}
bool replyFile(int sock, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        replyError(sock, "Server could not read session.tdbg!");
        return false;
    }
    bool sent = sendFramedFile(sock, STATUS_OK, f);
    fclose(f);
    return sent;
}

void handleClient(int sock)
{
    setSocketTimeouts(sock, SOCKET_TIMEOUT_SEC, SOCKET_TIMEOUT_SEC); // a dead client can not hang the server

    string err = "";
    if (!receiveUpload(sock, SOURCE_PATH, err))
    {
        cout << "[server] upload rejected: " << err << endl;
        replyError(sock, err);
    }
    else if (!buildDebugFile(err))
    {
        cout << "[server] program rejected: " << err << endl;
        replyError(sock, err);
    }
    else
    {
        cout << "[server] ok, sending " << TDBG_PATH << endl;
        replyFile(sock, TDBG_PATH);
    }
    shutdown(sock, SHUT_WR); // the reply is finished, the load balancer reads until it sees the end
    close(sock);
}

int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC, set SO_REUSEADDR so the server can be restarted right away,
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
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

void handler(int sg)
{
    (void)sg;
    flag = 0;
}
// main section
int32_t main(int argc, char *argv[])
{
    // the port is the first argument, for example: ./server 5001
    int port = DEFAULT_SERVER_PORT;
    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535)
        {
            cout << "usage: ./server <port>" << endl;
            return 1;
        }
    }

    signal(SIGINT, handler); // ctrl+c sets the flag, so the loop below ends
    signal(SIGTERM, handler);
    signal(SIGPIPE, SIG_IGN);

    int listen_sock = createListenSocket(port);
    cout << "[server] listening on port " << port << endl;

    while (flag)
    {
        // poll() with a timeout, so the flag is looked at even when nobody connects
        struct pollfd pfd;
        pfd.fd = listen_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ready = poll(&pfd, 1, 200);
        if (ready <= 0)
            continue;

        int client_sock = acceptClient(listen_sock, nullptr, nullptr);
        if (client_sock < 0)
            continue;

        handleClient(client_sock); // one client at a time, the load balancer sends them one by one
    }

    close(listen_sock);
    return 0;
}