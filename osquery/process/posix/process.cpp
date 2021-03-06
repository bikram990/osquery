/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under both the Apache 2.0 license (found in the
 *  LICENSE file in the root directory of this source tree) and the GPLv2 (found
 *  in the COPYING file in the root directory of this source tree).
 *  You may select, at your option, one of the above-listed licenses.
 */

#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <vector>

#include <osquery/logger.h>
#include <osquery/process/process.h>
#include <osquery/system.h>
#include <osquery/utils/info/platform_type.h>

#ifndef NSIG
#define NSIG 32
#endif

extern char** environ;

namespace osquery {

PlatformProcess::PlatformProcess(PlatformPidType id) : id_(id) {}

bool PlatformProcess::operator==(const PlatformProcess& process) const {
  return (nativeHandle() == process.nativeHandle());
}

bool PlatformProcess::operator!=(const PlatformProcess& process) const {
  return (nativeHandle() != process.nativeHandle());
}

PlatformProcess::~PlatformProcess() {}

int PlatformProcess::pid() const {
  return id_;
}

bool PlatformProcess::kill() const {
  if (!isValid()) {
    return false;
  }

  int status = ::kill(nativeHandle(), SIGKILL);
  return (status == 0);
}

bool PlatformProcess::killGracefully() const {
  if (!isValid()) {
    return false;
  }

  int status = ::kill(nativeHandle(), SIGTERM);
  return (status == 0);
}

ProcessState PlatformProcess::checkStatus(int& status) const {
  int process_status = 0;
  if (!isValid()) {
    return PROCESS_ERROR;
  }

  pid_t result = ::waitpid(nativeHandle(), &process_status, WNOHANG);
  if (result < 0) {
    if (errno == ECHILD) {
      return PROCESS_EXITED;
    }
    process_status = -1;
    return PROCESS_ERROR;
  }

  if (result == 0) {
    return PROCESS_STILL_ALIVE;
  }

  if (WIFEXITED(process_status)) {
    status = WEXITSTATUS(process_status);
    return PROCESS_EXITED;
  }

  // process's state has changed but the state isn't that which we expect!
  return PROCESS_STATE_CHANGE;
}

std::shared_ptr<PlatformProcess> PlatformProcess::getCurrentProcess() {
  pid_t pid = ::getpid();
  return std::make_shared<PlatformProcess>(pid);
}

int PlatformProcess::getCurrentPid() {
  return PlatformProcess::getCurrentProcess()->pid();
}

std::shared_ptr<PlatformProcess> PlatformProcess::getLauncherProcess() {
  pid_t ppid = ::getppid();
  return std::make_shared<PlatformProcess>(ppid);
}

std::shared_ptr<PlatformProcess> PlatformProcess::launchWorker(
    const std::string& exec_path, int argc /* unused */, char** argv) {
  auto worker_pid = ::fork();
  if (worker_pid < 0) {
    return std::shared_ptr<PlatformProcess>();
  } else if (worker_pid == 0) {
    setEnvVar("OSQUERY_WORKER", std::to_string(::getpid()).c_str());
    ::execve(exec_path.c_str(), argv, ::environ);

    // Code should never reach this point
    LOG(ERROR) << "osqueryd could not start worker process";
    Initializer::shutdown(EXIT_CATASTROPHIC);
    return std::shared_ptr<PlatformProcess>();
  }
  return std::make_shared<PlatformProcess>(worker_pid);
}

std::shared_ptr<PlatformProcess> PlatformProcess::launchExtension(
    const std::string& exec_path,
    const std::string& extensions_socket,
    const std::string& extensions_timeout,
    const std::string& extensions_interval,
    bool verbose) {
  auto ext_pid = ::fork();
  if (ext_pid < 0) {
    return std::shared_ptr<PlatformProcess>();
  } else if (ext_pid == 0) {
    setEnvVar("OSQUERY_EXTENSION", std::to_string(::getpid()).c_str());

    struct sigaction sig_action;
    sig_action.sa_handler = SIG_DFL;
    sig_action.sa_flags = 0;
    sigemptyset(&sig_action.sa_mask);

    for (auto i = NSIG; i >= 0; i--) {
      sigaction(i, &sig_action, nullptr);
    }

    std::vector<const char*> arguments;
    arguments.push_back(exec_path.c_str());
    arguments.push_back(exec_path.c_str());

    std::string arg_verbose("--verbose");
    if (verbose) {
      arguments.push_back(arg_verbose.c_str());
    }

    std::string arg_socket("--socket");
    arguments.push_back(arg_socket.c_str());
    arguments.push_back(extensions_socket.c_str());

    std::string arg_timeout("--timeout");
    arguments.push_back(arg_timeout.c_str());
    arguments.push_back(extensions_timeout.c_str());

    std::string arg_interval("--interval");
    arguments.push_back(arg_interval.c_str());
    arguments.push_back(extensions_interval.c_str());
    arguments.push_back(nullptr);

    char* const* argv = const_cast<char* const*>(&arguments[1]);
    ::execve(arguments[0], argv, ::environ);

    // Code should never reach this point
    VLOG(1) << "Could not start extension process: " << exec_path;
    Initializer::shutdown(EXIT_FAILURE);
    return std::shared_ptr<PlatformProcess>();
  }

  return std::make_shared<PlatformProcess>(ext_pid);
}

std::shared_ptr<PlatformProcess> PlatformProcess::launchTestPythonScript(
    const std::string& args) {
  std::string osquery_path;
  auto osquery_path_option = getEnvVar("OSQUERY_DEPS");
  if (osquery_path_option) {
    osquery_path = *osquery_path_option;
  } else {
    if (!isPlatform(PlatformType::TYPE_FREEBSD)) {
      osquery_path = "/usr/bin/env python";
    } else {
      osquery_path = "/usr/local/bin/python";
    }
  }

  // The whole-string, space-delimited, python process arguments.
  auto argv = osquery_path + " " + args;

  std::shared_ptr<PlatformProcess> process;
  int process_pid = ::fork();
  if (process_pid == 0) {
    // Start a Python script
    ::execlp("sh", "sh", "-c", argv.c_str(), nullptr);
    ::exit(0);
  } else if (process_pid > 0) {
    process.reset(new PlatformProcess(process_pid));
  }

  return process;
}
} // namespace osquery
