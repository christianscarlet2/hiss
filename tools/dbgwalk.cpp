// One-off: open a crash minidump and print the faulting thread stack + !analyze.
#include <windows.h>
#include <dbgeng.h>
#include <stdio.h>

class OutCB : public IDebugOutputCallbacks {
public:
    STDMETHOD(QueryInterface)(REFIID iid, void** o) {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugOutputCallbacks)) { *o = this; return S_OK; }
        *o = 0; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return 1; }
    STDMETHOD_(ULONG, Release)() { return 1; }
    STDMETHOD(Output)(ULONG, PCSTR text) { fputs(text, stdout); fflush(stdout); return S_OK; }
};

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: dbgwalk <dump>\n"); return 1; }
    IDebugClient* client = 0;
    HRESULT hr = DebugCreate(__uuidof(IDebugClient), (void**)&client);
    if (hr != S_OK) { printf("DebugCreate 0x%x\n", hr); return 1; }
    IDebugControl* control = 0; client->QueryInterface(__uuidof(IDebugControl), (void**)&control);
    IDebugSymbols* symbols = 0; client->QueryInterface(__uuidof(IDebugSymbols), (void**)&symbols);
    static OutCB outcb; client->SetOutputCallbacks(&outcb);
    symbols->SetSymbolPath("C:\\www\\openholdembot_old\\Release");
    hr = client->OpenDumpFile(argv[1]);
    if (hr != S_OK) { printf("OpenDumpFile 0x%x\n", hr); return 1; }
    control->WaitForEvent(0, INFINITE);
    symbols->Reload("");
    printf("\n==== exception context (.ecxr) ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, ".ecxr", DEBUG_EXECUTE_DEFAULT);
    printf("\n==== faulting stack after .ecxr (kb 80) ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, "kb 80", DEBUG_EXECUTE_DEFAULT);
    printf("\n==== exception record (.exr -1) ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, ".exr -1", DEBUG_EXECUTE_DEFAULT);
    printf("\n==== registers ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, "r", DEBUG_EXECUTE_DEFAULT);
    printf("\n==== ln of frames ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, "ln 004e3eaf", DEBUG_EXECUTE_DEFAULT);
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, "ln 004e3dc0", DEBUG_EXECUTE_DEFAULT);
    printf("\n==== kb with source lines (kn/kp) ====\n");
    control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, "kp 8", DEBUG_EXECUTE_DEFAULT);
    return 0;
}
