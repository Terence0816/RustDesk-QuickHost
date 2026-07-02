#include "portable_host.h"

#include <shellapi.h>
#include <wchar.h>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\RustDeskQSCppSingleInstance";

HWND WaitForRunningInstanceWindow(DWORD wait_ms) {
  const DWORD started = GetTickCount();
  while (true) {
    const HWND window = FindWindowW(PortableHostWindowClassName(), nullptr);
    if (window != nullptr) {
      return window;
    }
    if (GetTickCount() - started >= wait_ms) {
      break;
    }
    Sleep(100);
  }
  return nullptr;
}

void ActivateRunningInstanceWindow(HWND window) {
  if (window == nullptr) {
    return;
  }
  if (IsIconic(window)) {
    ShowWindowAsync(window, SW_RESTORE);
  } else {
    ShowWindowAsync(window, SW_SHOW);
  }
  SetForegroundWindow(window);
}

bool CommandLineHasSwitch(const wchar_t* expected_switch) {
  if (expected_switch == nullptr || *expected_switch == L'\0') {
    return false;
  }

  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments == nullptr) {
    return false;
  }

  bool found = false;
  for (int index = 1; index < argument_count; ++index) {
    if (_wcsicmp(arguments[index], expected_switch) == 0) {
      found = true;
      break;
    }
  }

  LocalFree(arguments);
  return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const bool startup_tray_launch =
      CommandLineHasSwitch(PortableHostStartupTrayArgument());
  HANDLE single_instance_mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
  const DWORD mutex_error = GetLastError();
  const bool another_instance_exists = mutex_error == ERROR_ALREADY_EXISTS;

  if (another_instance_exists) {
    if (startup_tray_launch) {
      if (single_instance_mutex != nullptr) {
        CloseHandle(single_instance_mutex);
      }
      return 0;
    }

    const HWND running_window = WaitForRunningInstanceWindow(10000);
    if (running_window != nullptr) {
      ActivateRunningInstanceWindow(running_window);
      if (single_instance_mutex != nullptr) {
        CloseHandle(single_instance_mutex);
      }
      return 0;
    }

    MessageBoxW(
        nullptr,
        L"Another RustDeskQS Host instance is already starting or running.",
        L"RustDeskQS Host",
        MB_ICONWARNING | MB_OK);
    if (single_instance_mutex != nullptr) {
      CloseHandle(single_instance_mutex);
    }
    return 1;
  }

  PortableHostApp app;
  if (!app.Initialize(instance, startup_tray_launch)) {
    MessageBoxW(
        nullptr,
        L"Failed to start the portable C++ host shell.",
        L"RustDeskQS Host",
        MB_ICONERROR | MB_OK);
    if (single_instance_mutex != nullptr) {
      CloseHandle(single_instance_mutex);
    }
    return 1;
  }
  const int exit_code = app.Run();
  if (single_instance_mutex != nullptr) {
    CloseHandle(single_instance_mutex);
  }
  return exit_code;
}
