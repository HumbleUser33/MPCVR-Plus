// What a frozen process is waiting for: the wait chain Windows itself keeps -- a
// critical section, a message sent to another thread, a handle, and whether the chain
// closes on itself, which is a deadlock -- then every thread's stack, with the
// filter's own names where its pdb is beside its .ax.
//
// Shared by freeze_dump.exe, which is run against a frozen player, and by
// playback_test --toggle, which reports on itself when a call does not come back.

#include <dbghelp.h>
#include <tlhelp32.h>
#include <wct.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")

static FILE* g_freezeFile = nullptr;   // set to also write the report to a file

static void FreezeSay(const char* format, ...)
{
	char line[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	fputs(line, stdout);
	if (g_freezeFile) {
		fputs(line, g_freezeFile);
	}
}

static std::vector<DWORD> FreezeThreadsOf(DWORD pid)
{
	std::vector<DWORD> threads;
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		THREADENTRY32 entry = { sizeof(entry) };
		for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
			if (entry.th32OwnerProcessID == pid) {
				threads.push_back(entry.th32ThreadID);
			}
		}
		CloseHandle(snapshot);
	}
	return threads;
}

static void FreezeWaitChains(const std::vector<DWORD>& threads)
{
	const HWCT session = OpenThreadWaitChainSession(0, nullptr);
	if (!session) {
		FreezeSay("  (no wait chain session: %lu)\n", GetLastError());
		return;
	}
	for (const DWORD threadId : threads) {
		WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT] = {};
		DWORD count = WCT_MAX_NODE_COUNT;
		BOOL cycle = FALSE;
		if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, threadId, &count, nodes, &cycle)) {
			continue;
		}
		if (count <= 1 && !cycle) {
			continue;   // running, or waiting on nothing Windows can name
		}
		FreezeSay("  thread %lu%s\n", threadId, cycle ? "   *** DEADLOCK CYCLE ***" : "");
		for (DWORD i = 0; i < count; i++) {
			const auto& node = nodes[i];
			if (node.ObjectType == WctThreadType) {
				FreezeSay("    thread %lu of process %lu, %s\n", node.ThreadObject.ThreadId, node.ThreadObject.ProcessId,
					node.ObjectStatus == WctStatusBlocked ? "blocked"
					: node.ObjectStatus == WctStatusRunning ? "running" : "other");
			} else {
				static const char* kinds[] = { "?", "critical section", "sent message", "mutex", "alpc", "com", "thread wait",
					"process wait", "thread", "com activation", "unknown", "socket i/o", "smb i/o" };
				const char* kind = (node.ObjectType < _countof(kinds)) ? kinds[node.ObjectType] : "?";
				const wchar_t* name = node.LockObject.ObjectName;
				FreezeSay("      waits on %s%s%S%s\n", kind,
					name[0] ? " \"" : "", name[0] ? name : L"", name[0] ? "\"" : "");
			}
		}
	}
	CloseThreadWaitChainSession(session);
}

static void FreezeStack(HANDLE process, DWORD threadId)
{
	const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
	if (!thread) {
		FreezeSay("  thread %lu: cannot open (%lu)\n", threadId, GetLastError());
		return;
	}
	// What the thread says about itself, when it says anything. The renderer writes
	// here what a long wait is waiting for, which a stack alone cannot tell apart.
	{
		using PFN_GetThreadDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
		static PFN_GetThreadDescription pfnGet = []() -> PFN_GetThreadDescription {
			const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
			return kernel ? (PFN_GetThreadDescription)GetProcAddress(kernel, "GetThreadDescription") : nullptr;
		}();
		PWSTR described = nullptr;
		if (pfnGet && SUCCEEDED(pfnGet(thread, &described)) && described) {
			if (*described) {
				FreezeSay("  thread %lu -- %S\n", threadId, described);
			} else {
				FreezeSay("  thread %lu\n", threadId);
			}
			LocalFree(described);
		} else {
			FreezeSay("  thread %lu\n", threadId);
		}
	}
	SuspendThread(thread);

	CONTEXT context = {};
	context.ContextFlags = CONTEXT_FULL;
	if (GetThreadContext(thread, &context)) {
		STACKFRAME64 frame = {};
		frame.AddrPC.Offset = context.Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = context.Rsp;
		frame.AddrStack.Mode = AddrModeFlat;

		for (int depth = 0; depth < 40; depth++) {
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
					nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) {
				break;
			}
			BYTE buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
			auto* symbol = (SYMBOL_INFO*)buffer;
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement = 0;

			IMAGEHLP_MODULE64 module = { sizeof(module) };
			const char* moduleName = SymGetModuleInfo64(process, frame.AddrPC.Offset, &module) ? module.ModuleName : "?";

			if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
				// A module with no pdb still has its exports, and dbghelp will happily
				// name the nearest one however far away it is -- which reads as a real
				// name and is not one. The distance says which it is, so it is printed
				// whenever it is more than a function away.
				if (displacement > 0x1000) {
					FreezeSay("    %s + 0x%llX (past %s)\n", moduleName,
						(unsigned long long)(frame.AddrPC.Offset - module.BaseOfImage), symbol->Name);
				} else {
					FreezeSay("    %s!%s + 0x%llX\n", moduleName, symbol->Name, (unsigned long long)displacement);
				}
			} else {
				FreezeSay("    %s + 0x%llX\n", moduleName, (unsigned long long)(frame.AddrPC.Offset - module.BaseOfImage));
			}
		}
	}
	ResumeThread(thread);
	CloseHandle(thread);
}

// symbolPath: where the filter's pdb is, or nullptr for the usual places.
static void FreezeReport(DWORD pid, DWORD skipThreadId, const char* symbolPath)
{
	const bool self = (pid == GetCurrentProcessId());
	const HANDLE process = self ? GetCurrentProcess()
		: OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!process) {
		FreezeSay("cannot open process %lu (%lu). Run this as the same user, or elevated.\n", pid, GetLastError());
		return;
	}

	const std::vector<DWORD> threads = FreezeThreadsOf(pid);
	FreezeSay("What each blocked thread is waiting for:\n");
	FreezeWaitChains(threads);

	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(process, symbolPath, TRUE);
	FreezeSay("\nStacks:\n");
	for (const DWORD threadId : threads) {
		if (threadId != skipThreadId) {
			FreezeStack(process, threadId);
		}
	}
	SymCleanup(process);

	if (!self) {
		CloseHandle(process);
	}
}
