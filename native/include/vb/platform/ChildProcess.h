#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vb {

class ChildProcess {
public:
  ChildProcess() = default;
  ~ChildProcess() { close(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::string& exeUtf8, const std::vector<std::string>& args,
             bool captureStdin);
  bool writeStdin(const void* data, size_t bytes);
  void closeStdin();
  bool running() const;
  int waitFinished(int timeoutMs);
  void kill();
  void close();
  std::string readAllOutput();

private:
  struct Impl;
  Impl* impl_ = nullptr;
};

std::string findExecutableOnPath(const std::string& name);

}  // namespace vb
