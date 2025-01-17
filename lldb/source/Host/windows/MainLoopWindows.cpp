//===-- MainLoopWindows.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/windows/MainLoopWindows.h"
#include "lldb/Host/Config.h"
#include "lldb/Utility/Status.h"
#include "llvm/Config/llvm-config.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <vector>
#include <winsock2.h>

using namespace lldb;
using namespace lldb_private;

llvm::Expected<size_t> MainLoopWindows::Poll() {
  std::vector<WSAEVENT> read_events;
  read_events.reserve(m_read_fds.size());
  for (auto &[fd, info] : m_read_fds) {
    int result = WSAEventSelect(fd, info.event, FD_READ | FD_ACCEPT | FD_CLOSE);
    assert(result == 0);

    read_events.push_back(info.event);
  }

  DWORD result = WSAWaitForMultipleEvents(
      read_events.size(), read_events.data(), FALSE, WSA_INFINITE, FALSE);

  for (auto &fd : m_read_fds) {
    int result = WSAEventSelect(fd.first, WSA_INVALID_EVENT, 0);
    assert(result == 0);
  }

  if (result >= WSA_WAIT_EVENT_0 &&
      result < WSA_WAIT_EVENT_0 + read_events.size())
    return result - WSA_WAIT_EVENT_0;

  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "WSAWaitForMultipleEvents failed");
}

MainLoopWindows::ReadHandleUP
MainLoopWindows::RegisterReadObject(const IOObjectSP &object_sp,
                                    const Callback &callback, Status &error) {
  if (!object_sp || !object_sp->IsValid()) {
    error.SetErrorString("IO object is not valid.");
    return nullptr;
  }
  if (object_sp->GetFdType() != IOObject::eFDTypeSocket) {
    error.SetErrorString(
        "MainLoopWindows: non-socket types unsupported on Windows");
    return nullptr;
  }

  WSAEVENT event = WSACreateEvent();
  if (event == WSA_INVALID_EVENT) {
    error.SetErrorStringWithFormat("Cannot create monitoring event.");
    return nullptr;
  }

  const bool inserted =
      m_read_fds
          .try_emplace(object_sp->GetWaitableHandle(), FdInfo{event, callback})
          .second;
  if (!inserted) {
    WSACloseEvent(event);
    error.SetErrorStringWithFormat("File descriptor %d already monitored.",
                                   object_sp->GetWaitableHandle());
    return nullptr;
  }

  return CreateReadHandle(object_sp);
}

void MainLoopWindows::UnregisterReadObject(IOObject::WaitableHandle handle) {
  auto it = m_read_fds.find(handle);
  assert(it != m_read_fds.end());
  BOOL result = WSACloseEvent(it->second.event);
  assert(result == TRUE);
  m_read_fds.erase(it);
}

void MainLoopWindows::ProcessReadObject(IOObject::WaitableHandle handle) {
  auto it = m_read_fds.find(handle);
  if (it != m_read_fds.end())
    it->second.callback(*this); // Do the work
}

Status MainLoopWindows::Run() {
  m_terminate_request = false;

  Status error;

  // run until termination or until we run out of things to listen to
  while (!m_terminate_request && !m_read_fds.empty()) {

    llvm::Expected<size_t> signaled_event = Poll();
    if (!signaled_event)
      return Status(signaled_event.takeError());

    auto &KV = *std::next(m_read_fds.begin(), *signaled_event);

    ProcessReadObject(KV.first);
    ProcessPendingCallbacks();
  }
  return Status();
}
