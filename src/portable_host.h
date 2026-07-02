#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

inline const wchar_t* PortableHostWindowClassName() noexcept {
  return L"RustDeskCppPortableHostWindow";
}

inline const wchar_t* PortableHostStartupTrayArgument() noexcept {
  return L"--startup-tray";
}

class Win32Mutex {
 public:
  Win32Mutex() noexcept {
    InitializeCriticalSection(&critical_section_);
  }

  ~Win32Mutex() {
    DeleteCriticalSection(&critical_section_);
  }

  void Lock() noexcept {
    EnterCriticalSection(&critical_section_);
  }

  void Unlock() noexcept {
    LeaveCriticalSection(&critical_section_);
  }

 private:
  Win32Mutex(const Win32Mutex&) = delete;
  Win32Mutex& operator=(const Win32Mutex&) = delete;

  CRITICAL_SECTION critical_section_ = {};
};

class Win32LockGuard {
 public:
  explicit Win32LockGuard(Win32Mutex& mutex) noexcept : mutex_(mutex) {
    mutex_.Lock();
  }

  ~Win32LockGuard() {
    mutex_.Unlock();
  }

 private:
  Win32LockGuard(const Win32LockGuard&) = delete;
  Win32LockGuard& operator=(const Win32LockGuard&) = delete;

  Win32Mutex& mutex_;
};

class Win32Thread {
 public:
  Win32Thread() noexcept = default;

  ~Win32Thread() {
    Join();
  }

  bool Joinable() const noexcept {
    return handle_ != nullptr;
  }

  void Join() noexcept {
    if (handle_ != nullptr) {
      WaitForSingleObject(handle_, INFINITE);
      CloseHandle(handle_);
      handle_ = nullptr;
    }
  }

  template <typename Callable>
  bool Start(Callable callable) {
    Join();
    std::unique_ptr<ThreadStart> thread_start(
        new (std::nothrow) ThreadStartImpl<Callable>(std::move(callable)));
    if (!thread_start) {
      return false;
    }

    handle_ = CreateThread(nullptr, 0, &Win32Thread::ThreadProc, thread_start.get(), 0, nullptr);
    if (handle_ == nullptr) {
      return false;
    }

    thread_start.release();
    return true;
  }

 private:
  Win32Thread(const Win32Thread&) = delete;
  Win32Thread& operator=(const Win32Thread&) = delete;

  struct ThreadStart {
    virtual ~ThreadStart() {}
    virtual DWORD Run() = 0;
  };

  template <typename Callable>
  struct ThreadStartImpl final : ThreadStart {
    explicit ThreadStartImpl(Callable callable) : callable_(std::move(callable)) {}

    DWORD Run() override {
      callable_();
      return 0;
    }

    Callable callable_;
  };

  static DWORD WINAPI ThreadProc(LPVOID parameter) {
    std::unique_ptr<ThreadStart> thread_start(reinterpret_cast<ThreadStart*>(parameter));
    if (!thread_start) {
      return 1;
    }

    try {
      return thread_start->Run();
    } catch (...) {
      return 1;
    }
  }

  HANDLE handle_ = nullptr;
};

struct HostConfig {
  std::wstring exe_dir;
  std::wstring config_path;
  std::wstring id_server;
  std::wstring relay_server;
  std::wstring api_server;
  std::wstring key;
  std::wstring host_id;
  std::wstring language_file;
  int temporary_password_length = 6;
  bool random_password_enabled = true;
  std::wstring fixed_password_protected;
  bool force_relay = true;
  bool direct_access_enabled = true;
  int direct_access_port = 21118;
  std::wstring preferred_codec;
  int video_fps = 30;
  int video_bitrate_kbps = 20000;
};

enum class ServerState {
  kUnknown,
  kReachable,
  kUnreachable,
};

class PortableHostApp {
 public:
  PortableHostApp();
  ~PortableHostApp();

  bool Initialize(HINSTANCE instance, bool start_hidden_on_launch = false);
  int Run();

 private:
  enum class IncomingApprovalDecision {
    kPending,
    kAccepted,
    kRejected,
  };

  static constexpr UINT_PTR kUiRefreshTimerId = 1;
  static constexpr int kWindowWidth = 252;
  static constexpr int kWindowHeight = 418;
  static constexpr int kControlMargin = 16;
  static constexpr int kEditHeight = 34;

  static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
  LRESULT WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
  static LRESULT CALLBACK IncomingApprovalWindowProcStatic(
      HWND hwnd,
      UINT message,
      WPARAM w_param,
      LPARAM l_param);
  LRESULT IncomingApprovalWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

  bool CreateMainWindow();
  void DestroyIcons();
  void CreateFonts();
  void DestroyFonts();
  void CreateControls();
  void ApplyFonts();
  void LayoutControls(int client_width, int client_height);
  bool AddTrayIcon();
  void RemoveTrayIcon();
  void ShowMainWindow();
  void HideMainWindowToTray();
  void ShowTrayMenu();
  unsigned long BeginIncomingApproval(
      const std::wstring& remote_id,
      const std::wstring& remote_name);
  IncomingApprovalDecision GetIncomingApprovalDecision(unsigned long token) const;
  void ResolveIncomingApproval(IncomingApprovalDecision decision);
  void CompleteIncomingApproval(unsigned long token);
  void ResetIncomingApproval();
  void UpdateIncomingApprovalUi();
  bool EnsureIncomingApprovalWindow();
  void DestroyIncomingApprovalWindow();
  void LayoutIncomingApprovalWindow(int client_width, int client_height);
  void CaptureIncomingApprovalRemoteIdentity(
      std::wstring* remote_id,
      std::wstring* remote_name) const;
  std::wstring GetIncomingApprovalDisplayName() const;
  std::wstring GetIncomingApprovalSecondaryText() const;
  bool DrawOwnerButton(const DRAWITEMSTRUCT* draw_item) const;
  void DrawConnectionStatusCard(HDC dc) const;
  void InvalidateConnectionStatusCard() const;
  void StoreActiveSessionIdentity(
      const std::wstring& remote_id,
      const std::wstring& remote_name);
  void ClearActiveSessionIdentity();
  std::wstring GetActiveSessionDisplayName() const;

  void LoadOrCreateConfig();
  void SaveConfig() const;
  void RefreshPassword();
  void RefreshServerState();
  void RefreshUiText();
  void ShowOptionsMenu();
  void ToggleLaunchOnStartup();
  void ToggleRandomPassword();
  void ConfigureFixedPassword();
  void ConfigureHostId();
  void ConfigureLanguage();
  void ShowAboutDialog() const;
  void StartRendezvousWorker();
  void StopRendezvousWorker();
  void StopActiveSession(bool notify_peer = false);
  void StopAuxiliarySession();
  void ActiveSessionWorker();
  void RendezvousWorker();
  void SetRendezvousStatus(const std::wstring& text, bool registered);
  std::wstring GetRendezvousStatusText() const;
  bool IsRendezvousRegistered() const;
  bool HasActiveSession() const;
  bool IsActiveSessionStopRequested() const;
  bool HasAuxiliarySession() const;
  bool IsAuxiliarySessionStopRequested() const;
  void RegisterActiveSessionConnection(void* connection);
  void ClearActiveSessionConnection(void* connection);
  void RegisterAuxiliarySessionConnection(void* connection);
  void ClearAuxiliarySessionConnection(void* connection);
  bool StartActiveSessionThread(
      const std::wstring& starting_status,
      bool registered,
      const std::wstring& failure_prefix,
      std::function<bool(std::wstring*)> runner);
  bool StartAuxiliarySessionThread(
      const std::wstring& starting_status,
      bool registered,
      const std::wstring& failure_prefix,
      std::function<bool(std::wstring*)> runner);

  std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* default_value) const;
  void WriteIniString(const wchar_t* section, const wchar_t* key, const std::wstring& value) const;
  void EnsureDefaultLanguageFiles() const;
  void LoadLanguageStrings();
  std::wstring GetText(const wchar_t* key, const wchar_t* fallback) const;
  std::wstring FormatText(
      const wchar_t* key,
      const wchar_t* fallback,
      const std::wstring& arg0 = std::wstring()) const;
  std::wstring LocalizeRendezvousStatusText(const std::wstring& text) const;
  std::wstring BuildLanguageFilePath(const std::wstring& language_file) const;
  std::wstring GetDefaultLanguageFile() const;
  std::wstring NormalizeLanguageFileSelection(const std::wstring& selected_path) const;
  bool RestartApplication();

  std::wstring GetExecutableDirectory() const;
  std::wstring GetExecutablePath() const;
  std::wstring BuildConfigPath(const std::wstring& exe_dir) const;
  std::wstring GenerateRustDeskStyleId() const;
  std::wstring GenerateNumericPassword(int length) const;
  std::wstring GetActivePassword() const;
  bool IsLaunchOnStartupEnabled() const;
  bool SetLaunchOnStartupEnabled(bool enabled);
  bool SetFixedPassword(const std::wstring& password);
  bool CanReachTcpHost(const std::wstring& host, unsigned short port, unsigned long timeout_ms) const;
  bool GetOrCreateIdentity(
      std::vector<unsigned char>* public_key,
      std::vector<unsigned char>* secret_key,
      std::vector<unsigned char>* device_uuid,
      std::wstring* error_text);
  std::wstring BuildStableDeviceUuid() const;

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HMENU tray_menu_ = nullptr;
  HICON window_icon_large_ = nullptr;
  HICON window_icon_small_ = nullptr;
  HICON tray_icon_ = nullptr;
  HBITMAP logo_bitmap_ = nullptr;
  HBITMAP options_icon_bitmap_ = nullptr;
  HBITMAP refresh_icon_bitmap_ = nullptr;
  bool tray_icon_added_ = false;
  UINT taskbar_created_message_ = 0;
  ULONG_PTR gdiplus_token_ = 0;
  bool gdiplus_ready_ = false;
  HWND incoming_approval_window_ = nullptr;
  HWND incoming_approval_accept_button_ = nullptr;
  HWND incoming_approval_dismiss_button_ = nullptr;

  HFONT font_title_ = nullptr;
  HFONT font_body_ = nullptr;
  HFONT font_value_ = nullptr;
  HFONT font_small_ = nullptr;
  HFONT font_button_ = nullptr;
  HFONT font_dialog_title_ = nullptr;
  HFONT font_dialog_name_ = nullptr;
  HFONT font_dialog_body_ = nullptr;
  HFONT font_avatar_ = nullptr;

  HBRUSH dark_brush_ = nullptr;
  HBRUSH panel_brush_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  HBRUSH button_brush_ = nullptr;

  HWND logo_icon_ = nullptr;
  HWND title_label_ = nullptr;
  HWND subtitle_label_ = nullptr;
  HWND hint_label_ = nullptr;
  HWND id_accent_ = nullptr;
  HWND id_label_ = nullptr;
  HWND id_value_ = nullptr;
  HWND options_button_ = nullptr;
  HWND password_accent_ = nullptr;
  HWND password_label_ = nullptr;
  HWND password_value_ = nullptr;
  HWND refresh_password_button_ = nullptr;
  HWND disconnect_button_ = nullptr;
  HWND server_status_label_ = nullptr;
  HWND server_value_label_ = nullptr;
  HWND config_path_label_ = nullptr;
  RECT connection_status_card_rect_ = {};

  HostConfig config_;
  std::wstring temporary_password_;
  std::wstring fixed_password_;
  std::unordered_map<std::wstring, std::wstring> language_strings_;
  bool language_base_is_traditional_ = true;
  ServerState server_state_ = ServerState::kUnknown;
  bool winsock_ready_ = false;
  bool ole_ready_ = false;
  bool start_hidden_on_launch_ = false;
  std::wstring rendezvous_status_text_;
  bool rendezvous_registered_ = false;
  std::wstring public_key_hex_;
  std::wstring secret_key_hex_;
  std::wstring device_uuid_text_;
  std::wstring active_session_remote_id_;
  std::wstring active_session_remote_name_;
  Win32Thread rendezvous_thread_;
  Win32Thread active_session_thread_;
  Win32Thread auxiliary_session_thread_;
  std::atomic<bool> stop_rendezvous_{false};
  std::atomic<bool> stop_active_session_{false};
  std::atomic<bool> stop_auxiliary_session_{false};
  std::atomic<bool> active_session_manual_close_requested_{false};
  std::atomic<bool> active_session_running_{false};
  std::atomic<bool> auxiliary_session_running_{false};
  std::atomic<bool> active_session_connected_{false};
  std::atomic<long> desktop_session_count_{0};
  std::atomic<unsigned long> active_session_generation_{0};
  void* active_session_connection_ = nullptr;
  void* auxiliary_session_connection_ = nullptr;
  std::function<bool(std::wstring*)> pending_session_runner_;
  std::wstring pending_session_starting_status_;
  std::wstring pending_session_failure_prefix_;
  bool pending_session_registered_ = false;
  bool pending_session_requested_ = false;
  unsigned long pending_session_generation_ = 0;
  unsigned long incoming_approval_token_seed_ = 0;
  unsigned long incoming_approval_token_ = 0;
  unsigned int ui_refresh_tick_ = 0;
  IncomingApprovalDecision incoming_approval_decision_ = IncomingApprovalDecision::kPending;
  std::wstring incoming_approval_remote_id_;
  std::wstring incoming_approval_remote_name_;
  mutable Win32Mutex active_session_mutex_;
  mutable Win32Mutex active_session_thread_mutex_;
  mutable Win32Mutex auxiliary_session_mutex_;
  mutable Win32Mutex auxiliary_session_thread_mutex_;
  mutable Win32Mutex incoming_approval_mutex_;
  mutable Win32Mutex rendezvous_mutex_;
};
