#include "vb/platform/ChildProcess.h"

#include "vb/fs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <cstring>

namespace vb {

#ifdef _WIN32

struct ChildProcess::Impl {
  PROCESS_INFORMATION pi{};
  HANDLE stdinWr = nullptr;
  HANDLE stdoutRd = nullptr;
  bool started = false;
};

static std::wstring quoteArg(const std::wstring& a) {
  if (a.find_first_of(L" \t\"") == std::wstring::npos)
    return a;
  std::wstring out = L"\"";
  for (wchar_t c : a) {
    if (c == L'"')
      out += L"\\\"";
    else
      out += c;
  }
  out += L'"';
  return out;
}

bool ChildProcess::start(const std::string& exeUtf8,
                         const std::vector<std::string>& args, bool captureStdin) {
  close();
  impl_ = new Impl();

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE stdinRd = nullptr;
  HANDLE stdoutWr = nullptr;
  if (captureStdin) {
    if (!CreatePipe(&stdinRd, &impl_->stdinWr, &sa, 1 << 20))
      return false;
    SetHandleInformation(impl_->stdinWr, HANDLE_FLAG_INHERIT, 0);
  }
  if (!CreatePipe(&impl_->stdoutRd, &stdoutWr, &sa, 1 << 16))
    return false;
  SetHandleInformation(impl_->stdoutRd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = captureStdin ? stdinRd : GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = stdoutWr;
  si.hStdError = stdoutWr;

  std::wstring cmd = quoteArg(utf8ToWide(exeUtf8));
  for (const auto& a : args) {
    cmd += L' ';
    cmd += quoteArg(utf8ToWide(a));
  }
  std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back(L'\0');

  const BOOL ok =
      CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &impl_->pi);
  if (stdinRd)
    CloseHandle(stdinRd);
  if (stdoutWr)
    CloseHandle(stdoutWr);
  if (!ok)
    return false;
  impl_->started = true;
  return true;
}

bool ChildProcess::writeStdin(const void* data, size_t bytes) {
  if (!impl_ || !impl_->stdinWr)
    return false;
  const char* p = static_cast<const char*>(data);
  size_t left = bytes;
  while (left > 0) {
    DWORD n = 0;
    if (!WriteFile(impl_->stdinWr, p, static_cast<DWORD>(left), &n, nullptr))
      return false;
    if (n == 0)
      return false;
    p += n;
    left -= n;
  }
  return true;
}

void ChildProcess::closeStdin() {
  if (impl_ && impl_->stdinWr) {
    CloseHandle(impl_->stdinWr);
    impl_->stdinWr = nullptr;
  }
}

bool ChildProcess::running() const {
  if (!impl_ || !impl_->started)
    return false;
  const DWORD r = WaitForSingleObject(impl_->pi.hProcess, 0);
  return r == WAIT_TIMEOUT;
}

int ChildProcess::waitFinished(int timeoutMs) {
  if (!impl_ || !impl_->started)
    return -1;
  const DWORD r = WaitForSingleObject(impl_->pi.hProcess, timeoutMs < 0 ? INFINITE
                                                                        : timeoutMs);
  if (r != WAIT_OBJECT_0)
    return -1;
  DWORD code = 1;
  GetExitCodeProcess(impl_->pi.hProcess, &code);
  return static_cast<int>(code);
}

void ChildProcess::kill() {
  if (impl_ && impl_->started)
    TerminateProcess(impl_->pi.hProcess, 1);
}

void ChildProcess::close() {
  if (!impl_)
    return;
  closeStdin();
  if (impl_->stdoutRd)
    CloseHandle(impl_->stdoutRd);
  if (impl_->started) {
    CloseHandle(impl_->pi.hThread);
    CloseHandle(impl_->pi.hProcess);
  }
  delete impl_;
  impl_ = nullptr;
}

std::string ChildProcess::readAllOutput() {
  if (!impl_ || !impl_->stdoutRd)
    return {};
  std::string out;
  char buf[4096];
  for (;;) {
    DWORD n = 0;
    if (!ReadFile(impl_->stdoutRd, buf, sizeof(buf), &n, nullptr) || n == 0)
      break;
    out.append(buf, n);
  }
  return out;
}

std::string findExecutableOnPath(const std::string& name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = SearchPathW(nullptr, utf8ToWide(name).c_str(), L".exe", MAX_PATH,
                              buf, nullptr);
  if (n == 0 || n >= MAX_PATH)
    return {};
  return wideToUtf8(buf);
}

#else

struct ChildProcess::Impl {};

bool ChildProcess::start(const std::string&, const std::vector<std::string>&, bool) {
  return false;
}
bool ChildProcess::writeStdin(const void*, size_t) { return false; }
void ChildProcess::closeStdin() {}
bool ChildProcess::running() const { return false; }
int ChildProcess::waitFinished(int) { return -1; }
void ChildProcess::kill() {}
void ChildProcess::close() {}
std::string ChildProcess::readAllOutput() { return {}; }
std::string findExecutableOnPath(const std::string&) { return {}; }

#endif

}  // namespace vb
