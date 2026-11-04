// Development-only launcher. Windows releases file locks and the child job when its owner exits.
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::wstring environment(const wchar_t* key) {
    const auto size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(key, value.data(), size));
    return value;
}
// CommandLineToArgvW/MSVC quoting, including empty arguments and trailing backslashes.
std::wstring quote(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\r\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        if (c == L'\"') out += L'\\';
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}
DWORD seconds(const wchar_t* text) {
    std::size_t used = 0;
    const auto value = std::stoul(text, &used);
    if (used != std::wstring(text).size() || value > 86400 || text[0] == L'-')
        throw std::runtime_error("Invalid seconds (expected 0..86400)");
    return DWORD(value * 1000);
}
struct Options { bool desktop = false, privateDesktop = false, exclusive = false, machine = false; DWORD wait = 1800000, run = INFINITE; int command = 0; std::filesystem::path testLock; };
struct Timing {
    ULONGLONG started = GetTickCount64(), admitted = 0;
    int exitCode = 125;
    const char* scope = "setup";
    ~Timing() {
        const auto finished = GetTickCount64();
        std::ostringstream line;
        line << "NATIVE_TIMING wait_ms=" << (admitted ? admitted : finished) - started
             << " run_ms=" << (admitted ? finished - admitted : 0) << " exit_code=" << exitCode << " scope=" << scope << '\n';
        std::cerr << line.str();
    }
};
Options parse(int argc, wchar_t** argv) {
    Options o;
    o.machine = environment(L"AIRACK_NATIVE_REQUIRE_MACHINE") == L"1";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--") { o.command = i + 1; break; }
        if (arg == L"--desktop") o.desktop = true;
        else if (arg == L"--private-desktop") o.privateDesktop = true;
        else if (arg == L"--exclusive") o.exclusive = true;
        else if (arg == L"--require-machine-resources") o.machine = true;
        else if (arg == L"--wait-seconds" && i + 1 < argc) o.wait = seconds(argv[++i]);
        else if (arg == L"--run-seconds" && i + 1 < argc) o.run = seconds(argv[++i]);
        else if (arg == L"--test-lock-file" && i + 1 < argc) o.testLock = argv[++i];
        else throw std::runtime_error("Expected [--desktop | --private-desktop] [--exclusive] [--wait-seconds N] [--run-seconds N] -- program args...");
    }
    if (!o.command || o.command == argc) throw std::runtime_error("Missing command after --");
    o.desktop = o.desktop || o.exclusive || environment(L"AIRACK_NATIVE_DESKTOP") == L"1";
    o.privateDesktop = o.privateDesktop || environment(L"AIRACK_NATIVE_PRIVATE_DESKTOP") == L"1"
        || !environment(L"AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE").empty();
    if (o.desktop && o.privateDesktop && !o.exclusive) throw std::runtime_error("Choose physical or private desktop access");
    if (!o.testLock.empty() && !o.testLock.is_absolute()) throw std::runtime_error("Test lock file must be absolute");
    if (o.machine && !o.testLock.empty()) throw std::runtime_error("Machine resources cannot use a test lock file");
    return o;
}
std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)), "Known resource folder");
    const auto folder = std::filesystem::path(path);
    CoTaskMemFree(path);
    return folder;
}
struct Resources { std::filesystem::path path; const char* scope; };
Resources lockFile(bool machineRequired) {
    const auto suffix = std::filesystem::path(L"AI Rack Synth") / L"Development" / L"locks";
    const auto machine = knownFolder(FOLDERID_ProgramData) / suffix / L"native-resources-v1.lock";
    if (std::filesystem::exists(machine)) {
        require(std::filesystem::is_regular_file(machine) && !(GetFileAttributesW(machine.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Plain machine resource file");
        return {machine, "machine"};
    }
    if (machineRequired) throw std::runtime_error("Machine native resources are not provisioned");
    const auto folder = knownFolder(FOLDERID_LocalAppData) / suffix;
    std::filesystem::create_directories(folder);
    return {folder / L"native-resources-v1.lock", "account"};
}
struct Lock {
    Handle file;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED overlap{};
    Lock(const std::filesystem::path& path, DWORD offset, bool exclusive, ULONGLONG deadline)
      : file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_FLAG_OVERLAPPED, nullptr)) {
        require(file.value != INVALID_HANDLE_VALUE && event.value, "Open resource lock");
        overlap.Offset = offset;
        overlap.hEvent = event.value;
        if (!LockFileEx(file.value, exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, 1, 0, &overlap)) {
            require(GetLastError() == ERROR_IO_PENDING, "Lock resource");
            std::wcerr << L"Waiting for native " << (offset == 2 ? L"desktop" : L"activity") << L" slot...\n";
            const auto now = GetTickCount64();
            const auto wait = now >= deadline ? 0 : DWORD(deadline - now);
            const auto result = WaitForSingleObject(event.value, wait);
            if (result == WAIT_TIMEOUT) {
                CancelIoEx(file.value, &overlap);
                DWORD transferred;
                GetOverlappedResult(file.value, &overlap, &transferred, TRUE);
                throw std::runtime_error("Resource wait expired");
            }
            require(result == WAIT_OBJECT_0, "Wait for resource");
            DWORD transferred;
            require(GetOverlappedResult(file.value, &overlap, &transferred, FALSE), "Acquire resource");
        }
    }
    ~Lock() { UnlockFileEx(file.value, 0, 1, 0, &overlap); }
};
struct PrivateDesktop {
    HDESK handle = nullptr;
    std::wstring name;
    explicit PrivateDesktop(bool enabled) {
        if (!enabled) return;
        USEROBJECTFLAGS flags{};
        require(GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), nullptr), "Read window station flags");
        if (!(flags.dwFlags & WSF_VISIBLE)) throw std::runtime_error("Private desktop requires an interactive window station");
        name = environment(L"AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE");
        if (!name.empty()) {
            DWORD bytes = 0;
            const auto current = GetThreadDesktop(GetCurrentThreadId());
            GetUserObjectInformationW(current, UOI_NAME, nullptr, 0, &bytes);
            std::vector<wchar_t> actual(bytes / sizeof(wchar_t) + 1);
            require(GetUserObjectInformationW(current, UOI_NAME, actual.data(), DWORD(actual.size() * sizeof(wchar_t)), &bytes), "Read inherited desktop");
            if (!name.starts_with(L"RackSynthNative-") || name != actual.data())
                throw std::runtime_error("Inherited private desktop does not match the current desktop");
            return;
        }
        name = L"RackSynthNative-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        handle = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS, nullptr);
        require(handle != nullptr, "Create private desktop");
    }
    ~PrivateDesktop() { if (handle) CloseDesktop(handle); }
    PrivateDesktop(const PrivateDesktop&) = delete;
    PrivateDesktop& operator=(const PrivateDesktop&) = delete;
};
DWORD launch(int argc, wchar_t** argv, const Options& o) {
    std::wstring command;
    for (int i = o.command; i < argc; ++i) { if (!command.empty()) command += L' '; command += quote(argv[i]); }
    // Keep this desktop alive until the entire owned command tree has exited. Never switch the input desktop.
    PrivateDesktop desktop(o.privateDesktop);
    if (o.privateDesktop) require(SetEnvironmentVariableW(L"AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE", desktop.name.c_str()), "Set private desktop context");
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    require(job.value != nullptr, "Create child job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Set child job lifetime");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (o.privateDesktop) startup.lpDesktop = desktop.name.data();
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process), "Launch command");
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        const auto error = GetLastError();
        TerminateProcess(child.value, 125);
        SetLastError(error);
        require(false, "Attach command to job");
    }
    require(ResumeThread(thread.value) != DWORD(-1), "Start command");
    const auto waited = WaitForSingleObject(child.value, o.run);
    DWORD exitCode = 124;
    if (waited == WAIT_TIMEOUT) {
        std::wcerr << L"Native command runtime expired\n";
    } else {
        require(waited == WAIT_OBJECT_0, "Wait for command");
        require(GetExitCodeProcess(child.value, &exitCode), "Read command exit status");
    }
    // Stop any remaining descendants before releasing their resource lease.
    require(TerminateJobObject(job.value, exitCode), "Close command tree");
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    do {
        require(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr), "Wait for child tree");
        if (accounting.ActiveProcesses) Sleep(10);
    } while (accounting.ActiveProcesses);
    return exitCode;
}
}

int wmain(int argc, wchar_t** argv) {
    Timing timing;
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--resource-info") {
            const auto resources = lockFile(environment(L"AIRACK_NATIVE_REQUIRE_MACHINE") == L"1");
            timing.scope = resources.scope;
            std::cout << "NATIVE_RESOURCES scope=" << resources.scope << '\n';
            std::wcout << resources.path.wstring() << std::endl;
            timing.exitCode = 0;
            return 0;
        }
        const auto o = parse(argc, argv);
        const auto inherited = environment(L"AIRACK_NATIVE_LEASE");
        if (!inherited.empty() && inherited != L"shared" && inherited != L"exclusive") throw std::runtime_error("Invalid inherited native lease");
        if (o.exclusive && inherited == L"shared") throw std::runtime_error("Cannot upgrade a shared parent lease; run measurements separately");
        const auto resources = o.testLock.empty() ? lockFile(o.machine) : Resources{o.testLock, "test"};
        timing.scope = resources.scope;
        const auto& path = resources.path;
        const auto deadline = GetTickCount64() + o.wait;
        std::unique_ptr<Lock> desktop, activity;
        if (inherited.empty()) {
            // The turnstile stops new readers from starving a queued measurement.
            Lock turnstile(path, 0, true, deadline);
            activity = std::make_unique<Lock>(path, 1, o.exclusive, deadline);
        }
        // Activity always precedes desktop, including inherited leases, to avoid a lock-order cycle.
        if (o.desktop && inherited != L"exclusive" && environment(L"AIRACK_NATIVE_DESKTOP_HELD") != L"1")
            desktop = std::make_unique<Lock>(path, 2, true, deadline);
        timing.admitted = GetTickCount64();
        require(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE", inherited == L"exclusive" || o.exclusive ? L"exclusive" : L"shared"), "Set activity lease");
        if (o.desktop) require(SetEnvironmentVariableW(L"AIRACK_NATIVE_DESKTOP_HELD", L"1"), "Set desktop lease");
        // Keep MSBuild workers in this command's lifetime instead of borrowing persistent nodes from another build.
        require(SetEnvironmentVariableW(L"MSBUILDDISABLENODEREUSE", L"1"), "Disable MSBuild node reuse");
        // Each direct invocation gets private data without relying on an agent to remember the override.
        if (environment(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT").empty()) {
            auto root = std::filesystem::current_path() / L"build" / L"native-diagnostics"
                / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(root);
            require(SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", root.c_str()), "Set private diagnostics");
        }
        timing.exitCode = int(launch(argc, argv, o));
        return timing.exitCode;
    } catch (const std::exception& e) {
        std::cerr << "Native runner: " << e.what() << '\n';
        timing.exitCode = std::string(e.what()) == "Resource wait expired" ? 124 : 125;
        return timing.exitCode;
    }
}
