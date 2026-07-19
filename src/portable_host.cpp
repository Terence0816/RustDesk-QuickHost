#include "portable_host.h"
#include "app_resources.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <propidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <iphlpapi.h>
#include <libyuv/convert.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <shlobj.h>
#include <shellapi.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_codec.h>
#include <vpx/vpx_encoder.h>
#include <wincrypt.h>
#ifndef SODIUM_STATIC
#define SODIUM_STATIC 1
#endif
#include <sodium.h>
#include <winreg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "zstd.h"

#define ResetPortableHostLog(...) ((void)0)
#define AppendPortableHostLog(...) ((void)0)

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x01000
#endif

#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif

#ifndef __ICodecAPI_INTERFACE_DEFINED__
#define __ICodecAPI_INTERFACE_DEFINED__
struct __declspec(uuid("901db4c7-31ce-41a2-85dc-8fa0bf41b8da")) ICodecAPI : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID* api) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID* api) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterRange(
      const GUID* api,
      VARIANT* value_min,
      VARIANT* value_max,
      VARIANT* stepping_delta) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterValues(
      const GUID* api,
      VARIANT** values,
      ULONG* value_count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID* api, VARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID* api, VARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID* api, VARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID* api, LONG_PTR user_data) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID* api) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetAllDefaults() = 0;
  virtual HRESULT STDMETHODCALLTYPE SetValueWithNotify(
      const GUID* api,
      VARIANT* value,
      GUID** changed_param,
      ULONG* changed_param_count) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetAllDefaultsWithNotify(
      GUID** changed_param,
      ULONG* changed_param_count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAllSettings(IStream* stream) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetAllSettings(IStream* stream) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetAllSettingsWithNotify(
      IStream* stream,
      GUID** changed_param,
      ULONG* changed_param_count) = 0;
};
#endif

namespace {

using namespace Gdiplus;

const GUID kCodecApiAvLowLatencyMode = {0x9c27891a,
                                        0xed7a,
                                        0x40e1,
                                        {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
const GUID kCodecApiAvEncVideoForceKeyFrame = {0x398c1b98,
                                               0x8353,
                                               0x475a,
                                               {0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45}};
const UINT32 kAvEncH264ProfileBase = 66U;

template <typename T>
class ComPtr {
 public:
  ComPtr() noexcept = default;
  ComPtr(std::nullptr_t) noexcept {}

  ComPtr(T* value) noexcept : value_(value) {
    InternalAddRef();
  }

  ComPtr(const ComPtr& other) noexcept : value_(other.value_) {
    InternalAddRef();
  }

  ComPtr(ComPtr&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }

  ~ComPtr() {
    InternalRelease();
  }

  ComPtr& operator=(std::nullptr_t) noexcept {
    Reset();
    return *this;
  }

  ComPtr& operator=(T* value) noexcept {
    if (value_ != value) {
      InternalRelease();
      value_ = value;
      InternalAddRef();
    }
    return *this;
  }

  ComPtr& operator=(const ComPtr& other) noexcept {
    return operator=(other.value_);
  }

  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      InternalRelease();
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }

  T* Get() const noexcept {
    return value_;
  }

  T** GetAddressOf() noexcept {
    return &value_;
  }

  T** ReleaseAndGetAddressOf() noexcept {
    Reset();
    return &value_;
  }

  void Reset() noexcept {
    InternalRelease();
  }

  T* operator->() const noexcept {
    return value_;
  }

  explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

  template <typename U>
  HRESULT As(U** result) const noexcept {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    if (value_ == nullptr) {
      return E_NOINTERFACE;
    }
    return value_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(result));
  }

  template <typename U>
  HRESULT As(ComPtr<U>* result) const noexcept {
    if (result == nullptr) {
      return E_POINTER;
    }
    return As(result->ReleaseAndGetAddressOf());
  }

 private:
  void InternalAddRef() noexcept {
    if (value_ != nullptr) {
      value_->AddRef();
    }
  }

  void InternalRelease() noexcept {
    T* old_value = value_;
    value_ = nullptr;
    if (old_value != nullptr) {
      old_value->Release();
    }
  }

  T* value_ = nullptr;
};

class ScopeExit {
 public:
  explicit ScopeExit(std::function<void()> callback)
      : callback_(std::move(callback)) {}

  ~ScopeExit() {
    if (callback_) {
      callback_();
    }
  }

  void Release() {
    callback_ = std::function<void()>();
  }

 private:
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  std::function<void()> callback_;
};

std::string WideToAnsiCompat(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(
      CP_ACP,
      0,
      value.c_str(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string result(size, '\0');
  WideCharToMultiByte(
      CP_ACP,
      0,
      value.c_str(),
      static_cast<int>(value.size()),
      &result[0],
      size,
      nullptr,
      nullptr);
  return result;
}

std::wstring AnsiToWideCompat(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(
      CP_ACP,
      0,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring result(size, L'\0');
  MultiByteToWideChar(
      CP_ACP,
      0,
      value.data(),
      static_cast<int>(value.size()),
      &result[0],
      size);
  return result;
}

bool CopyWideResult(const std::wstring& source, wchar_t* destination, size_t destination_count) {
  if (destination == nullptr || destination_count == 0) {
    return source.empty();
  }
  const size_t copy_count = (std::min)(destination_count - 1, source.size());
  if (copy_count > 0) {
    std::memcpy(destination, source.data(), copy_count * sizeof(wchar_t));
  }
  destination[copy_count] = L'\0';
  return copy_count == source.size();
}

HMODULE GetWs2_32Module() {
  HMODULE module = GetModuleHandleW(L"ws2_32.dll");
  if (module == nullptr) {
    module = LoadLibraryW(L"ws2_32.dll");
  }
  return module;
}

int CompatGetAddrInfoW(
    const wchar_t* node_name,
    const wchar_t* service_name,
    const addrinfoW* hints,
    addrinfoW** results) {
  if (results == nullptr) {
    return 1;
  }
  *results = nullptr;

  HMODULE ws2_module = GetWs2_32Module();
  if (ws2_module == nullptr) {
    return 1;
  }

  typedef INT(WSAAPI* GetAddrInfoWProc)(
      PCWSTR,
      PCWSTR,
      const ADDRINFOW*,
      PADDRINFOW*);
  GetAddrInfoWProc get_addr_info_w =
      reinterpret_cast<GetAddrInfoWProc>(GetProcAddress(ws2_module, "GetAddrInfoW"));
  if (get_addr_info_w != nullptr) {
    return get_addr_info_w(
        node_name,
        service_name,
        reinterpret_cast<const ADDRINFOW*>(hints),
        reinterpret_cast<PADDRINFOW*>(results));
  }

  typedef INT(WSAAPI* GetAddrInfoAProc)(
      PCSTR,
      PCSTR,
      const ADDRINFOA*,
      PADDRINFOA*);
  GetAddrInfoAProc get_addr_info_a =
      reinterpret_cast<GetAddrInfoAProc>(GetProcAddress(ws2_module, "getaddrinfo"));
  if (get_addr_info_a == nullptr) {
    return 1;
  }

  std::string node_name_a;
  std::string service_name_a;
  if (node_name != nullptr) {
    node_name_a = WideToAnsiCompat(node_name);
  }
  if (service_name != nullptr) {
    service_name_a = WideToAnsiCompat(service_name);
  }

  ADDRINFOA hints_a = {};
  if (hints != nullptr) {
    hints_a.ai_flags = hints->ai_flags;
    hints_a.ai_family = hints->ai_family;
    hints_a.ai_socktype = hints->ai_socktype;
    hints_a.ai_protocol = hints->ai_protocol;
  }

  PADDRINFOA results_a = nullptr;
  const int result = get_addr_info_a(
      node_name != nullptr ? node_name_a.c_str() : nullptr,
      service_name != nullptr ? service_name_a.c_str() : nullptr,
      hints != nullptr ? &hints_a : nullptr,
      &results_a);
  *results = reinterpret_cast<addrinfoW*>(results_a);
  return result;
}

void CompatFreeAddrInfoW(addrinfoW* results) {
  if (results == nullptr) {
    return;
  }

  HMODULE ws2_module = GetWs2_32Module();
  if (ws2_module == nullptr) {
    return;
  }

  typedef VOID(WSAAPI* FreeAddrInfoWProc)(PADDRINFOW);
  FreeAddrInfoWProc free_addr_info_w =
      reinterpret_cast<FreeAddrInfoWProc>(GetProcAddress(ws2_module, "FreeAddrInfoW"));
  if (free_addr_info_w != nullptr) {
    free_addr_info_w(reinterpret_cast<PADDRINFOW>(results));
    return;
  }

  typedef VOID(WSAAPI* FreeAddrInfoAProc)(PADDRINFOA);
  FreeAddrInfoAProc free_addr_info_a =
      reinterpret_cast<FreeAddrInfoAProc>(GetProcAddress(ws2_module, "freeaddrinfo"));
  if (free_addr_info_a != nullptr) {
    free_addr_info_a(reinterpret_cast<PADDRINFOA>(results));
  }
}

int CompatGetNameInfoW(
    const sockaddr* address,
    socklen_t address_length,
    wchar_t* host,
    DWORD host_count,
    wchar_t* service,
    DWORD service_count,
    int flags) {
  HMODULE ws2_module = GetWs2_32Module();
  if (ws2_module == nullptr) {
    return 1;
  }

  typedef INT(WSAAPI* GetNameInfoWProc)(
      const SOCKADDR*,
      socklen_t,
      PWCHAR,
      DWORD,
      PWCHAR,
      DWORD,
      INT);
  GetNameInfoWProc get_name_info_w =
      reinterpret_cast<GetNameInfoWProc>(GetProcAddress(ws2_module, "GetNameInfoW"));
  if (get_name_info_w != nullptr) {
    return get_name_info_w(address, address_length, host, host_count, service, service_count, flags);
  }

  typedef INT(WSAAPI* GetNameInfoAProc)(
      const SOCKADDR*,
      socklen_t,
      PCHAR,
      DWORD,
      PCHAR,
      DWORD,
      INT);
  GetNameInfoAProc get_name_info_a =
      reinterpret_cast<GetNameInfoAProc>(GetProcAddress(ws2_module, "getnameinfo"));
  if (get_name_info_a == nullptr) {
    return 1;
  }

  char host_a[NI_MAXHOST] = {};
  char service_a[NI_MAXSERV] = {};
  const int result = get_name_info_a(
      address,
      address_length,
      host != nullptr ? host_a : nullptr,
      host != nullptr ? static_cast<DWORD>(sizeof(host_a)) : 0,
      service != nullptr ? service_a : nullptr,
      service != nullptr ? static_cast<DWORD>(sizeof(service_a)) : 0,
      flags);
  if (result != 0) {
    return result;
  }

  if (host != nullptr &&
      !CopyWideResult(
          AnsiToWideCompat(host_a),
          host,
          static_cast<size_t>(host_count))) {
    return 1;
  }
  if (service != nullptr &&
      !CopyWideResult(
          AnsiToWideCompat(service_a),
          service,
          static_cast<size_t>(service_count))) {
    return 1;
  }

  return 0;
}

int CompatInetPtonW(int family, const wchar_t* address_text, void* address) {
  if (address_text == nullptr || address == nullptr) {
    return -1;
  }

  HMODULE ws2_module = GetWs2_32Module();
  if (ws2_module != nullptr) {
    typedef INT(WSAAPI* InetPtonWProc)(INT, PCWSTR, PVOID);
    InetPtonWProc inet_pton_w =
        reinterpret_cast<InetPtonWProc>(GetProcAddress(ws2_module, "InetPtonW"));
    if (inet_pton_w != nullptr) {
      return inet_pton_w(family, address_text, address);
    }

    typedef INT(WSAAPI* InetPtonAProc)(INT, PCSTR, PVOID);
    InetPtonAProc inet_pton_a =
        reinterpret_cast<InetPtonAProc>(GetProcAddress(ws2_module, "InetPtonA"));
    if (inet_pton_a != nullptr) {
      const std::string address_text_a = WideToAnsiCompat(address_text);
      return inet_pton_a(family, address_text_a.c_str(), address);
    }
  }

  std::vector<wchar_t> mutable_address(
      address_text,
      address_text + std::wcslen(address_text) + 1);
  if (family == AF_INET) {
    sockaddr_in parsed = {};
    int parsed_length = sizeof(parsed);
    if (WSAStringToAddressW(
            mutable_address.data(),
            AF_INET,
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&parsed),
            &parsed_length) == 0) {
      *reinterpret_cast<IN_ADDR*>(address) = parsed.sin_addr;
      return 1;
    }
    return WSAGetLastError() == WSAEINVAL ? 0 : -1;
  }
  if (family == AF_INET6) {
    sockaddr_in6 parsed = {};
    int parsed_length = sizeof(parsed);
    if (WSAStringToAddressW(
            mutable_address.data(),
            AF_INET6,
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&parsed),
            &parsed_length) == 0) {
      *reinterpret_cast<IN6_ADDR*>(address) = parsed.sin6_addr;
      return 1;
    }
    return WSAGetLastError() == WSAEINVAL ? 0 : -1;
  }
  return -1;
}

#define GetAddrInfoW CompatGetAddrInfoW
#define FreeAddrInfoW CompatFreeAddrInfoW
#define GetNameInfoW CompatGetNameInfoW
#define InetPtonW CompatInetPtonW

constexpr UINT kAppRendezvousStatus = WM_APP + 1;
constexpr UINT kAppInstallRemoteFileClipboard = WM_APP + 2;
constexpr UINT kAppTrayIcon = WM_APP + 3;
constexpr UINT kAppIncomingApprovalUpdated = WM_APP + 4;
constexpr UINT kIdValue = 1001;
constexpr UINT kPasswordValue = 1002;
constexpr UINT kRefreshPasswordButton = 1003;
constexpr UINT kDisconnectButton = 1004;
constexpr UINT kOptionsButton = 1005;
constexpr UINT kTrayMenuShowWindow = 1101;
constexpr UINT kTrayMenuExit = 1102;
constexpr UINT kIncomingApprovalAcceptButton = 1201;
constexpr UINT kIncomingApprovalDismissButton = 1202;
constexpr UINT kOptionsMenuLaunchOnStartup = 1301;
constexpr UINT kOptionsMenuSetFixedPassword = 1302;
constexpr UINT kOptionsMenuDisableRandomPassword = 1303;
constexpr UINT kOptionsMenuChangeId = 1304;
constexpr UINT kOptionsMenuLanguage = 1305;
constexpr UINT kOptionsMenuAbout = 1306;
constexpr UINT kTrayIconId = 1;

constexpr COLORREF kWindowColor = RGB(35, 35, 35);
constexpr COLORREF kPanelColor = RGB(18, 18, 18);
constexpr COLORREF kCardColor = RGB(35, 35, 35);
constexpr COLORREF kCardBorderColor = RGB(64, 64, 64);
constexpr COLORREF kTextColor = RGB(255, 255, 255);
constexpr COLORREF kMutedTextColor = RGB(184, 186, 191);
constexpr COLORREF kAccentColor = RGB(48, 133, 255);
constexpr COLORREF kSecondaryButtonColor = RGB(52, 54, 61);
constexpr COLORREF kSecondaryButtonBorderColor = RGB(74, 77, 84);
constexpr COLORREF kConnectionCardIdleFillColor = RGB(48, 49, 55);
constexpr COLORREF kConnectionCardIdleBorderColor = RGB(101, 104, 113);
constexpr COLORREF kConnectionCardConnectedFillColor = RGB(26, 48, 79);
constexpr COLORREF kConnectionCardConnectedBorderColor = RGB(48, 133, 255);
constexpr COLORREF kConnectionCardConnectedTextColor = RGB(97, 187, 255);
constexpr COLORREF kConnectionNameAccentColor = RGB(255, 209, 84);
constexpr COLORREF kDisabledButtonFillColor = RGB(78, 80, 86);
constexpr COLORREF kDisabledButtonBorderColor = RGB(86, 88, 94);
constexpr COLORREF kDisabledButtonTextColor = RGB(120, 122, 128);
constexpr COLORREF kDisconnectButtonColor = RGB(255, 79, 62);
constexpr COLORREF kDisconnectButtonPressedColor = RGB(237, 66, 50);
constexpr COLORREF kDialogColor = RGB(27, 29, 34);
constexpr COLORREF kAvatarColor = RGB(132, 85, 131);
constexpr COLORREF kGoodColor = RGB(68, 204, 153);
constexpr COLORREF kBadColor = RGB(232, 99, 99);
constexpr int kLogoTargetWidth = 200;
constexpr int kLogoTargetHeight = 64;
constexpr int kDefaultIdServerPort = 21116;
constexpr int kDefaultRelayServerPort = 21117;
constexpr int kDefaultDirectAccessPort = 21118;
constexpr int kDefaultKeepAliveMs = 60000;
constexpr int kRegisterIntervalMs = 15000;
constexpr unsigned long kConnectTimeoutMs = 18000;
constexpr unsigned long kLoginWaitTimeoutMs = 60000;
constexpr unsigned long kManualPasswordEntryTimeoutMs = 15000;
constexpr unsigned long kIncomingApprovalTimeoutMs = 60000;
constexpr unsigned long kReceivePollMs = 1000;
constexpr unsigned long kSessionPollMs = 50;
constexpr unsigned long kFrameBodyTimeoutMs = 30000;
constexpr unsigned long kClipboardVirtualFileTimeoutMs = 60000;
constexpr int kRegisterRetryBeforeReadyMs = 3000;
constexpr int kRegisterTcpFallbackBeforeReadyMs = 6000;
constexpr int kRegisterSocketRestartBeforeReadyMs = 15000;
constexpr int kDefaultTargetVideoFps = 30;
constexpr int kDefaultVideoBitrateKbps = 20000;
constexpr int kClipboardFormatText = 0;
constexpr int kClipboardFormatRtf = 1;
constexpr int kClipboardFormatHtml = 2;
constexpr int kClipboardFormatSpecial = 31;
constexpr int kCliprdrResponseOk = 0x1;
constexpr int kCliprdrFileContentsSizeFlag = 0x00000001;
constexpr int kCliprdrFileContentsRangeFlag = 0x00000002;
constexpr size_t kClipboardFileTransferChunkSize = 64 * 1024;
constexpr size_t kFileTransferBlockSize = 128 * 1024;
constexpr wchar_t kProjectUrl[] = L"https://github.com/Terence0816/RustDesk-QuickHost";
constexpr wchar_t kAboutDisplayVersion[] = L"1.1.2.5";
constexpr wchar_t kCppHostVersion[] = L"1.3.0-cpp";
constexpr wchar_t kAppWindowTitle[] = L"RustDeskQS Host";
constexpr wchar_t kAppWindowClassName[] = L"RustDeskCppPortableHostWindow";
constexpr wchar_t kStartupRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupRunValueName[] = L"RustDeskQSCppHost";
constexpr wchar_t kLoginMsgWrongPassword[] = L"Wrong Password";
constexpr wchar_t kLoginMsgClosedManuallyByPeer[] = L"Closed manually by the peer";

struct LanguageEntry {
  const wchar_t* key;
  const wchar_t* value;
};

const LanguageEntry kTraditionalChineseLanguageEntries[] = {
    {L"app_window_title", L"RustDeskQS Host"},
    {L"tray_show_main", L"顯示主頁"},
    {L"tray_exit", L"離開"},
    {L"menu_launch_on_startup", L"開機啟動"},
    {L"menu_set_fixed_password", L"設定固定密碼"},
    {L"menu_disable_random_password", L"停用隨機密碼"},
    {L"menu_change_id", L"更改 ID"},
    {L"menu_language", L"語言"},
    {L"menu_about", L"\u95dc\u65bc"},
    {L"button_check_updates", L"\u6aa2\u67e5\u66f4\u65b0"},
    {L"msg_open_project_failed", L"\u7121\u6cd5\u958b\u555f\u5c08\u6848\u7db2\u5740\u3002"},
    {L"welcome_title", L"歡迎使用遠端協助"},
    {L"hint_provide_id_password", L"請提供下面的 ID 及密碼"},
    {L"label_id", L"ID"},
    {L"label_one_time_password", L"一次性密碼"},
    {L"connection_status_disconnected", L"\u672a\u9023\u7dda"},
    {L"connection_status_authorized", L"\u5df2\u9023\u5165 (\u5df2\u6388\u6b0a)"},
    {L"connection_status_remote_user", L"\u9023\u7dda\u4eba\u54e1"},
    {L"button_disconnect_session", L"\u4e2d\u65b7\u9023\u7dda"},
    {L"status_server_ready", L"伺服器已就緒"},
    {L"status_server_connecting", L"正在連線伺服器"},
    {L"status_server_unreachable", L"伺服器無法連線"},
    {L"status_server_checking", L"正在檢查伺服器"},
    {L"dialog_ok", L"確定"},
    {L"dialog_cancel", L"取消"},
    {L"msg_startup_update_failed", L"無法更新開機啟動設定。"},
    {L"dialog_fixed_password_title", L"設定固定密碼"},
    {L"dialog_fixed_password_prompt", L"請輸入要儲存的固定密碼"},
    {L"msg_fixed_password_empty", L"固定密碼不可為空白。"},
    {L"msg_fixed_password_save_failed", L"固定密碼儲存失敗。"},
    {L"dialog_change_id_title", L"更改 ID"},
    {L"dialog_change_id_prompt", L"請輸入新的 ID（僅限數字）"},
    {L"msg_id_digits_only", L"ID 只能輸入數字。"},
    {L"msg_id_length_invalid", L"ID 長度需要在 6 到 16 碼之間。"},
    {L"dialog_language_title", L"選擇語言檔"},
    {L"msg_language_restart_failed", L"語言檔已套用，但程式無法自動重新啟動。"},
    {L"incoming_window_title", L"遠端連入"},
    {L"incoming_dismiss", L"關閉"},
    {L"incoming_accept", L"接受"},
    {L"incoming_question", L"是否接受？"},
    {L"incoming_body", L"收到遠端連入請求，是否接受？"},
    {L"incoming_unknown_device", L"未知裝置"},
    {L"incoming_rustdesk_id_prefix", L"RustDesk ID"},
};

const LanguageEntry kEnglishLanguageEntries[] = {
    {L"app_window_title", L"RustDeskQS Host"},
    {L"tray_show_main", L"Show Main Window"},
    {L"tray_exit", L"Exit"},
    {L"menu_launch_on_startup", L"Launch on Startup"},
    {L"menu_set_fixed_password", L"Set Fixed Password"},
    {L"menu_disable_random_password", L"Disable Random Password"},
    {L"menu_change_id", L"Change ID"},
    {L"menu_language", L"Language"},
    {L"menu_about", L"About"},
    {L"button_check_updates", L"Check for Updates"},
    {L"msg_open_project_failed", L"Unable to open the project page."},
    {L"welcome_title", L"Remote Support"},
    {L"hint_provide_id_password", L"Share this ID and password"},
    {L"label_id", L"ID"},
    {L"label_one_time_password", L"Passcode"},
    {L"connection_status_disconnected", L"Not connected"},
    {L"connection_status_authorized", L"Connected (Authorized)"},
    {L"connection_status_remote_user", L"Connected user"},
    {L"button_disconnect_session", L"Disconnect"},
    {L"status_server_ready", L"Server Ready"},
    {L"status_server_connecting", L"Connecting to Server"},
    {L"status_server_unreachable", L"Server Unreachable"},
    {L"status_server_checking", L"Checking Server"},
    {L"dialog_ok", L"OK"},
    {L"dialog_cancel", L"Cancel"},
    {L"msg_startup_update_failed", L"Unable to update the startup setting."},
    {L"dialog_fixed_password_title", L"Set Fixed Password"},
    {L"dialog_fixed_password_prompt", L"Enter the fixed password to save"},
    {L"msg_fixed_password_empty", L"The fixed password cannot be empty."},
    {L"msg_fixed_password_save_failed", L"Failed to save the fixed password."},
    {L"dialog_change_id_title", L"Change ID"},
    {L"dialog_change_id_prompt", L"Enter the new ID (digits only)"},
    {L"msg_id_digits_only", L"The ID can contain digits only."},
    {L"msg_id_length_invalid", L"The ID length must be between 6 and 16 digits."},
    {L"dialog_language_title", L"Select Language File"},
    {L"msg_language_restart_failed", L"The language file was applied, but the application could not restart automatically."},
    {L"incoming_window_title", L"Remote Access"},
    {L"incoming_dismiss", L"Close"},
    {L"incoming_accept", L"Accept"},
    {L"incoming_question", L"Accept?"},
    {L"incoming_body", L"A remote device wants to connect to this device."},
    {L"incoming_unknown_device", L"Unknown Device"},
    {L"incoming_rustdesk_id_prefix", L"RustDesk ID"},
};

const LanguageEntry kTraditionalChineseStatusLanguageEntries[] = {
    {L"status_rendezvous_worker_start_failed", L"\u7121\u6cd5\u555f\u52d5\u4f3a\u670d\u5668\u9023\u7dda\u57f7\u884c\u7dd2\u3002"},
    {L"status_active_session_worker_start_failed", L"\u7121\u6cd5\u555f\u52d5\u5de5\u4f5c\u968e\u6bb5\u57f7\u884c\u7dd2\u3002"},
    {L"status_identity_setup_failed", L"\u8eab\u5206\u521d\u59cb\u5316\u5931\u6557\uff1a{0}"},
    {L"status_id_server_empty", L"id_server \u70ba\u7a7a"},
    {L"status_connecting_udp", L"\u6b63\u5728\u900f\u904e UDP \u9023\u7dda {0}"},
    {L"status_udp_connect_failed", L"UDP \u9023\u7dda\u5931\u6557\uff1a{0}"},
    {L"status_register_pk_udp_send_failed", L"RegisterPk UDP \u50b3\u9001\u5931\u6557\uff1a{0}"},
    {L"status_register_peer_udp_send_failed", L"RegisterPeer UDP \u50b3\u9001\u5931\u6557\uff1a{0}"},
    {L"status_connecting_tcp", L"\u6b63\u5728\u900f\u904e TCP \u9023\u7dda {0}"},
    {L"status_tcp_connect_failed", L"TCP \u9023\u7dda\u5931\u6557\uff1a{0}"},
    {L"status_tcp_secure_handshake_failed", L"TCP \u5b89\u5168\u63e1\u624b\u5931\u6557\uff1a{0}"},
    {L"status_register_pk_tcp_send_failed", L"RegisterPk TCP \u50b3\u9001\u5931\u6557\uff1a{0}"},
    {L"status_hbbs_tcp_timeout", L"hbbs TCP \u903e\u6642"},
    {L"status_hbbs_tcp_closed", L"hbbs TCP \u5df2\u95dc\u9589\u9023\u7dda"},
    {L"status_tcp_receive_failed", L"TCP \u63a5\u6536\u5931\u6557\uff1a{0}"},
    {L"status_hbbs_request_pk_tcp", L"hbbs \u8981\u6c42\u91cd\u65b0\u50b3\u9001 RegisterPk\uff08TCP\uff09"},
    {L"status_registered_tcp", L"\u5df2\u900f\u904e TCP \u8a3b\u518a\u5230 hbbs"},
    {L"status_register_peer_response_tcp", L"\u5df2\u6536\u5230 RegisterPeerResponse\uff08TCP\uff09"},
    {L"status_uuid_mismatch_tcp_retry", L"hbbs \u900f\u904e TCP \u56de\u8986 UUID_MISMATCH\uff1b\u6539\u7528\u65b0 ID {0} \u91cd\u8a66"},
    {L"status_uuid_mismatch_tcp", L"hbbs \u900f\u904e TCP \u56de\u8986 UUID_MISMATCH"},
    {L"status_id_exists_tcp", L"hbbs \u900f\u904e TCP \u56de\u8986 ID_EXISTS"},
    {L"status_not_deployed_tcp", L"hbbs \u900f\u904e TCP \u56de\u8986 NOT_DEPLOYED"},
    {L"status_register_pk_response_tcp", L"RegisterPkResponse\uff08TCP\uff09\uff1a{0}"},
    {L"status_request_relay_tcp_queue_failed", L"\u7121\u6cd5\u6392\u5165 RequestRelay TCP \u5de5\u4f5c\u968e\u6bb5"},
    {L"status_punch_hole_tcp_queue_failed", L"\u7121\u6cd5\u6392\u5165 PunchHole TCP \u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5"},
    {L"status_fetch_local_addr_tcp_relay_queue_failed", L"\u7121\u6cd5\u6392\u5165 FetchLocalAddr TCP \u4e2d\u7e7c\u5099\u63f4"},
    {L"status_fetch_local_addr_tcp_direct_queue_failed", L"\u7121\u6cd5\u6392\u5165 FetchLocalAddr TCP \u76f4\u9023/\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5"},
    {L"status_unhandled_hbbs_tcp_message", L"\u6536\u5230\u672a\u8655\u7406\u7684 hbbs TCP \u8a0a\u606f\uff1a{0}"},
    {L"status_hbbs_udp_no_response_reconnecting", L"hbbs \u5728\u5c31\u7dd2\u524d\u672a\u56de\u61c9 UDP\uff1b\u91cd\u65b0\u9023\u7dda\u4e2d"},
    {L"status_hbbs_udp_timeout", L"hbbs UDP \u903e\u6642"},
    {L"status_udp_receive_failed", L"UDP \u63a5\u6536\u5931\u6557\uff1a{0}"},
    {L"status_hbbs_request_pk_udp", L"hbbs \u8981\u6c42\u91cd\u65b0\u50b3\u9001 RegisterPk\uff08UDP\uff09"},
    {L"status_registered_udp", L"\u5df2\u900f\u904e UDP \u8a3b\u518a\u5230 hbbs"},
    {L"status_register_peer_response_udp", L"\u5df2\u6536\u5230 RegisterPeerResponse\uff08UDP\uff09"},
    {L"status_uuid_mismatch_udp_retry", L"hbbs \u56de\u8986 UUID_MISMATCH\uff1b\u6539\u7528\u65b0 ID {0} \u91cd\u8a66"},
    {L"status_uuid_mismatch_udp", L"hbbs \u56de\u8986 UUID_MISMATCH"},
    {L"status_id_exists_udp", L"hbbs \u56de\u8986 ID_EXISTS"},
    {L"status_not_deployed_udp", L"hbbs \u56de\u8986 NOT_DEPLOYED"},
    {L"status_register_pk_response_udp", L"RegisterPkResponse\uff1a{0}"},
    {L"status_relay_session_queue_failed", L"\u7121\u6cd5\u6392\u5165\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5"},
    {L"status_punch_hole_relay_queue_failed", L"\u7121\u6cd5\u6392\u5165 Punch-hole \u4e2d\u7e7c\u5099\u63f4"},
    {L"status_fetch_local_addr_relay_queue_failed", L"\u7121\u6cd5\u6392\u5165 FetchLocalAddr \u4e2d\u7e7c\u5099\u63f4"},
    {L"status_fetch_local_addr_direct_queue_failed", L"\u7121\u6cd5\u6392\u5165 FetchLocalAddr \u76f4\u9023/\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5"},
    {L"status_unhandled_hbbs_udp_message", L"\u6536\u5230\u672a\u8655\u7406\u7684 hbbs UDP \u8a0a\u606f\uff1a{0}"},
    {L"status_request_relay_tcp_session_failed", L"RequestRelay TCP \u5de5\u4f5c\u968e\u6bb5\u5931\u6557\uff1a{0}"},
    {L"status_punch_hole_tcp_relay_failed", L"PunchHole TCP \u4e2d\u7e7c\u5931\u6557\uff1a{0}"},
    {L"status_fetch_local_addr_tcp_relay_failed", L"FetchLocalAddr TCP \u4e2d\u7e7c\u5931\u6557\uff1a{0}"},
    {L"status_fetch_local_addr_tcp_direct_failed", L"FetchLocalAddr TCP \u76f4\u9023/\u4e2d\u7e7c\u5931\u6557\uff1a{0}"},
    {L"status_relay_session_setup_failed", L"\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5\u5efa\u7acb\u5931\u6557\uff1a{0}"},
    {L"status_punch_hole_relay_fallback_failed", L"Punch-hole \u4e2d\u7e7c\u5099\u63f4\u5931\u6557\uff1a{0}"},
    {L"status_fetch_local_addr_relay_fallback_failed", L"FetchLocalAddr \u4e2d\u7e7c\u5099\u63f4\u5931\u6557\uff1a{0}"},
    {L"status_fetch_local_addr_direct_failed", L"FetchLocalAddr \u76f4\u9023/\u4e2d\u7e7c\u5931\u6557\uff1a{0}"},
    {L"status_online_request_relay_tcp_secure", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 TCP \u4e2d\u7e7c\u8981\u6c42\uff1b\u6b63\u5728\u958b\u555f\u5b89\u5168\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5\uff08{0}\uff09"},
    {L"status_online_request_relay_tcp_plain", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 TCP \u4e2d\u7e7c\u8981\u6c42\uff1b\u6b63\u5728\u958b\u555f\u4e00\u822c\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5\uff08{0}\uff09"},
    {L"status_online_punch_hole_tcp_force_relay", L"\u7dda\u4e0a\u4e2d\uff0cPunchHole \u8981\u6c42\u900f\u904e TCP \u5f37\u5236\u4e2d\u7e7c\uff08{0}\uff09"},
    {L"status_online_punch_hole_tcp_relay", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 TCP PunchHole\uff1b\u6b63\u5728\u958b\u555f\u4e2d\u7e7c\u8def\u5f91\uff08{0}\uff09"},
    {L"status_online_fetch_local_addr_tcp_relay", L"\u7dda\u4e0a\u4e2d\uff0cFetchLocalAddr over TCP \u8981\u6c42\u4e2d\u7e7c\u5099\u63f4"},
    {L"status_online_fetch_local_addr_tcp_direct", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 TCP FetchLocalAddr\uff1b\u6b63\u5728\u5617\u8a66\u5167\u7db2\u76f4\u9023\uff08{0}\uff09"},
    {L"status_online_request_relay_udp_secure", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684\u4e2d\u7e7c\u8981\u6c42\uff1b\u6b63\u5728\u958b\u555f\u5b89\u5168\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5\uff08{0}\uff09"},
    {L"status_online_request_relay_udp_plain", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684\u4e2d\u7e7c\u8981\u6c42\uff1b\u6b63\u5728\u958b\u555f\u4e00\u822c\u4e2d\u7e7c\u5de5\u4f5c\u968e\u6bb5\uff08{0}\uff09"},
    {L"status_online_punch_hole_udp_relay", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 Punch-hole \u8981\u6c42\uff1b\u5207\u63db\u5230\u4e2d\u7e7c\u5099\u63f4\uff08{0}\uff09"},
    {L"status_online_fetch_local_addr_udp_relay", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 FetchLocalAddr\uff1b\u5df2\u555f\u7528\u5f37\u5236\u4e2d\u7e7c\uff0c\u76f4\u63a5\u5207\u63db\u81f3\u4e2d\u7e7c\uff08{0}\uff09"},
    {L"status_online_fetch_local_addr_udp_direct", L"\u7dda\u4e0a\u4e2d\uff0c\u6536\u5230 hbbs \u7684 FetchLocalAddr\uff1b\u6b63\u5728\u5617\u8a66\u5167\u7db2\u76f4\u9023\uff08{0}\uff09"},
};

const LanguageEntry kEnglishStatusLanguageEntries[] = {
    {L"status_rendezvous_worker_start_failed", L"Failed to start the rendezvous worker thread."},
    {L"status_active_session_worker_start_failed", L"Failed to start the active session worker thread."},
    {L"status_identity_setup_failed", L"Identity setup failed: {0}"},
    {L"status_id_server_empty", L"id_server is empty"},
    {L"status_connecting_udp", L"Connecting over UDP to {0}"},
    {L"status_udp_connect_failed", L"UDP connection failed: {0}"},
    {L"status_register_pk_udp_send_failed", L"RegisterPk UDP send failed: {0}"},
    {L"status_register_peer_udp_send_failed", L"RegisterPeer UDP send failed: {0}"},
    {L"status_connecting_tcp", L"Connecting over TCP to {0}"},
    {L"status_tcp_connect_failed", L"TCP connection failed: {0}"},
    {L"status_tcp_secure_handshake_failed", L"TCP secure handshake failed: {0}"},
    {L"status_register_pk_tcp_send_failed", L"RegisterPk TCP send failed: {0}"},
    {L"status_hbbs_tcp_timeout", L"hbbs TCP timeout"},
    {L"status_hbbs_tcp_closed", L"hbbs TCP closed the connection"},
    {L"status_tcp_receive_failed", L"TCP receive failed: {0}"},
    {L"status_hbbs_request_pk_tcp", L"hbbs requested RegisterPk over TCP"},
    {L"status_registered_tcp", L"Registered to hbbs via TCP"},
    {L"status_register_peer_response_tcp", L"RegisterPeerResponse received over TCP"},
    {L"status_uuid_mismatch_tcp_retry", L"UUID_MISMATCH from hbbs over TCP; retrying with new ID {0}"},
    {L"status_uuid_mismatch_tcp", L"UUID_MISMATCH from hbbs over TCP"},
    {L"status_id_exists_tcp", L"ID_EXISTS from hbbs over TCP"},
    {L"status_not_deployed_tcp", L"NOT_DEPLOYED from hbbs over TCP"},
    {L"status_register_pk_response_tcp", L"RegisterPkResponse over TCP: {0}"},
    {L"status_request_relay_tcp_queue_failed", L"Failed to queue the RequestRelay TCP session"},
    {L"status_punch_hole_tcp_queue_failed", L"Failed to queue the PunchHole TCP relay session"},
    {L"status_fetch_local_addr_tcp_relay_queue_failed", L"Failed to queue the FetchLocalAddr TCP relay fallback"},
    {L"status_fetch_local_addr_tcp_direct_queue_failed", L"Failed to queue the FetchLocalAddr TCP direct/relay session"},
    {L"status_unhandled_hbbs_tcp_message", L"Received an unhandled hbbs TCP message: {0}"},
    {L"status_hbbs_udp_no_response_reconnecting", L"hbbs gave no UDP response before ready; reconnecting"},
    {L"status_hbbs_udp_timeout", L"hbbs UDP timeout"},
    {L"status_udp_receive_failed", L"UDP receive failed: {0}"},
    {L"status_hbbs_request_pk_udp", L"hbbs requested RegisterPk over UDP"},
    {L"status_registered_udp", L"Registered to hbbs via UDP"},
    {L"status_register_peer_response_udp", L"RegisterPeerResponse received over UDP"},
    {L"status_uuid_mismatch_udp_retry", L"UUID_MISMATCH from hbbs; retrying with new ID {0}"},
    {L"status_uuid_mismatch_udp", L"UUID_MISMATCH from hbbs"},
    {L"status_id_exists_udp", L"ID_EXISTS from hbbs"},
    {L"status_not_deployed_udp", L"NOT_DEPLOYED from hbbs"},
    {L"status_register_pk_response_udp", L"RegisterPkResponse: {0}"},
    {L"status_relay_session_queue_failed", L"Failed to queue the relay session"},
    {L"status_punch_hole_relay_queue_failed", L"Failed to queue the punch-hole relay fallback"},
    {L"status_fetch_local_addr_relay_queue_failed", L"Failed to queue the FetchLocalAddr relay fallback"},
    {L"status_fetch_local_addr_direct_queue_failed", L"Failed to queue the FetchLocalAddr direct/relay session"},
    {L"status_unhandled_hbbs_udp_message", L"Received an unhandled hbbs UDP message: {0}"},
    {L"status_request_relay_tcp_session_failed", L"RequestRelay TCP session failed: {0}"},
    {L"status_punch_hole_tcp_relay_failed", L"PunchHole TCP relay failed: {0}"},
    {L"status_fetch_local_addr_tcp_relay_failed", L"FetchLocalAddr TCP relay failed: {0}"},
    {L"status_fetch_local_addr_tcp_direct_failed", L"FetchLocalAddr TCP direct/relay failed: {0}"},
    {L"status_relay_session_setup_failed", L"Relay session setup failed: {0}"},
    {L"status_punch_hole_relay_fallback_failed", L"Punch-hole relay fallback failed: {0}"},
    {L"status_fetch_local_addr_relay_fallback_failed", L"FetchLocalAddr relay fallback failed: {0}"},
    {L"status_fetch_local_addr_direct_failed", L"FetchLocalAddr direct/relay failed: {0}"},
    {L"status_online_request_relay_tcp_secure", L"Online, relay request received from hbbs over TCP; opening secure relay session ({0})"},
    {L"status_online_request_relay_tcp_plain", L"Online, relay request received from hbbs over TCP; opening plain relay session ({0})"},
    {L"status_online_punch_hole_tcp_force_relay", L"Online, PunchHole requested relay over TCP ({0})"},
    {L"status_online_punch_hole_tcp_relay", L"Online, PunchHole received from hbbs over TCP; opening relay path ({0})"},
    {L"status_online_fetch_local_addr_tcp_relay", L"Online, FetchLocalAddr over TCP requested relay fallback"},
    {L"status_online_fetch_local_addr_tcp_direct", L"Online, FetchLocalAddr received from hbbs over TCP; trying direct intranet path ({0})"},
    {L"status_online_request_relay_udp_secure", L"Online, relay request received from hbbs; opening secure relay session ({0})"},
    {L"status_online_request_relay_udp_plain", L"Online, relay request received from hbbs; opening plain relay session ({0})"},
    {L"status_online_punch_hole_udp_relay", L"Online, punch-hole request received from hbbs; switching to relay fallback ({0})"},
    {L"status_online_fetch_local_addr_udp_relay", L"Online, FetchLocalAddr received from hbbs; force relay enabled, switching directly to relay ({0})"},
    {L"status_online_fetch_local_addr_udp_direct", L"Online, FetchLocalAddr received from hbbs; trying direct intranet path ({0})"},
};

constexpr int kMouseTypeMask = 0x7;
constexpr int kMouseTypeMove = 0;
constexpr int kMouseTypeDown = 1;
constexpr int kMouseTypeUp = 2;
constexpr int kMouseTypeWheel = 3;
constexpr int kMouseTypeTrackpad = 4;
constexpr int kMouseTypeMoveRelative = 5;
constexpr int kMouseButtonLeft = 0x01;
constexpr int kMouseButtonRight = 0x02;
constexpr int kMouseButtonWheel = 0x04;
constexpr int kMouseButtonBack = 0x08;
constexpr int kMouseButtonForward = 0x10;
constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
const GUID kClsidCmsH264EncoderMft = {
    0x6ca50344, 0x051a, 0x4ded, {0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35}};

int ScaleForSystemDpi(int logical_pixels);

Color ToGdiPlusColor(COLORREF color) {
  return Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void DrawRoundedBlock(
    HDC dc,
    const RECT& rect,
    int radius,
    COLORREF fill_color,
    COLORREF border_color) {
  HBRUSH fill_brush = CreateSolidBrush(fill_color);
  HPEN border_pen = CreatePen(PS_SOLID, 1, border_color);
  if (fill_brush == nullptr || border_pen == nullptr) {
    if (fill_brush != nullptr) {
      DeleteObject(fill_brush);
    }
    if (border_pen != nullptr) {
      DeleteObject(border_pen);
    }
    return;
  }

  HGDIOBJ old_brush = SelectObject(dc, fill_brush);
  HGDIOBJ old_pen = SelectObject(dc, border_pen);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(border_pen);
  DeleteObject(fill_brush);
}

RECT GetControlRectInParent(HWND parent, HWND child) {
  RECT rect = {};
  if (parent == nullptr || child == nullptr || !GetWindowRect(child, &rect)) {
    return rect;
  }
  MapWindowPoints(nullptr, parent, reinterpret_cast<LPPOINT>(&rect), 2);
  return rect;
}

wchar_t GetAvatarGlyph(const std::wstring& text) {
  for (wchar_t c : text) {
    if (!iswspace(c)) {
      return c;
    }
  }
  return L'?';
}

HBITMAP LoadBitmapFromResource(
    HINSTANCE instance,
    UINT resource_id,
    const wchar_t* type_name,
    int target_width = 0,
    int target_height = 0) {
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), type_name);
  if (resource == nullptr) {
    return nullptr;
  }
  HGLOBAL resource_handle = LoadResource(instance, resource);
  if (resource_handle == nullptr) {
    return nullptr;
  }
  const DWORD resource_size = SizeofResource(instance, resource);
  const void* resource_data = LockResource(resource_handle);
  if (resource_data == nullptr || resource_size == 0) {
    return nullptr;
  }

  HGLOBAL copy_handle = GlobalAlloc(GMEM_MOVEABLE, resource_size);
  if (copy_handle == nullptr) {
    return nullptr;
  }
  void* copy_data = GlobalLock(copy_handle);
  if (copy_data == nullptr) {
    GlobalFree(copy_handle);
    return nullptr;
  }
  std::memcpy(copy_data, resource_data, resource_size);
  GlobalUnlock(copy_handle);

  IStream* stream = nullptr;
  if (CreateStreamOnHGlobal(copy_handle, TRUE, &stream) != S_OK || stream == nullptr) {
    GlobalFree(copy_handle);
    return nullptr;
  }

  Bitmap bitmap(stream, FALSE);
  HBITMAP hbitmap = nullptr;
  if (bitmap.GetLastStatus() == Ok) {
    const Color background = ToGdiPlusColor(kWindowColor);
    if (target_width > 0 &&
        target_height > 0 &&
        (bitmap.GetWidth() != static_cast<UINT>(target_width) ||
         bitmap.GetHeight() != static_cast<UINT>(target_height))) {
      Bitmap scaled(target_width, target_height, PixelFormat32bppPARGB);
      Graphics graphics(&scaled);
      graphics.Clear(background);
      graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
      graphics.SetSmoothingMode(SmoothingModeAntiAlias);
      graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
      graphics.DrawImage(&bitmap, Rect(0, 0, target_width, target_height));
      if (scaled.GetLastStatus() == Ok) {
        scaled.GetHBITMAP(background, &hbitmap);
      }
    } else {
      bitmap.GetHBITMAP(background, &hbitmap);
    }
  }
  stream->Release();
  return hbitmap;
}

HBITMAP LoadTintedBitmapFromResource(
    HINSTANCE instance,
    UINT resource_id,
    const wchar_t* type_name,
    COLORREF foreground,
    COLORREF background,
    int target_width = 0,
    int target_height = 0) {
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), type_name);
  if (resource == nullptr) {
    return nullptr;
  }
  HGLOBAL resource_handle = LoadResource(instance, resource);
  if (resource_handle == nullptr) {
    return nullptr;
  }
  const DWORD resource_size = SizeofResource(instance, resource);
  const void* resource_data = LockResource(resource_handle);
  if (resource_data == nullptr || resource_size == 0) {
    return nullptr;
  }

  HGLOBAL copy_handle = GlobalAlloc(GMEM_MOVEABLE, resource_size);
  if (copy_handle == nullptr) {
    return nullptr;
  }
  void* copy_data = GlobalLock(copy_handle);
  if (copy_data == nullptr) {
    GlobalFree(copy_handle);
    return nullptr;
  }
  std::memcpy(copy_data, resource_data, resource_size);
  GlobalUnlock(copy_handle);

  IStream* stream = nullptr;
  if (CreateStreamOnHGlobal(copy_handle, TRUE, &stream) != S_OK || stream == nullptr) {
    GlobalFree(copy_handle);
    return nullptr;
  }

  Bitmap bitmap(stream, FALSE);
  HBITMAP hbitmap = nullptr;
  if (bitmap.GetLastStatus() == Ok) {
    const int width = target_width > 0 ? target_width : static_cast<int>(bitmap.GetWidth());
    const int height = target_height > 0 ? target_height : static_cast<int>(bitmap.GetHeight());
    Bitmap scaled(width, height, PixelFormat32bppPARGB);
    Graphics scaled_graphics(&scaled);
    scaled_graphics.Clear(Color(0, 0, 0, 0));
    scaled_graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    scaled_graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    scaled_graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    scaled_graphics.DrawImage(&bitmap, Rect(0, 0, width, height));

    Bitmap tinted(width, height, PixelFormat32bppPARGB);
    const BYTE red = GetRValue(foreground);
    const BYTE green = GetGValue(foreground);
    const BYTE blue = GetBValue(foreground);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        Color pixel;
        scaled.GetPixel(x, y, &pixel);
        if (pixel.GetA() == 0) {
          tinted.SetPixel(x, y, Color(0, 0, 0, 0));
          continue;
        }
        tinted.SetPixel(x, y, Color(pixel.GetA(), red, green, blue));
      }
    }

    tinted.GetHBITMAP(ToGdiPlusColor(background), &hbitmap);
  }

  stream->Release();
  return hbitmap;
}

void DrawBitmapCentered(HDC dc, const RECT& rect, HBITMAP bitmap) {
  if (dc == nullptr || bitmap == nullptr) {
    return;
  }

  BITMAP bitmap_info = {};
  if (GetObject(bitmap, sizeof(bitmap_info), &bitmap_info) == 0) {
    return;
  }

  HDC memory_dc = CreateCompatibleDC(dc);
  if (memory_dc == nullptr) {
    return;
  }

  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
  const int x = rect.left + ((rect.right - rect.left - bitmap_info.bmWidth) / 2);
  const int y = rect.top + ((rect.bottom - rect.top - bitmap_info.bmHeight) / 2);
  BitBlt(dc, x, y, bitmap_info.bmWidth, bitmap_info.bmHeight, memory_dc, 0, 0, SRCCOPY);
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
}

struct WindowsVersionInfo {
  DWORD major = 0;
  DWORD minor = 0;
};

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

WindowsVersionInfo QueryWindowsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  WindowsVersionInfo info;
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    const auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version != nullptr) {
      RTL_OSVERSIONINFOW version = {};
      version.dwOSVersionInfoSize = sizeof(version);
      if (rtl_get_version(&version) == 0) {
        info.major = version.dwMajorVersion;
        info.minor = version.dwMinorVersion;
        return info;
      }
    }
  }

  OSVERSIONINFOW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
#pragma warning(push)
#pragma warning(disable : 4996)
  if (GetVersionExW(&version)) {
    info.major = version.dwMajorVersion;
    info.minor = version.dwMinorVersion;
  }
#pragma warning(pop)
  return info;
}

bool IsWindowsXpOrEarlier() {
  const WindowsVersionInfo version = QueryWindowsVersion();
  return version.major != 0 && version.major < 6;
}

bool IsWindowsPreinstallationEnvironment() {
  static int cached_result = -1;
  if (cached_result >= 0) {
    return cached_result != 0;
  }

  HKEY key = nullptr;
  const LONG result = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
      0,
      KEY_READ,
      &key);
  if (result == ERROR_SUCCESS) {
    RegCloseKey(key);
    cached_result = 1;
    return true;
  }

  cached_result = 0;
  return false;
}

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::wstring Trim(const std::wstring& value);

std::wstring NormalizePreferredCodec(const std::wstring& preferred_codec) {
  const std::wstring normalized = Trim(preferred_codec);
  if (normalized.empty()) {
    return L"h264";
  }
  if (_wcsicmp(normalized.c_str(), L"vp8-software") == 0 ||
      _wcsicmp(normalized.c_str(), L"vp8") == 0) {
    return L"vp8-software";
  }
  return L"h264";
}

std::wstring GetEffectivePreferredCodec(const std::wstring& preferred_codec) {
  if (IsWindowsXpOrEarlier() || IsWindowsPreinstallationEnvironment()) {
    return L"vp8-software";
  }
  return NormalizePreferredCodec(preferred_codec);
}

bool ParseIniBoolValue(const std::wstring& value, bool default_value) {
  const std::wstring normalized = Trim(value);
  if (normalized.empty()) {
    return default_value;
  }
  if (_wcsicmp(normalized.c_str(), L"1") == 0 ||
      _wcsicmp(normalized.c_str(), L"true") == 0 ||
      _wcsicmp(normalized.c_str(), L"yes") == 0 ||
      _wcsicmp(normalized.c_str(), L"y") == 0 ||
      _wcsicmp(normalized.c_str(), L"on") == 0) {
    return true;
  }
  if (_wcsicmp(normalized.c_str(), L"0") == 0 ||
      _wcsicmp(normalized.c_str(), L"false") == 0 ||
      _wcsicmp(normalized.c_str(), L"no") == 0 ||
      _wcsicmp(normalized.c_str(), L"n") == 0 ||
      _wcsicmp(normalized.c_str(), L"off") == 0) {
    return false;
  }
  return default_value;
}

bool TryFillRandomBytes(void* buffer, size_t size) {
  if (buffer == nullptr || size == 0) {
    return false;
  }

  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(
          &provider,
          nullptr,
          nullptr,
          PROV_RSA_FULL,
          CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    return false;
  }

  const BOOL ok = CryptGenRandom(
      provider,
      static_cast<DWORD>(size),
      static_cast<BYTE*>(buffer));
  CryptReleaseContext(provider, 0);
  return ok != FALSE;
}

struct MediaFoundationFunctions {
  HMODULE module = nullptr;
  HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
  HRESULT(WINAPI* create_media_type)(IMFMediaType**) = nullptr;
  HRESULT(WINAPI* create_sample)(IMFSample**) = nullptr;
  HRESULT(WINAPI* create_memory_buffer)(DWORD, IMFMediaBuffer**) = nullptr;
  bool tried = false;
};

MediaFoundationFunctions g_media_foundation_functions;
HRESULT g_media_foundation_init_result = E_UNEXPECTED;
LONG g_media_foundation_init_state = 0;

MediaFoundationFunctions& GetMediaFoundationFunctions() {
  MediaFoundationFunctions& functions = g_media_foundation_functions;
  if (functions.tried) {
    return functions;
  }
  functions.tried = true;
  if (IsWindowsXpOrEarlier()) {
    return functions;
  }

  functions.module = LoadLibraryW(L"mfplat.dll");
  if (functions.module == nullptr) {
    return functions;
  }

  functions.startup = reinterpret_cast<HRESULT(WINAPI*)(ULONG, DWORD)>(
      GetProcAddress(functions.module, "MFStartup"));
  functions.create_media_type = reinterpret_cast<HRESULT(WINAPI*)(IMFMediaType**)>(
      GetProcAddress(functions.module, "MFCreateMediaType"));
  functions.create_sample = reinterpret_cast<HRESULT(WINAPI*)(IMFSample**)>(
      GetProcAddress(functions.module, "MFCreateSample"));
  functions.create_memory_buffer = reinterpret_cast<HRESULT(WINAPI*)(DWORD, IMFMediaBuffer**)>(
      GetProcAddress(functions.module, "MFCreateMemoryBuffer"));
  if (functions.startup == nullptr ||
      functions.create_media_type == nullptr ||
      functions.create_sample == nullptr ||
      functions.create_memory_buffer == nullptr) {
    if (functions.module != nullptr) {
      FreeLibrary(functions.module);
      functions.module = nullptr;
    }
    functions.startup = nullptr;
    functions.create_media_type = nullptr;
    functions.create_sample = nullptr;
    functions.create_memory_buffer = nullptr;
  }
  return functions;
}

struct ParsedHostPort {
  std::wstring host;
  unsigned short port = 0;
};

struct RegisterPkResponseData {
  int result = 0;
  int keep_alive_ms = 0;
};

struct RegisterPeerResponseData {
  bool request_pk = false;
};

struct RequestRelayData {
  std::vector<unsigned char> socket_addr;
  std::wstring relay_server;
  std::wstring uuid;
  bool secure = false;
};

struct PunchHoleData {
  std::vector<unsigned char> socket_addr;
  std::wstring relay_server;
  bool force_relay = false;
};

struct FetchLocalAddrData {
  std::vector<unsigned char> socket_addr;
  std::wstring relay_server;
};

struct PublicKeyMessageData {
  std::vector<unsigned char> asymmetric_value;
  std::vector<unsigned char> symmetric_value;
};

struct RelayResponseData {
  std::wstring uuid;
  std::wstring relay_server;
  std::wstring refuse_reason;
  std::vector<unsigned char> signed_peer_public_key;
};

struct PunchHoleResponseData {
  std::vector<unsigned char> socket_addr;
  std::vector<unsigned char> signed_peer_public_key;
  std::wstring relay_server;
  std::wstring other_failure;
  int failure = 0;
};

struct LoginRequestData {
  std::vector<unsigned char> password;
  std::wstring my_id;
  std::wstring my_name;
  std::wstring version;
  std::wstring my_platform;
  bool has_session_id = false;
  uint64_t session_id = 0;
  bool has_file_transfer = false;
  std::wstring file_transfer_dir;
  bool file_transfer_show_hidden = false;
};

enum class FileTransferActionKind {
  kNone,
  kReadDir,
  kSend,
  kReceive,
  kCreate,
  kRemoveDir,
  kRemoveFile,
  kAllFiles,
  kCancel,
  kSendConfirm,
  kRename,
  kReadEmptyDirs,
};

enum class FileTransferResponseKind {
  kNone,
  kDir,
  kBlock,
  kError,
  kDone,
  kDigest,
  kEmptyDirs,
};

enum class FileTransferEntryType {
  kDir = 0,
  kDirLink = 2,
  kDirDrive = 3,
  kFile = 4,
  kFileLink = 5,
};

struct FileTransferEntryData {
  int entry_type = static_cast<int>(FileTransferEntryType::kFile);
  std::wstring name;
  bool is_hidden = false;
  uint64_t size = 0;
  uint64_t modified_time = 0;
};

struct FileTransferReadDirData {
  std::wstring path;
  bool include_hidden = false;
};

struct FileTransferSendRequestData {
  int id = 0;
  std::wstring path;
  bool include_hidden = false;
  int file_num = 0;
  int file_type = 0;
};

struct FileTransferReceiveRequestData {
  int id = 0;
  std::wstring path;
  std::vector<FileTransferEntryData> files;
  int file_num = 0;
  uint64_t total_size = 0;
};

struct FileTransferRemoveDirData {
  int id = 0;
  std::wstring path;
  bool recursive = false;
};

struct FileTransferRemoveFileData {
  int id = 0;
  std::wstring path;
  int file_num = 0;
};

struct FileTransferCreateDirData {
  int id = 0;
  std::wstring path;
};

struct FileTransferReadAllFilesData {
  int id = 0;
  std::wstring path;
  bool include_hidden = false;
};

struct FileTransferCancelData {
  int id = 0;
};

struct FileTransferSendConfirmData {
  int id = 0;
  int file_num = 0;
  bool has_skip = false;
  bool skip = false;
  bool has_offset_blk = false;
  unsigned int offset_blk = 0;
};

struct FileTransferRenameData {
  int id = 0;
  std::wstring path;
  std::wstring new_name;
};

struct FileTransferDoneData {
  int id = 0;
  int file_num = 0;
};

struct FileTransferDigestData {
  int id = 0;
  int file_num = 0;
  uint64_t last_modified = 0;
  uint64_t file_size = 0;
  bool is_upload = false;
  bool is_identical = false;
  uint64_t transferred_size = 0;
  bool is_resume = false;
};

struct FileTransferBlockData {
  int id = 0;
  int file_num = 0;
  std::vector<unsigned char> data;
  bool compressed = false;
  unsigned int blk_id = 0;
};

struct FileTransferErrorData {
  int id = 0;
  std::wstring error;
  int file_num = 0;
};

struct FileTransferActionData {
  FileTransferActionKind kind = FileTransferActionKind::kNone;
  FileTransferReadDirData read_dir;
  FileTransferSendRequestData send;
  FileTransferReceiveRequestData receive;
  FileTransferCreateDirData create;
  FileTransferRemoveDirData remove_dir;
  FileTransferRemoveFileData remove_file;
  FileTransferReadAllFilesData all_files;
  FileTransferCancelData cancel;
  FileTransferSendConfirmData send_confirm;
  FileTransferRenameData rename;
};

struct FileTransferResponseData {
  FileTransferResponseKind kind = FileTransferResponseKind::kNone;
  int dir_id = 0;
  std::wstring dir_path;
  std::vector<FileTransferEntryData> dir_entries;
  FileTransferBlockData block;
  FileTransferErrorData error;
  FileTransferDoneData done;
  FileTransferDigestData digest;
  std::wstring empty_dirs_path;
};

struct FileTransferReadJob {
  int id = 0;
  std::wstring source_path;
  std::vector<FileTransferEntryData> files;
  int file_num = 0;
  HANDLE file = INVALID_HANDLE_VALUE;
  bool sent_digest = false;
  bool waiting_for_confirm = false;
  bool file_confirmed = false;
  uint64_t resume_offset = 0;
};

struct FileTransferWriteJob {
  int id = 0;
  std::wstring target_path;
  std::vector<FileTransferEntryData> files;
  int current_file_num = -1;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::wstring current_temp_path;
  std::wstring current_final_path;
};

struct HashMessageData {
  std::string salt;
  std::string challenge;
};

struct MouseEventData {
  int mask = 0;
  int x = 0;
  int y = 0;
  std::vector<int> modifiers;
};

std::atomic<bool> g_rustdesk_last_absolute_mouse_valid{false};
std::atomic<LONG> g_rustdesk_last_absolute_mouse_x{0};
std::atomic<LONG> g_rustdesk_last_absolute_mouse_y{0};

struct KeyEventData {
  bool down = false;
  bool press = false;
  bool has_control_key = false;
  int control_key = 0;
  bool has_chr = false;
  unsigned int chr = 0;
  bool has_unicode = false;
  unsigned int unicode = 0;
  std::wstring seq;
  bool has_win2win_hotkey = false;
  unsigned int win2win_hotkey = 0;
  std::vector<int> modifiers;
  int mode = 0;
};

struct ClipboardMessageData {
  bool compress = false;
  std::vector<unsigned char> content;
  int width = 0;
  int height = 0;
  int format = 0;
  std::wstring special_name;
};

struct FormattedTextClipboardContent {
  bool has_text = false;
  std::wstring text;
  bool has_html = false;
  std::vector<unsigned char> html;
  bool has_rtf = false;
  std::vector<unsigned char> rtf;
  // Excel prefers the native "XML Spreadsheet" clipboard format over
  // HTML and plain text. Preserving it keeps cell styling and line breaks.
  bool has_excel_xml = false;
  std::vector<unsigned char> excel_xml;
};

enum class CliprdrMessageKind {
  kNone,
  kReady,
  kFormatList,
  kFormatListResponse,
  kFormatDataRequest,
  kFormatDataResponse,
  kFileContentsRequest,
  kFileContentsResponse,
  kTryEmpty,
  kFiles,
};

struct CliprdrFormatData {
  int id = 0;
  std::wstring format_name;
};

struct CliprdrFileAuditEntry {
  std::wstring name;
  uint64_t size = 0;
};

struct CliprdrMessageData {
  CliprdrMessageKind kind = CliprdrMessageKind::kNone;
  std::vector<CliprdrFormatData> formats;
  int msg_flags = 0;
  int requested_format_id = 0;
  std::vector<unsigned char> payload;
  int stream_id = 0;
  int list_index = 0;
  int dw_flags = 0;
  int n_position_low = 0;
  int n_position_high = 0;
  int cb_requested = 0;
  bool have_clip_data_id = false;
  int clip_data_id = 0;
  std::vector<CliprdrFileAuditEntry> files;
};

struct SessionMessageType {
  bool has_mouse = false;
  MouseEventData mouse;
  bool has_key = false;
  KeyEventData key;
  bool wants_refresh_video = false;
  bool has_close_reason = false;
  std::wstring close_reason;
  std::vector<ClipboardMessageData> clipboards;
  bool has_cliprdr = false;
  CliprdrMessageData cliprdr;
};

UINT GetFileDescriptorClipboardFormat();
UINT GetFileContentsClipboardFormat();
std::vector<unsigned char> EncodeCliprdrFileContentsRequestMessage(
    int stream_id,
    int list_index,
    int dw_flags,
    int n_position_low,
    int n_position_high,
    int cb_requested);

class TcpFramedConnection;
class RemoteFileClipboardBridge;
class VirtualFileDataObject;

struct RemoteClipboardFileDescriptor {
  FILEDESCRIPTORW descriptor = {};
  uint64_t size = 0;
  bool size_known = false;
  bool is_directory = false;
  std::wstring display_name;
};

struct PendingRemoteClipboardFileRequest {
  PendingRemoteClipboardFileRequest()
      : completion_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

  ~PendingRemoteClipboardFileRequest() {
    if (completion_event != nullptr) {
      CloseHandle(completion_event);
      completion_event = nullptr;
    }
  }

  int stream_id = 0;
  bool completed = false;
  bool succeeded = false;
  std::vector<unsigned char> payload;
  std::wstring error_text;
  HANDLE completion_event = nullptr;
};

struct InstallRemoteFileClipboardRequest {
  std::shared_ptr<RemoteFileClipboardBridge> bridge;
};

struct DesktopCaptureBounds {
  int origin_x = 0;
  int origin_y = 0;
  int width = 0;
  int height = 0;
};

struct DesktopFrameBgra {
  int origin_x = 0;
  int origin_y = 0;
  int width = 0;
  int height = 0;
  std::vector<unsigned char> pixels;
};

struct ClipboardStagedFileDescriptor {
  std::wstring relative_path;
  std::wstring staged_path;
  uint64_t size = 0;
  bool is_directory = false;
  bool is_top_level = false;
};

struct LocalClipboardFileDescriptor {
  FILEDESCRIPTORW descriptor = {};
  std::wstring absolute_path;
  uint64_t size = 0;
  bool is_directory = false;
};

struct PendingFileClipboardTransfer {
  bool active = false;
  std::wstring staging_root;
  int file_descriptor_format_id = 0;
  int file_contents_format_id = 0;
  int next_stream_id = 1;
  int active_entry_index = -1;
  int active_stream_id = 0;
  uint64_t active_offset = 0;
  uint64_t active_expected_size = 0;
  size_t last_requested_size = 0;
  bool waiting_for_size = false;
  bool waiting_for_range = false;
  std::vector<ClipboardStagedFileDescriptor> entries;
  std::vector<std::wstring> top_level_paths;
};

struct ParsedServerFrame {
  std::vector<unsigned int> observed_fields;
  bool has_register_peer_response = false;
  RegisterPeerResponseData register_peer_response;
  bool has_key_exchange = false;
  std::vector<std::vector<unsigned char>> key_exchange_keys;
  bool has_register_pk_response = false;
  RegisterPkResponseData register_pk_response;
  bool has_request_relay = false;
  RequestRelayData request_relay;
  bool has_punch_hole = false;
  PunchHoleData punch_hole;
  bool has_fetch_local_addr = false;
  FetchLocalAddrData fetch_local_addr;
};

std::wstring HResultToText(HRESULT hr) {
  std::wstringstream stream;
  stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return stream.str();
}

bool EnsureMediaFoundationInitialized(std::wstring* error_text) {
  LONG init_state = InterlockedCompareExchange(&g_media_foundation_init_state, 1, 0);
  if (init_state == 0) {
    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE) {
      g_media_foundation_init_result = coinit;
    } else if (IsWindowsXpOrEarlier()) {
      g_media_foundation_init_result = E_NOTIMPL;
    } else {
      MediaFoundationFunctions& functions = GetMediaFoundationFunctions();
      if (functions.startup == nullptr) {
        g_media_foundation_init_result = E_NOTIMPL;
      } else {
        g_media_foundation_init_result = functions.startup(MF_VERSION, MFSTARTUP_NOSOCKET);
      }
    }
    InterlockedExchange(&g_media_foundation_init_state, 2);
  } else {
    while (InterlockedCompareExchange(&g_media_foundation_init_state, 2, 2) != 2) {
      Sleep(1);
    }
  }
  if (FAILED(g_media_foundation_init_result)) {
    if (error_text != nullptr) {
      *error_text =
          L"Media Foundation init failed: " + HResultToText(g_media_foundation_init_result);
    }
    return false;
  }
  return true;
}

int NormalizeVideoDimension(int value) {
  if (value < 2) {
    return 2;
  }
  return value & ~1;
}

unsigned char ClampByte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return static_cast<unsigned char>(value);
}

int ClampInt(int value, int minimum, int maximum) {
  if (maximum < minimum) {
    return minimum;
  }
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

bool EnableBestEffortDpiAwareness() {
  using SetProcessDpiAwareFn = BOOL(WINAPI*)();
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return false;
  }
  const auto set_process_dpi_aware =
      reinterpret_cast<SetProcessDpiAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
  return set_process_dpi_aware != nullptr && set_process_dpi_aware() != FALSE;
}

HRESULT CreateMediaTypeCompat(IMFMediaType** media_type) {
  MediaFoundationFunctions& functions = GetMediaFoundationFunctions();
  return functions.create_media_type != nullptr
             ? functions.create_media_type(media_type)
             : E_NOTIMPL;
}

HRESULT CreateSampleCompat(IMFSample** sample) {
  MediaFoundationFunctions& functions = GetMediaFoundationFunctions();
  return functions.create_sample != nullptr
             ? functions.create_sample(sample)
             : E_NOTIMPL;
}

HRESULT CreateMemoryBufferCompat(DWORD size, IMFMediaBuffer** buffer) {
  MediaFoundationFunctions& functions = GetMediaFoundationFunctions();
  return functions.create_memory_buffer != nullptr
             ? functions.create_memory_buffer(size, buffer)
             : E_NOTIMPL;
}

HRESULT SetAttributeSizeCompat(IMFAttributes* attributes, const GUID& key, UINT32 width, UINT32 height) {
  if (attributes == nullptr) {
    return E_POINTER;
  }
  const UINT64 value =
      (static_cast<UINT64>(width) << 32U) | static_cast<UINT64>(height);
  return attributes->SetUINT64(key, value);
}

HRESULT SetAttributeRatioCompat(IMFAttributes* attributes, const GUID& key, UINT32 numerator, UINT32 denominator) {
  if (attributes == nullptr) {
    return E_POINTER;
  }
  const UINT64 value =
      (static_cast<UINT64>(numerator) << 32U) | static_cast<UINT64>(denominator);
  return attributes->SetUINT64(key, value);
}

bool EnsureDirectoryExistsRecursive(const std::wstring& directory_path, std::wstring* error_text) {
  if (directory_path.empty()) {
    return true;
  }

  std::wstring normalized = directory_path;
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  size_t start = 0;
  if (normalized.size() >= 2 && normalized[1] == L':') {
    start = 3;
  } else if (normalized.size() >= 2 && normalized[0] == L'\\' && normalized[1] == L'\\') {
    size_t first = normalized.find(L'\\', 2);
    if (first == std::wstring::npos) {
      return true;
    }
    size_t second = normalized.find(L'\\', first + 1);
    if (second == std::wstring::npos) {
      return true;
    }
    start = second + 1;
  }

  while (start <= normalized.size()) {
    const size_t separator = normalized.find(L'\\', start);
    const std::wstring partial =
        separator == std::wstring::npos ? normalized : normalized.substr(0, separator);
    if (!partial.empty() && partial.back() != L':') {
      if (!CreateDirectoryW(partial.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
          if (error_text != nullptr) {
            *error_text = L"CreateDirectoryW failed";
          }
          return false;
        }
      }
    }
    if (separator == std::wstring::npos) {
      break;
    }
    start = separator + 1;
  }
  return true;
}

UINT GetSystemDpi() {
  HDC screen_dc = GetDC(nullptr);
  if (screen_dc == nullptr) {
    return 96U;
  }
  const int dpi = GetDeviceCaps(screen_dc, LOGPIXELSX);
  ReleaseDC(nullptr, screen_dc);
  return dpi > 0 ? static_cast<UINT>(dpi) : 96U;
}

int ScaleForSystemDpi(int logical_pixels) {
  return MulDiv(logical_pixels, static_cast<int>(GetSystemDpi()), 96);
}

bool QueryDesktopBoundsFromDisplaySettings(DesktopCaptureBounds* bounds) {
  if (bounds == nullptr) {
    return false;
  }

  bool found_any = false;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  for (DWORD adapter_index = 0;; ++adapter_index) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, adapter_index, &adapter, 0)) {
      break;
    }
    if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0 ||
        (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
      continue;
    }

    const int monitor_left = mode.dmPosition.x;
    const int monitor_top = mode.dmPosition.y;
    const int monitor_right = monitor_left + static_cast<int>(mode.dmPelsWidth);
    const int monitor_bottom = monitor_top + static_cast<int>(mode.dmPelsHeight);
    if (monitor_right <= monitor_left || monitor_bottom <= monitor_top) {
      continue;
    }

    if (!found_any) {
      left = monitor_left;
      top = monitor_top;
      right = monitor_right;
      bottom = monitor_bottom;
      found_any = true;
    } else {
      if (monitor_left < left) {
        left = monitor_left;
      }
      if (monitor_top < top) {
        top = monitor_top;
      }
      if (monitor_right > right) {
        right = monitor_right;
      }
      if (monitor_bottom > bottom) {
        bottom = monitor_bottom;
      }
    }
  }

  if (!found_any) {
    return false;
  }

  bounds->origin_x = left;
  bounds->origin_y = top;
  bounds->width = right - left;
  bounds->height = bottom - top;
  return bounds->width > 0 && bounds->height > 0;
}

DesktopCaptureBounds GetDesktopCaptureBounds() {
  DesktopCaptureBounds bounds;
  if (!QueryDesktopBoundsFromDisplaySettings(&bounds)) {
    bounds.origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  }
  if (bounds.width < 1 || bounds.height < 1) {
    bounds.origin_x = 0;
    bounds.origin_y = 0;
    HDC screen_dc = GetDC(nullptr);
    if (screen_dc != nullptr) {
      bounds.width = GetDeviceCaps(screen_dc, DESKTOPHORZRES);
      bounds.height = GetDeviceCaps(screen_dc, DESKTOPVERTRES);
      ReleaseDC(nullptr, screen_dc);
    }
    if (bounds.width < 1 || bounds.height < 1) {
      bounds.width = GetSystemMetrics(SM_CXSCREEN);
      bounds.height = GetSystemMetrics(SM_CYSCREEN);
    }
  }
  if (bounds.width < 1) {
    bounds.width = 1;
  }
  if (bounds.height < 1) {
    bounds.height = 1;
  }
  bounds.width = NormalizeVideoDimension(bounds.width);
  bounds.height = NormalizeVideoDimension(bounds.height);
  return bounds;
}

class GdiScreenCapturer {
 public:
  GdiScreenCapturer() = default;

  ~GdiScreenCapturer() {
    if (bitmap_ != nullptr) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    if (memory_dc_ != nullptr) {
      DeleteDC(memory_dc_);
      memory_dc_ = nullptr;
    }
    if (screen_dc_ != nullptr) {
      ReleaseDC(nullptr, screen_dc_);
      screen_dc_ = nullptr;
    }
  }

  bool Capture(DesktopFrameBgra* frame, std::wstring* error_text) {
    if (frame == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"capture frame output is null";
      }
      return false;
    }

    const DesktopCaptureBounds bounds = GetDesktopCaptureBounds();
    if (!EnsureBitmap(bounds.width, bounds.height, error_text)) {
      return false;
    }

    if (!BitBlt(
            memory_dc_,
            0,
            0,
            bounds.width,
            bounds.height,
            screen_dc_,
            bounds.origin_x,
            bounds.origin_y,
            SRCCOPY | CAPTUREBLT)) {
      if (error_text != nullptr) {
        *error_text = L"BitBlt failed";
      }
      return false;
    }

    frame->origin_x = bounds.origin_x;
    frame->origin_y = bounds.origin_y;
    frame->width = bounds.width;
    frame->height = bounds.height;
    frame->pixels.resize(static_cast<size_t>(bounds.width) * static_cast<size_t>(bounds.height) * 4U);
    std::memcpy(frame->pixels.data(), bits_, frame->pixels.size());
    return true;
  }

 private:
  bool EnsureBitmap(int width, int height, std::wstring* error_text) {
    if (screen_dc_ == nullptr) {
      screen_dc_ = GetDC(nullptr);
      if (screen_dc_ == nullptr) {
        if (error_text != nullptr) {
          *error_text = L"GetDC failed";
        }
        return false;
      }
    }
    if (memory_dc_ == nullptr) {
      memory_dc_ = CreateCompatibleDC(screen_dc_);
      if (memory_dc_ == nullptr) {
        if (error_text != nullptr) {
          *error_text = L"CreateCompatibleDC failed";
        }
        return false;
      }
    }
    if (bitmap_ != nullptr && width_ == width && height_ == height && bits_ != nullptr) {
      return true;
    }

    if (bitmap_ != nullptr) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
      bits_ = nullptr;
    }

    BITMAPINFO bitmap_info = {};
    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(
        memory_dc_,
        &bitmap_info,
        DIB_RGB_COLORS,
        &bits_,
        nullptr,
        0);
    if (bitmap_ == nullptr || bits_ == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"CreateDIBSection failed";
      }
      return false;
    }
    SelectObject(memory_dc_, bitmap_);
    width_ = width;
    height_ = height;
    return true;
  }

  HDC screen_dc_ = nullptr;
  HDC memory_dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  void* bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

void ConvertBgraToNv12(
    const DesktopFrameBgra& frame,
    std::vector<unsigned char>* nv12) {
  const int width = frame.width;
  const int height = frame.height;
  const size_t y_plane_size = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t uv_plane_size = y_plane_size / 2U;
  nv12->assign(y_plane_size + uv_plane_size, 0);

  unsigned char* y_plane = nv12->data();
  unsigned char* uv_plane = nv12->data() + y_plane_size;

  const unsigned char* bgra = frame.pixels.data();
  const size_t stride = static_cast<size_t>(width) * 4U;

  for (int y = 0; y < height; y += 2) {
    for (int x = 0; x < width; x += 2) {
      int u_acc = 0;
      int v_acc = 0;
      int uv_count = 0;
      for (int dy = 0; dy < 2; ++dy) {
        const unsigned char* row = bgra + static_cast<size_t>(y + dy) * stride;
        for (int dx = 0; dx < 2; ++dx) {
          const unsigned char* pixel = row + static_cast<size_t>(x + dx) * 4U;
          const int b = pixel[0];
          const int g = pixel[1];
          const int r = pixel[2];

          const int y_value = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
          const int u_value = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
          const int v_value = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

          y_plane[static_cast<size_t>(y + dy) * static_cast<size_t>(width) + static_cast<size_t>(x + dx)] =
              ClampByte(y_value);
          u_acc += u_value;
          v_acc += v_value;
          ++uv_count;
        }
      }

      const size_t uv_index =
          (static_cast<size_t>(y) / 2U) * static_cast<size_t>(width) + static_cast<size_t>(x);
      uv_plane[uv_index] = ClampByte(u_acc / uv_count);
      uv_plane[uv_index + 1U] = ClampByte(v_acc / uv_count);
    }
  }
}

bool ConvertBgraToI420(
    const DesktopFrameBgra& frame,
    std::vector<unsigned char>* i420,
    std::wstring* error_text) {
  if (i420 == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"I420 output buffer is null";
    }
    return false;
  }

  const int width = frame.width;
  const int height = frame.height;
  const int y_stride = width;
  const int uv_stride = (width + 1) / 2;
  const size_t y_size = static_cast<size_t>(y_stride) * static_cast<size_t>(height);
  const size_t uv_height = static_cast<size_t>((height + 1) / 2);
  const size_t uv_size = static_cast<size_t>(uv_stride) * uv_height;
  i420->assign(y_size + uv_size + uv_size, 0);

  uint8_t* y_plane = i420->data();
  uint8_t* u_plane = i420->data() + y_size;
  uint8_t* v_plane = u_plane + uv_size;
  // Windows 32-bit BI_RGB DIB pixels are laid out as BGRA bytes in memory.
  // libyuv names this little-endian layout as ARGB, so ARGBToI420 is the
  // correct conversion here. Using BGRAToI420 swaps channels and produces the
  // yellow/brown tint seen on the XP VP8 path.
  const int result = libyuv::ARGBToI420(
      frame.pixels.data(),
      width * 4,
      y_plane,
      y_stride,
      u_plane,
      uv_stride,
      v_plane,
      uv_stride,
      width,
      height);
  if (result != 0) {
    if (error_text != nullptr) {
      *error_text = L"ARGBToI420 failed";
    }
    i420->clear();
    return false;
  }
  return true;
}

bool IsAnnexBBuffer(const std::vector<unsigned char>& bytes) {
  return bytes.size() >= 4 &&
         bytes[0] == 0x00 &&
         bytes[1] == 0x00 &&
         ((bytes[2] == 0x00 && bytes[3] == 0x01) || bytes[2] == 0x01);
}

bool AnnexBContainsIdr(const std::vector<unsigned char>& bytes) {
  for (size_t i = 0; i + 4 < bytes.size(); ++i) {
    if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 &&
        ((bytes[i + 2] == 0x01) || (bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01))) {
      const size_t nal_index = (bytes[i + 2] == 0x01) ? (i + 3) : (i + 4);
      if (nal_index < bytes.size()) {
        const unsigned char nal_type = bytes[nal_index] & 0x1FU;
        if (nal_type == 5U) {
          return true;
        }
      }
    }
  }
  return false;
}

bool ParseAvccSequenceHeader(
    const std::vector<unsigned char>& avcc,
    int* nal_length_size,
    std::vector<unsigned char>* annexb_header) {
  if (avcc.size() < 7U || avcc[0] != 1U) {
    return false;
  }
  *nal_length_size = (avcc[4] & 0x03U) + 1U;
  annexb_header->clear();

  size_t offset = 5;
  const unsigned int sps_count = avcc[offset] & 0x1FU;
  ++offset;
  for (unsigned int index = 0; index < sps_count; ++index) {
    if (offset + 2U > avcc.size()) {
      return false;
    }
    const unsigned int size = (static_cast<unsigned int>(avcc[offset]) << 8U) |
                              static_cast<unsigned int>(avcc[offset + 1U]);
    offset += 2U;
    if (offset + size > avcc.size()) {
      return false;
    }
    annexb_header->insert(annexb_header->end(), {0x00, 0x00, 0x00, 0x01});
    annexb_header->insert(annexb_header->end(), avcc.begin() + static_cast<std::ptrdiff_t>(offset),
                          avcc.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
  }

  if (offset >= avcc.size()) {
    return false;
  }
  const unsigned int pps_count = avcc[offset];
  ++offset;
  for (unsigned int index = 0; index < pps_count; ++index) {
    if (offset + 2U > avcc.size()) {
      return false;
    }
    const unsigned int size = (static_cast<unsigned int>(avcc[offset]) << 8U) |
                              static_cast<unsigned int>(avcc[offset + 1U]);
    offset += 2U;
    if (offset + size > avcc.size()) {
      return false;
    }
    annexb_header->insert(annexb_header->end(), {0x00, 0x00, 0x00, 0x01});
    annexb_header->insert(annexb_header->end(), avcc.begin() + static_cast<std::ptrdiff_t>(offset),
                          avcc.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
  }
  return !annexb_header->empty();
}

bool ConvertLengthPrefixedH264ToAnnexB(
    const std::vector<unsigned char>& sample,
    int nal_length_size,
    std::vector<unsigned char>* annexb,
    bool* contains_idr) {
  annexb->clear();
  *contains_idr = false;
  size_t offset = 0;
  while (offset + static_cast<size_t>(nal_length_size) <= sample.size()) {
    uint32_t nal_size = 0;
    for (int index = 0; index < nal_length_size; ++index) {
      nal_size = (nal_size << 8U) | sample[offset + static_cast<size_t>(index)];
    }
    offset += static_cast<size_t>(nal_length_size);
    if (nal_size == 0 || offset + nal_size > sample.size()) {
      return false;
    }
    annexb->insert(annexb->end(), {0x00, 0x00, 0x00, 0x01});
    if ((sample[offset] & 0x1FU) == 5U) {
      *contains_idr = true;
    }
    annexb->insert(
        annexb->end(),
        sample.begin() + static_cast<std::ptrdiff_t>(offset),
        sample.begin() + static_cast<std::ptrdiff_t>(offset + nal_size));
    offset += nal_size;
  }
  return offset == sample.size() && !annexb->empty();
}

class MinimalH264Encoder {
 public:
  bool Initialize(int width, int height, int fps, int bitrate_kbps, std::wstring* error_text) {
    Reset();
    if (!EnsureMediaFoundationInitialized(error_text)) {
      return false;
    }

    const HRESULT create_hr = CoCreateInstance(
        kClsidCmsH264EncoderMft,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(transform_.GetAddressOf()));
    if (FAILED(create_hr)) {
      if (error_text != nullptr) {
        *error_text = L"CoCreateInstance(CMSH264EncoderMFT) failed: " + HResultToText(create_hr);
      }
      return false;
    }
    transform_.As(&codec_api_);

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform_->GetAttributes(attributes.GetAddressOf())) && attributes) {
      attributes->SetUINT32(kCodecApiAvLowLatencyMode, TRUE);
    }

    width_ = static_cast<UINT32>(NormalizeVideoDimension(width));
    height_ = static_cast<UINT32>(NormalizeVideoDimension(height));
    if (fps < 1) {
      fps = 1;
    }
    fps_ = static_cast<UINT32>(fps);

    ComPtr<IMFMediaType> output_type;
    if (FAILED(CreateMediaTypeCompat(output_type.GetAddressOf()))) {
      if (error_text != nullptr) {
        *error_text = L"MFCreateMediaType(output) failed";
      }
      return false;
    }
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_kbps * 1000));
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output_type->SetUINT32(MF_MT_MPEG2_PROFILE, kAvEncH264ProfileBase);
    SetAttributeSizeCompat(output_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    SetAttributeRatioCompat(output_type.Get(), MF_MT_FRAME_RATE, fps_, 1);
    SetAttributeRatioCompat(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    const HRESULT set_output_hr = transform_->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(set_output_hr)) {
      if (error_text != nullptr) {
        *error_text = L"SetOutputType(H264) failed: " + HResultToText(set_output_hr);
      }
      return false;
    }

    ComPtr<IMFMediaType> input_type;
    if (FAILED(CreateMediaTypeCompat(input_type.GetAddressOf()))) {
      if (error_text != nullptr) {
        *error_text = L"MFCreateMediaType(input) failed";
      }
      return false;
    }
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input_type->SetUINT32(MF_MT_SAMPLE_SIZE, width_ * height_ * 3U / 2U);
    input_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    SetAttributeSizeCompat(input_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    SetAttributeRatioCompat(input_type.Get(), MF_MT_FRAME_RATE, fps_, 1);
    SetAttributeRatioCompat(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    const HRESULT set_input_hr = transform_->SetInputType(0, input_type.Get(), 0);
    if (FAILED(set_input_hr)) {
      if (error_text != nullptr) {
        *error_text = L"SetInputType(NV12) failed: " + HResultToText(set_input_hr);
      }
      return false;
    }

    const HRESULT stream_info_hr = transform_->GetOutputStreamInfo(0, &output_stream_info_);
    if (FAILED(stream_info_hr)) {
      if (error_text != nullptr) {
        *error_text = L"GetOutputStreamInfo failed: " + HResultToText(stream_info_hr);
      }
      return false;
    }

    RefreshOutputMetadata();
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    initialized_ = true;
    return true;
  }

  void Reset() {
    transform_.Reset();
    codec_api_.Reset();
    output_stream_info_ = {};
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    initialized_ = false;
    avcc_nal_length_size_ = 4;
    annexb_sequence_header_.clear();
  }

  bool IsInitializedFor(int width, int height) const {
    return initialized_ &&
           width_ == static_cast<UINT32>(NormalizeVideoDimension(width)) &&
           height_ == static_cast<UINT32>(NormalizeVideoDimension(height));
  }

  bool EncodeFrame(
      const std::vector<unsigned char>& nv12,
      bool request_keyframe,
      int64_t pts_ms,
      std::vector<std::vector<unsigned char>>* access_units,
      std::vector<bool>* key_flags,
      std::wstring* error_text) {
    access_units->clear();
    key_flags->clear();
    if (!initialized_) {
      if (error_text != nullptr) {
        *error_text = L"H264 encoder is not initialized";
      }
      return false;
    }

    if (codec_api_ && request_keyframe) {
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_UI4;
      value.ulVal = 1;
      codec_api_->SetValue(&kCodecApiAvEncVideoForceKeyFrame, &value);
    }

    ComPtr<IMFSample> input_sample;
    ComPtr<IMFMediaBuffer> input_buffer;
    const HRESULT sample_hr = CreateSampleCompat(input_sample.GetAddressOf());
    const HRESULT buffer_hr = CreateMemoryBufferCompat(
        static_cast<DWORD>(nv12.size()),
        input_buffer.GetAddressOf());
    if (FAILED(sample_hr) || FAILED(buffer_hr)) {
      if (error_text != nullptr) {
        *error_text = L"failed to create encoder input sample";
      }
      return false;
    }

    BYTE* locked = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    if (FAILED(input_buffer->Lock(&locked, &max_length, &current_length))) {
      if (error_text != nullptr) {
        *error_text = L"input buffer lock failed";
      }
      return false;
    }
    std::memcpy(locked, nv12.data(), nv12.size());
    input_buffer->Unlock();
    input_buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));
    input_sample->AddBuffer(input_buffer.Get());
    input_sample->SetSampleTime(pts_ms * 10000LL);
    input_sample->SetSampleDuration(10000000LL / static_cast<LONGLONG>(fps_));

    const HRESULT process_input_hr = transform_->ProcessInput(0, input_sample.Get(), 0);
    if (FAILED(process_input_hr)) {
      if (error_text != nullptr) {
        *error_text = L"ProcessInput failed: " + HResultToText(process_input_hr);
      }
      return false;
    }

    while (true) {
      ComPtr<IMFSample> output_sample;
      ComPtr<IMFMediaBuffer> output_buffer;
      MFT_OUTPUT_DATA_BUFFER output = {};
      output.dwStreamID = 0;

      if ((output_stream_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        DWORD buffer_size = output_stream_info_.cbSize;
        const DWORD minimum_buffer_size = static_cast<DWORD>(width_ * height_);
        if (buffer_size < minimum_buffer_size) {
          buffer_size = minimum_buffer_size;
        }
        if (FAILED(CreateSampleCompat(output_sample.GetAddressOf())) ||
            FAILED(CreateMemoryBufferCompat(buffer_size, output_buffer.GetAddressOf())) ||
            FAILED(output_sample->AddBuffer(output_buffer.Get()))) {
          if (error_text != nullptr) {
            *error_text = L"failed to allocate encoder output sample";
          }
          return false;
        }
        output.pSample = output_sample.Get();
      }

      DWORD status = 0;
      const HRESULT process_output_hr = transform_->ProcessOutput(0, 1, &output, &status);
      if (process_output_hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return true;
      }
      if (process_output_hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        RefreshOutputMetadata();
        continue;
      }
      if (FAILED(process_output_hr)) {
        if (error_text != nullptr) {
          *error_text = L"ProcessOutput failed: " + HResultToText(process_output_hr);
        }
        return false;
      }

      ComPtr<IMFSample> result_sample = output.pSample;
      if (!result_sample) {
        if (output_sample) {
          result_sample = output_sample;
        } else {
          continue;
        }
      }

      ComPtr<IMFMediaBuffer> contiguous;
      if (FAILED(result_sample->ConvertToContiguousBuffer(contiguous.GetAddressOf()))) {
        if (error_text != nullptr) {
          *error_text = L"ConvertToContiguousBuffer failed";
        }
        return false;
      }

      BYTE* bytes = nullptr;
      DWORD contiguous_max = 0;
      DWORD contiguous_size = 0;
      if (FAILED(contiguous->Lock(&bytes, &contiguous_max, &contiguous_size))) {
        if (error_text != nullptr) {
          *error_text = L"output buffer lock failed";
        }
        return false;
      }
      std::vector<unsigned char> sample(bytes, bytes + contiguous_size);
      contiguous->Unlock();

      std::vector<unsigned char> annexb;
      bool contains_idr = false;
      if (IsAnnexBBuffer(sample)) {
        annexb = sample;
        contains_idr = AnnexBContainsIdr(annexb);
      } else if (!ConvertLengthPrefixedH264ToAnnexB(
                     sample, avcc_nal_length_size_, &annexb, &contains_idr)) {
        annexb = sample;
      }
      if (contains_idr && !annexb_sequence_header_.empty()) {
        std::vector<unsigned char> with_header = annexb_sequence_header_;
        with_header.insert(with_header.end(), annexb.begin(), annexb.end());
        annexb.swap(with_header);
      }
      if (!annexb.empty()) {
        access_units->push_back(std::move(annexb));
        key_flags->push_back(contains_idr || request_keyframe);
      }
    }
  }

 private:
  void RefreshOutputMetadata() {
    avcc_nal_length_size_ = 4;
    annexb_sequence_header_.clear();

    ComPtr<IMFMediaType> current_output_type;
    if (FAILED(transform_->GetOutputCurrentType(0, current_output_type.GetAddressOf())) ||
        !current_output_type) {
      return;
    }

    UINT32 blob_size = 0;
    if (FAILED(current_output_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) ||
        blob_size == 0) {
      return;
    }

    std::vector<unsigned char> blob(blob_size);
    if (FAILED(current_output_type->GetBlob(
            MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), blob_size, &blob_size))) {
      return;
    }
    blob.resize(blob_size);
    std::vector<unsigned char> annexb_header;
    int nal_length_size = 4;
    if (ParseAvccSequenceHeader(blob, &nal_length_size, &annexb_header)) {
      avcc_nal_length_size_ = nal_length_size;
      annexb_sequence_header_ = std::move(annexb_header);
    }
  }

  ComPtr<IMFTransform> transform_;
  ComPtr<ICodecAPI> codec_api_;
  MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};
  UINT32 width_ = 0;
  UINT32 height_ = 0;
  UINT32 fps_ = 0;
  bool initialized_ = false;
  int avcc_nal_length_size_ = 4;
  std::vector<unsigned char> annexb_sequence_header_;
};

class MinimalVp8Encoder {
 public:
  ~MinimalVp8Encoder() {
    Reset();
  }

  bool Initialize(int width, int height, int fps, int bitrate_kbps, std::wstring* error_text) {
    Reset();

    vpx_codec_iface_t* iface = vpx_codec_vp8_cx();
    if (iface == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"vpx_codec_vp8_cx returned null";
      }
      return false;
    }

    vpx_codec_enc_cfg_t config = {};
    vpx_codec_err_t codec_result = vpx_codec_enc_config_default(iface, &config, 0);
    if (codec_result != VPX_CODEC_OK) {
      if (error_text != nullptr) {
        *error_text = L"vpx_codec_enc_config_default failed";
      }
      return false;
    }

    width_ = static_cast<unsigned int>(NormalizeVideoDimension(width));
    height_ = static_cast<unsigned int>(NormalizeVideoDimension(height));
    fps_ = fps < 1 ? 1U : static_cast<unsigned int>(fps);
    bitrate_kbps_ = bitrate_kbps < 1 ? 1U : static_cast<unsigned int>(bitrate_kbps);

    config.g_w = width_;
    config.g_h = height_;
    config.g_threads = 1;
    config.g_timebase.num = 1;
    config.g_timebase.den = 1000;
    config.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    config.g_lag_in_frames = 0;
    config.rc_end_usage = VPX_CBR;
    config.rc_target_bitrate = bitrate_kbps_;
    config.rc_min_quantizer = 12;
    config.rc_max_quantizer = 42;
    config.kf_mode = VPX_KF_DISABLED;
    config.kf_min_dist = 0;
    config.kf_max_dist = fps_ * 10U;

    codec_result = vpx_codec_enc_init(&codec_, iface, &config, 0);
    if (codec_result != VPX_CODEC_OK) {
      if (error_text != nullptr) {
        *error_text = L"vpx_codec_enc_init failed: " + GetLastCodecError();
      }
      Reset();
      return false;
    }

    codec_result = vpx_codec_control_(&codec_, VP8E_SET_CPUUSED, 12);
    if (codec_result != VPX_CODEC_OK) {
      if (error_text != nullptr) {
        *error_text = L"VP8E_SET_CPUUSED failed: " + GetLastCodecError();
      }
      Reset();
      return false;
    }

    initialized_ = true;
    return true;
  }

  void Reset() {
    if (initialized_) {
      vpx_codec_destroy(&codec_);
    }
    std::memset(&codec_, 0, sizeof(codec_));
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    bitrate_kbps_ = 0;
    initialized_ = false;
  }

  bool IsInitializedFor(int width, int height) const {
    return initialized_ &&
           width_ == static_cast<unsigned int>(NormalizeVideoDimension(width)) &&
           height_ == static_cast<unsigned int>(NormalizeVideoDimension(height));
  }

  bool EncodeFrame(
      const std::vector<unsigned char>& i420,
      bool request_keyframe,
      int64_t pts_ms,
      std::vector<std::vector<unsigned char>>* frames,
      std::vector<bool>* key_flags,
      std::wstring* error_text) {
    frames->clear();
    key_flags->clear();
    if (!initialized_) {
      if (error_text != nullptr) {
        *error_text = L"VP8 encoder is not initialized";
      }
      return false;
    }

    vpx_image_t image = {};
    if (vpx_img_wrap(
            &image,
            VPX_IMG_FMT_I420,
            width_,
            height_,
            1,
            const_cast<unsigned char*>(i420.data())) == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"vpx_img_wrap failed";
      }
      return false;
    }

    const long flags = request_keyframe ? VPX_EFLAG_FORCE_KF : 0;
    const vpx_codec_err_t encode_result = vpx_codec_encode(
        &codec_,
        &image,
        pts_ms,
        1,
        flags,
        VPX_DL_REALTIME);
    if (encode_result != VPX_CODEC_OK) {
      if (error_text != nullptr) {
        *error_text = L"vpx_codec_encode failed: " + GetLastCodecError();
      }
      return false;
    }

    vpx_codec_iter_t iter = nullptr;
    while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&codec_, &iter)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT) {
        continue;
      }
      const auto& frame = packet->data.frame;
      if (frame.buf == nullptr || frame.sz == 0) {
        continue;
      }
      frames->emplace_back(
          static_cast<const unsigned char*>(frame.buf),
          static_cast<const unsigned char*>(frame.buf) + frame.sz);
      key_flags->push_back((frame.flags & VPX_FRAME_IS_KEY) != 0);
    }

    return true;
  }

 private:
  std::wstring GetLastCodecError() const {
    const char* error_text = vpx_codec_error(&codec_);
    if (error_text == nullptr || *error_text == '\0') {
      return L"unknown";
    }
    return Utf8ToWide(error_text);
  }

  vpx_codec_ctx_t codec_ = {};
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int fps_ = 0;
  unsigned int bitrate_kbps_ = 0;
  bool initialized_ = false;
};

class UdpMessageSocket {
 public:
  enum class ReceiveState {
    kDatagram,
    kTimeout,
    kError,
  };

  UdpMessageSocket() = default;

  ~UdpMessageSocket() {
    Close();
  }

  bool Connect(
      const std::wstring& host,
      unsigned short port,
      std::wstring* error_text) {
    Close();

    addrinfoW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfoW* results = nullptr;
    const std::wstring port_text = std::to_wstring(port);
    if (GetAddrInfoW(host.c_str(), port_text.c_str(), &hints, &results) != 0) {
      if (error_text != nullptr) {
        *error_text = L"GetAddrInfoW failed";
      }
      return false;
    }

    bool connected = false;
    for (addrinfoW* current = results; current != nullptr && !connected; current = current->ai_next) {
      SOCKET candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
      if (candidate == INVALID_SOCKET) {
        continue;
      }

      if (connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
        socket_ = candidate;
        connected = true;
        break;
      }

      if (error_text != nullptr) {
        *error_text = L"UDP connect failed, WSA=" + std::to_wstring(WSAGetLastError());
      }
      closesocket(candidate);
    }

    FreeAddrInfoW(results);
    return connected;
  }

  void Close() {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
  }

  bool SendMessage(const std::vector<unsigned char>& payload, std::wstring* error_text) {
    if (socket_ == INVALID_SOCKET) {
      if (error_text != nullptr) {
        *error_text = L"UDP socket is not connected";
      }
      return false;
    }
    const int sent = send(
        socket_,
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()),
        0);
    if (sent == SOCKET_ERROR) {
      if (error_text != nullptr) {
        *error_text = L"udp send failed, WSA=" + std::to_wstring(WSAGetLastError());
      }
      return false;
    }
    return static_cast<size_t>(sent) == payload.size();
  }

  ReceiveState ReceiveMessage(
      std::vector<unsigned char>* payload,
      unsigned long timeout_ms,
      std::wstring* error_text) const {
    payload->clear();
    if (socket_ == INVALID_SOCKET) {
      if (error_text != nullptr) {
        *error_text = L"UDP socket is not connected";
      }
      return ReceiveState::kError;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);

    TIMEVAL timeout = {};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

    const int select_result = select(0, &read_set, nullptr, nullptr, &timeout);
    if (select_result == 0) {
      return ReceiveState::kTimeout;
    }
    if (select_result == SOCKET_ERROR) {
      if (error_text != nullptr) {
        *error_text = L"udp select failed, WSA=" + std::to_wstring(WSAGetLastError());
      }
      return ReceiveState::kError;
    }

    std::array<unsigned char, 64 * 1024> buffer = {};
    const int received = recv(
        socket_,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);
    if (received == SOCKET_ERROR) {
      if (error_text != nullptr) {
        *error_text = L"udp recv failed, WSA=" + std::to_wstring(WSAGetLastError());
      }
      return ReceiveState::kError;
    }

    payload->assign(buffer.begin(), buffer.begin() + received);
    return ReceiveState::kDatagram;
  }

  std::wstring PeerEndpointText() const {
    if (socket_ == INVALID_SOCKET) {
      return L"";
    }

    sockaddr_storage storage = {};
    int storage_length = sizeof(storage);
    if (getpeername(socket_, reinterpret_cast<sockaddr*>(&storage), &storage_length) != 0) {
      return L"";
    }

    std::array<wchar_t, NI_MAXHOST> host = {};
    std::array<wchar_t, NI_MAXSERV> service = {};
    if (GetNameInfoW(
            reinterpret_cast<sockaddr*>(&storage),
            storage_length,
            host.data(),
            static_cast<DWORD>(host.size()),
            service.data(),
            static_cast<DWORD>(service.size()),
            NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      return L"";
    }

    return std::wstring(host.data()) + L":" + service.data();
  }

 private:
  SOCKET socket_ = INVALID_SOCKET;
};

class TcpFramedConnection {
 public:
  enum class ReceiveState {
    kFrame,
    kTimeout,
    kClosed,
    kError,
  };

  TcpFramedConnection() = default;

  ~TcpFramedConnection() {
    Close();
  }

  bool Connect(
      const std::wstring& host,
      unsigned short port,
      unsigned long timeout_ms,
      std::wstring* error_text) {
    Close();

    if (!ConnectWithFamily(host, port, AF_INET, timeout_ms, error_text)) {
      if (error_text != nullptr && error_text->empty()) {
        *error_text = L"TCP connect failed";
      }
      return false;
    }
    return true;
  }

  void Close() {
    const SOCKET socket_handle = socket_.exchange(INVALID_SOCKET);
    if (socket_handle != INVALID_SOCKET) {
      shutdown(socket_handle, SD_BOTH);
      closesocket(socket_handle);
    }
    has_symmetric_key_ = false;
    send_seq_ = 0;
    recv_seq_ = 0;
  }

  void Abort() {
    Close();
  }

  bool SendRawFrame(const std::vector<unsigned char>& payload, std::wstring* error_text) {
    return SendFrameInternal(payload, false, error_text);
  }

  bool SendFrame(const std::vector<unsigned char>& payload, std::wstring* error_text) {
    return SendFrameInternal(payload, has_symmetric_key_, error_text);
  }

  ReceiveState ReceiveFrame(
      std::vector<unsigned char>* payload,
      unsigned long timeout_ms,
      std::wstring* error_text) {
    payload->clear();
    unsigned char first = 0;
    int recv_state = 0;
    if (!ReceiveExact(&first, 1, timeout_ms, &recv_state, error_text)) {
      return StateFromRecvResult(recv_state);
    }
    const unsigned long body_timeout_ms =
        timeout_ms < kFrameBodyTimeoutMs ? kFrameBodyTimeoutMs : timeout_ms;

    const size_t head_len = static_cast<size_t>((first & 0x03U) + 1U);
    std::array<unsigned char, 4> header = {first, 0, 0, 0};
    if (head_len > 1) {
      if (!ReceiveExact(header.data() + 1, head_len - 1, body_timeout_ms, &recv_state, error_text)) {
        return StateFromRecvResult(recv_state);
      }
    }

    unsigned int length = header[0];
    if (head_len > 1) {
      length |= static_cast<unsigned int>(header[1]) << 8U;
    }
    if (head_len > 2) {
      length |= static_cast<unsigned int>(header[2]) << 16U;
    }
    if (head_len > 3) {
      length |= static_cast<unsigned int>(header[3]) << 24U;
    }
    length >>= 2U;

    payload->resize(length);
    if (length > 0) {
      if (!ReceiveExact(payload->data(), length, body_timeout_ms, &recv_state, error_text)) {
        payload->clear();
        return StateFromRecvResult(recv_state);
      }
    }

    if (has_symmetric_key_ && payload->size() > 1) {
      std::vector<unsigned char> plain;
      if (!DecryptPayload(*payload, &plain, error_text)) {
        payload->clear();
        return ReceiveState::kError;
      }
      payload->swap(plain);
    }

    return ReceiveState::kFrame;
  }

  std::wstring PeerEndpointText() const {
    const SOCKET socket_handle = socket_.load();
    if (socket_handle == INVALID_SOCKET) {
      return L"";
    }

    sockaddr_storage storage = {};
    int storage_length = sizeof(storage);
    if (getpeername(socket_handle, reinterpret_cast<sockaddr*>(&storage), &storage_length) != 0) {
      return L"";
    }

    std::array<wchar_t, NI_MAXHOST> host = {};
    std::array<wchar_t, NI_MAXSERV> service = {};
    if (GetNameInfoW(
            reinterpret_cast<sockaddr*>(&storage),
            storage_length,
            host.data(),
            static_cast<DWORD>(host.size()),
            service.data(),
            static_cast<DWORD>(service.size()),
            NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      return L"";
    }

    std::wstring endpoint = host.data();
    if (storage.ss_family == AF_INET6) {
      endpoint = L"[" + endpoint + L"]";
    }
    endpoint += L":";
    endpoint += service.data();
    return endpoint;
  }

  std::wstring LocalEndpointText() const {
    const SOCKET socket_handle = socket_.load();
    if (socket_handle == INVALID_SOCKET) {
      return L"";
    }

    sockaddr_storage storage = {};
    int storage_length = sizeof(storage);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&storage), &storage_length) != 0) {
      return L"";
    }

    std::array<wchar_t, NI_MAXHOST> host = {};
    std::array<wchar_t, NI_MAXSERV> service = {};
    if (GetNameInfoW(
            reinterpret_cast<sockaddr*>(&storage),
            storage_length,
            host.data(),
            static_cast<DWORD>(host.size()),
            service.data(),
            static_cast<DWORD>(service.size()),
            NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      return L"";
    }

    std::wstring endpoint = host.data();
    if (storage.ss_family == AF_INET6) {
      endpoint = L"[" + endpoint + L"]";
    }
    endpoint += L":";
    endpoint += service.data();
    return endpoint;
  }

  bool GetLocalSocketAddress(sockaddr_storage* storage, int* storage_length) const {
    const SOCKET socket_handle = socket_.load();
    if (storage == nullptr || storage_length == nullptr || socket_handle == INVALID_SOCKET) {
      return false;
    }
    *storage_length = sizeof(*storage);
    std::memset(storage, 0, sizeof(*storage));
    return getsockname(socket_handle, reinterpret_cast<sockaddr*>(storage), storage_length) == 0;
  }

  bool GetPeerSocketAddress(sockaddr_storage* storage, int* storage_length) const {
    const SOCKET socket_handle = socket_.load();
    if (storage == nullptr || storage_length == nullptr || socket_handle == INVALID_SOCKET) {
      return false;
    }
    *storage_length = sizeof(*storage);
    std::memset(storage, 0, sizeof(*storage));
    return getpeername(socket_handle, reinterpret_cast<sockaddr*>(storage), storage_length) == 0;
  }

  void AttachSocket(SOCKET socket_handle) {
    Close();
    socket_.store(socket_handle);
  }

  unsigned long AvailableBytes() const {
    const SOCKET socket_handle = socket_.load();
    if (socket_handle == INVALID_SOCKET) {
      return 0;
    }
    u_long available = 0;
    if (ioctlsocket(socket_handle, FIONREAD, &available) != 0) {
      return 0;
    }
    return static_cast<unsigned long>(available);
  }

  void SetSymmetricKey(const std::array<unsigned char, crypto_secretbox_KEYBYTES>& key) {
    symmetric_key_ = key;
    has_symmetric_key_ = true;
    send_seq_ = 0;
    recv_seq_ = 0;
  }

 private:
  bool ConnectWithFamily(
      const std::wstring& host,
      unsigned short port,
      int family,
      unsigned long timeout_ms,
      std::wstring* error_text) {
    addrinfoW hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfoW* results = nullptr;
    const std::wstring port_text = std::to_wstring(port);
    if (GetAddrInfoW(host.c_str(), port_text.c_str(), &hints, &results) != 0) {
      if (error_text != nullptr && error_text->empty()) {
        *error_text = L"GetAddrInfoW failed";
      }
      return false;
    }

    bool connected = false;
    for (addrinfoW* current = results; current != nullptr && !connected; current = current->ai_next) {
      SOCKET candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
      if (candidate == INVALID_SOCKET) {
        continue;
      }

      u_long non_blocking = 1;
      ioctlsocket(candidate, FIONBIO, &non_blocking);

      const int connect_result =
          connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen));
      if (connect_result == 0) {
        socket_.store(candidate);
        connected = true;
        break;
      }

      const int connect_error = WSAGetLastError();
      if (connect_error == WSAEWOULDBLOCK || connect_error == WSAEINPROGRESS ||
          connect_error == WSAEINVAL) {
        std::wstring wait_error;
        if (WaitForSocket(candidate, true, timeout_ms, &wait_error)) {
          int socket_error = 0;
          int option_length = sizeof(socket_error);
          if (getsockopt(
                  candidate,
                  SOL_SOCKET,
                  SO_ERROR,
                  reinterpret_cast<char*>(&socket_error),
                  &option_length) == 0 &&
              socket_error == 0) {
            socket_.store(candidate);
            connected = true;
            break;
          }
        } else if (error_text != nullptr && error_text->empty()) {
          *error_text = wait_error;
        }
      }

      closesocket(candidate);
    }

    FreeAddrInfoW(results);
    return connected;
  }

  static ReceiveState StateFromRecvResult(int recv_state) {
    switch (recv_state) {
      case 1:
        return ReceiveState::kTimeout;
      case 2:
        return ReceiveState::kClosed;
      default:
        return ReceiveState::kError;
    }
  }

  static std::array<unsigned char, crypto_secretbox_NONCEBYTES> BuildSecretboxNonce(uint64_t sequence) {
    std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce = {};
    std::memcpy(nonce.data(), &sequence, sizeof(sequence));
    return nonce;
  }

  bool WaitForSocket(
      SOCKET socket_handle,
      bool write_ready,
      unsigned long timeout_ms,
      std::wstring* error_text) const {
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (write_ready) {
      FD_SET(socket_handle, &write_set);
    } else {
      FD_SET(socket_handle, &read_set);
    }

    TIMEVAL timeout = {};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

    const int select_result = select(0, write_ready ? nullptr : &read_set, write_ready ? &write_set : nullptr, nullptr, &timeout);
    if (select_result > 0) {
      return true;
    }
    if (select_result == 0) {
      if (error_text != nullptr && error_text->empty()) {
        *error_text = write_ready ? L"socket timeout waiting for write" : L"socket timeout waiting for read";
      }
      return false;
    }
    if (error_text != nullptr) {
      *error_text = L"select failed, WSA=" + std::to_wstring(WSAGetLastError());
    }
    return false;
  }

  bool ReceiveExact(
      void* buffer,
      size_t length,
      unsigned long timeout_ms,
      int* recv_state,
      std::wstring* error_text) {
    unsigned char* cursor = static_cast<unsigned char*>(buffer);
    size_t remaining = length;
    while (remaining > 0) {
      const SOCKET socket_handle = socket_.load();
      if (socket_handle == INVALID_SOCKET) {
        if (recv_state != nullptr) {
          *recv_state = 2;
        }
        if (error_text != nullptr) {
          *error_text = L"socket closed";
        }
        return false;
      }
      std::wstring wait_error;
      if (!WaitForSocket(socket_handle, false, timeout_ms, &wait_error)) {
        if (recv_state != nullptr) {
          *recv_state = wait_error.find(L"timeout") != std::wstring::npos ? 1 : 0;
        }
        if (error_text != nullptr) {
          *error_text = wait_error;
        }
        return false;
      }

      const int chunk = recv(socket_handle, reinterpret_cast<char*>(cursor), static_cast<int>(remaining), 0);
      if (chunk == 0) {
        if (recv_state != nullptr) {
          *recv_state = 2;
        }
        return false;
      }
      if (chunk == SOCKET_ERROR) {
        const int socket_error = WSAGetLastError();
        if (socket_error == WSAEWOULDBLOCK) {
          continue;
        }
        if (recv_state != nullptr) {
          *recv_state = 0;
        }
        if (error_text != nullptr) {
          *error_text = L"recv failed, WSA=" + std::to_wstring(socket_error);
        }
        return false;
      }

      cursor += chunk;
      remaining -= static_cast<size_t>(chunk);
    }
    if (recv_state != nullptr) {
      *recv_state = 3;
    }
    return true;
  }

  bool SendAll(const unsigned char* data, size_t size, std::wstring* error_text) {
    size_t sent = 0;
    while (sent < size) {
      const SOCKET socket_handle = socket_.load();
      if (socket_handle == INVALID_SOCKET) {
        if (error_text != nullptr) {
          *error_text = L"socket closed";
        }
        return false;
      }
      std::wstring wait_error;
      if (!WaitForSocket(socket_handle, true, kConnectTimeoutMs, &wait_error)) {
        if (error_text != nullptr) {
          *error_text = wait_error;
        }
        return false;
      }

      const int chunk = send(
          socket_handle,
          reinterpret_cast<const char*>(data + sent),
          static_cast<int>(size - sent),
          0);
      if (chunk == SOCKET_ERROR) {
        const int socket_error = WSAGetLastError();
        if (socket_error == WSAEWOULDBLOCK) {
          continue;
        }
        if (error_text != nullptr) {
          *error_text = L"send failed, WSA=" + std::to_wstring(socket_error);
        }
        return false;
      }
      sent += static_cast<size_t>(chunk);
    }
    return true;
  }

  bool EncryptPayload(
      const std::vector<unsigned char>& plain,
      std::vector<unsigned char>* cipher,
      std::wstring* error_text) {
    ++send_seq_;
    const std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce = BuildSecretboxNonce(send_seq_);
    cipher->resize(plain.size() + crypto_secretbox_MACBYTES);
    if (crypto_secretbox_easy(
            cipher->data(),
            plain.data(),
            static_cast<unsigned long long>(plain.size()),
            nonce.data(),
            symmetric_key_.data()) != 0) {
      if (error_text != nullptr) {
        *error_text = L"crypto_secretbox_easy failed";
      }
      return false;
    }
    return true;
  }

  bool DecryptPayload(
      const std::vector<unsigned char>& cipher,
      std::vector<unsigned char>* plain,
      std::wstring* error_text) {
    if (cipher.size() < crypto_secretbox_MACBYTES) {
      if (error_text != nullptr) {
        *error_text = L"encrypted payload too short";
      }
      return false;
    }

    ++recv_seq_;
    const std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce = BuildSecretboxNonce(recv_seq_);
    plain->resize(cipher.size() - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(
            plain->data(),
            cipher.data(),
            static_cast<unsigned long long>(cipher.size()),
            nonce.data(),
            symmetric_key_.data()) != 0) {
      if (error_text != nullptr) {
        *error_text = L"crypto_secretbox_open_easy failed";
      }
      return false;
    }
    return true;
  }

  bool SendFrameInternal(
      const std::vector<unsigned char>& payload,
      bool encrypt,
      std::wstring* error_text) {
    Win32LockGuard send_lock(send_mutex_);
    std::vector<unsigned char> frame = payload;
    if (encrypt) {
      std::vector<unsigned char> encrypted;
      if (!EncryptPayload(payload, &encrypted, error_text)) {
        return false;
      }
      frame.swap(encrypted);
    }

    std::vector<unsigned char> header;
    if (frame.size() <= 0x3FU) {
      header.push_back(static_cast<unsigned char>(frame.size() << 2U));
    } else if (frame.size() <= 0x3FFFU) {
      const unsigned short value = static_cast<unsigned short>((frame.size() << 2U) | 0x1U);
      header.push_back(static_cast<unsigned char>(value & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    } else if (frame.size() <= 0x3FFFFFU) {
      const unsigned int value = static_cast<unsigned int>((frame.size() << 2U) | 0x2U);
      header.push_back(static_cast<unsigned char>(value & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    } else if (frame.size() <= 0x3FFFFFFFU) {
      const unsigned int value = static_cast<unsigned int>((frame.size() << 2U) | 0x3U);
      header.push_back(static_cast<unsigned char>(value & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
      header.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    } else {
      if (error_text != nullptr) {
        *error_text = L"frame too large";
      }
      return false;
    }

    if (!SendAll(header.data(), header.size(), error_text)) {
      return false;
    }
    if (!frame.empty() && !SendAll(frame.data(), frame.size(), error_text)) {
      return false;
    }
    return true;
  }

  std::atomic<SOCKET> socket_{INVALID_SOCKET};
  bool has_symmetric_key_ = false;
  std::array<unsigned char, crypto_secretbox_KEYBYTES> symmetric_key_ = {};
  uint64_t send_seq_ = 0;
  uint64_t recv_seq_ = 0;
  Win32Mutex send_mutex_;
};

UINT GetFileDescriptorClipboardFormat() {
  const UINT format = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
  return format;
}

UINT GetFileContentsClipboardFormat() {
  const UINT format = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  return format;
}

class RemoteFileClipboardBridge {
 public:
  RemoteFileClipboardBridge(
      TcpFramedConnection* connection,
      std::atomic<int>* active_transfer_count,
      std::atomic<int>* next_stream_id,
      std::wstring channel_name)
      : connection_(connection),
        active_transfer_count_(active_transfer_count),
        next_stream_id_(next_stream_id),
        channel_name_(std::move(channel_name)) {}

  bool InitializeFromFileGroupDescriptorPayload(
      const std::vector<unsigned char>& payload,
      std::wstring* error_text) {
    std::vector<RemoteClipboardFileDescriptor> parsed_files;
    if (!ParseDescriptors(payload, &parsed_files, error_text)) {
      return false;
    }
    Win32LockGuard lock(mutex_);
    file_group_descriptor_payload_ = payload;
    file_descriptors_ = std::move(parsed_files);
    closed_ = false;
    close_reason_.clear();
    return true;
  }

  std::vector<unsigned char> GetFileGroupDescriptorPayload() const {
    Win32LockGuard lock(mutex_);
    return file_group_descriptor_payload_;
  }

  bool IsValidFileIndex(LONG index) const {
    Win32LockGuard lock(mutex_);
    return index >= 0 && static_cast<size_t>(index) < file_descriptors_.size();
  }

  bool GetFileDescriptor(LONG index, RemoteClipboardFileDescriptor* descriptor) const {
    if (descriptor == nullptr) {
      return false;
    }
    Win32LockGuard lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= file_descriptors_.size()) {
      return false;
    }
    *descriptor = file_descriptors_[index];
    return true;
  }

  bool RequestFileSize(LONG index, uint64_t* size, std::wstring* error_text) {
    if (size == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"clipboard size request target was null";
      }
      return false;
    }

    {
      Win32LockGuard lock(mutex_);
      if (index < 0 || static_cast<size_t>(index) >= file_descriptors_.size()) {
        if (error_text != nullptr) {
          *error_text = L"clipboard size request index was out of range";
        }
        return false;
      }
      RemoteClipboardFileDescriptor& descriptor = file_descriptors_[index];
      if (descriptor.is_directory) {
        *size = 0;
        descriptor.size = 0;
        descriptor.size_known = true;
        return true;
      }
      if (descriptor.size_known) {
        *size = descriptor.size;
        return true;
      }
    }

    std::vector<unsigned char> payload;
    if (!RequestRemoteFileContents(
            index,
            kCliprdrFileContentsSizeFlag,
            0,
            sizeof(uint64_t),
            &payload,
            error_text)) {
      return false;
    }
    if (payload.size() < sizeof(uint64_t)) {
      if (error_text != nullptr) {
        *error_text = L"clipboard size response was truncated";
      }
      return false;
    }

    uint64_t remote_size = 0;
    std::memcpy(&remote_size, payload.data(), sizeof(remote_size));
    {
      Win32LockGuard lock(mutex_);
      if (index >= 0 && static_cast<size_t>(index) < file_descriptors_.size()) {
        file_descriptors_[index].size = remote_size;
        file_descriptors_[index].size_known = true;
      }
    }
    *size = remote_size;
    return true;
  }

  bool RequestFileRange(
      LONG index,
      uint64_t offset,
      size_t requested_size,
      std::vector<unsigned char>* payload,
      std::wstring* error_text) {
    if (payload == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"clipboard range payload target was null";
      }
      return false;
    }
    return RequestRemoteFileContents(
        index,
        kCliprdrFileContentsRangeFlag,
        offset,
        static_cast<int>(requested_size),
        payload,
        error_text);
  }

  bool HandleFileContentsResponse(const CliprdrMessageData& cliprdr) {
    std::shared_ptr<PendingRemoteClipboardFileRequest> request;
    {
      Win32LockGuard lock(mutex_);
      auto found = pending_requests_.find(cliprdr.stream_id);
      if (found == pending_requests_.end()) {
        return false;
      }
      request = found->second;
      request->completed = true;
      request->succeeded = cliprdr.msg_flags == kCliprdrResponseOk;
      if (request->succeeded) {
        request->payload = cliprdr.payload;
      } else {
        request->error_text = L"controller rejected clipboard file request";
      }
    }
    if (request->completion_event != nullptr) {
      SetEvent(request->completion_event);
    }
    return true;
  }

  void Close(const std::wstring& reason) {
    std::vector<std::shared_ptr<PendingRemoteClipboardFileRequest>> pending;
    {
      Win32LockGuard lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      close_reason_ = reason;
      for (const auto& pending_entry : pending_requests_) {
        pending.push_back(pending_entry.second);
      }
    }

    for (const std::shared_ptr<PendingRemoteClipboardFileRequest>& request : pending) {
      if (request != nullptr) {
        if (request->completion_event != nullptr) {
          SetEvent(request->completion_event);
        }
      }
    }
  }

  const std::wstring& channel_name() const {
    return channel_name_;
  }

 private:
  bool ParseDescriptors(
      const std::vector<unsigned char>& payload,
      std::vector<RemoteClipboardFileDescriptor>* descriptors,
      std::wstring* error_text) const {
    descriptors->clear();
    if (payload.size() < offsetof(FILEGROUPDESCRIPTORW, fgd)) {
      if (error_text != nullptr) {
        *error_text = L"clipboard file descriptor payload too small";
      }
      return false;
    }

    UINT count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    const size_t header_size = offsetof(FILEGROUPDESCRIPTORW, fgd);
    const size_t required_bytes =
        header_size + static_cast<size_t>(count) * sizeof(FILEDESCRIPTORW);
    if (payload.size() < required_bytes) {
      if (error_text != nullptr) {
        *error_text = L"clipboard file descriptor payload truncated";
      }
      return false;
    }

    descriptors->reserve(count);
    for (UINT index = 0; index < count; ++index) {
      FILEDESCRIPTORW descriptor = {};
      std::memcpy(
          &descriptor,
          payload.data() + header_size + static_cast<size_t>(index) * sizeof(FILEDESCRIPTORW),
          sizeof(descriptor));

      RemoteClipboardFileDescriptor entry;
      entry.descriptor = descriptor;
      entry.size = (static_cast<uint64_t>(descriptor.nFileSizeHigh) << 32ULL) |
                   descriptor.nFileSizeLow;
      entry.is_directory =
          (descriptor.dwFlags & FD_ATTRIBUTES) != 0 &&
          (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      entry.size_known = entry.is_directory || (descriptor.dwFlags & FD_FILESIZE) != 0;
      entry.display_name = descriptor.cFileName[0] != L'\0'
                               ? std::wstring(descriptor.cFileName)
                               : (L"clipboard-file-" + std::to_wstring(index + 1));
      descriptors->push_back(entry);
    }
    return true;
  }

  bool RequestRemoteFileContents(
      LONG index,
      int dw_flags,
      uint64_t offset,
      int requested_size,
      std::vector<unsigned char>* payload,
      std::wstring* error_text) {
    if (payload == nullptr) {
      if (error_text != nullptr) {
        *error_text = L"clipboard request payload target was null";
      }
      return false;
    }

    std::shared_ptr<PendingRemoteClipboardFileRequest> request =
        std::make_shared<PendingRemoteClipboardFileRequest>();
    int stream_id = 0;
    std::wstring send_error;

    {
      Win32LockGuard lock(mutex_);
      if (closed_) {
        if (error_text != nullptr) {
          *error_text =
              close_reason_.empty() ? L"clipboard session closed" : close_reason_;
        }
        return false;
      }
      if (connection_ == nullptr || next_stream_id_ == nullptr) {
        if (error_text != nullptr) {
          *error_text = L"clipboard bridge is not attached to an active session";
        }
        return false;
      }
      stream_id = (*next_stream_id_)++;
      request->stream_id = stream_id;
      pending_requests_.emplace(stream_id, request);
    }

    const auto complete_request = [&]() {
      if (active_transfer_count_ != nullptr) {
        active_transfer_count_->fetch_sub(1);
      }
    };

    if (active_transfer_count_ != nullptr) {
      active_transfer_count_->fetch_add(1);
    }

    const std::vector<unsigned char> message = EncodeCliprdrFileContentsRequestMessage(
        stream_id,
        static_cast<int>(index),
        dw_flags,
        static_cast<int>(offset & 0xFFFFFFFFULL),
        static_cast<int>((offset >> 32ULL) & 0xFFFFFFFFULL),
        requested_size);
    if (!connection_->SendFrame(message, &send_error)) {
      {
        Win32LockGuard lock(mutex_);
        pending_requests_.erase(stream_id);
      }
      complete_request();
      if (error_text != nullptr) {
        *error_text = L"clipboard file request send failed: " + send_error;
      }
      return false;
    }

    const DWORD wait_result = request->completion_event == nullptr
                                  ? WAIT_FAILED
                                  : WaitForSingleObject(
                                        request->completion_event,
                                        static_cast<DWORD>(kClipboardVirtualFileTimeoutMs));
    {
      Win32LockGuard lock(mutex_);
      pending_requests_.erase(stream_id);
      if (wait_result != WAIT_OBJECT_0) {
        complete_request();
        if (error_text != nullptr) {
          *error_text = wait_result == WAIT_TIMEOUT
                            ? L"clipboard file request timed out"
                            : L"clipboard file request wait failed";
        }
        return false;
      }
      if (!request->completed) {
        complete_request();
        if (error_text != nullptr) {
          *error_text =
              close_reason_.empty() ? L"clipboard session closed" : close_reason_;
        }
        return false;
      }
    }

    complete_request();
    if (!request->succeeded) {
      if (error_text != nullptr) {
        *error_text =
            request->error_text.empty() ? L"clipboard file request failed" : request->error_text;
      }
      return false;
    }

    *payload = std::move(request->payload);
    return true;
  }

  TcpFramedConnection* connection_ = nullptr;
  std::atomic<int>* active_transfer_count_ = nullptr;
  std::atomic<int>* next_stream_id_ = nullptr;
  std::wstring channel_name_;
  mutable Win32Mutex mutex_;
  bool closed_ = false;
  std::wstring close_reason_;
  std::vector<unsigned char> file_group_descriptor_payload_;
  std::vector<RemoteClipboardFileDescriptor> file_descriptors_;
  std::unordered_map<int, std::shared_ptr<PendingRemoteClipboardFileRequest>>
      pending_requests_;
};

class VirtualFileStream : public IStream {
 public:
  VirtualFileStream(
      std::shared_ptr<RemoteFileClipboardBridge> bridge,
      LONG file_index)
      : ref_count_(1),
        bridge_(std::move(bridge)),
        file_index_(file_index) {
    if (bridge_ != nullptr) {
      bridge_->GetFileDescriptor(file_index_, &descriptor_);
      size_ = descriptor_.size;
      size_known_ = descriptor_.size_known;
      is_directory_ = descriptor_.is_directory;
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream)) {
      *object = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG size, ULONG* bytes_read) override {
    if (bytes_read != nullptr) {
      *bytes_read = 0;
    }
    if (buffer == nullptr) {
      return STG_E_INVALIDPOINTER;
    }
    if (size == 0) {
      return S_OK;
    }
    if (bridge_ == nullptr || is_directory_) {
      return S_FALSE;
    }
    if (!EnsureKnownSize()) {
      return STG_E_READFAULT;
    }
    if (offset_ >= size_) {
      return S_FALSE;
    }

    const size_t request_size = static_cast<size_t>(std::min<ULONG>(
        size,
        static_cast<ULONG>(kClipboardFileTransferChunkSize)));
    std::vector<unsigned char> payload;
    std::wstring error_text;
    if (!bridge_->RequestFileRange(
            file_index_,
            offset_,
            request_size,
            &payload,
            &error_text)) {
      return STG_E_READFAULT;
    }

    if (payload.size() > request_size) {
      payload.resize(request_size);
    }
    if (!payload.empty()) {
      std::memcpy(buffer, payload.data(), payload.size());
      offset_ += payload.size();
    }
    if (bytes_read != nullptr) {
      *bytes_read = static_cast<ULONG>(payload.size());
    }
    return payload.size() < request_size || offset_ >= size_ ? S_FALSE : S_OK;
  }

  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE Seek(
      LARGE_INTEGER move,
      DWORD origin,
      ULARGE_INTEGER* new_position) override {
    if (bridge_ == nullptr) {
      return STG_E_INVALIDHANDLE;
    }
    if (!EnsureKnownSize()) {
      return STG_E_READFAULT;
    }

    int64_t candidate_offset = 0;
    switch (origin) {
      case STREAM_SEEK_SET:
        candidate_offset = move.QuadPart;
        break;
      case STREAM_SEEK_CUR:
        candidate_offset = static_cast<int64_t>(offset_) + move.QuadPart;
        break;
      case STREAM_SEEK_END:
        candidate_offset = static_cast<int64_t>(size_) + move.QuadPart;
        break;
      default:
        return STG_E_INVALIDFUNCTION;
    }

    if (candidate_offset < 0 ||
        static_cast<uint64_t>(candidate_offset) > size_) {
      return STG_E_SEEKERROR;
    }

    offset_ = static_cast<uint64_t>(candidate_offset);
    if (new_position != nullptr) {
      new_position->QuadPart = offset_;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CopyTo(
      IStream*,
      ULARGE_INTEGER,
      ULARGE_INTEGER*,
      ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Revert() override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE LockRegion(
      ULARGE_INTEGER,
      ULARGE_INTEGER,
      DWORD) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE UnlockRegion(
      ULARGE_INTEGER,
      ULARGE_INTEGER,
      DWORD) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flag) override {
    if (stat == nullptr) {
      return STG_E_INVALIDPOINTER;
    }
    if (!EnsureKnownSize()) {
      return STG_E_READFAULT;
    }
    std::memset(stat, 0, sizeof(*stat));
    if (flag == STATFLAG_DEFAULT) {
      return STG_E_INSUFFICIENTMEMORY;
    }
    if (flag != STATFLAG_NONAME) {
      return STG_E_INVALIDFLAG;
    }
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size_;
    stat->grfMode = STGM_READ;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream**) override {
    return E_NOTIMPL;
  }

 private:
  bool EnsureKnownSize() {
    if (size_known_) {
      return true;
    }
    if (bridge_ == nullptr) {
      return false;
    }
    uint64_t resolved_size = 0;
    std::wstring error_text;
    if (!bridge_->RequestFileSize(file_index_, &resolved_size, &error_text)) {
      return false;
    }
    size_ = resolved_size;
    size_known_ = true;
    return true;
  }

  ~VirtualFileStream() = default;

  LONG ref_count_ = 1;
  std::shared_ptr<RemoteFileClipboardBridge> bridge_;
  LONG file_index_ = 0;
  RemoteClipboardFileDescriptor descriptor_;
  uint64_t size_ = 0;
  uint64_t offset_ = 0;
  bool size_known_ = false;
  bool is_directory_ = false;
};

class VirtualFormatEtcEnumerator : public IEnumFORMATETC {
 public:
  explicit VirtualFormatEtcEnumerator(const std::vector<FORMATETC>& formats)
      : ref_count_(1), formats_(formats) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC)) {
      *object = static_cast<IEnumFORMATETC*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE Next(
      ULONG requested,
      FORMATETC* format,
      ULONG* fetched) override {
    if (requested == 0 || format == nullptr) {
      return E_INVALIDARG;
    }
    ULONG produced = 0;
    while (produced < requested && index_ < formats_.size()) {
      format[produced] = formats_[index_];
      ++produced;
      ++index_;
    }
    if (fetched != nullptr) {
      *fetched = produced;
    }
    return produced == requested ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    index_ = std::min<size_t>(formats_.size(), index_ + count);
    return index_ < formats_.size() ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    index_ = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** enumerator) override {
    if (enumerator == nullptr) {
      return E_POINTER;
    }
    auto* clone = new (std::nothrow) VirtualFormatEtcEnumerator(formats_);
    if (clone == nullptr) {
      return E_OUTOFMEMORY;
    }
    clone->index_ = index_;
    *enumerator = clone;
    return S_OK;
  }

 private:
  ~VirtualFormatEtcEnumerator() = default;

  LONG ref_count_ = 1;
  size_t index_ = 0;
  std::vector<FORMATETC> formats_;
};

class VirtualFileDataObject : public IDataObject {
 public:
  explicit VirtualFileDataObject(std::shared_ptr<RemoteFileClipboardBridge> bridge)
      : ref_count_(1), bridge_(std::move(bridge)) {
    FORMATETC file_descriptor = {};
    file_descriptor.cfFormat = static_cast<CLIPFORMAT>(GetFileDescriptorClipboardFormat());
    file_descriptor.ptd = nullptr;
    file_descriptor.dwAspect = DVASPECT_CONTENT;
    file_descriptor.lindex = 0;
    file_descriptor.tymed = TYMED_HGLOBAL;
    formats_.push_back(file_descriptor);

    FORMATETC file_contents = {};
    file_contents.cfFormat = static_cast<CLIPFORMAT>(GetFileContentsClipboardFormat());
    file_contents.ptd = nullptr;
    file_contents.dwAspect = DVASPECT_CONTENT;
    file_contents.lindex = 0;
    file_contents.tymed = TYMED_ISTREAM;
    formats_.push_back(file_contents);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
      *object = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (format == nullptr || medium == nullptr) {
      return E_POINTER;
    }
    std::memset(medium, 0, sizeof(*medium));
    if (bridge_ == nullptr) {
      return E_FAIL;
    }

    if (format->cfFormat == GetFileDescriptorClipboardFormat() &&
        (format->tymed & TYMED_HGLOBAL) != 0) {
      const std::vector<unsigned char> payload = bridge_->GetFileGroupDescriptorPayload();
      if (payload.empty()) {
        return DV_E_FORMATETC;
      }

      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, payload.size());
      if (memory == nullptr) {
        return STG_E_MEDIUMFULL;
      }
      void* locked = GlobalLock(memory);
      if (locked == nullptr) {
        GlobalFree(memory);
        return STG_E_MEDIUMFULL;
      }
      std::memcpy(locked, payload.data(), payload.size());
      GlobalUnlock(memory);

      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = memory;
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }

    if (format->cfFormat == GetFileContentsClipboardFormat() &&
        (format->tymed & TYMED_ISTREAM) != 0) {
      if (!bridge_->IsValidFileIndex(format->lindex)) {
        return DV_E_LINDEX;
      }
      auto* stream = new (std::nothrow) VirtualFileStream(bridge_, format->lindex);
      if (stream == nullptr) {
        return E_OUTOFMEMORY;
      }
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = stream;
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }

    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format == nullptr) {
      return E_POINTER;
    }
    if (format->cfFormat == GetFileDescriptorClipboardFormat() &&
        (format->tymed & TYMED_HGLOBAL) != 0) {
      return S_OK;
    }
    if (format->cfFormat == GetFileContentsClipboardFormat() &&
        (format->tymed & TYMED_ISTREAM) != 0) {
      return S_OK;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* format_out) override {
    if (format_out == nullptr) {
      return E_POINTER;
    }
    format_out->ptd = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(
      DWORD direction,
      IEnumFORMATETC** enumerator) override {
    if (enumerator == nullptr) {
      return E_POINTER;
    }
    *enumerator = nullptr;
    if (direction != DATADIR_GET) {
      return E_NOTIMPL;
    }
    auto* value = new (std::nothrow) VirtualFormatEtcEnumerator(formats_);
    if (value == nullptr) {
      return E_OUTOFMEMORY;
    }
    *enumerator = value;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  ~VirtualFileDataObject() = default;

  LONG ref_count_ = 1;
  std::shared_ptr<RemoteFileClipboardBridge> bridge_;
  std::vector<FORMATETC> formats_;
};

ParsedHostPort ParseHostPort(const std::wstring& endpoint, unsigned short default_port);
std::wstring BuildDisplayEndpoint(const std::wstring& host, unsigned short port);
bool IsRustDeskPublicHost(const std::wstring& host);

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(
      CP_UTF8,
      0,
      value.c_str(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string result(size, '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.c_str(),
      static_cast<int>(value.size()),
      result.data(),
      size,
      nullptr,
      nullptr);
  return result;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring result(size, L'\0');
  MultiByteToWideChar(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      size);
  return result;
}

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return std::wstring();
  }
  const size_t last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::wstring NormalizePathSeparators(const std::wstring& value) {
  std::wstring normalized = value;
  for (size_t index = 0; index < normalized.size(); ++index) {
    if (normalized[index] == L'/') {
      normalized[index] = L'\\';
    }
  }
  return normalized;
}

bool IsAbsoluteWindowsPath(const std::wstring& value) {
  return (value.size() >= 2 && value[1] == L':') ||
         (value.size() >= 2 && value[0] == L'\\' && value[1] == L'\\');
}

std::wstring GetFileNamePart(const std::wstring& path) {
  const size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return path;
  }
  return path.substr(separator + 1);
}

bool IsTraditionalChineseLanguageId(LANGID language_id) {
  if (PRIMARYLANGID(language_id) != LANG_CHINESE) {
    return false;
  }
  const WORD sub_language = SUBLANGID(language_id);
  return sub_language == SUBLANG_CHINESE_TRADITIONAL ||
         sub_language == SUBLANG_CHINESE_HONGKONG ||
         sub_language == SUBLANG_CHINESE_MACAU;
}

const LanguageEntry* GetBuiltinLanguageEntries(
    bool traditional,
    size_t* entry_count) {
  if (entry_count == nullptr) {
    return nullptr;
  }
  if (traditional) {
    *entry_count = _countof(kTraditionalChineseLanguageEntries);
    return kTraditionalChineseLanguageEntries;
  }
  *entry_count = _countof(kEnglishLanguageEntries);
  return kEnglishLanguageEntries;
}

const LanguageEntry* GetBuiltinStatusLanguageEntries(
    bool traditional,
    size_t* entry_count) {
  if (entry_count == nullptr) {
    return nullptr;
  }
  if (traditional) {
    *entry_count = _countof(kTraditionalChineseStatusLanguageEntries);
    return kTraditionalChineseStatusLanguageEntries;
  }
  *entry_count = _countof(kEnglishStatusLanguageEntries);
  return kEnglishStatusLanguageEntries;
}

const wchar_t* LookupBuiltinLanguageEntry(
    bool traditional,
    const std::wstring& key) {
  size_t entry_count = 0;
  const LanguageEntry* entries = GetBuiltinLanguageEntries(traditional, &entry_count);
  if (entries == nullptr) {
    return nullptr;
  }
  for (size_t index = 0; index < entry_count; ++index) {
    if (_wcsicmp(entries[index].key, key.c_str()) == 0) {
      return entries[index].value;
    }
  }

  entry_count = 0;
  entries = GetBuiltinStatusLanguageEntries(traditional, &entry_count);
  if (entries == nullptr) {
    return nullptr;
  }
  for (size_t index = 0; index < entry_count; ++index) {
    if (_wcsicmp(entries[index].key, key.c_str()) == 0) {
      return entries[index].value;
    }
  }
  return nullptr;
}

std::wstring BuildDefaultLanguageFileContent(bool traditional) {
  std::wstring content;
  content += L"# RustDeskQS Host language file\r\n";
  content += L"# Copy this file to another name such as jp.txt and translate only the values.\r\n";
  content += L"# Keep the keys on the left side unchanged.\r\n";
  content += traditional ? L"_base=tw\r\n" : L"_base=en\r\n";

  size_t entry_count = 0;
  const LanguageEntry* entries = GetBuiltinLanguageEntries(traditional, &entry_count);
  for (size_t index = 0; index < entry_count; ++index) {
    content += entries[index].key;
    content += L"=";
    content += entries[index].value;
    content += L"\r\n";
  }

  entry_count = 0;
  entries = GetBuiltinStatusLanguageEntries(traditional, &entry_count);
  for (size_t index = 0; index < entry_count; ++index) {
    content += entries[index].key;
    content += L"=";
    content += entries[index].value;
    content += L"\r\n";
  }
  return content;
}

bool WriteUtf8TextFile(const std::wstring& path, const std::wstring& text) {
  const HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  const unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF};
  DWORD written = 0;
  BOOL ok = WriteFile(file, utf8_bom, sizeof(utf8_bom), &written, nullptr);
  const std::string utf8 = WideToUtf8(text);
  if (ok && !utf8.empty()) {
    ok = WriteFile(
             file,
             utf8.data(),
             static_cast<DWORD>(utf8.size()),
             &written,
             nullptr) != FALSE;
  }
  CloseHandle(file);
  return ok != FALSE;
}

bool ReadUtf8TextFile(const std::wstring& path, std::wstring* text) {
  if (text == nullptr) {
    return false;
  }
  text->clear();

  const HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    CloseHandle(file);
    return false;
  }
  if (size.QuadPart == 0) {
    CloseHandle(file);
    return true;
  }
  if (size.QuadPart > (1024 * 1024)) {
    CloseHandle(file);
    return false;
  }

  std::vector<char> buffer(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(
      file,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      &read,
      nullptr);
  CloseHandle(file);
  if (!ok) {
    return false;
  }
  buffer.resize(read);

  if (buffer.size() >= 2 &&
      static_cast<unsigned char>(buffer[0]) == 0xFF &&
      static_cast<unsigned char>(buffer[1]) == 0xFE) {
    std::wstring wide;
    wide.reserve((buffer.size() - 2) / 2);
    for (size_t index = 2; index + 1 < buffer.size(); index += 2) {
      const wchar_t value = static_cast<wchar_t>(
          static_cast<unsigned char>(buffer[index]) |
          (static_cast<unsigned char>(buffer[index + 1]) << 8));
      wide.push_back(value);
    }
    if (!wide.empty() && wide.back() == L'\0') {
      wide.pop_back();
    }
    *text = wide;
    return true;
  }

  size_t start = 0;
  if (buffer.size() >= 3 &&
      static_cast<unsigned char>(buffer[0]) == 0xEF &&
      static_cast<unsigned char>(buffer[1]) == 0xBB &&
      static_cast<unsigned char>(buffer[2]) == 0xBF) {
    start = 3;
  }

  const std::string text_bytes(buffer.begin() + start, buffer.end());
  std::wstring wide = Utf8ToWide(text_bytes);
  if (wide.empty() && !text_bytes.empty()) {
    wide = AnsiToWideCompat(text_bytes);
  }
  *text = wide;
  return true;
}

void ParseLanguageFileText(
    const std::wstring& text,
    std::unordered_map<std::wstring, std::wstring>* values,
    bool* base_is_traditional) {
  if (values == nullptr) {
    return;
  }

  std::wistringstream stream(text);
  std::wstring line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line[0] == 0xFEFF) {
      line.erase(line.begin());
    }
    const std::wstring trimmed_line = Trim(line);
    if (trimmed_line.empty() || trimmed_line[0] == L'#' || trimmed_line[0] == L';') {
      continue;
    }

    const size_t separator = trimmed_line.find(L'=');
    if (separator == std::wstring::npos) {
      continue;
    }

    const std::wstring key = Trim(trimmed_line.substr(0, separator));
    const std::wstring value = Trim(trimmed_line.substr(separator + 1));
    if (key.empty()) {
      continue;
    }

    if (_wcsicmp(key.c_str(), L"_base") == 0) {
      if (base_is_traditional != nullptr) {
        *base_is_traditional = _wcsicmp(value.c_str(), L"en") != 0;
      }
      continue;
    }

    (*values)[key] = value;
  }
}

bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  return _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}

void ReplaceAllSubstrings(
    std::wstring* value,
    const std::wstring& from,
    const std::wstring& to) {
  if (value == nullptr || from.empty()) {
    return;
  }
  size_t position = 0;
  while ((position = value->find(from, position)) != std::wstring::npos) {
    value->replace(position, from.size(), to);
    position += to.size();
  }
}

ParsedHostPort ParseHostPort(const std::wstring& endpoint, unsigned short default_port);
ParsedHostPort ParseHostPort(const std::wstring& endpoint, unsigned short default_port) {
  ParsedHostPort parsed;
  parsed.port = default_port;
  const std::wstring trimmed = Trim(endpoint);
  if (trimmed.empty()) {
    return parsed;
  }

  if (trimmed.front() == L'[') {
    const size_t closing = trimmed.find(L']');
    if (closing != std::wstring::npos) {
      parsed.host = trimmed.substr(1, closing - 1);
      if (closing + 1 < trimmed.size() && trimmed[closing + 1] == L':') {
        const unsigned long value = wcstoul(trimmed.c_str() + closing + 2, nullptr, 10);
        if (value > 0 && value <= 65535) {
          parsed.port = static_cast<unsigned short>(value);
        }
      }
      return parsed;
    }
  }

  const size_t last_colon = trimmed.find_last_of(L':');
  const size_t first_colon = trimmed.find_first_of(L':');
  if (last_colon != std::wstring::npos && first_colon == last_colon) {
    const std::wstring host = trimmed.substr(0, last_colon);
    const std::wstring port_text = trimmed.substr(last_colon + 1);
    const unsigned long value = wcstoul(port_text.c_str(), nullptr, 10);
    if (!host.empty() && value > 0 && value <= 65535) {
      parsed.host = host;
      parsed.port = static_cast<unsigned short>(value);
      return parsed;
    }
  }

  parsed.host = trimmed;
  return parsed;
}

std::wstring BuildDisplayEndpoint(const std::wstring& host, unsigned short port) {
  if (host.find(L':') != std::wstring::npos && host.find(L']') == std::wstring::npos) {
    return std::wstring(L"[") + host + L"]:" + std::to_wstring(port);
  }
  return host + L":" + std::to_wstring(port);
}

bool IsRustDeskPublicHost(const std::wstring& host) {
  return _wcsicmp(host.c_str(), L"rs-ny.rustdesk.com") == 0;
}

std::wstring BytesToHex(const std::vector<unsigned char>& bytes) {
  std::wstring result;
  result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    result.push_back(kHexDigits[(byte >> 4) & 0x0F]);
    result.push_back(kHexDigits[byte & 0x0F]);
  }
  return result;
}

std::vector<unsigned char> HexToBytes(const std::wstring& hex) {
  auto hex_value = [](wchar_t c) -> int {
    if (c >= L'0' && c <= L'9') {
      return static_cast<int>(c - L'0');
    }
    if (c >= L'a' && c <= L'f') {
      return static_cast<int>(10 + (c - L'a'));
    }
    if (c >= L'A' && c <= L'F') {
      return static_cast<int>(10 + (c - L'A'));
    }
    return -1;
  };

  if (hex.size() % 2 != 0) {
    return {};
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t index = 0; index < hex.size(); index += 2) {
    const int high = hex_value(hex[index]);
    const int low = hex_value(hex[index + 1]);
    if (high < 0 || low < 0) {
      return {};
    }
    bytes.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return bytes;
}

std::wstring FormatDisplayHostId(const std::wstring& host_id) {
  bool all_digits = !host_id.empty();
  for (wchar_t c : host_id) {
    if (c < L'0' || c > L'9') {
      all_digits = false;
      break;
    }
  }
  if (!all_digits || host_id.size() <= 3) {
    return host_id;
  }

  std::wstring formatted;
  for (size_t index = 0; index < host_id.size(); ++index) {
    if (index != 0 && (index % 3) == 0) {
      formatted.push_back(L' ');
    }
    formatted.push_back(host_id[index]);
  }
  return formatted;
}

std::wstring ProtectLocalMachineString(const std::wstring& plain_text) {
  if (plain_text.empty()) {
    return std::wstring();
  }
  const std::string utf8 = WideToUtf8(plain_text);
  DATA_BLOB input_blob = {};
  input_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.data()));
  input_blob.cbData = static_cast<DWORD>(utf8.size());

  DATA_BLOB output_blob = {};
  if (!CryptProtectData(
          &input_blob,
          L"RustDeskQSCPP",
          nullptr,
          nullptr,
          nullptr,
          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
          &output_blob)) {
    return std::wstring();
  }

  std::vector<unsigned char> encrypted(
      output_blob.pbData,
      output_blob.pbData + output_blob.cbData);
  LocalFree(output_blob.pbData);
  return BytesToHex(encrypted);
}

std::wstring UnprotectLocalMachineString(const std::wstring& protected_hex) {
  const std::vector<unsigned char> encrypted = HexToBytes(protected_hex);
  if (encrypted.empty()) {
    return std::wstring();
  }

  DATA_BLOB input_blob = {};
  input_blob.pbData = const_cast<BYTE*>(encrypted.data());
  input_blob.cbData = static_cast<DWORD>(encrypted.size());

  DATA_BLOB output_blob = {};
  if (!CryptUnprotectData(
          &input_blob,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          CRYPTPROTECT_UI_FORBIDDEN,
          &output_blob)) {
    return std::wstring();
  }

  std::string decrypted(
      reinterpret_cast<const char*>(output_blob.pbData),
      reinterpret_cast<const char*>(output_blob.pbData) + output_blob.cbData);
  LocalFree(output_blob.pbData);
  return Utf8ToWide(decrypted);
}

struct SimplePromptDialogState {
  HINSTANCE instance = nullptr;
  HWND owner = nullptr;
  HWND hwnd = nullptr;
  HWND label = nullptr;
  HWND edit = nullptr;
  HWND ok_button = nullptr;
  HWND cancel_button = nullptr;
  HFONT font = nullptr;
  std::wstring title;
  std::wstring prompt;
  std::wstring initial_value;
  std::wstring ok_text;
  std::wstring cancel_text;
  std::wstring result;
  bool password_mode = false;
  bool accepted = false;
  bool done = false;
  int max_length = 64;
};

LRESULT CALLBACK SimplePromptDialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* state =
      reinterpret_cast<SimplePromptDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
    state = reinterpret_cast<SimplePromptDialogState*>(create_struct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    if (state != nullptr) {
      state->hwnd = hwnd;
    }
  }

  if (state == nullptr) {
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }

  switch (message) {
    case WM_CREATE: {
      const int margin = ScaleForSystemDpi(16);
      const int width = ScaleForSystemDpi(360);
      const int label_height = ScaleForSystemDpi(42);
      const int edit_height = ScaleForSystemDpi(28);
      const int button_width = ScaleForSystemDpi(88);
      const int button_height = ScaleForSystemDpi(28);
      const int gap = ScaleForSystemDpi(10);

      state->label = CreateWindowExW(
          0,
          L"STATIC",
          state->prompt.c_str(),
          WS_CHILD | WS_VISIBLE,
          margin,
          margin,
          width - margin * 2,
          label_height,
          hwnd,
          nullptr,
          state->instance,
          nullptr);

      state->edit = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          L"EDIT",
          state->initial_value.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
              (state->password_mode ? ES_PASSWORD : 0),
          margin,
          margin + label_height + ScaleForSystemDpi(4),
          width - margin * 2,
          edit_height,
          hwnd,
          reinterpret_cast<HMENU>(100),
          state->instance,
          nullptr);
      SendMessageW(state->edit, EM_LIMITTEXT, static_cast<WPARAM>(state->max_length), 0);

      state->ok_button = CreateWindowExW(
          0,
          L"BUTTON",
          state->ok_text.empty() ? L"OK" : state->ok_text.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          width - margin - button_width * 2 - gap,
          margin + label_height + edit_height + ScaleForSystemDpi(18),
          button_width,
          button_height,
          hwnd,
          reinterpret_cast<HMENU>(IDOK),
          state->instance,
          nullptr);

      state->cancel_button = CreateWindowExW(
          0,
          L"BUTTON",
          state->cancel_text.empty() ? L"Cancel" : state->cancel_text.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          width - margin - button_width,
          margin + label_height + edit_height + ScaleForSystemDpi(18),
          button_width,
          button_height,
          hwnd,
          reinterpret_cast<HMENU>(IDCANCEL),
          state->instance,
          nullptr);

      const HWND controls[] = {state->label, state->edit, state->ok_button, state->cancel_button};
      for (HWND control : controls) {
        if (control != nullptr && state->font != nullptr) {
          SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        }
      }

      if (state->edit != nullptr) {
        SetFocus(state->edit);
      }
      return 0;
    }
    case WM_COMMAND: {
      const UINT command_id = LOWORD(w_param);
      if (command_id == IDOK) {
        const int length = GetWindowTextLengthW(state->edit);
        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(state->edit, buffer.data(), length + 1);
        state->result.assign(buffer.data(), length);
        state->accepted = true;
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (command_id == IDCANCEL) {
        state->accepted = false;
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      state->accepted = false;
      state->done = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (state->owner != nullptr) {
        EnableWindow(state->owner, TRUE);
        SetForegroundWindow(state->owner);
      }
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

bool PromptForTextInput(
    HINSTANCE instance,
    HWND owner,
    HFONT font,
    const std::wstring& title,
    const std::wstring& prompt,
    const std::wstring& initial_value,
    bool password_mode,
    int max_length,
    const std::wstring& ok_text,
    const std::wstring& cancel_text,
    std::wstring* result) {
  if (result == nullptr) {
    return false;
  }

  const wchar_t kPromptWindowClass[] = L"RustDeskCppSimplePromptWindow";
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = SimplePromptDialogProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  window_class.lpszClassName = kPromptWindowClass;
  RegisterClassExW(&window_class);

  SimplePromptDialogState state;
  state.instance = instance;
  state.owner = owner;
  state.font = font;
  state.title = title;
  state.prompt = prompt;
  state.initial_value = initial_value;
  state.ok_text = ok_text;
  state.cancel_text = cancel_text;
  state.password_mode = password_mode;
  state.max_length = max_length;

  if (owner != nullptr) {
    EnableWindow(owner, FALSE);
  }

  const int width = ScaleForSystemDpi(360);
  const int height = ScaleForSystemDpi(160);
  RECT owner_rect = {};
  if (owner != nullptr) {
    GetWindowRect(owner, &owner_rect);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
  }
  const int x = owner_rect.left + (((owner_rect.right - owner_rect.left) - width) / 2);
  const int y = owner_rect.top + (((owner_rect.bottom - owner_rect.top) - height) / 2);

  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME,
      kPromptWindowClass,
      title.c_str(),
      WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
      x,
      y,
      width,
      height,
      owner,
      nullptr,
      instance,
      &state);
  if (dialog == nullptr) {
    if (owner != nullptr) {
      EnableWindow(owner, TRUE);
      SetForegroundWindow(owner);
    }
    return false;
  }

  ShowWindow(dialog, SW_SHOWNORMAL);
  UpdateWindow(dialog);

  MSG message = {};
  while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (state.hwnd != nullptr && IsDialogMessageW(state.hwnd, &message)) {
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (state.accepted) {
    *result = state.result;
    return true;
  }
  return false;
}

struct AboutDialogState {
  HINSTANCE instance = nullptr;
  HWND owner = nullptr;
  HWND hwnd = nullptr;
  HWND icon = nullptr;
  HWND text = nullptr;
  HWND ok_button = nullptr;
  HWND update_button = nullptr;
  HFONT font = nullptr;
  std::wstring title;
  std::wstring about_text;
  std::wstring ok_text;
  std::wstring update_text;
  std::wstring open_failed_text;
  std::wstring project_url;
  bool done = false;
};

LRESULT CALLBACK AboutDialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
    state = reinterpret_cast<AboutDialogState*>(create_struct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    if (state != nullptr) {
      state->hwnd = hwnd;
    }
  }

  if (state == nullptr) {
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }

  switch (message) {
    case WM_CREATE: {
      const int margin = ScaleForSystemDpi(16);
      const int icon_size = ScaleForSystemDpi(32);
      const int icon_gap = ScaleForSystemDpi(12);
      const int button_height = ScaleForSystemDpi(28);
      const int ok_width = ScaleForSystemDpi(88);
      const int update_width = ScaleForSystemDpi(124);
      const int button_gap = ScaleForSystemDpi(10);
      const int button_top_gap = ScaleForSystemDpi(16);

      RECT client_rect = {};
      GetClientRect(hwnd, &client_rect);
      const int client_width = client_rect.right - client_rect.left;
      const int client_height = client_rect.bottom - client_rect.top;
      const int button_top = client_height - margin - button_height;

      state->icon = CreateWindowExW(
          0,
          L"STATIC",
          nullptr,
          WS_CHILD | WS_VISIBLE | SS_ICON,
          margin,
          margin,
          icon_size,
          icon_size,
          hwnd,
          nullptr,
          state->instance,
          nullptr);
      if (state->icon != nullptr) {
        SendMessageW(
            state->icon,
            STM_SETIMAGE,
            IMAGE_ICON,
            reinterpret_cast<LPARAM>(LoadIconW(nullptr, IDI_INFORMATION)));
      }

      const int text_left = margin + icon_size + icon_gap;
      const int text_height = button_top - margin - button_top_gap;
      state->text = CreateWindowExW(
          0,
          L"STATIC",
          state->about_text.c_str(),
          WS_CHILD | WS_VISIBLE,
          text_left,
          margin,
          client_width - text_left - margin,
          text_height,
          hwnd,
          nullptr,
          state->instance,
          nullptr);

      const int ok_left = client_width - margin - ok_width;
      const int update_left = ok_left - button_gap - update_width;
      state->update_button = CreateWindowExW(
          0,
          L"BUTTON",
          state->update_text.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          update_left,
          button_top,
          update_width,
          button_height,
          hwnd,
          reinterpret_cast<HMENU>(1001),
          state->instance,
          nullptr);
      state->ok_button = CreateWindowExW(
          0,
          L"BUTTON",
          state->ok_text.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          ok_left,
          button_top,
          ok_width,
          button_height,
          hwnd,
          reinterpret_cast<HMENU>(IDOK),
          state->instance,
          nullptr);

      const HWND controls[] = {state->text, state->update_button, state->ok_button};
      for (HWND control : controls) {
        if (control != nullptr && state->font != nullptr) {
          SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        }
      }

      if (state->ok_button != nullptr) {
        SetFocus(state->ok_button);
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(w_param);
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_COMMAND: {
      const UINT command_id = LOWORD(w_param);
      if (command_id == IDOK || command_id == IDCANCEL) {
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (command_id == 1001) {
        const HINSTANCE open_result = ShellExecuteW(
            hwnd,
            L"open",
            state->project_url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(open_result) <= 32) {
          MessageBoxW(
              hwnd,
              state->open_failed_text.c_str(),
              state->title.c_str(),
              MB_OK | MB_ICONWARNING);
          return 0;
        }
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      state->done = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (state->owner != nullptr) {
        EnableWindow(state->owner, TRUE);
        SetForegroundWindow(state->owner);
      }
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

void ShowAboutDialogModal(
    HINSTANCE instance,
    HWND owner,
    HFONT font,
    HICON icon,
    const std::wstring& title,
    const std::wstring& about_text,
    const std::wstring& ok_text,
    const std::wstring& update_text,
    const std::wstring& open_failed_text,
    const std::wstring& project_url) {
  const wchar_t kAboutWindowClass[] = L"RustDeskCppAboutDialogWindow";
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = AboutDialogProc;
  window_class.hInstance = instance;
  window_class.hIcon = icon != nullptr ? icon : LoadIconW(nullptr, IDI_INFORMATION);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kAboutWindowClass;
  window_class.hIconSm = window_class.hIcon;
  RegisterClassExW(&window_class);

  AboutDialogState state;
  state.instance = instance;
  state.owner = owner;
  state.font = font;
  state.title = title;
  state.about_text = about_text;
  state.ok_text = ok_text;
  state.update_text = update_text;
  state.open_failed_text = open_failed_text;
  state.project_url = project_url;

  if (owner != nullptr) {
    EnableWindow(owner, FALSE);
  }

  const int width = ScaleForSystemDpi(540);
  const int height = ScaleForSystemDpi(400);
  RECT owner_rect = {};
  if (owner != nullptr) {
    GetWindowRect(owner, &owner_rect);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
  }
  const int x = owner_rect.left + (((owner_rect.right - owner_rect.left) - width) / 2);
  const int y = owner_rect.top + (((owner_rect.bottom - owner_rect.top) - height) / 2);

  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME,
      kAboutWindowClass,
      title.c_str(),
      WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
      x,
      y,
      width,
      height,
      owner,
      nullptr,
      instance,
      &state);
  if (dialog == nullptr) {
    if (owner != nullptr) {
      EnableWindow(owner, TRUE);
      SetForegroundWindow(owner);
    }
    return;
  }

  ShowWindow(dialog, SW_SHOWNORMAL);
  UpdateWindow(dialog);

  MSG message = {};
  while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (state.hwnd != nullptr && IsDialogMessageW(state.hwnd, &message)) {
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

std::vector<unsigned char> DecodeBase64(const std::string& text) {
  auto decode_char = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return 26 + (c - 'a');
    }
    if (c >= '0' && c <= '9') {
      return 52 + (c - '0');
    }
    if (c == '+') {
      return 62;
    }
    if (c == '/') {
      return 63;
    }
    return -1;
  };

  std::string normalized;
  normalized.reserve(text.size());
  for (char c : text) {
    if (c != '\r' && c != '\n' && c != ' ' && c != '\t') {
      normalized.push_back(c);
    }
  }

  if (normalized.empty() || (normalized.size() % 4U) != 0U) {
    return {};
  }

  std::vector<unsigned char> output;
  output.reserve((normalized.size() / 4U) * 3U);
  for (size_t i = 0; i < normalized.size(); i += 4U) {
    const char c0 = normalized[i];
    const char c1 = normalized[i + 1];
    const char c2 = normalized[i + 2];
    const char c3 = normalized[i + 3];

    const int v0 = decode_char(c0);
    const int v1 = decode_char(c1);
    if (v0 < 0 || v1 < 0) {
      return {};
    }

    const int v2 = (c2 == '=') ? 0 : decode_char(c2);
    const int v3 = (c3 == '=') ? 0 : decode_char(c3);
    if ((c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) {
      return {};
    }

    output.push_back(static_cast<unsigned char>((v0 << 2) | (v1 >> 4)));
    if (c2 != '=') {
      output.push_back(static_cast<unsigned char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
    }
    if (c3 != '=') {
      output.push_back(static_cast<unsigned char>(((v2 & 0x03) << 6) | v3));
    }
  }
  return output;
}

void AppendVarint(uint64_t value, std::vector<unsigned char>* out) {
  while (value >= 0x80U) {
    out->push_back(static_cast<unsigned char>((value & 0x7FU) | 0x80U));
    value >>= 7U;
  }
  out->push_back(static_cast<unsigned char>(value));
}

void AppendFieldKey(unsigned int field_number, unsigned int wire_type, std::vector<unsigned char>* out) {
  AppendVarint((static_cast<uint64_t>(field_number) << 3U) | wire_type, out);
}

void AppendVarintField(unsigned int field_number, uint64_t value, std::vector<unsigned char>* out) {
  AppendFieldKey(field_number, 0U, out);
  AppendVarint(value, out);
}

void AppendSint32Field(unsigned int field_number, int value, std::vector<unsigned char>* out) {
  const uint32_t zigzag =
      (static_cast<uint32_t>(value) << 1U) ^ static_cast<uint32_t>(value >> 31);
  AppendFieldKey(field_number, 0U, out);
  AppendVarint(zigzag, out);
}

void AppendFixed64Field(unsigned int field_number, uint64_t value, std::vector<unsigned char>* out) {
  AppendFieldKey(field_number, 1U, out);
  for (int index = 0; index < 8; ++index) {
    out->push_back(static_cast<unsigned char>((value >> (index * 8)) & 0xFFU));
  }
}

void AppendDoubleField(unsigned int field_number, double value, std::vector<unsigned char>* out) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendFixed64Field(field_number, bits, out);
}

void AppendLengthDelimitedField(
    unsigned int field_number,
    const std::vector<unsigned char>& value,
    std::vector<unsigned char>* out) {
  AppendFieldKey(field_number, 2U, out);
  AppendVarint(value.size(), out);
  out->insert(out->end(), value.begin(), value.end());
}

void AppendStringField(unsigned int field_number, const std::string& value, std::vector<unsigned char>* out) {
  std::vector<unsigned char> bytes(value.begin(), value.end());
  AppendLengthDelimitedField(field_number, bytes, out);
}

void AppendBytesField(unsigned int field_number, const std::vector<unsigned char>& value, std::vector<unsigned char>* out) {
  AppendLengthDelimitedField(field_number, value, out);
}

std::wstring GenerateSessionUuidText() {
  GUID guid = {};
  if (CoCreateGuid(&guid) != S_OK) {
    return L"00000000-0000-0000-0000-000000000000";
  }

  wchar_t buffer[64] = {};
  if (StringFromGUID2(guid, buffer, static_cast<int>(_countof(buffer))) <= 0) {
    return L"00000000-0000-0000-0000-000000000000";
  }

  std::wstring value(buffer);
  if (!value.empty() && value.front() == L'{') {
    value.erase(value.begin());
  }
  if (!value.empty() && value.back() == L'}') {
    value.pop_back();
  }
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

std::wstring FormatObservedFields(const std::vector<unsigned int>& fields) {
  if (fields.empty()) {
    return L"none";
  }
  std::wstringstream stream;
  for (size_t index = 0; index < fields.size(); ++index) {
    if (index > 0) {
      stream << L",";
    }
    stream << fields[index];
  }
  return stream.str();
}

std::vector<unsigned char> EncodeMangledIpv4SocketAddress(const sockaddr_in& address) {
  const auto now = std::chrono::system_clock::now();
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
      now.time_since_epoch()).count();
  const unsigned int timestamp =
      static_cast<unsigned int>(static_cast<unsigned long long>(micros) & 0xFFFFFFFFULL);

  const unsigned char* ip_octets =
      reinterpret_cast<const unsigned char*>(&address.sin_addr.S_un.S_addr);
  const unsigned int ip_le =
      static_cast<unsigned int>(ip_octets[0]) |
      (static_cast<unsigned int>(ip_octets[1]) << 8U) |
      (static_cast<unsigned int>(ip_octets[2]) << 16U) |
      (static_cast<unsigned int>(ip_octets[3]) << 24U);
  const unsigned int port = ntohs(address.sin_port);
  const unsigned long long top = static_cast<unsigned long long>(ip_le) + timestamp;
  unsigned long long lo = top << 49U;
  unsigned long long hi = top >> 15U;

  const unsigned long long mid = static_cast<unsigned long long>(timestamp) << 17U;
  const unsigned long long lo_before_mid = lo;
  lo += mid;
  if (lo < lo_before_mid) {
    ++hi;
  }

  const unsigned long long tail =
      static_cast<unsigned long long>(port + (timestamp & 0xFFFFU));
  const unsigned long long lo_before_tail = lo;
  lo += tail;
  if (lo < lo_before_tail) {
    ++hi;
  }

  std::array<unsigned char, 16> bytes = {};
  for (size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<unsigned char>((lo >> (index * 8U)) & 0xFFU);
    bytes[index + 8] = static_cast<unsigned char>((hi >> (index * 8U)) & 0xFFU);
  }

  size_t used = bytes.size();
  while (used > 0 && bytes[used - 1] == 0) {
    --used;
  }
  return std::vector<unsigned char>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(used));
}

bool ReadVarint(const std::vector<unsigned char>& data, size_t* offset, uint64_t* value) {
  uint64_t result = 0;
  int shift = 0;
  while (*offset < data.size() && shift <= 63) {
    const unsigned char byte = data[*offset];
    ++(*offset);
    result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

bool ReadLengthDelimited(
    const std::vector<unsigned char>& data,
    size_t* offset,
    std::vector<unsigned char>* value) {
  uint64_t size = 0;
  if (!ReadVarint(data, offset, &size)) {
    return false;
  }
  if (size > data.size() - *offset) {
    return false;
  }
  value->assign(data.begin() + static_cast<std::ptrdiff_t>(*offset),
                data.begin() + static_cast<std::ptrdiff_t>(*offset + size));
  *offset += static_cast<size_t>(size);
  return true;
}

bool SkipField(unsigned int wire_type, const std::vector<unsigned char>& data, size_t* offset) {
  switch (wire_type) {
    case 0: {
      uint64_t ignored = 0;
      return ReadVarint(data, offset, &ignored);
    }
    case 2: {
      std::vector<unsigned char> ignored;
      return ReadLengthDelimited(data, offset, &ignored);
    }
    default:
      return false;
  }
}

std::vector<unsigned char> EncodeRegisterPkMessage(
    const std::wstring& host_id,
    const std::vector<unsigned char>& uuid_bytes,
    const std::vector<unsigned char>& public_key_bytes) {
  std::vector<unsigned char> register_pk;
  AppendStringField(1U, WideToUtf8(host_id), &register_pk);
  AppendBytesField(2U, uuid_bytes, &register_pk);
  AppendBytesField(3U, public_key_bytes, &register_pk);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(15U, register_pk, &message);
  return message;
}

std::vector<unsigned char> EncodeRegisterPeerMessage(
    const std::wstring& host_id,
    int serial) {
  std::vector<unsigned char> register_peer;
  AppendStringField(1U, WideToUtf8(host_id), &register_peer);
  AppendVarintField(2U, static_cast<uint64_t>(serial), &register_peer);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(6U, register_peer, &message);
  return message;
}

std::vector<unsigned char> EncodeRequestRelayMessage(
    const std::wstring& uuid_text,
    const std::wstring& licence_key) {
  std::vector<unsigned char> request_relay;
  AppendStringField(2U, WideToUtf8(uuid_text), &request_relay);
  if (!licence_key.empty()) {
    AppendStringField(6U, WideToUtf8(licence_key), &request_relay);
  }

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, request_relay, &message);
  return message;
}

std::vector<unsigned char> EncodeOutboundRequestRelayMessage(
    const std::wstring& target_id,
    const std::wstring& uuid_text,
    const std::wstring& relay_server,
    bool secure) {
  std::vector<unsigned char> request_relay;
  if (!target_id.empty()) {
    AppendStringField(1U, WideToUtf8(target_id), &request_relay);
  }
  if (!uuid_text.empty()) {
    AppendStringField(2U, WideToUtf8(uuid_text), &request_relay);
  }
  if (!relay_server.empty()) {
    AppendStringField(4U, WideToUtf8(relay_server), &request_relay);
  }
  AppendVarintField(5U, secure ? 1U : 0U, &request_relay);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, request_relay, &message);
  return message;
}

std::vector<unsigned char> EncodeOutboundRelayConnectMessage(
    const std::wstring& target_id,
    const std::wstring& uuid_text,
    const std::wstring& licence_key) {
  std::vector<unsigned char> request_relay;
  if (!target_id.empty()) {
    AppendStringField(1U, WideToUtf8(target_id), &request_relay);
  }
  if (!uuid_text.empty()) {
    AppendStringField(2U, WideToUtf8(uuid_text), &request_relay);
  }
  if (!licence_key.empty()) {
    AppendStringField(6U, WideToUtf8(licence_key), &request_relay);
  }
  AppendVarintField(7U, 0U, &request_relay);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, request_relay, &message);
  return message;
}

std::vector<unsigned char> EncodeRelayResponseMessage(
    const std::vector<unsigned char>& socket_addr,
    const std::wstring& relay_server,
    const std::wstring& uuid_text,
    const std::wstring& host_id,
    bool initiate) {
  std::vector<unsigned char> relay_response;
  AppendBytesField(1U, socket_addr, &relay_response);
  if (initiate) {
    if (!uuid_text.empty()) {
      AppendStringField(2U, WideToUtf8(uuid_text), &relay_response);
    }
    if (!relay_server.empty()) {
      AppendStringField(3U, WideToUtf8(relay_server), &relay_response);
    }
    if (!host_id.empty()) {
      AppendStringField(4U, WideToUtf8(host_id), &relay_response);
    }
  }
  AppendStringField(7U, WideToUtf8(kCppHostVersion), &relay_response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(19U, relay_response, &message);
  return message;
}

std::vector<unsigned char> EncodeLocalAddrMessage(
    const std::vector<unsigned char>& controller_socket_addr,
    const std::vector<unsigned char>& local_socket_addr,
    const std::wstring& relay_server,
    const std::wstring& host_id) {
  std::vector<unsigned char> local_addr;
  AppendBytesField(1U, controller_socket_addr, &local_addr);
  AppendBytesField(2U, local_socket_addr, &local_addr);
  if (!relay_server.empty()) {
    AppendStringField(3U, WideToUtf8(relay_server), &local_addr);
  }
  AppendStringField(4U, WideToUtf8(host_id), &local_addr);
  AppendStringField(5U, WideToUtf8(kCppHostVersion), &local_addr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(13U, local_addr, &message);
  return message;
}

std::vector<unsigned char> EncodeKeyExchangeMessage(
    const std::vector<unsigned char>& asymmetric_value,
    const std::vector<unsigned char>& symmetric_value) {
  std::vector<unsigned char> key_exchange;
  AppendBytesField(1U, asymmetric_value, &key_exchange);
  AppendBytesField(1U, symmetric_value, &key_exchange);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(25U, key_exchange, &message);
  return message;
}

std::vector<unsigned char> EncodeSignedIdMessage(
    const std::wstring& host_id,
    const std::vector<unsigned char>& signing_secret_key,
    std::array<unsigned char, crypto_box_PUBLICKEYBYTES>* curve_public_key,
    std::array<unsigned char, crypto_box_SECRETKEYBYTES>* curve_secret_key,
    std::wstring* error_text) {
  curve_public_key->fill(0);
  curve_secret_key->fill(0);
  if (crypto_box_keypair(curve_public_key->data(), curve_secret_key->data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"crypto_box_keypair failed";
    }
    return {};
  }

  std::vector<unsigned char> id_pk;
  AppendStringField(1U, WideToUtf8(host_id), &id_pk);
  std::vector<unsigned char> curve_public(curve_public_key->begin(), curve_public_key->end());
  AppendBytesField(2U, curve_public, &id_pk);

  std::vector<unsigned char> signed_id(
      id_pk.size() + crypto_sign_ed25519_BYTES,
      0);
  unsigned long long signed_size = 0;
  if (crypto_sign_ed25519(
          signed_id.data(),
          &signed_size,
          id_pk.data(),
          static_cast<unsigned long long>(id_pk.size()),
          signing_secret_key.data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"crypto_sign_ed25519 failed";
    }
    return {};
  }
  signed_id.resize(static_cast<size_t>(signed_size));

  std::vector<unsigned char> signed_id_message;
  AppendBytesField(1U, signed_id, &signed_id_message);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(3U, signed_id_message, &message);
  return message;
}

std::vector<unsigned char> EncodeLoginResponseErrorMessage(const std::wstring& error_message) {
  std::vector<unsigned char> login_response;
  AppendStringField(1U, WideToUtf8(error_message), &login_response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(8U, login_response, &message);
  return message;
}

std::wstring GetWindowsUsername() {
  wchar_t buffer[256] = {};
  DWORD size = static_cast<DWORD>(_countof(buffer));
  if (GetUserNameW(buffer, &size) && buffer[0] != L'\0') {
    std::wstring value(buffer);
    if (!value.empty() && value.back() == L'\0') {
      value.pop_back();
    }
    return value;
  }
  return L"user";
}

std::wstring GetWindowsHostname() {
  wchar_t buffer[256] = {};
  DWORD size = static_cast<DWORD>(_countof(buffer));
  if (GetComputerNameW(buffer, &size) && buffer[0] != L'\0') {
    return std::wstring(buffer, size);
  }
  return L"WIN-CPLUS";
}

std::vector<unsigned char> EncodeResolutionMessage(int width, int height) {
  std::vector<unsigned char> resolution;
  AppendVarintField(1U, static_cast<uint64_t>(width), &resolution);
  AppendVarintField(2U, static_cast<uint64_t>(height), &resolution);
  return resolution;
}

std::vector<unsigned char> EncodeDisplayInfoMessage() {
  std::vector<unsigned char> display;
  const DesktopCaptureBounds bounds = GetDesktopCaptureBounds();
  AppendSint32Field(1U, bounds.origin_x, &display);
  AppendSint32Field(2U, bounds.origin_y, &display);
  AppendVarintField(3U, static_cast<uint64_t>(bounds.width), &display);
  AppendVarintField(4U, static_cast<uint64_t>(bounds.height), &display);
  AppendStringField(5U, "Display 1", &display);
  AppendVarintField(6U, 1U, &display);
  AppendVarintField(7U, 0U, &display);
  AppendLengthDelimitedField(8U, EncodeResolutionMessage(bounds.width, bounds.height), &display);
  AppendDoubleField(9U, 1.0, &display);
  return display;
}

std::vector<unsigned char> EncodeFeaturesMessage() {
  std::vector<unsigned char> features;
  AppendVarintField(1U, 0U, &features);
  AppendVarintField(2U, 0U, &features);
  return features;
}

std::vector<unsigned char> EncodeSupportedEncodingMessage(const std::wstring& preferred_codec) {
  std::vector<unsigned char> encoding;
  const std::wstring effective_codec = GetEffectivePreferredCodec(preferred_codec);
  const bool want_h264 = _wcsicmp(effective_codec.c_str(), L"h264") == 0;
  const bool want_vp8 = true;
  AppendVarintField(1U, want_h264 ? 1U : 0U, &encoding);
  AppendVarintField(2U, 0U, &encoding);  // h265
  AppendVarintField(3U, want_vp8 ? 1U : 0U, &encoding);
  AppendVarintField(4U, 0U, &encoding);  // av1
  return encoding;
}

std::vector<unsigned char> EncodePeerInfoMessage(const HostConfig& config) {
  std::vector<unsigned char> peer_info;
  AppendStringField(1U, WideToUtf8(GetWindowsUsername()), &peer_info);
  AppendStringField(2U, WideToUtf8(GetWindowsHostname()), &peer_info);
  AppendStringField(3U, "Windows", &peer_info);
  AppendLengthDelimitedField(4U, EncodeDisplayInfoMessage(), &peer_info);
  AppendVarintField(5U, 0U, &peer_info);
  AppendVarintField(6U, 0U, &peer_info);
  AppendStringField(7U, WideToUtf8(kCppHostVersion), &peer_info);
  AppendLengthDelimitedField(9U, EncodeFeaturesMessage(), &peer_info);
  AppendLengthDelimitedField(10U, EncodeSupportedEncodingMessage(config.preferred_codec), &peer_info);
  return peer_info;
}

std::vector<unsigned char> EncodeLoginResponsePeerInfoMessage(const HostConfig& config) {
  std::vector<unsigned char> login_response;
  AppendLengthDelimitedField(2U, EncodePeerInfoMessage(config), &login_response);
  AppendVarintField(3U, 0U, &login_response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(8U, login_response, &message);
  return message;
}

std::vector<unsigned char> EncodeHashMessage(
    const std::string& salt,
    const std::string& challenge) {
  std::vector<unsigned char> hash_message;
  AppendStringField(1U, salt, &hash_message);
  AppendStringField(2U, challenge, &hash_message);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(9U, hash_message, &message);
  return message;
}

std::vector<unsigned char> EncodePublicKeyFallbackMessage() {
  std::vector<unsigned char> message;
  std::vector<unsigned char> public_key;
  AppendLengthDelimitedField(4U, public_key, &message);
  return message;
}

std::vector<unsigned char> EncodePublicKeyMessage(
    const std::vector<unsigned char>& asymmetric_value,
    const std::vector<unsigned char>& symmetric_value) {
  std::vector<unsigned char> public_key;
  AppendBytesField(1U, asymmetric_value, &public_key);
  AppendBytesField(2U, symmetric_value, &public_key);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(4U, public_key, &message);
  return message;
}

std::vector<unsigned char> EncodeCloseReasonMessage(const std::wstring& close_reason) {
  std::vector<unsigned char> misc;
  AppendStringField(9U, WideToUtf8(close_reason), &misc);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(19U, misc, &message);
  return message;
}

std::vector<unsigned char> ComputeSha256(const std::vector<unsigned char>& input) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest = {};
  crypto_hash_sha256(
      digest.data(),
      input.empty() ? nullptr : input.data(),
      static_cast<unsigned long long>(input.size()));
  return std::vector<unsigned char>(digest.begin(), digest.end());
}

std::vector<unsigned char> ComputeRustDeskLoginPasswordDigest(
    const std::wstring& plain_password,
    const std::string& salt,
    const std::string& challenge) {
  std::vector<unsigned char> h1_input;
  const std::string password_utf8 = WideToUtf8(plain_password);
  h1_input.reserve(password_utf8.size() + salt.size());
  h1_input.insert(h1_input.end(), password_utf8.begin(), password_utf8.end());
  h1_input.insert(h1_input.end(), salt.begin(), salt.end());
  std::vector<unsigned char> h1 = ComputeSha256(h1_input);

  std::vector<unsigned char> h2_input;
  h2_input.reserve(h1.size() + challenge.size());
  h2_input.insert(h2_input.end(), h1.begin(), h1.end());
  h2_input.insert(h2_input.end(), challenge.begin(), challenge.end());
  return ComputeSha256(h2_input);
}

bool DecompressZstdBytes(
    const std::vector<unsigned char>& compressed,
    std::vector<unsigned char>* plain,
    std::wstring* error_text) {
  plain->clear();
  if (compressed.empty()) {
    return true;
  }

  const unsigned long long frame_size =
      ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
    if (error_text != nullptr) {
      *error_text = L"invalid zstd clipboard payload";
    }
    return false;
  }
  if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN ||
      frame_size > static_cast<unsigned long long>(64 * 1024 * 1024)) {
    if (error_text != nullptr) {
      *error_text = L"unsupported zstd clipboard size";
    }
    return false;
  }

  plain->resize(static_cast<size_t>(frame_size));
  const size_t decompressed = ZSTD_decompress(
      plain->data(),
      plain->size(),
      compressed.data(),
      compressed.size());
  if (ZSTD_isError(decompressed) != 0) {
    if (error_text != nullptr) {
      *error_text = L"zstd decompress failed";
    }
    plain->clear();
    return false;
  }
  plain->resize(decompressed);
  return true;
}

bool GetClipboardPayloadBytes(
    const ClipboardMessageData& clipboard,
    std::vector<unsigned char>* payload,
    std::wstring* error_text) {
  payload->clear();
  if (!clipboard.compress) {
    *payload = clipboard.content;
    return true;
  }
  return DecompressZstdBytes(clipboard.content, payload, error_text);
}

bool ExtractClipboardWideText(
    const ClipboardMessageData& clipboard,
    std::wstring* text,
    std::wstring* error_text);

bool CompressZstdBytes(
    const std::vector<unsigned char>& plain,
    std::vector<unsigned char>* compressed,
    std::wstring* error_text) {
  compressed->clear();
  if (plain.empty()) {
    return true;
  }

  const size_t bound = ZSTD_compressBound(plain.size());
  if (bound == 0) {
    if (error_text != nullptr) {
      *error_text = L"zstd compress bound failed";
    }
    return false;
  }

  compressed->resize(bound);
  const size_t compressed_size = ZSTD_compress(
      compressed->data(),
      compressed->size(),
      plain.data(),
      plain.size(),
      1);
  if (ZSTD_isError(compressed_size) != 0) {
    compressed->clear();
    if (error_text != nullptr) {
      *error_text = L"zstd compress failed";
    }
    return false;
  }
  compressed->resize(compressed_size);
  return true;
}


int RustDeskFirstSetBitIndex(uint32_t value) {
  if (value == 0) {
    return -1;
  }
  int index = 0;
  while ((value & 1U) == 0U) {
    value >>= 1U;
    ++index;
  }
  return index;
}

bool RustDeskGetBitmapMetadata(HBITMAP bitmap, BITMAP* metadata) {
  if (bitmap == nullptr || metadata == nullptr) {
    return false;
  }
  std::memset(metadata, 0, sizeof(*metadata));
  return GetObjectW(
             bitmap,
             static_cast<int>(sizeof(*metadata)),
             metadata) == static_cast<int>(sizeof(*metadata));
}

bool RustDeskReadColorCursorRgba(
    HBITMAP color_bitmap,
    int width,
    int height,
    std::vector<unsigned char>* colors) {
  if (color_bitmap == nullptr || width < 1 || height < 1 || colors == nullptr) {
    return false;
  }

  const size_t byte_count =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
  if (byte_count < 16U || byte_count > 16U * 1024U * 1024U) {
    return false;
  }
  colors->assign(byte_count, 0);

  HDC screen_dc = GetDC(nullptr);
  if (screen_dc == nullptr) {
    colors->clear();
    return false;
  }
  HDC bitmap_dc = CreateCompatibleDC(screen_dc);
  if (bitmap_dc == nullptr) {
    ReleaseDC(nullptr, screen_dc);
    colors->clear();
    return false;
  }
  HGDIOBJ old_bitmap = SelectObject(bitmap_dc, color_bitmap);
  if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
    DeleteDC(bitmap_dc);
    ReleaseDC(nullptr, screen_dc);
    colors->clear();
    return false;
  }

  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = width;
  header.bV5Height = -height;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x000000FFU;
  header.bV5GreenMask = 0x0000FF00U;
  header.bV5BlueMask = 0x00FF0000U;
  header.bV5AlphaMask = 0xFF000000U;

  const int copied_rows = GetDIBits(
      bitmap_dc,
      color_bitmap,
      0,
      static_cast<UINT>(height),
      colors->data(),
      reinterpret_cast<BITMAPINFO*>(&header),
      DIB_RGB_COLORS);

  SelectObject(bitmap_dc, old_bitmap);
  DeleteDC(bitmap_dc);
  ReleaseDC(nullptr, screen_dc);
  if (copied_rows != height) {
    colors->clear();
    return false;
  }

  const int red_bit = RustDeskFirstSetBitIndex(header.bV5RedMask);
  const int green_bit = RustDeskFirstSetBitIndex(header.bV5GreenMask);
  const int blue_bit = RustDeskFirstSetBitIndex(header.bV5BlueMask);
  if (red_bit < 0 || green_bit < 0 || blue_bit < 0) {
    colors->clear();
    return false;
  }
  const int red_index = red_bit / 8;
  const int green_index = green_bit / 8;
  const int blue_index = blue_bit / 8;
  if (red_index < 0 || red_index > 3 ||
      green_index < 0 || green_index > 3 ||
      blue_index < 0 || blue_index > 3 ||
      red_index == green_index || red_index == blue_index ||
      green_index == blue_index) {
    colors->clear();
    return false;
  }
  const int alpha_index = 6 - red_index - green_index - blue_index;
  if (alpha_index < 0 || alpha_index > 3) {
    colors->clear();
    return false;
  }

  for (size_t offset = 0; offset + 3U < colors->size(); offset += 4U) {
    const unsigned char red = (*colors)[offset + red_index];
    const unsigned char green = (*colors)[offset + green_index];
    const unsigned char blue = (*colors)[offset + blue_index];
    const unsigned char alpha = (*colors)[offset + alpha_index];
    (*colors)[offset] = red;
    (*colors)[offset + 1U] = green;
    (*colors)[offset + 2U] = blue;
    (*colors)[offset + 3U] = alpha;
  }
  return true;
}

bool RustDeskFixColorCursorMask(
    std::vector<unsigned char>* mask_bits,
    std::vector<unsigned char>* colors,
    int width,
    int height,
    int mask_width_bytes) {
  if (mask_bits == nullptr || colors == nullptr || width < 1 || height < 1 ||
      mask_width_bytes < 1) {
    return false;
  }

  bool any_alpha = false;
  for (size_t offset = 3U; offset < colors->size(); offset += 4U) {
    if ((*colors)[offset] != 0U) {
      any_alpha = true;
      break;
    }
  }
  if (any_alpha) {
    return false;
  }

  const size_t packed_width_bytes =
      (static_cast<size_t>(width) + 7U) >> 3U;
  const size_t mask_size = mask_bits->size();
  for (int y = 0; y < height; ++y) {
    for (size_t x_byte = 0; x_byte < packed_width_bytes; ++x_byte) {
      const size_t packed_index =
          static_cast<size_t>(y) * packed_width_bytes + x_byte;
      const size_t bitmap_index =
          static_cast<size_t>(y) * static_cast<size_t>(mask_width_bytes) + x_byte;
      if (packed_index < mask_size && bitmap_index < mask_size) {
        (*mask_bits)[packed_index] =
            static_cast<unsigned char>(~(*mask_bits)[bitmap_index]);
      }
    }
  }

  const size_t bytes_per_row = static_cast<size_t>(width) * 4U;
  for (int y = 0; y < height; ++y) {
    unsigned char bit_mask = 0x80U;
    for (int x = 0; x < width; ++x) {
      const size_t mask_index =
          static_cast<size_t>(y) * packed_width_bytes +
          static_cast<size_t>(x >> 3);
      const size_t pixel_index =
          static_cast<size_t>(y) * bytes_per_row +
          static_cast<size_t>(x) * 4U;
      if (mask_index < mask_size && pixel_index + 3U < colors->size()) {
        if (((*mask_bits)[mask_index] & bit_mask) == 0U) {
          for (size_t channel = 0; channel < 4U; ++channel) {
            if ((*colors)[pixel_index + channel] != 0U) {
              (*mask_bits)[mask_index] ^= bit_mask;
              for (size_t clear_channel = channel; clear_channel < 4U; ++clear_channel) {
                (*colors)[pixel_index + clear_channel] = 0U;
              }
              break;
            }
          }
        }
      }
      bit_mask >>= 1U;
      if (bit_mask == 0U) {
        bit_mask = 0x80U;
      }
    }
  }

  size_t pixel_index = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t mask_index =
          static_cast<size_t>(y) * packed_width_bytes +
          static_cast<size_t>(x >> 3);
      unsigned char alpha = 0xFFU;
      if (mask_index < mask_size &&
          (((*mask_bits)[mask_index] << (x & 7)) & 0x80U) == 0U) {
        alpha = 0U;
      }
      if (pixel_index + 3U >= colors->size()) {
        return false;
      }
      const unsigned char blue = (*colors)[pixel_index];
      const unsigned char green = (*colors)[pixel_index + 1U];
      const unsigned char red = (*colors)[pixel_index + 2U];
      (*colors)[pixel_index] = red;
      (*colors)[pixel_index + 1U] = green;
      (*colors)[pixel_index + 2U] = blue;
      (*colors)[pixel_index + 3U] = alpha;
      pixel_index += 4U;
    }
  }
  return true;
}

bool RustDeskHandleMonochromeCursorMask(
    std::vector<unsigned char>* colors,
    const std::vector<unsigned char>& mask_bits,
    int width,
    int height,
    int mask_width_bytes,
    int mask_height) {
  if (colors == nullptr || width < 1 || height < 1 || mask_width_bytes < 1 ||
      mask_height < height * 2) {
    return false;
  }
  const size_t and_mask_size =
      static_cast<size_t>(mask_width_bytes) * static_cast<size_t>(mask_height);
  const size_t xor_offset =
      static_cast<size_t>(height) * static_cast<size_t>(mask_width_bytes);
  if (mask_bits.size() < and_mask_size || xor_offset >= mask_bits.size()) {
    return false;
  }

  bool needs_outline = false;
  size_t pixel_index = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t byte_index =
          static_cast<size_t>(y) * static_cast<size_t>(mask_width_bytes) +
          static_cast<size_t>(x >> 3);
      const int bit = 7 - (x & 7);
      const unsigned char bit_value = static_cast<unsigned char>(1U << bit);
      if (pixel_index + 3U >= colors->size() ||
          byte_index >= mask_bits.size() ||
          xor_offset + byte_index >= mask_bits.size()) {
        return false;
      }
      const bool and_set = (mask_bits[byte_index] & bit_value) != 0U;
      const bool xor_set = (mask_bits[xor_offset + byte_index] & bit_value) != 0U;
      if (!and_set) {
        const unsigned char value = xor_set ? 0xFFU : 0U;
        (*colors)[pixel_index] = value;
        (*colors)[pixel_index + 1U] = value;
        (*colors)[pixel_index + 2U] = value;
        (*colors)[pixel_index + 3U] = 0xFFU;
      } else if (xor_set) {
        (*colors)[pixel_index] = 0U;
        (*colors)[pixel_index + 1U] = 0U;
        (*colors)[pixel_index + 2U] = 0U;
        (*colors)[pixel_index + 3U] = 0xFFU;
        needs_outline = true;
      } else {
        (*colors)[pixel_index] = 0U;
        (*colors)[pixel_index + 1U] = 0U;
        (*colors)[pixel_index + 2U] = 0U;
        (*colors)[pixel_index + 3U] = 0U;
      }
      pixel_index += 4U;
    }
  }
  return needs_outline;
}

void RustDeskDrawCursorOutline(
    const std::vector<unsigned char>& input,
    int width,
    int height,
    std::vector<unsigned char>* output) {
  if (output == nullptr || width < 1 || height < 1) {
    return;
  }
  const int output_width = width + 2;
  const int output_height = height + 2;
  output->assign(
      static_cast<size_t>(output_width) *
          static_cast<size_t>(output_height) * 4U,
      0U);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t input_index =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) * 4U;
      if (input_index + 3U >= input.size() || input[input_index + 3U] == 0U) {
        continue;
      }
      for (int outline_y = y; outline_y <= y + 2; ++outline_y) {
        for (int outline_x = x; outline_x <= x + 2; ++outline_x) {
          const size_t output_index =
              (static_cast<size_t>(outline_y) *
                   static_cast<size_t>(output_width) +
               static_cast<size_t>(outline_x)) * 4U;
          if (output_index + 3U < output->size()) {
            (*output)[output_index] = 0xFFU;
            (*output)[output_index + 1U] = 0xFFU;
            (*output)[output_index + 2U] = 0xFFU;
            (*output)[output_index + 3U] = 0xFFU;
          }
        }
      }
    }
  }

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t input_index =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) * 4U;
      if (input_index + 3U >= input.size() || input[input_index + 3U] == 0U) {
        continue;
      }
      const size_t output_index =
          (static_cast<size_t>(y + 1) * static_cast<size_t>(output_width) +
           static_cast<size_t>(x + 1)) * 4U;
      if (output_index + 3U < output->size()) {
        std::memcpy(output->data() + output_index, input.data() + input_index, 4U);
      }
    }
  }
}

bool EncodeRustDeskCursorDataMessage(
    HCURSOR cursor,
    std::vector<unsigned char>* message,
    std::wstring* error_text) {
  if (cursor == nullptr || message == nullptr) {
    return false;
  }

  ICONINFO icon_info = {};
  if (!GetIconInfo(cursor, &icon_info)) {
    if (error_text != nullptr) {
      *error_text = L"GetIconInfo failed";
    }
    return false;
  }

  BITMAP mask_metadata = {};
  const bool have_mask =
      RustDeskGetBitmapMetadata(icon_info.hbmMask, &mask_metadata);
  if (!have_mask || mask_metadata.bmWidth < 1 || mask_metadata.bmHeight < 1 ||
      mask_metadata.bmWidth > 512 || mask_metadata.bmHeight > 1024 ||
      mask_metadata.bmWidthBytes < 1) {
    if (icon_info.hbmColor != nullptr) {
      DeleteObject(icon_info.hbmColor);
    }
    if (icon_info.hbmMask != nullptr) {
      DeleteObject(icon_info.hbmMask);
    }
    if (error_text != nullptr) {
      *error_text = L"invalid cursor mask bitmap";
    }
    return false;
  }

  int width = mask_metadata.bmWidth;
  int height = icon_info.hbmColor != nullptr
      ? mask_metadata.bmHeight
      : mask_metadata.bmHeight / 2;
  if (height < 1 || width > 512 || height > 512) {
    if (icon_info.hbmColor != nullptr) {
      DeleteObject(icon_info.hbmColor);
    }
    if (icon_info.hbmMask != nullptr) {
      DeleteObject(icon_info.hbmMask);
    }
    if (error_text != nullptr) {
      *error_text = L"invalid cursor dimensions";
    }
    return false;
  }

  std::vector<unsigned char> mask_bits(
      static_cast<size_t>(mask_metadata.bmWidthBytes) *
          static_cast<size_t>(mask_metadata.bmHeight),
      0U);
  const LONG mask_bytes = GetBitmapBits(
      icon_info.hbmMask,
      static_cast<LONG>(mask_bits.size()),
      mask_bits.data());
  if (mask_bytes != static_cast<LONG>(mask_bits.size())) {
    if (icon_info.hbmColor != nullptr) {
      DeleteObject(icon_info.hbmColor);
    }
    DeleteObject(icon_info.hbmMask);
    if (error_text != nullptr) {
      *error_text = L"failed to read cursor mask bitmap";
    }
    return false;
  }

  std::vector<unsigned char> colors;
  bool needs_outline = false;
  if (icon_info.hbmColor != nullptr) {
    if (!RustDeskReadColorCursorRgba(
            icon_info.hbmColor, width, height, &colors)) {
      DeleteObject(icon_info.hbmColor);
      DeleteObject(icon_info.hbmMask);
      if (error_text != nullptr) {
        *error_text = L"failed to read color cursor bitmap";
      }
      return false;
    }
    needs_outline = RustDeskFixColorCursorMask(
        &mask_bits,
        &colors,
        width,
        height,
        mask_metadata.bmWidthBytes);
  } else {
    colors.assign(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4U,
        0U);
    needs_outline = RustDeskHandleMonochromeCursorMask(
        &colors,
        mask_bits,
        width,
        height,
        mask_metadata.bmWidthBytes,
        mask_metadata.bmHeight);
  }

  if (icon_info.hbmColor != nullptr) {
    DeleteObject(icon_info.hbmColor);
  }
  if (icon_info.hbmMask != nullptr) {
    DeleteObject(icon_info.hbmMask);
  }

  int hot_x = static_cast<int>(icon_info.xHotspot);
  int hot_y = static_cast<int>(icon_info.yHotspot);
  if (needs_outline) {
    std::vector<unsigned char> outlined;
    RustDeskDrawCursorOutline(colors, width, height, &outlined);
    if (!outlined.empty()) {
      colors.swap(outlined);
      width += 2;
      height += 2;
      ++hot_x;
      ++hot_y;
    }
  }

  std::vector<unsigned char> compressed_colors;
  if (!CompressZstdBytes(colors, &compressed_colors, error_text)) {
    return false;
  }

  std::vector<unsigned char> cursor_data;
  AppendVarintField(
      1U,
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cursor)),
      &cursor_data);
  AppendSint32Field(2U, hot_x, &cursor_data);
  AppendSint32Field(3U, hot_y, &cursor_data);
  AppendVarintField(4U, static_cast<uint64_t>(width), &cursor_data);
  AppendVarintField(5U, static_cast<uint64_t>(height), &cursor_data);
  AppendBytesField(6U, compressed_colors, &cursor_data);

  message->clear();
  AppendLengthDelimitedField(12U, cursor_data, message);
  return true;
}

bool TryGetVisibleRustDeskCursor(HCURSOR* cursor) {
  if (cursor == nullptr) {
    return false;
  }
  CURSORINFO cursor_info = {};
  cursor_info.cbSize = sizeof(cursor_info);
  if (!GetCursorInfo(&cursor_info) ||
      (cursor_info.flags & CURSOR_SHOWING) == 0 ||
      cursor_info.hCursor == nullptr) {
    return false;
  }
  *cursor = cursor_info.hCursor;
  return true;
}

UINT GetHtmlClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"HTML Format");
  return format;
}

UINT GetRtfClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"text/richtext");
  return format;
}

UINT GetLegacyRtfClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"Rich Text Format");
  return format;
}

UINT GetExcelXmlSpreadsheetClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"XML Spreadsheet");
  return format;
}

void TrimTrailingZeroBytes(std::vector<unsigned char>* bytes);

bool LooksLikeCfHtmlPayload(const std::vector<unsigned char>& bytes) {
  if (bytes.size() < 32U) {
    return false;
  }
  const std::string text(bytes.begin(), bytes.end());
  return text.find("Version:") == 0 &&
         text.find("StartHTML:") != std::string::npos;
}

bool TryParseCfHtmlOffset(
    const std::string& html,
    const char* label,
    size_t* value) {
  const size_t label_pos = html.find(label);
  if (label_pos == std::string::npos) {
    return false;
  }
  size_t digits_pos = label_pos + std::strlen(label);
  while (digits_pos < html.size() &&
         (html[digits_pos] == ' ' || html[digits_pos] == '\t')) {
    ++digits_pos;
  }
  size_t digits_end = digits_pos;
  while (digits_end < html.size() &&
         html[digits_end] >= '0' && html[digits_end] <= '9') {
    ++digits_end;
  }
  if (digits_end == digits_pos) {
    return false;
  }
  *value = static_cast<size_t>(
      std::strtoull(html.substr(digits_pos, digits_end - digits_pos).c_str(),
                    nullptr,
                    10));
  return true;
}

std::string FormatCfHtmlOffset(size_t value) {
  std::string digits = std::to_string(value);
  if (digits.size() < 10U) {
    digits.insert(digits.begin(), 10U - digits.size(), '0');
  }
  return digits;
}

bool ExtractCfHtmlTransferBytes(
    const std::vector<unsigned char>& cf_html,
    std::vector<unsigned char>* html,
    std::wstring* error_text) {
  html->clear();
  if (!LooksLikeCfHtmlPayload(cf_html)) {
    *html = cf_html;
    return true;
  }

  const std::string payload(cf_html.begin(), cf_html.end());
  size_t start_fragment = 0;
  size_t end_fragment = 0;
  bool have_fragment =
      TryParseCfHtmlOffset(payload, "StartFragment:", &start_fragment) &&
      TryParseCfHtmlOffset(payload, "EndFragment:", &end_fragment);
  size_t start_html = 0;
  size_t end_html = 0;
  bool have_html =
      TryParseCfHtmlOffset(payload, "StartHTML:", &start_html) &&
      TryParseCfHtmlOffset(payload, "EndHTML:", &end_html);

  size_t start = 0;
  size_t end = 0;
  if (have_fragment) {
    start = start_fragment;
    end = end_fragment;
  } else if (have_html) {
    start = start_html;
    end = end_html;
  } else {
    if (error_text != nullptr) {
      *error_text = L"CF_HTML offsets were missing";
    }
    return false;
  }

  if (start > end || end > cf_html.size()) {
    if (error_text != nullptr) {
      *error_text = L"CF_HTML offsets were invalid";
    }
    return false;
  }
  html->assign(cf_html.begin() + static_cast<std::ptrdiff_t>(start),
               cf_html.begin() + static_cast<std::ptrdiff_t>(end));
  TrimTrailingZeroBytes(html);
  return true;
}

std::vector<unsigned char> BuildCfHtmlClipboardPayload(
    const std::vector<unsigned char>& html_bytes) {
  if (LooksLikeCfHtmlPayload(html_bytes)) {
    return html_bytes;
  }

  const std::string fragment(html_bytes.begin(), html_bytes.end());
  const std::string version = "Version:0.9";
  const std::string start_html_label = "\r\nStartHTML:";
  const std::string end_html_label = "\r\nEndHTML:";
  const std::string start_fragment_label = "\r\nStartFragment:";
  const std::string end_fragment_label = "\r\nEndFragment:";
  const std::string body_header =
      "\r\n<html>\r\n<body>\r\n<!--StartFragment-->\r\n";
  const std::string body_footer =
      "\r\n<!--EndFragment-->\r\n</body>\r\n</html>";
  const size_t header_size =
      version.size() +
      start_html_label.size() + 10U +
      end_html_label.size() + 10U +
      start_fragment_label.size() + 10U +
      end_fragment_label.size() + 10U;
  const size_t start_html = header_size + 2U;
  const size_t start_fragment = header_size + body_header.size();
  const size_t end_fragment = start_fragment + fragment.size();
  const size_t end_html = end_fragment + body_footer.size();

  const std::string cf_html =
      version +
      start_html_label + FormatCfHtmlOffset(start_html) +
      end_html_label + FormatCfHtmlOffset(end_html) +
      start_fragment_label + FormatCfHtmlOffset(start_fragment) +
      end_fragment_label + FormatCfHtmlOffset(end_fragment) +
      body_header + fragment + body_footer;
  return std::vector<unsigned char>(cf_html.begin(), cf_html.end());
}

void TrimTrailingZeroBytes(std::vector<unsigned char>* bytes) {
  while (!bytes->empty() && bytes->back() == 0) {
    bytes->pop_back();
  }
}

bool CopyClipboardHandleBytes(
    HANDLE handle,
    std::vector<unsigned char>* bytes,
    std::wstring* error_text) {
  bytes->clear();
  if (handle == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"clipboard data handle was null";
    }
    return false;
  }

  const SIZE_T size = GlobalSize(handle);
  if (size == 0) {
    if (error_text != nullptr) {
      *error_text = L"GlobalSize failed for clipboard data";
    }
    return false;
  }

  const unsigned char* locked =
      static_cast<const unsigned char*>(GlobalLock(handle));
  if (locked == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"GlobalLock failed for clipboard data";
    }
    return false;
  }

  bytes->assign(locked, locked + size);
  GlobalUnlock(handle);
  return true;
}

std::wstring BuildFormattedTextClipboardSignature(
    const FormattedTextClipboardContent& content) {
  std::vector<unsigned char> canonical;
  if (content.has_text) {
    const std::string utf8_text = WideToUtf8(content.text);
    AppendVarintField(1U, 1U, &canonical);
    AppendBytesField(
        2U,
        std::vector<unsigned char>(utf8_text.begin(), utf8_text.end()),
        &canonical);
  }
  if (content.has_html) {
    AppendVarintField(3U, 1U, &canonical);
    AppendBytesField(4U, content.html, &canonical);
  }
  if (content.has_rtf) {
    AppendVarintField(5U, 1U, &canonical);
    AppendBytesField(6U, content.rtf, &canonical);
  }
  if (content.has_excel_xml) {
    AppendVarintField(7U, 1U, &canonical);
    AppendBytesField(8U, content.excel_xml, &canonical);
  }
  if (canonical.empty()) {
    return std::wstring();
  }
  return BytesToHex(ComputeSha256(canonical));
}

ClipboardMessageData BuildClipboardMessageData(
    int format,
    const std::vector<unsigned char>& content,
    const std::wstring& special_name = std::wstring(),
    int width = 0,
    int height = 0) {
  ClipboardMessageData clipboard;
  clipboard.format = format;
  clipboard.special_name = special_name;
  clipboard.width = width;
  clipboard.height = height;

  std::vector<unsigned char> compressed;
  if (CompressZstdBytes(content, &compressed, nullptr) &&
      !compressed.empty() &&
      compressed.size() < content.size()) {
    clipboard.compress = true;
    clipboard.content = std::move(compressed);
  } else {
    clipboard.compress = false;
    clipboard.content = content;
  }
  return clipboard;
}

std::vector<unsigned char> EncodeClipboardPayloadMessage(
    const ClipboardMessageData& clipboard) {
  std::vector<unsigned char> payload;
  AppendVarintField(1U, clipboard.compress ? 1U : 0U, &payload);
  AppendBytesField(2U, clipboard.content, &payload);
  if (clipboard.width != 0) {
    AppendVarintField(3U, static_cast<uint64_t>(clipboard.width), &payload);
  }
  if (clipboard.height != 0) {
    AppendVarintField(4U, static_cast<uint64_t>(clipboard.height), &payload);
  }
  AppendVarintField(5U, static_cast<uint64_t>(clipboard.format), &payload);
  if (!clipboard.special_name.empty()) {
    AppendStringField(6U, WideToUtf8(clipboard.special_name), &payload);
  }
  return payload;
}

std::vector<unsigned char> EncodeSingleClipboardMessage(
    const ClipboardMessageData& clipboard) {
  std::vector<unsigned char> message;
  AppendLengthDelimitedField(
      16U,
      EncodeClipboardPayloadMessage(clipboard),
      &message);
  return message;
}

std::vector<unsigned char> EncodeMultiClipboardMessage(
    const std::vector<ClipboardMessageData>& clipboards) {
  std::vector<unsigned char> multi_clipboards;
  for (const ClipboardMessageData& clipboard : clipboards) {
    AppendLengthDelimitedField(
        1U,
        EncodeClipboardPayloadMessage(clipboard),
        &multi_clipboards);
  }

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(28U, multi_clipboards, &message);
  return message;
}

bool BuildFormattedTextClipboardMessages(
    const FormattedTextClipboardContent& content,
    std::vector<ClipboardMessageData>* clipboards) {
  if (clipboards == nullptr) {
    return false;
  }
  clipboards->clear();

  // Excel uses this native format ahead of HTML/plain text. Without it,
  // rich cell formatting is lost and HTML paste may drop embedded newlines.
  if (content.has_excel_xml && !content.excel_xml.empty()) {
    clipboards->push_back(BuildClipboardMessageData(
        kClipboardFormatSpecial,
        content.excel_xml,
        L"XML Spreadsheet"));
  }
  if (content.has_rtf && !content.rtf.empty()) {
    clipboards->push_back(
        BuildClipboardMessageData(kClipboardFormatRtf, content.rtf));
  }
  if (content.has_html && !content.html.empty()) {
    clipboards->push_back(
        BuildClipboardMessageData(kClipboardFormatHtml, content.html));
  }
  if (content.has_text) {
    const std::string utf8_text = WideToUtf8(content.text);
    clipboards->push_back(BuildClipboardMessageData(
        kClipboardFormatText,
        std::vector<unsigned char>(utf8_text.begin(), utf8_text.end())));
  }
  return !clipboards->empty();
}

bool CaptureFormattedTextClipboardContent(
    FormattedTextClipboardContent* content,
    std::wstring* signature,
    std::wstring* error_text) {
  if (content == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"formatted clipboard target was null";
    }
    return false;
  }

  *content = FormattedTextClipboardContent();
  if (signature != nullptr) {
    signature->clear();
  }

  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle != nullptr) {
    const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(handle));
    if (locked != nullptr) {
      content->text = locked;
      content->has_text = true;
      GlobalUnlock(handle);
    }
  } else {
    handle = GetClipboardData(CF_TEXT);
    if (handle != nullptr) {
      const char* locked = static_cast<const char*>(GlobalLock(handle));
      if (locked != nullptr) {
        content->text = AnsiToWideCompat(locked);
        content->has_text = true;
        GlobalUnlock(handle);
      }
    }
  }

  std::vector<unsigned char> html_bytes;
  const UINT html_format = GetHtmlClipboardFormat();
  if (html_format != 0) {
    handle = GetClipboardData(html_format);
    if (handle != nullptr &&
        CopyClipboardHandleBytes(handle, &html_bytes, nullptr)) {
      TrimTrailingZeroBytes(&html_bytes);
      if (!html_bytes.empty()) {
        std::vector<unsigned char> transfer_html;
        if (ExtractCfHtmlTransferBytes(html_bytes, &transfer_html, nullptr) &&
            !transfer_html.empty()) {
          content->html = std::move(transfer_html);
          content->has_html = true;
        }
      }
    }
  }

  std::vector<unsigned char> excel_xml_bytes;
  const UINT excel_xml_format = GetExcelXmlSpreadsheetClipboardFormat();
  if (excel_xml_format != 0) {
    handle = GetClipboardData(excel_xml_format);
    if (handle != nullptr &&
        CopyClipboardHandleBytes(handle, &excel_xml_bytes, nullptr) &&
        !excel_xml_bytes.empty()) {
      // Keep the payload byte-for-byte. Excel's registered format is opaque
      // clipboard data, so trimming or re-encoding can corrupt it.
      content->excel_xml = std::move(excel_xml_bytes);
      content->has_excel_xml = true;
    }
  }

  std::vector<unsigned char> rtf_bytes;
  UINT rtf_formats[2] = {GetLegacyRtfClipboardFormat(), GetRtfClipboardFormat()};
  for (size_t rtf_index = 0; rtf_index < 2U && !content->has_rtf; ++rtf_index) {
    const UINT rtf_format = rtf_formats[rtf_index];
    if (rtf_format == 0) {
      continue;
    }
    handle = GetClipboardData(rtf_format);
    if (handle == nullptr ||
        !CopyClipboardHandleBytes(handle, &rtf_bytes, nullptr)) {
      continue;
    }
    TrimTrailingZeroBytes(&rtf_bytes);
    if (!rtf_bytes.empty()) {
      content->rtf = std::move(rtf_bytes);
      content->has_rtf = true;
    }
  }

  close_clipboard();
  if (!content->has_text && !content->has_html && !content->has_rtf &&
      !content->has_excel_xml) {
    if (error_text != nullptr) {
      *error_text = L"clipboard does not contain supported formats";
    }
    return false;
  }

  if (signature != nullptr) {
    *signature = BuildFormattedTextClipboardSignature(*content);
  }
  return true;
}

bool DecodeFormattedTextClipboardContent(
    const std::vector<ClipboardMessageData>& clipboards,
    FormattedTextClipboardContent* content,
    std::wstring* signature,
    std::wstring* error_text) {
  if (content == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"formatted clipboard target was null";
    }
    return false;
  }

  *content = FormattedTextClipboardContent();
  if (signature != nullptr) {
    signature->clear();
  }

  bool found_supported_format = false;
  std::wstring last_error;
  for (const ClipboardMessageData& clipboard : clipboards) {
    if (clipboard.format == kClipboardFormatText) {
      std::wstring text;
      if (ExtractClipboardWideText(clipboard, &text, &last_error)) {
        content->text = std::move(text);
        content->has_text = true;
        found_supported_format = true;
      }
      continue;
    }

    const bool is_html =
        clipboard.format == kClipboardFormatHtml ||
        (clipboard.format == kClipboardFormatSpecial &&
         _wcsicmp(clipboard.special_name.c_str(), L"HTML Format") == 0);
    const bool is_rtf =
        clipboard.format == kClipboardFormatRtf ||
        (clipboard.format == kClipboardFormatSpecial &&
         (_wcsicmp(clipboard.special_name.c_str(), L"Rich Text Format") == 0 ||
          _wcsicmp(clipboard.special_name.c_str(), L"text/richtext") == 0));
    const bool is_excel_xml =
        clipboard.format == kClipboardFormatSpecial &&
        _wcsicmp(clipboard.special_name.c_str(), L"XML Spreadsheet") == 0;
    if (!is_html && !is_rtf && !is_excel_xml) {
      continue;
    }

    std::vector<unsigned char> bytes;
    if (!GetClipboardPayloadBytes(clipboard, &bytes, &last_error)) {
      continue;
    }
    if (is_excel_xml) {
      // Registered clipboard formats are opaque byte payloads. Preserve the
      // Excel XML Spreadsheet bytes exactly as received.
      content->excel_xml = std::move(bytes);
      content->has_excel_xml = !content->excel_xml.empty();
    } else {
      TrimTrailingZeroBytes(&bytes);
      if (is_html) {
        std::vector<unsigned char> transfer_html;
        if (ExtractCfHtmlTransferBytes(bytes, &transfer_html, nullptr)) {
          content->html = std::move(transfer_html);
          content->has_html = !content->html.empty();
        } else {
          content->html = std::move(bytes);
          content->has_html = !content->html.empty();
        }
      } else {
        content->rtf = std::move(bytes);
        content->has_rtf = true;
      }
    }
    found_supported_format = true;
  }

  if (!found_supported_format) {
    if (error_text != nullptr && !last_error.empty()) {
      *error_text = last_error;
    }
    return false;
  }

  if (signature != nullptr) {
    *signature = BuildFormattedTextClipboardSignature(*content);
  }
  return true;
}

bool SetClipboardBinaryFormatData(
    UINT format,
    const std::vector<unsigned char>& bytes,
    bool append_nul,
    std::wstring* error_text) {
  const SIZE_T extra = append_nul ? 1U : 0U;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size() + extra);
  if (memory == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"GlobalAlloc failed for clipboard binary data";
    }
    return false;
  }

  unsigned char* locked = static_cast<unsigned char*>(GlobalLock(memory));
  if (locked == nullptr) {
    GlobalFree(memory);
    if (error_text != nullptr) {
      *error_text = L"GlobalLock failed for clipboard binary data";
    }
    return false;
  }
  if (!bytes.empty()) {
    std::memcpy(locked, bytes.data(), bytes.size());
  }
  if (append_nul) {
    locked[bytes.size()] = 0;
  }
  GlobalUnlock(memory);

  if (SetClipboardData(format, memory) == nullptr) {
    GlobalFree(memory);
    if (error_text != nullptr) {
      *error_text = L"SetClipboardData failed for clipboard binary data";
    }
    return false;
  }
  return true;
}

bool SetClipboardFormattedTextContent(
    const FormattedTextClipboardContent& content,
    std::wstring* error_text) {
  if (!content.has_text && !content.has_html && !content.has_rtf &&
      !content.has_excel_xml) {
    if (error_text != nullptr) {
      *error_text = L"no supported clipboard formats were provided";
    }
    return false;
  }

  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  if (!EmptyClipboard()) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"EmptyClipboard failed";
    }
    return false;
  }

  bool set_any = false;
  std::wstring first_error;

  if (content.has_excel_xml && !content.excel_xml.empty()) {
    const UINT excel_xml_format = GetExcelXmlSpreadsheetClipboardFormat();
    if (excel_xml_format != 0) {
      std::wstring set_error;
      if (SetClipboardBinaryFormatData(
              excel_xml_format,
              content.excel_xml,
              false,
              &set_error)) {
        set_any = true;
      } else if (first_error.empty()) {
        first_error = set_error;
      }
    }
  }

  if (content.has_rtf && !content.rtf.empty()) {
    const UINT rtf_formats[2] = {GetLegacyRtfClipboardFormat(), GetRtfClipboardFormat()};
    for (size_t rtf_index = 0; rtf_index < 2U; ++rtf_index) {
      const UINT rtf_format = rtf_formats[rtf_index];
      if (rtf_format == 0) {
        continue;
      }
      std::wstring set_error;
      if (SetClipboardBinaryFormatData(
              rtf_format,
              content.rtf,
              false,
              &set_error)) {
        set_any = true;
      } else if (first_error.empty()) {
        first_error = set_error;
      }
    }
  }

  const UINT html_format = GetHtmlClipboardFormat();
  if (content.has_html && !content.html.empty() && html_format != 0) {
    std::wstring set_error;
    if (SetClipboardBinaryFormatData(
            html_format,
            BuildCfHtmlClipboardPayload(content.html),
            false,
            &set_error)) {
      set_any = true;
    } else if (first_error.empty()) {
      first_error = set_error;
    }
  }

  if (content.has_text) {
    const size_t bytes = (content.text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
      if (first_error.empty()) {
        first_error = L"GlobalAlloc failed for clipboard text";
      }
    } else {
      void* locked = GlobalLock(memory);
      if (locked == nullptr) {
        GlobalFree(memory);
        if (first_error.empty()) {
          first_error = L"GlobalLock failed for clipboard text";
        }
      } else {
        std::memcpy(locked, content.text.c_str(), bytes);
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
          GlobalFree(memory);
          if (first_error.empty()) {
            first_error = L"SetClipboardData(CF_UNICODETEXT) failed";
          }
        } else {
          set_any = true;
        }
      }
    }
  }

  close_clipboard();
  if (!set_any && error_text != nullptr) {
    *error_text =
        first_error.empty() ? L"failed to apply clipboard formats" : first_error;
  }
  return set_any;
}

bool ExtractClipboardWideText(
    const ClipboardMessageData& clipboard,
    std::wstring* text,
    std::wstring* error_text) {
  if (clipboard.format != kClipboardFormatText) {
    return false;
  }

  std::vector<unsigned char> bytes;
  if (!GetClipboardPayloadBytes(clipboard, &bytes, error_text)) {
    return false;
  }
  *text = Utf8ToWide(std::string(bytes.begin(), bytes.end()));
  return !text->empty() || bytes.empty();
}

bool SetClipboardUnicodeText(const std::wstring& text, std::wstring* error_text) {
  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  if (!EmptyClipboard()) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"EmptyClipboard failed";
    }
    return false;
  }

  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"GlobalAlloc failed for clipboard text";
    }
    return false;
  }

  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"GlobalLock failed for clipboard text";
    }
    return false;
  }
  std::memcpy(locked, text.c_str(), bytes);
  GlobalUnlock(memory);

  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"SetClipboardData(CF_UNICODETEXT) failed";
    }
    return false;
  }

  close_clipboard();
  return true;
}

bool GetClipboardUnicodeText(std::wstring* text, std::wstring* error_text) {
  if (text == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"clipboard text target was null";
    }
    return false;
  }
  text->clear();

  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle != nullptr) {
    const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(handle));
    if (locked == nullptr) {
      close_clipboard();
      if (error_text != nullptr) {
        *error_text = L"GlobalLock failed for clipboard text";
      }
      return false;
    }
    *text = locked;
    GlobalUnlock(handle);
    close_clipboard();
    return true;
  }

  handle = GetClipboardData(CF_TEXT);
  if (handle != nullptr) {
    const char* locked = static_cast<const char*>(GlobalLock(handle));
    if (locked == nullptr) {
      close_clipboard();
      if (error_text != nullptr) {
        *error_text = L"GlobalLock failed for ANSI clipboard text";
      }
      return false;
    }
    *text = AnsiToWideCompat(locked);
    GlobalUnlock(handle);
    close_clipboard();
    return true;
  }

  close_clipboard();
  if (error_text != nullptr) {
    *error_text = L"clipboard does not contain text";
  }
  return false;
}

std::vector<unsigned char> EncodeClipboardTextMessage(const std::wstring& text) {
  const std::string utf8_text = WideToUtf8(text);
  return EncodeSingleClipboardMessage(BuildClipboardMessageData(
      kClipboardFormatText,
      std::vector<unsigned char>(utf8_text.begin(), utf8_text.end())));
}

bool GetClipboardFileDropList(
    std::vector<std::wstring>* file_paths,
    std::wstring* error_text) {
  if (file_paths == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"clipboard file path target was null";
    }
    return false;
  }
  file_paths->clear();

  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  HDROP drop_handle = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (drop_handle == nullptr) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"clipboard does not contain file drop list";
    }
    return false;
  }

  const UINT item_count = DragQueryFileW(drop_handle, 0xFFFFFFFFu, nullptr, 0);
  file_paths->reserve(item_count);
  for (UINT index = 0; index < item_count; ++index) {
    const UINT path_length = DragQueryFileW(drop_handle, index, nullptr, 0);
    if (path_length == 0) {
      continue;
    }
    std::wstring file_path(path_length + 1, L'\0');
    const UINT copied = DragQueryFileW(
        drop_handle,
        index,
        &file_path[0],
        static_cast<UINT>(file_path.size()));
    file_path.resize(copied);
    if (!file_path.empty()) {
      file_paths->push_back(std::move(file_path));
    }
  }

  close_clipboard();
  if (file_paths->empty()) {
    if (error_text != nullptr) {
      *error_text = L"clipboard file drop list was empty";
    }
    return false;
  }
  return true;
}

std::wstring GetClipboardPathLeafName(const std::wstring& path) {
  const size_t end = path.find_last_not_of(L"\\/");
  if (end == std::wstring::npos) {
    return L"clipboard_item";
  }
  const size_t separator = path.find_last_of(L"\\/", end);
  const size_t start = separator == std::wstring::npos ? 0 : separator + 1;
  const std::wstring leaf = path.substr(start, end - start + 1);
  return leaf.empty() ? L"clipboard_item" : leaf;
}

std::wstring JoinClipboardRelativePath(
    const std::wstring& base,
    const std::wstring& component) {
  if (base.empty()) {
    return component;
  }
  if (component.empty()) {
    return base;
  }
  return base + L"\\" + component;
}

void AppendLocalClipboardFileDescriptor(
    const WIN32_FILE_ATTRIBUTE_DATA& metadata,
    const std::wstring& absolute_path,
    const std::wstring& relative_path,
    std::vector<LocalClipboardFileDescriptor>* entries) {
  if (entries == nullptr) {
    return;
  }
  LocalClipboardFileDescriptor entry;
  entry.absolute_path = absolute_path;
  entry.is_directory = (metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  entry.size = (static_cast<uint64_t>(metadata.nFileSizeHigh) << 32ULL) |
               static_cast<uint64_t>(metadata.nFileSizeLow);
  entry.descriptor.dwFlags = FD_ATTRIBUTES | FD_WRITESTIME;
  entry.descriptor.dwFileAttributes = metadata.dwFileAttributes;
  entry.descriptor.ftLastWriteTime = metadata.ftLastWriteTime;
  if (!entry.is_directory) {
    entry.descriptor.dwFlags |= FD_FILESIZE;
    entry.descriptor.nFileSizeHigh = metadata.nFileSizeHigh;
    entry.descriptor.nFileSizeLow = metadata.nFileSizeLow;
  }
  wcsncpy_s(
      entry.descriptor.cFileName,
      _countof(entry.descriptor.cFileName),
      relative_path.c_str(),
      _TRUNCATE);
  entries->push_back(std::move(entry));
}

bool CollectLocalClipboardFileDescriptorsRecursive(
    const std::wstring& absolute_path,
    const std::wstring& relative_path,
    std::vector<LocalClipboardFileDescriptor>* entries,
    std::wstring* error_text) {
  WIN32_FILE_ATTRIBUTE_DATA metadata = {};
  if (!GetFileAttributesExW(
          absolute_path.c_str(),
          GetFileExInfoStandard,
          &metadata)) {
    if (error_text != nullptr) {
      *error_text = L"GetFileAttributesExW failed while reading local clipboard file";
    }
    return false;
  }

  if ((metadata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return true;
  }

  AppendLocalClipboardFileDescriptor(metadata, absolute_path, relative_path, entries);

  if ((metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return true;
  }

  std::wstring pattern = absolute_path;
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern += L"\\";
  }
  pattern += L"*";

  WIN32_FIND_DATAW find_data = {};
  HANDLE find_handle = FindFirstFileW(pattern.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    if (error_text != nullptr) {
      *error_text =
          L"FindFirstFileW failed while enumerating local clipboard directory";
    }
    return false;
  }

  do {
    const std::wstring child_name(find_data.cFileName);
    if (child_name == L"." || child_name == L"..") {
      continue;
    }
    std::wstring child_absolute_path = absolute_path;
    if (!child_absolute_path.empty() && child_absolute_path.back() != L'\\') {
      child_absolute_path += L"\\";
    }
    child_absolute_path += child_name;
    const std::wstring child_relative_path =
        JoinClipboardRelativePath(relative_path, child_name);
    if (!CollectLocalClipboardFileDescriptorsRecursive(
            child_absolute_path,
            child_relative_path,
            entries,
            error_text)) {
      FindClose(find_handle);
      return false;
    }
  } while (FindNextFileW(find_handle, &find_data) != FALSE);

  FindClose(find_handle);
  return true;
}

bool BuildLocalClipboardFileDescriptorPayload(
    const std::vector<LocalClipboardFileDescriptor>& entries,
    std::vector<unsigned char>* payload,
    std::wstring* error_text) {
  if (payload == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"clipboard file descriptor payload target was null";
    }
    return false;
  }
  payload->clear();
  if (entries.empty()) {
    if (error_text != nullptr) {
      *error_text = L"local clipboard file descriptor list was empty";
    }
    return false;
  }

  const size_t header_size = offsetof(FILEGROUPDESCRIPTORW, fgd);
  const size_t total_bytes =
      header_size + entries.size() * sizeof(FILEDESCRIPTORW);
  payload->assign(total_bytes, 0);

  const UINT count = static_cast<UINT>(entries.size());
  std::memcpy(payload->data(), &count, sizeof(count));
  for (size_t index = 0; index < entries.size(); ++index) {
    std::memcpy(
        payload->data() + header_size + index * sizeof(FILEDESCRIPTORW),
        &entries[index].descriptor,
        sizeof(FILEDESCRIPTORW));
  }
  return true;
}

bool CaptureLocalClipboardFileDescriptors(
    std::vector<LocalClipboardFileDescriptor>* entries,
    std::vector<unsigned char>* descriptor_payload,
    std::wstring* signature,
    std::wstring* error_text) {
  if (entries == nullptr || descriptor_payload == nullptr || signature == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"local clipboard capture target was null";
    }
    return false;
  }
  entries->clear();
  descriptor_payload->clear();
  signature->clear();

  std::vector<std::wstring> file_paths;
  if (!GetClipboardFileDropList(&file_paths, error_text)) {
    return false;
  }

  for (const std::wstring& file_path : file_paths) {
    if (!signature->empty()) {
      *signature += L"\n";
    }
    *signature += file_path;

    const std::wstring leaf_name = GetClipboardPathLeafName(file_path);
    if (!CollectLocalClipboardFileDescriptorsRecursive(
            file_path,
            leaf_name,
            entries,
            error_text)) {
      entries->clear();
      descriptor_payload->clear();
      signature->clear();
      return false;
    }
  }

  if (!BuildLocalClipboardFileDescriptorPayload(*entries, descriptor_payload, error_text)) {
    entries->clear();
    descriptor_payload->clear();
    signature->clear();
    return false;
  }
  return true;
}

std::vector<unsigned char> EncodeCliprdrFormatListMessage(
    const std::vector<CliprdrFormatData>& formats) {
  std::vector<unsigned char> format_list;
  for (const CliprdrFormatData& format : formats) {
    std::vector<unsigned char> format_payload;
    AppendVarintField(2U, static_cast<uint64_t>(format.id), &format_payload);
    AppendStringField(3U, WideToUtf8(format.format_name), &format_payload);
    AppendLengthDelimitedField(2U, format_payload, &format_list);
  }

  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(2U, format_list, &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

std::vector<unsigned char> EncodeCliprdrFormatDataResponseMessage(
    int msg_flags,
    const std::vector<unsigned char>& payload) {
  std::vector<unsigned char> response;
  AppendVarintField(2U, static_cast<uint64_t>(msg_flags), &response);
  AppendBytesField(3U, payload, &response);

  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(5U, response, &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

std::vector<unsigned char> EncodeCliprdrFileContentsResponseMessage(
    int msg_flags,
    int stream_id,
    const std::vector<unsigned char>& payload) {
  std::vector<unsigned char> response;
  AppendVarintField(3U, static_cast<uint64_t>(msg_flags), &response);
  AppendVarintField(4U, static_cast<uint64_t>(stream_id), &response);
  AppendBytesField(5U, payload, &response);

  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(7U, response, &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

bool BuildLocalClipboardFileContentsPayload(
    const std::vector<LocalClipboardFileDescriptor>& entries,
    const CliprdrMessageData& request,
    std::vector<unsigned char>* payload,
    std::wstring* error_text) {
  if (payload == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"local clipboard file payload target was null";
    }
    return false;
  }
  payload->clear();

  if (request.list_index < 0 ||
      static_cast<size_t>(request.list_index) >= entries.size()) {
    if (error_text != nullptr) {
      *error_text = L"local clipboard file index was out of range";
    }
    return false;
  }

  const LocalClipboardFileDescriptor& entry =
      entries[static_cast<size_t>(request.list_index)];
  if ((request.dw_flags & kCliprdrFileContentsSizeFlag) != 0) {
    payload->resize(sizeof(uint64_t), 0);
    std::memcpy(payload->data(), &entry.size, sizeof(entry.size));
    return true;
  }

  if ((request.dw_flags & kCliprdrFileContentsRangeFlag) == 0) {
    if (error_text != nullptr) {
      *error_text = L"local clipboard file request did not specify size or range";
    }
    return false;
  }

  if (entry.is_directory) {
    return true;
  }

  const uint64_t offset =
      (static_cast<uint64_t>(static_cast<unsigned long>(request.n_position_high)) << 32ULL) |
      static_cast<unsigned long>(request.n_position_low);
  if (offset >= entry.size || request.cb_requested <= 0) {
    return true;
  }

  const uint64_t remaining = entry.size - offset;
  const DWORD bytes_to_read = static_cast<DWORD>(std::min<uint64_t>(
      remaining,
      static_cast<uint64_t>(request.cb_requested)));
  if (bytes_to_read == 0) {
    return true;
  }

  HANDLE file = CreateFileW(
      entry.absolute_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error_text != nullptr) {
      *error_text = L"CreateFileW failed for local clipboard source";
    }
    return false;
  }

  LARGE_INTEGER file_offset = {};
  file_offset.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, file_offset, nullptr, FILE_BEGIN)) {
    CloseHandle(file);
    if (error_text != nullptr) {
      *error_text = L"SetFilePointerEx failed for local clipboard source";
    }
    return false;
  }

  payload->resize(bytes_to_read);
  DWORD bytes_read = 0;
  const BOOL read_ok =
      ReadFile(file, payload->data(), bytes_to_read, &bytes_read, nullptr);
  CloseHandle(file);
  if (!read_ok) {
    payload->clear();
    if (error_text != nullptr) {
      *error_text = L"ReadFile failed for local clipboard source";
    }
    return false;
  }
  payload->resize(bytes_read);
  return true;
}

std::wstring BuildClipboardStagingRoot(const HostConfig& config) {
  return config.exe_dir + L"\\clipboard_staging";
}

bool IsForbiddenClipboardPathCharacter(wchar_t value) {
  switch (value) {
    case L'<':
    case L'>':
    case L':':
    case L'"':
    case L'/':
    case L'\\':
    case L'|':
    case L'?':
    case L'*':
      return true;
    default:
      return false;
  }
}

std::wstring SanitizeClipboardPathComponent(const std::wstring& component) {
  std::wstring sanitized;
  sanitized.reserve(component.size());
  for (wchar_t value : component) {
    sanitized.push_back(IsForbiddenClipboardPathCharacter(value) ? L'_' : value);
  }
  while (!sanitized.empty() &&
         (sanitized.back() == L'.' || sanitized.back() == L' ')) {
    sanitized.pop_back();
  }
  if (sanitized.empty()) {
    return L"_";
  }
  return sanitized;
}

std::wstring SanitizeRelativeClipboardPath(const std::wstring& value) {
  std::wstring relative = value;
  std::replace(relative.begin(), relative.end(), L'/', L'\\');
  std::wstringstream stream(relative);
  std::wstring component;
  std::wstring sanitized;
  while (std::getline(stream, component, L'\\')) {
    if (component.empty() || component == L"." || component == L"..") {
      continue;
    }
    if (!sanitized.empty()) {
      sanitized += L"\\";
    }
    sanitized += SanitizeClipboardPathComponent(component);
  }
  return sanitized.empty() ? L"clipboard_item" : sanitized;
}

bool EnsureDirectoryForFilePath(const std::wstring& file_path, std::wstring* error_text) {
  const size_t separator = file_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return true;
  }
  const std::wstring parent = file_path.substr(0, separator);
  return EnsureDirectoryExistsRecursive(parent, error_text);
}

bool WriteBytesAtOffset(
    const std::wstring& file_path,
    uint64_t offset,
    const std::vector<unsigned char>& data,
    std::wstring* error_text) {
  if (!EnsureDirectoryForFilePath(file_path, error_text)) {
    return false;
  }

  const HANDLE file = CreateFileW(
      file_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error_text != nullptr) {
      *error_text = L"CreateFileW failed for clipboard staging";
    }
    return false;
  }

  LARGE_INTEGER position = {};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    CloseHandle(file);
    if (error_text != nullptr) {
      *error_text = L"SetFilePointerEx failed for clipboard staging";
    }
    return false;
  }

  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(data.size());
  const BOOL write_ok =
      size == 0 || WriteFile(file, data.data(), size, &written, nullptr);
  CloseHandle(file);
  if (!write_ok || written != size) {
    if (error_text != nullptr) {
      *error_text = L"WriteFile failed for clipboard staging";
    }
    return false;
  }
  return true;
}

bool SetClipboardFileDropList(
    const std::vector<std::wstring>& file_paths,
    std::wstring* error_text) {
  if (file_paths.empty()) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    if (error_text != nullptr) {
      *error_text = L"OpenClipboard failed";
    }
    return false;
  }

  const auto close_clipboard = []() {
    CloseClipboard();
  };

  if (!EmptyClipboard()) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"EmptyClipboard failed";
    }
    return false;
  }

  size_t character_count = 0;
  for (const std::wstring& file_path : file_paths) {
    character_count += file_path.size() + 1;
  }
  character_count += 1;

  const size_t total_bytes =
      sizeof(DROPFILES) + character_count * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_bytes);
  if (memory == nullptr) {
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"GlobalAlloc failed for CF_HDROP";
    }
    return false;
  }

  auto* drop_files = static_cast<DROPFILES*>(GlobalLock(memory));
  if (drop_files == nullptr) {
    GlobalFree(memory);
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"GlobalLock failed for CF_HDROP";
    }
    return false;
  }

  drop_files->pFiles = sizeof(DROPFILES);
  drop_files->fWide = TRUE;

  auto* cursor = reinterpret_cast<wchar_t*>(
      reinterpret_cast<unsigned char*>(drop_files) + sizeof(DROPFILES));
  for (const std::wstring& file_path : file_paths) {
    std::memcpy(cursor, file_path.c_str(), file_path.size() * sizeof(wchar_t));
    cursor += file_path.size();
    *cursor++ = L'\0';
  }
  *cursor = L'\0';
  GlobalUnlock(memory);

  if (SetClipboardData(CF_HDROP, memory) == nullptr) {
    GlobalFree(memory);
    close_clipboard();
    if (error_text != nullptr) {
      *error_text = L"SetClipboardData(CF_HDROP) failed";
    }
    return false;
  }

  close_clipboard();
  return true;
}

uint64_t FileDescriptorSize(const FILEDESCRIPTORW& descriptor) {
  return (static_cast<uint64_t>(descriptor.nFileSizeHigh) << 32ULL) |
         descriptor.nFileSizeLow;
}

bool ParseFileGroupDescriptorData(
    const std::vector<unsigned char>& payload,
    const std::wstring& staging_root,
    std::vector<ClipboardStagedFileDescriptor>* entries,
    std::vector<std::wstring>* top_level_paths,
    std::wstring* error_text) {
  entries->clear();
  top_level_paths->clear();
  if (payload.size() < sizeof(UINT)) {
    if (error_text != nullptr) {
      *error_text = L"clipboard file descriptor payload too small";
    }
    return false;
  }

  UINT count = 0;
  std::memcpy(&count, payload.data(), sizeof(count));
  const size_t header_size = offsetof(FILEGROUPDESCRIPTORW, fgd);
  if (payload.size() < header_size + static_cast<size_t>(count) * sizeof(FILEDESCRIPTORW)) {
    if (error_text != nullptr) {
      *error_text = L"clipboard file descriptor payload truncated";
    }
    return false;
  }

  for (UINT index = 0; index < count; ++index) {
    FILEDESCRIPTORW descriptor = {};
    std::memcpy(
        &descriptor,
        payload.data() + header_size + static_cast<size_t>(index) * sizeof(FILEDESCRIPTORW),
        sizeof(descriptor));

    const std::wstring relative_path = SanitizeRelativeClipboardPath(descriptor.cFileName);
    ClipboardStagedFileDescriptor entry;
    entry.relative_path = relative_path;
    entry.staged_path = staging_root + L"\\" + relative_path;
    entry.size = FileDescriptorSize(descriptor);
    entry.is_directory = (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.is_top_level =
        relative_path.find(L'\\') == std::wstring::npos &&
        relative_path.find(L'/') == std::wstring::npos;

    if (entry.is_directory) {
      if (!EnsureDirectoryExistsRecursive(entry.staged_path, error_text)) {
        if (error_text != nullptr && error_text->empty()) {
          *error_text = L"failed to create clipboard staging directory";
        }
        return false;
      }
    }

    if (entry.is_top_level) {
      top_level_paths->push_back(entry.staged_path);
    }
    entries->push_back(entry);
  }
  return true;
}

std::vector<unsigned char> EncodeCliprdrMonitorReadyMessage() {
  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(1U, std::vector<unsigned char>(), &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

std::vector<unsigned char> EncodeCliprdrFormatDataRequestMessage(int requested_format_id) {
  std::vector<unsigned char> request;
  AppendVarintField(2U, static_cast<uint64_t>(requested_format_id), &request);

  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(4U, request, &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

std::vector<unsigned char> EncodeCliprdrFileContentsRequestMessage(
    int stream_id,
    int list_index,
    int dw_flags,
    int n_position_low,
    int n_position_high,
    int cb_requested) {
  std::vector<unsigned char> request;
  AppendVarintField(2U, static_cast<uint64_t>(stream_id), &request);
  AppendVarintField(3U, static_cast<uint64_t>(list_index), &request);
  AppendVarintField(4U, static_cast<uint64_t>(dw_flags), &request);
  AppendVarintField(5U, static_cast<uint64_t>(n_position_low), &request);
  AppendVarintField(6U, static_cast<uint64_t>(n_position_high), &request);
  AppendVarintField(7U, static_cast<uint64_t>(cb_requested), &request);
  AppendVarintField(8U, 0U, &request);
  AppendVarintField(9U, 0U, &request);

  std::vector<unsigned char> cliprdr;
  AppendLengthDelimitedField(6U, request, &cliprdr);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(20U, cliprdr, &message);
  return message;
}

std::vector<unsigned char> EncodeH264VideoFrameMessage(
    const std::vector<std::vector<unsigned char>>& access_units,
    const std::vector<bool>& key_flags,
    int64_t pts_ms,
    int display_index) {
  std::vector<unsigned char> encoded_frames;
  for (size_t index = 0; index < access_units.size(); ++index) {
    std::vector<unsigned char> encoded_frame;
    AppendBytesField(1U, access_units[index], &encoded_frame);
    AppendVarintField(2U, key_flags[index] ? 1U : 0U, &encoded_frame);
    AppendVarintField(3U, static_cast<uint64_t>(pts_ms), &encoded_frame);
    AppendLengthDelimitedField(1U, encoded_frame, &encoded_frames);
  }

  std::vector<unsigned char> video_frame;
  AppendLengthDelimitedField(10U, encoded_frames, &video_frame);
  AppendVarintField(14U, static_cast<uint64_t>(display_index), &video_frame);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(6U, video_frame, &message);
  return message;
}

std::vector<unsigned char> EncodeVp8VideoFrameMessage(
    const std::vector<std::vector<unsigned char>>& frames,
    const std::vector<bool>& key_flags,
    int64_t pts_ms,
    int display_index) {
  std::vector<unsigned char> encoded_frames;
  for (size_t index = 0; index < frames.size(); ++index) {
    std::vector<unsigned char> encoded_frame;
    AppendBytesField(1U, frames[index], &encoded_frame);
    AppendVarintField(2U, key_flags[index] ? 1U : 0U, &encoded_frame);
    AppendVarintField(3U, static_cast<uint64_t>(pts_ms), &encoded_frame);
    AppendLengthDelimitedField(1U, encoded_frame, &encoded_frames);
  }

  std::vector<unsigned char> video_frame;
  AppendLengthDelimitedField(12U, encoded_frames, &video_frame);
  AppendVarintField(14U, static_cast<uint64_t>(display_index), &video_frame);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(6U, video_frame, &message);
  return message;
}

int DecodeZigZag32(uint64_t value) {
  return static_cast<int>((value >> 1U) ^ (~(value & 1U) + 1U));
}

bool ParseMiscMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 19U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }
    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 9U && subwire_type == 2U) {
        std::vector<unsigned char> close_reason_bytes;
        if (!ReadLengthDelimited(payload, &payload_offset, &close_reason_bytes)) {
          return false;
        }
        session_message->has_close_reason = true;
        session_message->close_reason =
            Utf8ToWide(std::string(close_reason_bytes.begin(), close_reason_bytes.end()));
      } else if (subfield_number == 10U && subwire_type == 0U) {
        uint64_t refresh = 0;
        if (!ReadVarint(payload, &payload_offset, &refresh)) {
          return false;
        }
        session_message->wants_refresh_video = refresh != 0;
      } else {
        if (!SkipField(subwire_type, payload, &payload_offset)) {
          return false;
        }
      }
    }
    return true;
  }
  return false;
}

bool TryParseTestDelayMessage(
    const std::vector<unsigned char>& frame,
    bool* found,
    bool* from_client) {
  if (found != nullptr) {
    *found = false;
  }
  if (from_client != nullptr) {
    *from_client = false;
  }

  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 5U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }
    if (found != nullptr) {
      *found = true;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 2U && subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        if (from_client != nullptr) {
          *from_client = value != 0;
        }
      } else {
        if (!SkipField(subwire_type, payload, &payload_offset)) {
          return false;
        }
      }
    }
    return true;
  }
  return true;
}

bool ParseMouseEventMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 10U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }
    MouseEventData parsed;
    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 1U && subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        parsed.mask = static_cast<int>(value);
      } else if ((subfield_number == 2U || subfield_number == 3U) && subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        if (subfield_number == 2U) {
          parsed.x = DecodeZigZag32(value);
        } else {
          parsed.y = DecodeZigZag32(value);
        }
      } else if (subfield_number == 4U && subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        parsed.modifiers.push_back(static_cast<int>(value));
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    session_message->has_mouse = true;
    session_message->mouse = parsed;
    return true;
  }
  return false;
}

bool ParseKeyEventMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 15U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }
    KeyEventData parsed;
    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        switch (subfield_number) {
          case 1U:
            parsed.down = value != 0;
            break;
          case 2U:
            parsed.press = value != 0;
            break;
          case 3U:
            parsed.has_control_key = true;
            parsed.control_key = static_cast<int>(value);
            break;
          case 4U:
            parsed.has_chr = true;
            parsed.chr = static_cast<unsigned int>(value);
            break;
          case 5U:
            parsed.has_unicode = true;
            parsed.unicode = static_cast<unsigned int>(value);
            break;
          case 7U:
            parsed.has_win2win_hotkey = true;
            parsed.win2win_hotkey = static_cast<unsigned int>(value);
            break;
          case 8U:
            parsed.modifiers.push_back(static_cast<int>(value));
            break;
          case 9U:
            parsed.mode = static_cast<int>(value);
            break;
          default:
            break;
        }
      } else if (subfield_number == 6U && subwire_type == 2U) {
        std::vector<unsigned char> value;
        if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
          return false;
        }
        parsed.seq = Utf8ToWide(std::string(value.begin(), value.end()));
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    session_message->has_key = true;
    session_message->key = parsed;
    return true;
  }
  return false;
}

bool ParseClipboardPayload(
    const std::vector<unsigned char>& payload,
    ClipboardMessageData* clipboard) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      clipboard->compress = value != 0;
    } else if (field_number == 2U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, &clipboard->content)) {
        return false;
      }
    } else if ((field_number == 3U || field_number == 4U || field_number == 5U) &&
               wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 3U) {
        clipboard->width = static_cast<int>(value);
      } else if (field_number == 4U) {
        clipboard->height = static_cast<int>(value);
      } else {
        clipboard->format = static_cast<int>(value);
      }
    } else if (field_number == 6U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      clipboard->special_name = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseClipboardMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 16U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    ClipboardMessageData clipboard;
    if (!ParseClipboardPayload(payload, &clipboard)) {
      return false;
    }
    session_message->clipboards.push_back(std::move(clipboard));
    return true;
  }
  return false;
}

bool ParseMultiClipboardsMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 28U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }
    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 1U && subwire_type == 2U) {
        std::vector<unsigned char> clip_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &clip_payload)) {
          return false;
        }
        ClipboardMessageData clipboard;
        if (!ParseClipboardPayload(clip_payload, &clipboard)) {
          return false;
        }
        session_message->clipboards.push_back(std::move(clipboard));
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool ParseCliprdrFormatListPayload(
    const std::vector<unsigned char>& payload,
    std::vector<CliprdrFormatData>* formats) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> format_payload;
      if (!ReadLengthDelimited(payload, &offset, &format_payload)) {
        return false;
      }
      CliprdrFormatData format;
      size_t format_offset = 0;
      while (format_offset < format_payload.size()) {
        uint64_t format_tag = 0;
        if (!ReadVarint(format_payload, &format_offset, &format_tag)) {
          return false;
        }
        const unsigned int format_field = static_cast<unsigned int>(format_tag >> 3U);
        const unsigned int format_wire_type = static_cast<unsigned int>(format_tag & 0x07U);
        if (format_field == 2U && format_wire_type == 0U) {
          uint64_t value = 0;
          if (!ReadVarint(format_payload, &format_offset, &value)) {
            return false;
          }
          format.id = static_cast<int>(value);
        } else if (format_field == 3U && format_wire_type == 2U) {
          std::vector<unsigned char> value;
          if (!ReadLengthDelimited(format_payload, &format_offset, &value)) {
            return false;
          }
          format.format_name = Utf8ToWide(std::string(value.begin(), value.end()));
        } else if (!SkipField(format_wire_type, format_payload, &format_offset)) {
          return false;
        }
      }
      formats->push_back(std::move(format));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseCliprdrFilesPayload(
    const std::vector<unsigned char>& payload,
    std::vector<CliprdrFileAuditEntry>* files) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      std::vector<unsigned char> file_payload;
      if (!ReadLengthDelimited(payload, &offset, &file_payload)) {
        return false;
      }
      CliprdrFileAuditEntry file;
      size_t file_offset = 0;
      while (file_offset < file_payload.size()) {
        uint64_t file_tag = 0;
        if (!ReadVarint(file_payload, &file_offset, &file_tag)) {
          return false;
        }
        const unsigned int file_field = static_cast<unsigned int>(file_tag >> 3U);
        const unsigned int file_wire_type = static_cast<unsigned int>(file_tag & 0x07U);
        if (file_field == 1U && file_wire_type == 2U) {
          std::vector<unsigned char> value;
          if (!ReadLengthDelimited(file_payload, &file_offset, &value)) {
            return false;
          }
          file.name = Utf8ToWide(std::string(value.begin(), value.end()));
        } else if (file_field == 2U && file_wire_type == 0U) {
          uint64_t value = 0;
          if (!ReadVarint(file_payload, &file_offset, &value)) {
            return false;
          }
          file.size = value;
        } else if (!SkipField(file_wire_type, file_payload, &file_offset)) {
          return false;
        }
      }
      files->push_back(std::move(file));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseCliprdrMessage(
    const std::vector<unsigned char>& frame,
    SessionMessageType* session_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 20U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    CliprdrMessageData cliprdr;
    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);

      if (subfield_number == 1U && subwire_type == 2U) {
        std::vector<unsigned char> ignored;
        if (!ReadLengthDelimited(payload, &payload_offset, &ignored)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kReady;
      } else if (subfield_number == 2U && subwire_type == 2U) {
        std::vector<unsigned char> format_list_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &format_list_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFormatList;
        if (!ParseCliprdrFormatListPayload(format_list_payload, &cliprdr.formats)) {
          return false;
        }
      } else if (subfield_number == 3U && subwire_type == 2U) {
        std::vector<unsigned char> response_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &response_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFormatListResponse;
        size_t response_offset = 0;
        while (response_offset < response_payload.size()) {
          uint64_t response_tag = 0;
          if (!ReadVarint(response_payload, &response_offset, &response_tag)) {
            return false;
          }
          const unsigned int response_field = static_cast<unsigned int>(response_tag >> 3U);
          const unsigned int response_wire = static_cast<unsigned int>(response_tag & 0x07U);
          if (response_field == 2U && response_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(response_payload, &response_offset, &value)) {
              return false;
            }
            cliprdr.msg_flags = static_cast<int>(value);
          } else if (!SkipField(response_wire, response_payload, &response_offset)) {
            return false;
          }
        }
      } else if (subfield_number == 4U && subwire_type == 2U) {
        std::vector<unsigned char> request_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &request_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFormatDataRequest;
        size_t request_offset = 0;
        while (request_offset < request_payload.size()) {
          uint64_t request_tag = 0;
          if (!ReadVarint(request_payload, &request_offset, &request_tag)) {
            return false;
          }
          const unsigned int request_field = static_cast<unsigned int>(request_tag >> 3U);
          const unsigned int request_wire = static_cast<unsigned int>(request_tag & 0x07U);
          if (request_field == 2U && request_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(request_payload, &request_offset, &value)) {
              return false;
            }
            cliprdr.requested_format_id = static_cast<int>(value);
          } else if (!SkipField(request_wire, request_payload, &request_offset)) {
            return false;
          }
        }
      } else if (subfield_number == 5U && subwire_type == 2U) {
        std::vector<unsigned char> response_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &response_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFormatDataResponse;
        size_t response_offset = 0;
        while (response_offset < response_payload.size()) {
          uint64_t response_tag = 0;
          if (!ReadVarint(response_payload, &response_offset, &response_tag)) {
            return false;
          }
          const unsigned int response_field = static_cast<unsigned int>(response_tag >> 3U);
          const unsigned int response_wire = static_cast<unsigned int>(response_tag & 0x07U);
          if (response_field == 2U && response_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(response_payload, &response_offset, &value)) {
              return false;
            }
            cliprdr.msg_flags = static_cast<int>(value);
          } else if (response_field == 3U && response_wire == 2U) {
            if (!ReadLengthDelimited(response_payload, &response_offset, &cliprdr.payload)) {
              return false;
            }
          } else if (!SkipField(response_wire, response_payload, &response_offset)) {
            return false;
          }
        }
      } else if (subfield_number == 6U && subwire_type == 2U) {
        std::vector<unsigned char> request_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &request_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFileContentsRequest;
        size_t request_offset = 0;
        while (request_offset < request_payload.size()) {
          uint64_t request_tag = 0;
          if (!ReadVarint(request_payload, &request_offset, &request_tag)) {
            return false;
          }
          const unsigned int request_field = static_cast<unsigned int>(request_tag >> 3U);
          const unsigned int request_wire = static_cast<unsigned int>(request_tag & 0x07U);
          if (request_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(request_payload, &request_offset, &value)) {
              return false;
            }
            switch (request_field) {
              case 2U: cliprdr.stream_id = static_cast<int>(value); break;
              case 3U: cliprdr.list_index = static_cast<int>(value); break;
              case 4U: cliprdr.dw_flags = static_cast<int>(value); break;
              case 5U: cliprdr.n_position_low = static_cast<int>(value); break;
              case 6U: cliprdr.n_position_high = static_cast<int>(value); break;
              case 7U: cliprdr.cb_requested = static_cast<int>(value); break;
              case 8U: cliprdr.have_clip_data_id = value != 0; break;
              case 9U: cliprdr.clip_data_id = static_cast<int>(value); break;
              default: break;
            }
          } else if (!SkipField(request_wire, request_payload, &request_offset)) {
            return false;
          }
        }
      } else if (subfield_number == 7U && subwire_type == 2U) {
        std::vector<unsigned char> response_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &response_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFileContentsResponse;
        size_t response_offset = 0;
        while (response_offset < response_payload.size()) {
          uint64_t response_tag = 0;
          if (!ReadVarint(response_payload, &response_offset, &response_tag)) {
            return false;
          }
          const unsigned int response_field = static_cast<unsigned int>(response_tag >> 3U);
          const unsigned int response_wire = static_cast<unsigned int>(response_tag & 0x07U);
          if (response_field == 3U && response_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(response_payload, &response_offset, &value)) {
              return false;
            }
            cliprdr.msg_flags = static_cast<int>(value);
          } else if (response_field == 4U && response_wire == 0U) {
            uint64_t value = 0;
            if (!ReadVarint(response_payload, &response_offset, &value)) {
              return false;
            }
            cliprdr.stream_id = static_cast<int>(value);
          } else if (response_field == 5U && response_wire == 2U) {
            if (!ReadLengthDelimited(response_payload, &response_offset, &cliprdr.payload)) {
              return false;
            }
          } else if (!SkipField(response_wire, response_payload, &response_offset)) {
            return false;
          }
        }
      } else if (subfield_number == 8U && subwire_type == 2U) {
        std::vector<unsigned char> ignored;
        if (!ReadLengthDelimited(payload, &payload_offset, &ignored)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kTryEmpty;
      } else if (subfield_number == 9U && subwire_type == 2U) {
        std::vector<unsigned char> files_payload;
        if (!ReadLengthDelimited(payload, &payload_offset, &files_payload)) {
          return false;
        }
        cliprdr.kind = CliprdrMessageKind::kFiles;
        if (!ParseCliprdrFilesPayload(files_payload, &cliprdr.files)) {
          return false;
        }
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }

    if (cliprdr.kind != CliprdrMessageKind::kNone) {
      session_message->has_cliprdr = true;
      session_message->cliprdr = std::move(cliprdr);
      return true;
    }
    return false;
  }
  return false;
}

SessionMessageType ParseSessionMessage(const std::vector<unsigned char>& frame) {
  SessionMessageType session_message;
  ParseMouseEventMessage(frame, &session_message);
  ParseKeyEventMessage(frame, &session_message);
  ParseMiscMessage(frame, &session_message);
  ParseClipboardMessage(frame, &session_message);
  ParseMultiClipboardsMessage(frame, &session_message);
  ParseCliprdrMessage(frame, &session_message);
  return session_message;
}

WORD VirtualKeyFromControlKey(int control_key) {
  switch (control_key) {
    case 1: return VK_MENU;          // Alt
    case 2: return VK_BACK;
    case 3: return VK_CAPITAL;
    case 4: return VK_CONTROL;
    case 5: return VK_DELETE;
    case 6: return VK_DOWN;
    case 7: return VK_END;
    case 8: return VK_ESCAPE;
    case 9: return VK_F1;
    case 10: return VK_F10;
    case 11: return VK_F11;
    case 12: return VK_F12;
    case 13: return VK_F2;
    case 14: return VK_F3;
    case 15: return VK_F4;
    case 16: return VK_F5;
    case 17: return VK_F6;
    case 18: return VK_F7;
    case 19: return VK_F8;
    case 20: return VK_F9;
    case 21: return VK_HOME;
    case 22: return VK_LEFT;
    case 23: return VK_LWIN;
    case 25: return VK_NEXT;
    case 26: return VK_PRIOR;
    case 27: return VK_RETURN;
    case 28: return VK_RIGHT;
    case 29: return VK_SHIFT;
    case 30: return VK_SPACE;
    case 31: return VK_TAB;
    case 32: return VK_UP;
    case 33: return VK_NUMPAD0;
    case 34: return VK_NUMPAD1;
    case 35: return VK_NUMPAD2;
    case 36: return VK_NUMPAD3;
    case 37: return VK_NUMPAD4;
    case 38: return VK_NUMPAD5;
    case 39: return VK_NUMPAD6;
    case 40: return VK_NUMPAD7;
    case 41: return VK_NUMPAD8;
    case 42: return VK_NUMPAD9;
    case 43: return VK_CANCEL;
    case 44: return VK_CLEAR;
    case 46: return VK_PAUSE;
    case 55: return VK_PRINT;
    case 57: return VK_SNAPSHOT;
    case 58: return VK_INSERT;
    case 60: return VK_SLEEP;
    case 62: return VK_SCROLL;
    case 63: return VK_NUMLOCK;
    case 64: return VK_RWIN;
    case 65: return VK_APPS;
    case 66: return VK_MULTIPLY;
    case 67: return VK_ADD;
    case 68: return VK_SUBTRACT;
    case 69: return VK_DECIMAL;
    case 70: return VK_DIVIDE;
    case 71: return VK_OEM_PLUS;
    case 72: return VK_RETURN;
    case 73: return VK_RSHIFT;
    case 74: return VK_RCONTROL;
    case 75: return VK_RMENU;
    case 76: return VK_VOLUME_MUTE;
    case 77: return VK_VOLUME_UP;
    case 78: return VK_VOLUME_DOWN;
    case 79: return static_cast<WORD>(0x5E);  // VK_POWER is missing in some older Windows SDKs.
    default: return 0;
  }
}

bool IsExtendedVirtualKey(WORD vk) {
  switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_APPS:
    case VK_RWIN:
    case VK_LWIN:
    case VK_BROWSER_BACK:
    case VK_BROWSER_FORWARD:
      return true;
    default:
      return false;
  }
}

void SendVirtualKey(WORD vk, bool down) {
  if (vk == 0) {
    return;
  }
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwFlags = down ? 0U : KEYEVENTF_KEYUP;
  if (IsExtendedVirtualKey(vk)) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  SendInput(1, &input, sizeof(input));
}

void SendScancode(WORD scan, bool down) {
  if (scan == 0) {
    return;
  }

  // RustDesk 1:1 / translate modes carry Windows scan codes with an E0/E1
  // prefix in the high byte (for example Win = 0xE05B). Preserve the packed
  // scan word and mark it as an extended key for SendInput.
  const bool extended =
      (scan & 0xFF00U) == 0xE000U || (scan & 0xFF00U) == 0xE100U;

  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = scan;
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0U : KEYEVENTF_KEYUP);
  if (extended) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  SendInput(1, &input, sizeof(input));
}

void SendUnicodeCharacter(wchar_t value) {
  INPUT inputs[2] = {};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wScan = value;
  inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
  inputs[1] = inputs[0];
  inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
  SendInput(2, inputs, sizeof(INPUT));
}

std::vector<WORD> MapModifierKeys(const std::vector<int>& modifiers) {
  std::vector<WORD> keys;
  for (int modifier : modifiers) {
    const WORD vk = VirtualKeyFromControlKey(modifier);
    if (vk != 0) {
      keys.push_back(vk);
    }
  }
  return keys;
}

void SendModifierSet(const std::vector<WORD>& modifiers, bool down) {
  for (WORD vk : modifiers) {
    SendVirtualKey(vk, down);
  }
}

bool IsHotkeyModifierVirtualKey(WORD vk) {
  switch (vk) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
      return true;
    default:
      return false;
  }
}

bool IsAsciiPrintableWide(wchar_t value) {
  return value >= 0x20 && value <= 0x7E;
}

std::vector<WORD> FilterCharacterInputModifiers(const std::vector<WORD>& modifiers) {
  std::vector<WORD> filtered;
  for (WORD vk : modifiers) {
    if (IsHotkeyModifierVirtualKey(vk)) {
      filtered.push_back(vk);
    }
  }
  return filtered;
}

void AppendModifierIfMissing(std::vector<WORD>* modifiers, WORD vk) {
  if (modifiers == nullptr || vk == 0) {
    return;
  }
  if (std::find(modifiers->begin(), modifiers->end(), vk) == modifiers->end()) {
    modifiers->push_back(vk);
  }
}

std::vector<WORD> NormalizeWin2WinHotkeyModifiers(
    uint32_t hotkey_code,
    WORD vk,
    const std::vector<WORD>& modifiers) {
  std::vector<WORD> effective_modifiers = modifiers;
  const wchar_t hotkey_char = static_cast<wchar_t>(hotkey_code & 0xFFFFU);
  if (!IsAsciiPrintableWide(hotkey_char)) {
    return effective_modifiers;
  }

  const SHORT vk_pair = VkKeyScanW(hotkey_char);
  if (vk_pair == -1) {
    return effective_modifiers;
  }

  const WORD char_vk = static_cast<WORD>(vk_pair & 0x00FF);
  if (char_vk == 0 || char_vk != vk) {
    return effective_modifiers;
  }

  effective_modifiers = FilterCharacterInputModifiers(modifiers);
  const BYTE shift_state = static_cast<BYTE>((vk_pair >> 8) & 0x00FF);
  if ((shift_state & 0x01U) != 0) {
    AppendModifierIfMissing(&effective_modifiers, VK_SHIFT);
  }
  if ((shift_state & 0x02U) != 0) {
    AppendModifierIfMissing(&effective_modifiers, VK_CONTROL);
  }
  if ((shift_state & 0x04U) != 0) {
    AppendModifierIfMissing(&effective_modifiers, VK_MENU);
  }
  return effective_modifiers;
}

bool TrySendLayoutCharacterPress(
    wchar_t value,
    const std::vector<WORD>& modifiers) {
  const SHORT vk_pair = VkKeyScanW(value);
  if (vk_pair == -1) {
    return false;
  }

  const WORD vk = static_cast<WORD>(vk_pair & 0x00FF);
  if (vk == 0) {
    return false;
  }

  std::vector<WORD> effective_modifiers = FilterCharacterInputModifiers(modifiers);
  const BYTE shift_state = static_cast<BYTE>((vk_pair >> 8) & 0x00FF);
  if ((shift_state & 0x01U) != 0) {
    effective_modifiers.push_back(VK_SHIFT);
  }
  if ((shift_state & 0x02U) != 0) {
    effective_modifiers.push_back(VK_CONTROL);
  }
  if ((shift_state & 0x04U) != 0) {
    effective_modifiers.push_back(VK_MENU);
  }

  SendModifierSet(effective_modifiers, true);
  SendVirtualKey(vk, true);
  SendVirtualKey(vk, false);
  SendModifierSet(effective_modifiers, false);
  return true;
}

bool SendLegacyCharacterInput(
    wchar_t value,
    bool down,
    bool press,
    const std::vector<WORD>& modifiers) {
  if (!press && !down) {
    return true;
  }

  if (IsAsciiPrintableWide(value) && TrySendLayoutCharacterPress(value, modifiers)) {
    return true;
  }

  SendUnicodeCharacter(value);
  return true;
}

bool BuildRustDeskAbsoluteMouseMoveInput(
    LONG screen_x,
    LONG screen_y,
    INPUT* input) {
  if (input == nullptr) {
    return false;
  }

  const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtual_width < 1 || virtual_height < 1) {
    return false;
  }

  const LONG clamped_x = static_cast<LONG>(ClampInt(
      static_cast<int>(screen_x),
      virtual_left,
      virtual_left + virtual_width - 1));
  const LONG clamped_y = static_cast<LONG>(ClampInt(
      static_cast<int>(screen_y),
      virtual_top,
      virtual_top + virtual_height - 1));
  const LONGLONG width_denominator = virtual_width > 1 ? virtual_width - 1 : 1;
  const LONGLONG height_denominator = virtual_height > 1 ? virtual_height - 1 : 1;

  std::memset(input, 0, sizeof(*input));
  input->type = INPUT_MOUSE;
  input->mi.dx = static_cast<LONG>(
      (static_cast<LONGLONG>(clamped_x - virtual_left) * 65535LL) /
      width_denominator);
  input->mi.dy = static_cast<LONG>(
      (static_cast<LONGLONG>(clamped_y - virtual_top) * 65535LL) /
      height_denominator);
  input->mi.dwFlags =
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  return true;
}

bool SendRustDeskAbsoluteMouseMove(LONG screen_x, LONG screen_y) {
  INPUT input = {};
  if (!BuildRustDeskAbsoluteMouseMoveInput(screen_x, screen_y, &input)) {
    return false;
  }
  if (SendInput(1, &input, sizeof(input)) != 1) {
    // Very old or restricted desktops can reject VIRTUALDESK injection. Keep
    // SetCursorPos only as a compatibility fallback, not as the normal path.
    if (!SetCursorPos(screen_x, screen_y)) {
      return false;
    }
  }
  g_rustdesk_last_absolute_mouse_x.store(screen_x);
  g_rustdesk_last_absolute_mouse_y.store(screen_y);
  g_rustdesk_last_absolute_mouse_valid.store(true);
  return true;
}

void SendRustDeskMouseActionAtLastPosition(DWORD flags, DWORD mouse_data) {
  INPUT inputs[2] = {};
  UINT count = 0;
  if (g_rustdesk_last_absolute_mouse_valid.load()) {
    if (BuildRustDeskAbsoluteMouseMoveInput(
            g_rustdesk_last_absolute_mouse_x.load(),
            g_rustdesk_last_absolute_mouse_y.load(),
            &inputs[count])) {
      ++count;
    }
  }
  inputs[count].type = INPUT_MOUSE;
  inputs[count].mi.dwFlags = flags;
  inputs[count].mi.mouseData = mouse_data;
  ++count;
  SendInput(count, inputs, sizeof(INPUT));
}

bool HandleMouseEvent(const MouseEventData& event, int display_width, int display_height) {
  const int event_type = event.mask & kMouseTypeMask;
  const int buttons = event.mask >> 3;

  if (event_type == kMouseTypeMove) {
    const DesktopCaptureBounds bounds = GetDesktopCaptureBounds();
    const int clamped_x = ClampInt(event.x, 0, display_width - 1);
    const int clamped_y = ClampInt(event.y, 0, display_height - 1);
    SendRustDeskAbsoluteMouseMove(
        static_cast<LONG>(bounds.origin_x + clamped_x),
        static_cast<LONG>(bounds.origin_y + clamped_y));
    return true;
  }

  if (event_type == kMouseTypeMoveRelative) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = event.x;
    input.mi.dy = event.y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
    POINT cursor = {};
    if (GetCursorPos(&cursor)) {
      g_rustdesk_last_absolute_mouse_x.store(cursor.x);
      g_rustdesk_last_absolute_mouse_y.store(cursor.y);
      g_rustdesk_last_absolute_mouse_valid.store(true);
    }
    return true;
  }

  switch (event_type) {
    case kMouseTypeDown: {
      DWORD flags = 0;
      DWORD mouse_data = 0;
      if (buttons & kMouseButtonLeft) flags |= MOUSEEVENTF_LEFTDOWN;
      if (buttons & kMouseButtonRight) flags |= MOUSEEVENTF_RIGHTDOWN;
      if (buttons & kMouseButtonWheel) flags |= MOUSEEVENTF_MIDDLEDOWN;
      if (buttons & kMouseButtonBack) {
        flags |= MOUSEEVENTF_XDOWN;
        mouse_data = XBUTTON1;
      }
      if (buttons & kMouseButtonForward) {
        flags |= MOUSEEVENTF_XDOWN;
        mouse_data = XBUTTON2;
      }
      if (flags != 0) {
        SendRustDeskMouseActionAtLastPosition(flags, mouse_data);
      }
      break;
    }
    case kMouseTypeUp: {
      DWORD flags = 0;
      DWORD mouse_data = 0;
      if (buttons & kMouseButtonLeft) flags |= MOUSEEVENTF_LEFTUP;
      if (buttons & kMouseButtonRight) flags |= MOUSEEVENTF_RIGHTUP;
      if (buttons & kMouseButtonWheel) flags |= MOUSEEVENTF_MIDDLEUP;
      if (buttons & kMouseButtonBack) {
        flags |= MOUSEEVENTF_XUP;
        mouse_data = XBUTTON1;
      }
      if (buttons & kMouseButtonForward) {
        flags |= MOUSEEVENTF_XUP;
        mouse_data = XBUTTON2;
      }
      if (flags != 0) {
        SendRustDeskMouseActionAtLastPosition(flags, mouse_data);
      }
      break;
    }
    case kMouseTypeWheel:
    case kMouseTypeTrackpad:
      if (event.x != 0) {
        SendRustDeskMouseActionAtLastPosition(
            MOUSEEVENTF_HWHEEL,
            static_cast<DWORD>(-event.x));
      }
      if (event.y != 0) {
        SendRustDeskMouseActionAtLastPosition(
            MOUSEEVENTF_WHEEL,
            static_cast<DWORD>(event.y));
      }
      break;
    default:
      return false;
  }
  return true;
}

bool HandleKeyEvent(const KeyEventData& event) {
  const std::vector<WORD> modifiers = MapModifierKeys(event.modifiers);

  if (!event.seq.empty()) {
    for (wchar_t value : event.seq) {
      SendLegacyCharacterInput(value, true, true, modifiers);
    }
    return true;
  }

  if (event.has_unicode) {
    return SendLegacyCharacterInput(
        static_cast<wchar_t>(event.unicode),
        event.down,
        event.press,
        modifiers);
  }

  if (event.has_win2win_hotkey) {
    const WORD vk = static_cast<WORD>((event.win2win_hotkey >> 16U) & 0xFFFFU);
    if (vk != 0) {
      const std::vector<WORD> effective_modifiers =
          NormalizeWin2WinHotkeyModifiers(event.win2win_hotkey, vk, modifiers);
      if (event.press) {
        SendModifierSet(effective_modifiers, true);
        SendVirtualKey(vk, true);
        SendVirtualKey(vk, false);
        SendModifierSet(effective_modifiers, false);
      } else {
        SendModifierSet(effective_modifiers, event.down);
        SendVirtualKey(vk, event.down);
      }
      return true;
    }
  }

  if (event.has_chr) {
    if (event.mode == 0) {
      return SendLegacyCharacterInput(
          static_cast<wchar_t>(event.chr),
          event.down,
          event.press,
          modifiers);
    }
    if (event.press) {
      SendModifierSet(modifiers, true);
      SendScancode(static_cast<WORD>(event.chr), true);
      SendScancode(static_cast<WORD>(event.chr), false);
      SendModifierSet(modifiers, false);
    } else {
      SendModifierSet(modifiers, event.down);
      SendScancode(static_cast<WORD>(event.chr), event.down);
    }
    return true;
  }

  if (event.has_control_key) {
    const WORD vk = VirtualKeyFromControlKey(event.control_key);
    if (vk != 0) {
      if (event.press) {
        SendModifierSet(modifiers, true);
        SendVirtualKey(vk, true);
        SendVirtualKey(vk, false);
        SendModifierSet(modifiers, false);
      } else {
        SendModifierSet(modifiers, event.down);
        SendVirtualKey(vk, event.down);
      }
      return true;
    }
  }
  return false;
}

bool ParseKeyExchange(
    const std::vector<unsigned char>& payload,
    std::vector<std::vector<unsigned char>>* keys) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      std::vector<unsigned char> key;
      if (!ReadLengthDelimited(payload, &offset, &key)) {
        return false;
      }
      keys->push_back(std::move(key));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return !keys->empty();
}

bool ParseRegisterPkResponse(
    const std::vector<unsigned char>& payload,
    RegisterPkResponseData* response) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      response->result = static_cast<int>(value);
    } else if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      response->keep_alive_ms = static_cast<int>(value) * 1000;
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseRegisterPeerResponse(
    const std::vector<unsigned char>& payload,
    RegisterPeerResponseData* response) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      response->request_pk = value != 0;
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseRequestRelay(
    const std::vector<unsigned char>& payload,
    RequestRelayData* relay) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 3U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, &relay->socket_addr)) {
        return false;
      }
    } else if (field_number == 5U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      relay->secure = value != 0;
    } else if ((field_number == 2U || field_number == 4U) && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 2U) {
        relay->uuid = Utf8ToWide(std::string(value.begin(), value.end()));
      } else {
        relay->relay_server = Utf8ToWide(std::string(value.begin(), value.end()));
      }
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParsePunchHole(
    const std::vector<unsigned char>& payload,
    PunchHoleData* punch_hole) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, &punch_hole->socket_addr)) {
        return false;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      punch_hole->relay_server = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 5U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      punch_hole->force_relay = value != 0;
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFetchLocalAddr(
    const std::vector<unsigned char>& payload,
    FetchLocalAddrData* fetch_local_addr) {
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, &fetch_local_addr->socket_addr)) {
        return false;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      fetch_local_addr->relay_server = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseRelayResponse(
    const std::vector<unsigned char>& frame,
    RelayResponseData* relay_response) {
  *relay_response = RelayResponseData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 19U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subwire_type == 2U) {
        std::vector<unsigned char> value;
        if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
          return false;
        }
        switch (subfield_number) {
          case 2U:
            relay_response->uuid = Utf8ToWide(std::string(value.begin(), value.end()));
            break;
          case 3U:
            relay_response->relay_server = Utf8ToWide(std::string(value.begin(), value.end()));
            break;
          case 5U:
            relay_response->signed_peer_public_key = value;
            break;
          case 6U:
            relay_response->refuse_reason = Utf8ToWide(std::string(value.begin(), value.end()));
            break;
          default:
            break;
        }
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool ParsePunchHoleResponse(
    const std::vector<unsigned char>& frame,
    PunchHoleResponseData* punch_hole_response) {
  *punch_hole_response = PunchHoleResponseData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 11U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 1U && subwire_type == 2U) {
        if (!ReadLengthDelimited(payload, &payload_offset, &punch_hole_response->socket_addr)) {
          return false;
        }
      } else if (subfield_number == 2U && subwire_type == 2U) {
        if (!ReadLengthDelimited(
                payload,
                &payload_offset,
                &punch_hole_response->signed_peer_public_key)) {
          return false;
        }
      } else if (subfield_number == 3U && subwire_type == 0U) {
        uint64_t value = 0;
        if (!ReadVarint(payload, &payload_offset, &value)) {
          return false;
        }
        punch_hole_response->failure = static_cast<int>(value);
      } else if ((subfield_number == 4U || subfield_number == 7U) && subwire_type == 2U) {
        std::vector<unsigned char> value;
        if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
          return false;
        }
        if (subfield_number == 4U) {
          punch_hole_response->relay_server = Utf8ToWide(std::string(value.begin(), value.end()));
        } else {
          punch_hole_response->other_failure = Utf8ToWide(std::string(value.begin(), value.end()));
        }
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool ParseSignedIdMessage(
    const std::vector<unsigned char>& frame,
    std::vector<unsigned char>* signed_id) {
  signed_id->clear();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 3U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 1U && subwire_type == 2U) {
        return ReadLengthDelimited(payload, &payload_offset, signed_id);
      }
      if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return false;
  }
  return false;
}

bool ParseHashChallengeMessage(
    const std::vector<unsigned char>& frame,
    HashMessageData* hash_message) {
  *hash_message = HashMessageData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 9U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if ((subfield_number == 1U || subfield_number == 2U) && subwire_type == 2U) {
        std::vector<unsigned char> value;
        if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
          return false;
        }
        if (subfield_number == 1U) {
          hash_message->salt.assign(value.begin(), value.end());
        } else {
          hash_message->challenge.assign(value.begin(), value.end());
        }
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return !hash_message->challenge.empty();
  }
  return false;
}

bool ParseIdPkPayload(
    const std::vector<unsigned char>& payload,
    std::wstring* id,
    std::vector<unsigned char>* public_key) {
  id->clear();
  public_key->clear();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      *id = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 2U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, public_key)) {
        return false;
      }
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return !public_key->empty();
}

bool ParsePublicKeyMessage(
    const std::vector<unsigned char>& frame,
    PublicKeyMessageData* public_key_message) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 4U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 1U && subwire_type == 2U) {
        if (!ReadLengthDelimited(payload, &payload_offset, &public_key_message->asymmetric_value)) {
          return false;
        }
      } else if (subfield_number == 2U && subwire_type == 2U) {
        if (!ReadLengthDelimited(payload, &payload_offset, &public_key_message->symmetric_value)) {
          return false;
        }
      } else if (!SkipField(subwire_type, payload, &payload_offset)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool ParseLoginRequestMessage(
    const std::vector<unsigned char>& frame,
    LoginRequestData* login_request) {
  *login_request = LoginRequestData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 7U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subfield_number == 10U && subwire_type == 0U) {
        uint64_t session_id = 0;
        if (!ReadVarint(payload, &payload_offset, &session_id)) {
          return false;
        }
        login_request->has_session_id = true;
        login_request->session_id = session_id;
        continue;
      }

      if (subwire_type != 2U) {
        if (!SkipField(subwire_type, payload, &payload_offset)) {
          return false;
        }
        continue;
      }

      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
        return false;
      }

      switch (subfield_number) {
        case 2U:
          login_request->password = value;
          break;
        case 4U:
          login_request->my_id = Utf8ToWide(std::string(value.begin(), value.end()));
          break;
        case 5U:
          login_request->my_name = Utf8ToWide(std::string(value.begin(), value.end()));
          break;
        case 11U:
          login_request->version = Utf8ToWide(std::string(value.begin(), value.end()));
          break;
        case 13U:
          login_request->my_platform = Utf8ToWide(std::string(value.begin(), value.end()));
          break;
        case 7U: {
          // An empty FileTransfer {} still means the controller explicitly requested
          // a file-transfer session. The nested payload may be zero-length when the
          // client keeps the default remote dir and show_hidden=false.
          login_request->has_file_transfer = true;
          size_t nested_offset = 0;
          while (nested_offset < value.size()) {
            uint64_t nested_tag = 0;
            if (!ReadVarint(value, &nested_offset, &nested_tag)) {
              return false;
            }
            const unsigned int nested_field_number =
                static_cast<unsigned int>(nested_tag >> 3U);
            const unsigned int nested_wire_type =
                static_cast<unsigned int>(nested_tag & 0x07U);
            if (nested_field_number == 1U && nested_wire_type == 2U) {
              std::vector<unsigned char> dir;
              if (!ReadLengthDelimited(value, &nested_offset, &dir)) {
                return false;
              }
              login_request->file_transfer_dir =
                  Utf8ToWide(std::string(dir.begin(), dir.end()));
            } else if (nested_field_number == 2U && nested_wire_type == 0U) {
              uint64_t show_hidden = 0;
              if (!ReadVarint(value, &nested_offset, &show_hidden)) {
                return false;
              }
              login_request->file_transfer_show_hidden = show_hidden != 0;
            } else if (!SkipField(nested_wire_type, value, &nested_offset)) {
              return false;
            }
          }
          break;
        }
        default:
          break;
      }
    }
    return true;
  }
  return false;
}

void CloseFileTransferHandle(HANDLE* file) {
  if (file == nullptr || *file == INVALID_HANDLE_VALUE) {
    return;
  }
  CloseHandle(*file);
  *file = INVALID_HANDLE_VALUE;
}

uint64_t FileTimeToUnixSeconds(const FILETIME& file_time) {
  ULARGE_INTEGER value = {};
  value.LowPart = file_time.dwLowDateTime;
  value.HighPart = file_time.dwHighDateTime;
  if (value.QuadPart < 116444736000000000ULL) {
    return 0;
  }
  return (value.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

FILETIME UnixSecondsToFileTime(uint64_t unix_seconds) {
  ULARGE_INTEGER value = {};
  value.QuadPart = unix_seconds * 10000000ULL + 116444736000000000ULL;
  FILETIME result = {};
  result.dwLowDateTime = value.LowPart;
  result.dwHighDateTime = value.HighPart;
  return result;
}

bool ApplyFileTransferModifiedTime(const std::wstring& path, uint64_t unix_seconds) {
  if (path.empty() || unix_seconds == 0) {
    return true;
  }
  const HANDLE file = CreateFileW(
      path.c_str(),
      FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const FILETIME modified = UnixSecondsToFileTime(unix_seconds);
  const BOOL ok = SetFileTime(file, nullptr, nullptr, &modified);
  CloseHandle(file);
  return ok != FALSE;
}

std::wstring ResolveFileTransferFilesystemPath(const std::wstring& remote_path) {
  std::wstring normalized = NormalizePathSeparators(Trim(remote_path));
  if (normalized.empty()) {
    return L"/";
  }
  // RustDesk's file-transfer UI may request the Windows drive list using either
  // "/" or "\" depending on the navigation path. Treat both as the virtual root
  // that enumerates all logical drives.
  if (normalized == L"\\") {
    return L"/";
  }
  if (normalized.size() == 2 && normalized[1] == L':') {
    normalized += L"\\";
  }
  return normalized;
}

std::wstring JoinWindowsPath(const std::wstring& base, const std::wstring& component) {
  if (component.empty()) {
    return NormalizePathSeparators(base);
  }
  if (base.empty()) {
    return NormalizePathSeparators(component);
  }
  std::wstring joined = NormalizePathSeparators(base);
  if (!joined.empty() && joined.back() != L'\\') {
    joined += L"\\";
  }
  joined += NormalizePathSeparators(component);
  return joined;
}

std::wstring GetDirectoryPathPart(const std::wstring& path) {
  const size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return std::wstring();
  }
  return path.substr(0, separator);
}

bool ValidateFileTransferRelativePath(
    const std::wstring& relative_path,
    bool allow_empty,
    std::wstring* error_text) {
  if (relative_path.empty()) {
    if (allow_empty) {
      return true;
    }
    if (error_text != nullptr) {
      *error_text = L"file transfer path is empty";
    }
    return false;
  }

  const std::wstring normalized = NormalizePathSeparators(relative_path);
  if (IsAbsoluteWindowsPath(normalized) ||
      normalized[0] == L'\\' ||
      normalized[0] == L'/') {
    if (error_text != nullptr) {
      *error_text = L"absolute file transfer paths are not allowed";
    }
    return false;
  }
  if (normalized.find(L':') != std::wstring::npos) {
    if (error_text != nullptr) {
      *error_text = L"drive-qualified file transfer paths are not allowed";
    }
    return false;
  }

  size_t start = 0;
  while (start <= normalized.size()) {
    const size_t end = normalized.find(L'\\', start);
    const std::wstring part =
        end == std::wstring::npos
            ? normalized.substr(start)
            : normalized.substr(start, end - start);
    if (part.empty() || part == L"." || part == L"..") {
      if (error_text != nullptr) {
        *error_text = L"invalid file transfer path component";
      }
      return false;
    }
    if (end == std::wstring::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

bool ResolveFileTransferTargetPath(
    const std::wstring& base_path,
    const std::wstring& relative_name,
    bool allow_empty_name,
    std::wstring* resolved_path,
    std::wstring* error_text) {
  if (resolved_path == nullptr) {
    return false;
  }
  resolved_path->clear();
  if (!ValidateFileTransferRelativePath(relative_name, allow_empty_name, error_text)) {
    return false;
  }
  *resolved_path = relative_name.empty()
                       ? ResolveFileTransferFilesystemPath(base_path)
                       : JoinWindowsPath(ResolveFileTransferFilesystemPath(base_path), relative_name);
  return true;
}

bool IsFileTransferDriveRootPath(const std::wstring& path) {
  const std::wstring normalized = NormalizePathSeparators(path);
  return normalized.size() == 3 &&
         std::iswalpha(normalized[0]) != 0 &&
         normalized[1] == L':' &&
         normalized[2] == L'\\';
}

bool IsFileTransferUncRootPath(const std::wstring& path) {
  const std::wstring normalized = NormalizePathSeparators(path);
  if (normalized.size() < 5 || normalized[0] != L'\\' || normalized[1] != L'\\') {
    return false;
  }
  const size_t server_end = normalized.find(L'\\', 2);
  if (server_end == std::wstring::npos) {
    return false;
  }
  const size_t share_end = normalized.find(L'\\', server_end + 1);
  return share_end == std::wstring::npos || share_end == normalized.size() - 1;
}

bool ResolveFileTransferMutablePath(
    const std::wstring& requested_path,
    std::wstring* resolved_path,
    std::wstring* error_text) {
  if (resolved_path == nullptr) {
    return false;
  }
  *resolved_path = ResolveFileTransferFilesystemPath(requested_path);
  if (resolved_path->empty()) {
    if (error_text != nullptr) {
      *error_text = L"file transfer path is empty";
    }
    return false;
  }
  if (*resolved_path == L"/") {
    if (error_text != nullptr) {
      *error_text = L"modifying the file-transfer root is not allowed";
    }
    return false;
  }
  if (!IsAbsoluteWindowsPath(*resolved_path)) {
    if (error_text != nullptr) {
      *error_text = L"absolute file transfer path required";
    }
    return false;
  }
  if (IsFileTransferDriveRootPath(*resolved_path)) {
    if (error_text != nullptr) {
      *error_text = L"modifying a drive root is not allowed";
    }
    return false;
  }
  if (IsFileTransferUncRootPath(*resolved_path)) {
    if (error_text != nullptr) {
      *error_text = L"modifying a network share root is not allowed";
    }
    return false;
  }
  return true;
}

bool TryRemoveReadOnlyAttribute(const std::wstring& path, DWORD attributes) {
  if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) {
    return true;
  }
  return SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != FALSE;
}

bool RemoveFileTransferDirectoryTree(
    const std::wstring& directory_path,
    std::wstring* error_text) {
  const DWORD attributes = GetFileAttributesW(directory_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (error_text != nullptr) {
      *error_text = L"file transfer directory not found";
    }
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    if (error_text != nullptr) {
      *error_text = L"file transfer path is not a directory";
    }
    return false;
  }

  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    TryRemoveReadOnlyAttribute(directory_path, attributes);
    if (RemoveDirectoryW(directory_path.c_str()) != FALSE) {
      return true;
    }
    if (error_text != nullptr) {
      *error_text =
          L"RemoveDirectoryW failed while removing directory link, error=" +
          std::to_wstring(GetLastError());
    }
    return false;
  }

  std::wstring pattern = directory_path;
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern += L"\\";
  }
  pattern += L"*";

  WIN32_FIND_DATAW find_data = {};
  HANDLE find_handle = FindFirstFileW(pattern.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      if (RemoveDirectoryW(directory_path.c_str()) != FALSE) {
        return true;
      }
      if (error_text != nullptr) {
        *error_text =
            L"RemoveDirectoryW failed while removing empty directory, error=" +
            std::to_wstring(GetLastError());
      }
      return false;
    }
    if (error_text != nullptr) {
      *error_text =
          L"FindFirstFileW failed while enumerating directory tree, error=" +
          std::to_wstring(error);
    }
    return false;
  }

  bool success = true;
  do {
    const std::wstring name(find_data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }

    const std::wstring entry_path = JoinWindowsPath(directory_path, name);
    const DWORD entry_attributes = find_data.dwFileAttributes;
    const bool is_directory = (entry_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_link = (entry_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    TryRemoveReadOnlyAttribute(entry_path, entry_attributes);

    if (is_directory && !is_link) {
      if (!RemoveFileTransferDirectoryTree(entry_path, error_text)) {
        success = false;
        break;
      }
      continue;
    }

    const BOOL removed = is_directory
                             ? RemoveDirectoryW(entry_path.c_str())
                             : DeleteFileW(entry_path.c_str());
    if (removed == FALSE) {
      if (error_text != nullptr) {
        *error_text =
            std::wstring(is_directory ? L"RemoveDirectoryW failed, error="
                                      : L"DeleteFileW failed, error=") +
            std::to_wstring(GetLastError());
      }
      success = false;
      break;
    }
  } while (FindNextFileW(find_handle, &find_data) != FALSE);

  const DWORD iterate_error = GetLastError();
  FindClose(find_handle);
  if (!success) {
    return false;
  }
  if (iterate_error != ERROR_NO_MORE_FILES) {
    if (error_text != nullptr) {
      *error_text =
          L"FindNextFileW failed while enumerating directory tree, error=" +
          std::to_wstring(iterate_error);
    }
    return false;
  }

  TryRemoveReadOnlyAttribute(directory_path, attributes);
  if (RemoveDirectoryW(directory_path.c_str()) == FALSE) {
    if (error_text != nullptr) {
      *error_text =
          L"RemoveDirectoryW failed while removing directory, error=" +
          std::to_wstring(GetLastError());
    }
    return false;
  }
  return true;
}

bool BuildFileTransferEntryFromFindData(
    const WIN32_FIND_DATAW& find_data,
    const std::wstring& name,
    FileTransferEntryData* entry) {
  if (entry == nullptr) {
    return false;
  }
  entry->name = name;
  entry->is_hidden = (find_data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
  entry->modified_time = FileTimeToUnixSeconds(find_data.ftLastWriteTime);
  const bool is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const bool is_link = (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  if (is_directory) {
    entry->entry_type = static_cast<int>(
        is_link ? FileTransferEntryType::kDirLink : FileTransferEntryType::kDir);
    entry->size = 0;
  } else {
    entry->entry_type = static_cast<int>(
        is_link ? FileTransferEntryType::kFileLink : FileTransferEntryType::kFile);
    entry->size =
        (static_cast<uint64_t>(find_data.nFileSizeHigh) << 32U) |
        static_cast<uint64_t>(find_data.nFileSizeLow);
  }
  return true;
}

bool CollectFileTransferDirectoryEntries(
    const std::wstring& requested_path,
    bool include_hidden,
    std::vector<FileTransferEntryData>* entries,
    std::wstring* error_text) {
  if (entries == nullptr) {
    return false;
  }
  entries->clear();

  const std::wstring resolved_path = ResolveFileTransferFilesystemPath(requested_path);
  if (resolved_path == L"/") {
    const DWORD drives = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
      if ((drives & (1UL << index)) == 0) {
        continue;
      }
      FileTransferEntryData drive;
      drive.entry_type = static_cast<int>(FileTransferEntryType::kDirDrive);
      drive.name = std::wstring(1, static_cast<wchar_t>(L'A' + index)) + L":";
      entries->push_back(drive);
    }
    return true;
  }

  const DWORD attributes = GetFileAttributesW(resolved_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (error_text != nullptr) {
      *error_text = L"file transfer path not found";
    }
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    if (error_text != nullptr) {
      *error_text = L"file transfer path is not a directory";
    }
    return false;
  }

  std::wstring pattern = resolved_path;
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern += L"\\";
  }
  pattern += L"*";

  WIN32_FIND_DATAW find_data = {};
  HANDLE find_handle = FindFirstFileW(pattern.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    if (error_text != nullptr) {
      *error_text = L"FindFirstFileW failed for file transfer directory";
    }
    return false;
  }

  do {
    const std::wstring name(find_data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    FileTransferEntryData entry;
    if (!BuildFileTransferEntryFromFindData(find_data, name, &entry)) {
      continue;
    }
    if (entry.is_hidden && !include_hidden) {
      continue;
    }
    entries->push_back(std::move(entry));
  } while (FindNextFileW(find_handle, &find_data) != FALSE);

  FindClose(find_handle);
  return true;
}

bool CollectFileTransferRecursiveFilesInternal(
    const std::wstring& current_path,
    const std::wstring& relative_prefix,
    bool include_hidden,
    std::vector<FileTransferEntryData>* entries,
    std::wstring* error_text) {
  std::wstring pattern = current_path;
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern += L"\\";
  }
  pattern += L"*";

  WIN32_FIND_DATAW find_data = {};
  HANDLE find_handle = FindFirstFileW(pattern.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    if (error_text != nullptr) {
      *error_text = L"FindFirstFileW failed while collecting recursive file transfer entries";
    }
    return false;
  }

  do {
    const std::wstring name(find_data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }

    FileTransferEntryData entry;
    if (!BuildFileTransferEntryFromFindData(find_data, name, &entry)) {
      continue;
    }
    if (entry.is_hidden && !include_hidden) {
      continue;
    }

    const bool is_directory =
        entry.entry_type == static_cast<int>(FileTransferEntryType::kDir) ||
        entry.entry_type == static_cast<int>(FileTransferEntryType::kDirLink);
    const bool is_link =
        entry.entry_type == static_cast<int>(FileTransferEntryType::kDirLink) ||
        entry.entry_type == static_cast<int>(FileTransferEntryType::kFileLink);
    const std::wstring child_relative =
        relative_prefix.empty() ? name : JoinWindowsPath(relative_prefix, name);
    const std::wstring child_path = JoinWindowsPath(current_path, name);

    if (is_directory) {
      if (is_link) {
        continue;
      }
      if (!CollectFileTransferRecursiveFilesInternal(
              child_path,
              child_relative,
              include_hidden,
              entries,
              error_text)) {
        FindClose(find_handle);
        return false;
      }
      continue;
    }

    if (is_link) {
      continue;
    }
    entry.entry_type = static_cast<int>(FileTransferEntryType::kFile);
    entry.name = child_relative;
    entries->push_back(std::move(entry));
  } while (FindNextFileW(find_handle, &find_data) != FALSE);

  FindClose(find_handle);
  return true;
}

bool CollectFileTransferRecursiveFiles(
    const std::wstring& requested_path,
    bool include_hidden,
    std::vector<FileTransferEntryData>* entries,
    std::wstring* error_text) {
  if (entries == nullptr) {
    return false;
  }
  entries->clear();

  const std::wstring resolved_path = ResolveFileTransferFilesystemPath(requested_path);
  const DWORD attributes = GetFileAttributesW(resolved_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (error_text != nullptr) {
      *error_text = L"file transfer path not found";
    }
    return false;
  }

  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    WIN32_FILE_ATTRIBUTE_DATA metadata = {};
    if (!GetFileAttributesExW(
            resolved_path.c_str(),
            GetFileExInfoStandard,
            &metadata)) {
      if (error_text != nullptr) {
        *error_text = L"GetFileAttributesExW failed for file transfer file";
      }
      return false;
    }
    FileTransferEntryData entry;
    entry.entry_type = static_cast<int>(FileTransferEntryType::kFile);
    entry.name.clear();
    entry.is_hidden = (metadata.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    entry.size =
        (static_cast<uint64_t>(metadata.nFileSizeHigh) << 32U) |
        static_cast<uint64_t>(metadata.nFileSizeLow);
    entry.modified_time = FileTimeToUnixSeconds(metadata.ftLastWriteTime);
    entries->push_back(std::move(entry));
    return true;
  }

  return CollectFileTransferRecursiveFilesInternal(
      resolved_path,
      std::wstring(),
      include_hidden,
      entries,
      error_text);
}

void AppendFileTransferEntryPayload(
    const FileTransferEntryData& entry,
    std::vector<unsigned char>* out) {
  std::vector<unsigned char> payload;
  AppendVarintField(1U, static_cast<uint64_t>(entry.entry_type), &payload);
  AppendStringField(2U, WideToUtf8(entry.name), &payload);
  AppendVarintField(3U, entry.is_hidden ? 1U : 0U, &payload);
  AppendVarintField(4U, entry.size, &payload);
  AppendVarintField(5U, entry.modified_time, &payload);
  AppendLengthDelimitedField(3U, payload, out);
}

std::vector<unsigned char> EncodeFileTransferDirectoryResponseMessage(
    int id,
    const std::wstring& path,
    const std::vector<FileTransferEntryData>& entries) {
  std::vector<unsigned char> dir;
  AppendVarintField(1U, static_cast<uint64_t>(id), &dir);
  AppendStringField(2U, WideToUtf8(path), &dir);
  for (const FileTransferEntryData& entry : entries) {
    AppendFileTransferEntryPayload(entry, &dir);
  }

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(1U, dir, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferBlockResponseMessage(
    int id,
    int file_num,
    const std::vector<unsigned char>& data,
    bool compressed,
    unsigned int blk_id) {
  std::vector<unsigned char> block;
  AppendVarintField(1U, static_cast<uint64_t>(id), &block);
  AppendSint32Field(2U, file_num, &block);
  AppendBytesField(3U, data, &block);
  AppendVarintField(4U, compressed ? 1U : 0U, &block);
  AppendVarintField(5U, static_cast<uint64_t>(blk_id), &block);

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(2U, block, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferErrorResponseMessage(
    int id,
    const std::wstring& error_text,
    int file_num) {
  std::vector<unsigned char> error;
  AppendVarintField(1U, static_cast<uint64_t>(id), &error);
  AppendStringField(2U, WideToUtf8(error_text), &error);
  AppendSint32Field(3U, file_num, &error);

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(3U, error, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferDoneResponseMessage(int id, int file_num) {
  std::vector<unsigned char> done;
  AppendVarintField(1U, static_cast<uint64_t>(id), &done);
  AppendSint32Field(2U, file_num, &done);

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(4U, done, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferDigestResponseMessage(
    int id,
    int file_num,
    uint64_t last_modified,
    uint64_t file_size,
    bool is_upload,
    bool is_identical,
    uint64_t transferred_size,
    bool is_resume) {
  std::vector<unsigned char> digest;
  AppendVarintField(1U, static_cast<uint64_t>(id), &digest);
  AppendSint32Field(2U, file_num, &digest);
  AppendVarintField(3U, last_modified, &digest);
  AppendVarintField(4U, file_size, &digest);
  AppendVarintField(5U, is_upload ? 1U : 0U, &digest);
  AppendVarintField(6U, is_identical ? 1U : 0U, &digest);
  AppendVarintField(7U, transferred_size, &digest);
  AppendVarintField(8U, is_resume ? 1U : 0U, &digest);

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(5U, digest, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferEmptyDirsResponseMessage(
    const std::wstring& path) {
  std::vector<unsigned char> empty_dirs;
  AppendStringField(1U, WideToUtf8(path), &empty_dirs);

  std::vector<unsigned char> response;
  AppendLengthDelimitedField(6U, empty_dirs, &response);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(18U, response, &message);
  return message;
}

std::vector<unsigned char> EncodeFileTransferSendConfirmActionMessage(
    int id,
    int file_num,
    bool skip,
    unsigned int offset_blk) {
  std::vector<unsigned char> confirm;
  AppendVarintField(1U, static_cast<uint64_t>(id), &confirm);
  AppendSint32Field(2U, file_num, &confirm);
  if (skip) {
    AppendVarintField(3U, 1U, &confirm);
  } else {
    AppendVarintField(4U, static_cast<uint64_t>(offset_blk), &confirm);
  }

  std::vector<unsigned char> action;
  AppendLengthDelimitedField(9U, confirm, &action);

  std::vector<unsigned char> message;
  AppendLengthDelimitedField(17U, action, &message);
  return message;
}

bool ParseFileTransferEntryPayload(
    const std::vector<unsigned char>& payload,
    FileTransferEntryData* entry) {
  if (entry == nullptr) {
    return false;
  }
  *entry = FileTransferEntryData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 3U || field_number == 4U || field_number == 5U) &&
        wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      switch (field_number) {
        case 1U:
          entry->entry_type = static_cast<int>(value);
          break;
        case 3U:
          entry->is_hidden = value != 0;
          break;
        case 4U:
          entry->size = value;
          break;
        case 5U:
          entry->modified_time = value;
          break;
        default:
          break;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      entry->name = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferReadDirPayload(
    const std::vector<unsigned char>& payload,
    FileTransferReadDirData* read_dir) {
  if (read_dir == nullptr) {
    return false;
  }
  *read_dir = FileTransferReadDirData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      read_dir->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      read_dir->include_hidden = value != 0;
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferSendRequestPayload(
    const std::vector<unsigned char>& payload,
    FileTransferSendRequestData* send_request) {
  if (send_request == nullptr) {
    return false;
  }
  *send_request = FileTransferSendRequestData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 3U || field_number == 4U || field_number == 5U) &&
        wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      switch (field_number) {
        case 1U:
          send_request->id = static_cast<int>(value);
          break;
        case 3U:
          send_request->include_hidden = value != 0;
          break;
        case 4U:
          send_request->file_num = static_cast<int>(value);
          break;
        case 5U:
          send_request->file_type = static_cast<int>(value);
          break;
        default:
          break;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      send_request->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferReceiveRequestPayload(
    const std::vector<unsigned char>& payload,
    FileTransferReceiveRequestData* receive_request) {
  if (receive_request == nullptr) {
    return false;
  }
  *receive_request = FileTransferReceiveRequestData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 4U || field_number == 5U) &&
        wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 1U) {
        receive_request->id = static_cast<int>(value);
      } else if (field_number == 4U) {
        receive_request->file_num = static_cast<int>(value);
      } else {
        receive_request->total_size = value;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      receive_request->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 3U && wire_type == 2U) {
      std::vector<unsigned char> entry_payload;
      if (!ReadLengthDelimited(payload, &offset, &entry_payload)) {
        return false;
      }
      FileTransferEntryData entry;
      if (!ParseFileTransferEntryPayload(entry_payload, &entry)) {
        return false;
      }
      receive_request->files.push_back(std::move(entry));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferCreateDirPayload(
    const std::vector<unsigned char>& payload,
    FileTransferCreateDirData* create_dir) {
  if (create_dir == nullptr) {
    return false;
  }
  *create_dir = FileTransferCreateDirData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      create_dir->id = static_cast<int>(value);
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      create_dir->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferRemoveDirPayload(
    const std::vector<unsigned char>& payload,
    FileTransferRemoveDirData* remove_dir) {
  if (remove_dir == nullptr) {
    return false;
  }
  *remove_dir = FileTransferRemoveDirData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 3U) && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 1U) {
        remove_dir->id = static_cast<int>(value);
      } else {
        remove_dir->recursive = value != 0;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      remove_dir->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferRemoveFilePayload(
    const std::vector<unsigned char>& payload,
    FileTransferRemoveFileData* remove_file) {
  if (remove_file == nullptr) {
    return false;
  }
  *remove_file = FileTransferRemoveFileData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      remove_file->id = static_cast<int>(value);
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      remove_file->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 3U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      remove_file->file_num = DecodeZigZag32(value);
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferCancelPayload(
    const std::vector<unsigned char>& payload,
    FileTransferCancelData* cancel) {
  if (cancel == nullptr) {
    return false;
  }
  *cancel = FileTransferCancelData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      cancel->id = static_cast<int>(value);
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferSendConfirmPayload(
    const std::vector<unsigned char>& payload,
    FileTransferSendConfirmData* confirm) {
  if (confirm == nullptr) {
    return false;
  }
  *confirm = FileTransferSendConfirmData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 3U || field_number == 4U) &&
        wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 1U) {
        confirm->id = static_cast<int>(value);
      } else if (field_number == 3U) {
        confirm->has_skip = true;
        confirm->skip = value != 0;
      } else {
        confirm->has_offset_blk = true;
        confirm->offset_blk = static_cast<unsigned int>(value);
      }
    } else if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      confirm->file_num = DecodeZigZag32(value);
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferRenamePayload(
    const std::vector<unsigned char>& payload,
    FileTransferRenameData* rename) {
  if (rename == nullptr) {
    return false;
  }
  *rename = FileTransferRenameData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      rename->id = static_cast<int>(value);
    } else if ((field_number == 2U || field_number == 3U) && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 2U) {
        rename->path = Utf8ToWide(std::string(value.begin(), value.end()));
      } else {
        rename->new_name = Utf8ToWide(std::string(value.begin(), value.end()));
      }
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferReadAllFilesPayload(
    const std::vector<unsigned char>& payload,
    FileTransferReadAllFilesData* all_files) {
  if (all_files == nullptr) {
    return false;
  }
  *all_files = FileTransferReadAllFilesData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 3U) && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 1U) {
        all_files->id = static_cast<int>(value);
      } else {
        all_files->include_hidden = value != 0;
      }
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      all_files->path = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferActionMessage(
    const std::vector<unsigned char>& frame,
    FileTransferActionData* action) {
  if (action == nullptr) {
    return false;
  }
  *action = FileTransferActionData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 17U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subwire_type != 2U) {
        if (!SkipField(subwire_type, payload, &payload_offset)) {
          return false;
        }
        continue;
      }

      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
        return false;
      }

      switch (subfield_number) {
        case 1U:
          action->kind = FileTransferActionKind::kReadDir;
          return ParseFileTransferReadDirPayload(value, &action->read_dir);
        case 2U:
          action->kind = FileTransferActionKind::kSend;
          return ParseFileTransferSendRequestPayload(value, &action->send);
        case 3U:
          action->kind = FileTransferActionKind::kReceive;
          return ParseFileTransferReceiveRequestPayload(value, &action->receive);
        case 4U:
          action->kind = FileTransferActionKind::kCreate;
          return ParseFileTransferCreateDirPayload(value, &action->create);
        case 5U:
          action->kind = FileTransferActionKind::kRemoveDir;
          return ParseFileTransferRemoveDirPayload(value, &action->remove_dir);
        case 6U:
          action->kind = FileTransferActionKind::kRemoveFile;
          return ParseFileTransferRemoveFilePayload(value, &action->remove_file);
        case 7U:
          action->kind = FileTransferActionKind::kAllFiles;
          return ParseFileTransferReadAllFilesPayload(value, &action->all_files);
        case 8U:
          action->kind = FileTransferActionKind::kCancel;
          return ParseFileTransferCancelPayload(value, &action->cancel);
        case 9U:
          action->kind = FileTransferActionKind::kSendConfirm;
          return ParseFileTransferSendConfirmPayload(value, &action->send_confirm);
        case 10U:
          action->kind = FileTransferActionKind::kRename;
          return ParseFileTransferRenamePayload(value, &action->rename);
        case 11U:
          action->kind = FileTransferActionKind::kReadEmptyDirs;
          return ParseFileTransferReadDirPayload(value, &action->read_dir);
        default:
          break;
      }
    }
    return action->kind != FileTransferActionKind::kNone;
  }
  return false;
}

bool ParseFileTransferBlockPayload(
    const std::vector<unsigned char>& payload,
    FileTransferBlockData* block) {
  if (block == nullptr) {
    return false;
  }
  *block = FileTransferBlockData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if ((field_number == 1U || field_number == 4U || field_number == 5U) &&
        wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      if (field_number == 1U) {
        block->id = static_cast<int>(value);
      } else if (field_number == 4U) {
        block->compressed = value != 0;
      } else {
        block->blk_id = static_cast<unsigned int>(value);
      }
    } else if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      block->file_num = DecodeZigZag32(value);
    } else if (field_number == 3U && wire_type == 2U) {
      if (!ReadLengthDelimited(payload, &offset, &block->data)) {
        return false;
      }
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferDonePayload(
    const std::vector<unsigned char>& payload,
    FileTransferDoneData* done) {
  if (done == nullptr) {
    return false;
  }
  *done = FileTransferDoneData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      done->id = static_cast<int>(value);
    } else if (field_number == 2U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      done->file_num = DecodeZigZag32(value);
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferDigestPayload(
    const std::vector<unsigned char>& payload,
    FileTransferDigestData* digest) {
  if (digest == nullptr) {
    return false;
  }
  *digest = FileTransferDigestData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      switch (field_number) {
        case 1U:
          digest->id = static_cast<int>(value);
          break;
        case 2U:
          digest->file_num = DecodeZigZag32(value);
          break;
        case 3U:
          digest->last_modified = value;
          break;
        case 4U:
          digest->file_size = value;
          break;
        case 5U:
          digest->is_upload = value != 0;
          break;
        case 6U:
          digest->is_identical = value != 0;
          break;
        case 7U:
          digest->transferred_size = value;
          break;
        case 8U:
          digest->is_resume = value != 0;
          break;
        default:
          break;
      }
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferErrorPayload(
    const std::vector<unsigned char>& payload,
    FileTransferErrorData* error) {
  if (error == nullptr) {
    return false;
  }
  *error = FileTransferErrorData();
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(payload, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == 1U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      error->id = static_cast<int>(value);
    } else if (field_number == 2U && wire_type == 2U) {
      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &offset, &value)) {
        return false;
      }
      error->error = Utf8ToWide(std::string(value.begin(), value.end()));
    } else if (field_number == 3U && wire_type == 0U) {
      uint64_t value = 0;
      if (!ReadVarint(payload, &offset, &value)) {
        return false;
      }
      error->file_num = DecodeZigZag32(value);
    } else if (!SkipField(wire_type, payload, &offset)) {
      return false;
    }
  }
  return true;
}

bool ParseFileTransferResponseMessage(
    const std::vector<unsigned char>& frame,
    FileTransferResponseData* response) {
  if (response == nullptr) {
    return false;
  }
  *response = FileTransferResponseData();
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number != 18U || wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return false;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return false;
    }

    size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
      uint64_t subtag = 0;
      if (!ReadVarint(payload, &payload_offset, &subtag)) {
        return false;
      }
      const unsigned int subfield_number = static_cast<unsigned int>(subtag >> 3U);
      const unsigned int subwire_type = static_cast<unsigned int>(subtag & 0x07U);
      if (subwire_type != 2U) {
        if (!SkipField(subwire_type, payload, &payload_offset)) {
          return false;
        }
        continue;
      }

      std::vector<unsigned char> value;
      if (!ReadLengthDelimited(payload, &payload_offset, &value)) {
        return false;
      }

      switch (subfield_number) {
        case 1U: {
          response->kind = FileTransferResponseKind::kDir;
          size_t dir_offset = 0;
          while (dir_offset < value.size()) {
            uint64_t dir_tag = 0;
            if (!ReadVarint(value, &dir_offset, &dir_tag)) {
              return false;
            }
            const unsigned int dir_field_number = static_cast<unsigned int>(dir_tag >> 3U);
            const unsigned int dir_wire_type = static_cast<unsigned int>(dir_tag & 0x07U);
            if (dir_field_number == 1U && dir_wire_type == 0U) {
              uint64_t dir_id = 0;
              if (!ReadVarint(value, &dir_offset, &dir_id)) {
                return false;
              }
              response->dir_id = static_cast<int>(dir_id);
            } else if (dir_field_number == 2U && dir_wire_type == 2U) {
              std::vector<unsigned char> path_bytes;
              if (!ReadLengthDelimited(value, &dir_offset, &path_bytes)) {
                return false;
              }
              response->dir_path = Utf8ToWide(std::string(path_bytes.begin(), path_bytes.end()));
            } else if (dir_field_number == 3U && dir_wire_type == 2U) {
              std::vector<unsigned char> entry_payload;
              if (!ReadLengthDelimited(value, &dir_offset, &entry_payload)) {
                return false;
              }
              FileTransferEntryData entry;
              if (!ParseFileTransferEntryPayload(entry_payload, &entry)) {
                return false;
              }
              response->dir_entries.push_back(std::move(entry));
            } else if (!SkipField(dir_wire_type, value, &dir_offset)) {
              return false;
            }
          }
          return true;
        }
        case 2U:
          response->kind = FileTransferResponseKind::kBlock;
          return ParseFileTransferBlockPayload(value, &response->block);
        case 3U:
          response->kind = FileTransferResponseKind::kError;
          return ParseFileTransferErrorPayload(value, &response->error);
        case 4U:
          response->kind = FileTransferResponseKind::kDone;
          return ParseFileTransferDonePayload(value, &response->done);
        case 5U:
          response->kind = FileTransferResponseKind::kDigest;
          return ParseFileTransferDigestPayload(value, &response->digest);
        case 6U: {
          response->kind = FileTransferResponseKind::kEmptyDirs;
          FileTransferReadDirData parsed;
          if (!ParseFileTransferReadDirPayload(value, &parsed)) {
            return false;
          }
          response->empty_dirs_path = parsed.path;
          return true;
        }
        default:
          break;
      }
    }
    return response->kind != FileTransferResponseKind::kNone;
  }
  return false;
}

bool OpenFileTransferReadHandle(FileTransferReadJob* job, std::wstring* error_text) {
  if (job == nullptr) {
    return false;
  }
  if (job->file != INVALID_HANDLE_VALUE) {
    return true;
  }
  if (job->file_num < 0 || job->file_num >= static_cast<int>(job->files.size())) {
    if (error_text != nullptr) {
      *error_text = L"file transfer read index out of range";
    }
    return false;
  }

  const FileTransferEntryData& entry = job->files[job->file_num];
  const std::wstring path =
      entry.name.empty() ? job->source_path : JoinWindowsPath(job->source_path, entry.name);
  job->file = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (job->file == INVALID_HANDLE_VALUE) {
    if (error_text != nullptr) {
      *error_text = L"CreateFileW failed while opening file transfer source";
    }
    return false;
  }

  if (job->resume_offset > 0) {
    LARGE_INTEGER position = {};
    position.QuadPart = static_cast<LONGLONG>(job->resume_offset);
    if (!SetFilePointerEx(job->file, position, nullptr, FILE_BEGIN)) {
      if (error_text != nullptr) {
        *error_text = L"SetFilePointerEx failed while resuming file transfer";
      }
      CloseFileTransferHandle(&job->file);
      return false;
    }
  }
  return true;
}

void ResetFileTransferReadJobCurrentFile(FileTransferReadJob* job) {
  if (job == nullptr) {
    return;
  }
  CloseFileTransferHandle(&job->file);
  job->sent_digest = false;
  job->waiting_for_confirm = false;
  job->file_confirmed = false;
  job->resume_offset = 0;
}

void AdvanceFileTransferReadJob(FileTransferReadJob* job) {
  if (job == nullptr) {
    return;
  }
  ResetFileTransferReadJobCurrentFile(job);
  ++job->file_num;
}

bool IsFileTransferReadJobComplete(const FileTransferReadJob& job) {
  return job.file_num >= static_cast<int>(job.files.size());
}

void DiscardFileTransferWriteCurrentFile(FileTransferWriteJob* job) {
  if (job == nullptr) {
    return;
  }
  CloseFileTransferHandle(&job->file);
  if (!job->current_temp_path.empty()) {
    DeleteFileW(job->current_temp_path.c_str());
  }
  job->current_file_num = -1;
  job->current_temp_path.clear();
  job->current_final_path.clear();
}

bool FinalizeFileTransferWriteCurrentFile(
    FileTransferWriteJob* job,
    std::wstring* error_text) {
  if (job == nullptr) {
    return false;
  }
  if (job->current_file_num < 0) {
    return true;
  }

  CloseFileTransferHandle(&job->file);
  const std::wstring temp_path = job->current_temp_path;
  const std::wstring final_path = job->current_final_path;
  const int file_index = job->current_file_num;

  job->current_file_num = -1;
  job->current_temp_path.clear();
  job->current_final_path.clear();

  if (temp_path.empty() || final_path.empty()) {
    return true;
  }

  if (!MoveFileExW(
          temp_path.c_str(),
          final_path.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    if (error_text != nullptr) {
      *error_text = L"MoveFileExW failed while finalizing file transfer target";
    }
    return false;
  }
  if (file_index >= 0 && file_index < static_cast<int>(job->files.size())) {
    ApplyFileTransferModifiedTime(final_path, job->files[file_index].modified_time);
  }
  return true;
}

bool PrepareFileTransferWriteTarget(
    FileTransferWriteJob* job,
    int file_num,
    std::wstring* error_text) {
  if (job == nullptr) {
    return false;
  }
  if (file_num < 0 || file_num >= static_cast<int>(job->files.size())) {
    if (error_text != nullptr) {
      *error_text = L"file transfer write index out of range";
    }
    return false;
  }
  if (job->current_file_num == file_num && job->file != INVALID_HANDLE_VALUE) {
    return true;
  }
  if (job->current_file_num != -1 && job->current_file_num != file_num) {
    if (!FinalizeFileTransferWriteCurrentFile(job, error_text)) {
      return false;
    }
  }

  const FileTransferEntryData& entry = job->files[file_num];
  std::wstring final_path;
  if (!ResolveFileTransferTargetPath(
          job->target_path,
          entry.name,
          job->files.size() == 1 && entry.name.empty(),
          &final_path,
          error_text)) {
    return false;
  }
  const std::wstring parent_directory = GetDirectoryPathPart(final_path);
  if (!parent_directory.empty() &&
      !EnsureDirectoryExistsRecursive(parent_directory, error_text)) {
    return false;
  }

  const std::wstring temp_path = final_path + L".download";
  DeleteFileW(temp_path.c_str());
  job->file = CreateFileW(
      temp_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (job->file == INVALID_HANDLE_VALUE) {
    if (error_text != nullptr) {
      *error_text = L"CreateFileW failed while opening file transfer target";
    }
    return false;
  }

  job->current_file_num = file_num;
  job->current_temp_path = temp_path;
  job->current_final_path = final_path;
  return true;
}

bool FrameContainsMessageField(const std::vector<unsigned char>& frame, unsigned int wanted_field) {
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return false;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (field_number == wanted_field) {
      return true;
    }
    if (!SkipField(wire_type, frame, &offset)) {
      return false;
    }
  }
  return false;
}

std::vector<unsigned int> ExtractTopLevelMessageFields(const std::vector<unsigned char>& frame) {
  std::vector<unsigned int> fields;
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      break;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    fields.push_back(field_number);
    if (!SkipField(wire_type, frame, &offset)) {
      break;
    }
  }
  return fields;
}

ParsedServerFrame ParseServerFrame(const std::vector<unsigned char>& frame) {
  ParsedServerFrame parsed;
  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t tag = 0;
    if (!ReadVarint(frame, &offset, &tag)) {
      return parsed;
    }
    const unsigned int field_number = static_cast<unsigned int>(tag >> 3U);
    const unsigned int wire_type = static_cast<unsigned int>(tag & 0x07U);
    if (wire_type != 2U) {
      if (!SkipField(wire_type, frame, &offset)) {
        return parsed;
      }
      continue;
    }

    std::vector<unsigned char> payload;
    if (!ReadLengthDelimited(frame, &offset, &payload)) {
      return parsed;
    }
    parsed.observed_fields.push_back(field_number);

    if (field_number == 16U) {
      parsed.has_register_pk_response =
          ParseRegisterPkResponse(payload, &parsed.register_pk_response);
    } else if (field_number == 7U) {
      parsed.has_register_peer_response =
          ParseRegisterPeerResponse(payload, &parsed.register_peer_response);
    } else if (field_number == 25U) {
      parsed.has_key_exchange = ParseKeyExchange(payload, &parsed.key_exchange_keys);
    } else if (field_number == 18U) {
      parsed.has_request_relay = ParseRequestRelay(payload, &parsed.request_relay);
    } else if (field_number == 9U) {
      parsed.has_punch_hole = ParsePunchHole(payload, &parsed.punch_hole);
    } else if (field_number == 12U) {
      parsed.has_fetch_local_addr = ParseFetchLocalAddr(payload, &parsed.fetch_local_addr);
    }
  }
  return parsed;
}

std::wstring FormatTcpStatus(const HostConfig& config, ServerState state) {
  const ParsedHostPort parsed = ParseHostPort(config.id_server, kDefaultIdServerPort);
  const std::wstring endpoint = parsed.host.empty()
      ? config.id_server
      : BuildDisplayEndpoint(parsed.host, parsed.port);
  std::wstringstream stream;
  switch (state) {
    case ServerState::kReachable:
      stream << L"Port probe reachable (tcp): " << endpoint;
      break;
    case ServerState::kUnreachable:
      stream << L"Port probe unreachable (tcp): " << endpoint;
      break;
    case ServerState::kUnknown:
    default:
      stream << L"Port probe not started";
      break;
  }
  return stream.str();
}

std::wstring GenerateFallbackDesktopId() {
  unsigned int high = 0;
  unsigned int low = 0;
  if (!TryFillRandomBytes(&high, sizeof(high))) {
    high = static_cast<unsigned int>(std::rand());
  }
  if (!TryFillRandomBytes(&low, sizeof(low))) {
    low = static_cast<unsigned int>(std::rand());
  }

  const unsigned long long combined =
      (static_cast<unsigned long long>(high) << 32ULL) | low;
  const unsigned long long value =
      1000000000ULL + (combined % 1000000000ULL);
  return std::to_wstring(value);
}

unsigned int GetRandomUint32() {
  unsigned int value = 0;
  if (TryFillRandomBytes(&value, sizeof(value))) {
    return value;
  }
  return static_cast<unsigned int>(std::rand());
}

bool EnsureSodiumInitialized(std::wstring* error_text) {
  const bool available = sodium_init() >= 0;
  if (!available && error_text != nullptr) {
    *error_text = L"sodium_init failed";
  }
  return available;
}

bool VerifySignedServerKey(
    const std::vector<unsigned char>& signed_message,
    const std::vector<unsigned char>& rendezvous_sign_public_key,
    std::vector<unsigned char>* message,
    std::wstring* error_text) {
  if (rendezvous_sign_public_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) {
    if (error_text != nullptr) {
      *error_text = L"invalid rendezvous public key length";
    }
    return false;
  }

  message->resize(signed_message.size());
  unsigned long long message_length = 0;
  if (crypto_sign_ed25519_open(
          message->data(),
          &message_length,
          signed_message.data(),
          static_cast<unsigned long long>(signed_message.size()),
          rendezvous_sign_public_key.data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"signature mismatch in key exchange";
    }
    message->clear();
    return false;
  }

  message->resize(static_cast<size_t>(message_length));
  return true;
}

bool CreateSymmetricKeyResponse(
    const std::vector<unsigned char>& their_curve_public_key,
    std::vector<unsigned char>* our_curve_public_key,
    std::vector<unsigned char>* sealed_key,
    std::array<unsigned char, crypto_secretbox_KEYBYTES>* symmetric_key,
    std::wstring* error_text) {
  if (their_curve_public_key.size() != crypto_box_PUBLICKEYBYTES) {
    if (error_text != nullptr) {
      *error_text = L"unexpected curve25519 public key length";
    }
    return false;
  }

  std::array<unsigned char, crypto_box_PUBLICKEYBYTES> our_public = {};
  std::array<unsigned char, crypto_box_SECRETKEYBYTES> our_secret = {};
  std::array<unsigned char, crypto_box_NONCEBYTES> zero_nonce = {};
  std::array<unsigned char, crypto_box_PUBLICKEYBYTES> their_public = {};
  std::memcpy(their_public.data(), their_curve_public_key.data(), their_public.size());

  if (crypto_box_keypair(our_public.data(), our_secret.data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"curve25519 keypair generation failed";
    }
    return false;
  }
  randombytes_buf(symmetric_key->data(), symmetric_key->size());

  sealed_key->resize(crypto_secretbox_KEYBYTES + crypto_box_MACBYTES);
  if (crypto_box_easy(
          sealed_key->data(),
          symmetric_key->data(),
          static_cast<unsigned long long>(symmetric_key->size()),
          zero_nonce.data(),
          their_public.data(),
          our_secret.data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"crypto_box_easy failed";
    }
    return false;
  }

  our_curve_public_key->assign(our_public.begin(), our_public.end());
  return true;
}

std::wstring RegisterPkResultText(int result) {
  switch (result) {
    case 0:
      return L"OK";
    case 2:
      return L"UUID_MISMATCH";
    case 3:
      return L"ID_EXISTS";
    case 4:
      return L"TOO_FREQUENT";
    case 5:
      return L"INVALID_ID_FORMAT";
    case 6:
      return L"NOT_SUPPORT";
    case 7:
      return L"SERVER_ERROR";
    case 8:
      return L"NOT_DEPLOYED";
    default:
      return L"UNKNOWN";
  }
}

std::wstring BuildLaunchOnStartupCommand(
    const std::wstring& executable_path,
    bool start_hidden_to_tray) {
  if (executable_path.empty()) {
    return std::wstring();
  }

  std::wstring command = L"\"" + executable_path + L"\"";
  if (start_hidden_to_tray) {
    command += L" ";
    command += PortableHostStartupTrayArgument();
  }
  return command;
}

}  // namespace

PortableHostApp::PortableHostApp() {
  ResetPortableHostLog();
  AppendPortableHostLog(L"app", L"========== PortableHostApp start ==========");
}

PortableHostApp::~PortableHostApp() {
  AppendPortableHostLog(L"app", L"========== PortableHostApp shutdown ==========");
  StopRendezvousWorker();
  DestroyIncomingApprovalWindow();
  RemoveTrayIcon();
  if (tray_menu_ != nullptr) {
    DestroyMenu(tray_menu_);
    tray_menu_ = nullptr;
  }
  DestroyIcons();
  DestroyFonts();
  if (logo_bitmap_ != nullptr) {
    DeleteObject(logo_bitmap_);
    logo_bitmap_ = nullptr;
  }
  if (options_icon_bitmap_ != nullptr) {
    DeleteObject(options_icon_bitmap_);
    options_icon_bitmap_ = nullptr;
  }
  if (refresh_icon_bitmap_ != nullptr) {
    DeleteObject(refresh_icon_bitmap_);
    refresh_icon_bitmap_ = nullptr;
  }
  if (ole_ready_) {
    OleUninitialize();
  }
  if (gdiplus_ready_) {
    GdiplusShutdown(gdiplus_token_);
    gdiplus_ready_ = false;
    gdiplus_token_ = 0;
  }
  if (card_brush_ != nullptr) {
    DeleteObject(card_brush_);
  }
  if (button_brush_ != nullptr) {
    DeleteObject(button_brush_);
  }
  if (panel_brush_ != nullptr) {
    DeleteObject(panel_brush_);
  }
  if (dark_brush_ != nullptr) {
    DeleteObject(dark_brush_);
  }
  if (winsock_ready_) {
    WSACleanup();
  }
}

bool PortableHostApp::Initialize(HINSTANCE instance, bool start_hidden_on_launch) {
  instance_ = instance;
  start_hidden_on_launch_ = start_hidden_on_launch;
  EnableBestEffortDpiAwareness();
  taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

  GdiplusStartupInput gdiplus_startup;
  gdiplus_ready_ = GdiplusStartup(&gdiplus_token_, &gdiplus_startup, nullptr) == Ok;

  const HRESULT ole_result = OleInitialize(nullptr);
  ole_ready_ = ole_result == S_OK || ole_result == S_FALSE;

  INITCOMMONCONTROLSEX common_controls = {};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&common_controls);

  WSADATA wsa_data = {};
  winsock_ready_ = WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;

  dark_brush_ = CreateSolidBrush(kWindowColor);
  panel_brush_ = CreateSolidBrush(kPanelColor);
  card_brush_ = CreateSolidBrush(kCardColor);
  button_brush_ = CreateSolidBrush(kAccentColor);

  LoadOrCreateConfig();
  if (IsLaunchOnStartupEnabled()) {
    SetLaunchOnStartupEnabled(true);
  }
  AppendPortableHostLog(
      L"app",
      L"Initialize config: exe_dir=" + config_.exe_dir +
          L", config_path=" + config_.config_path +
          L", id_server=" + config_.id_server +
          L", relay_server=" + config_.relay_server +
          L", direct_access_enabled=" + BoolToLogText(config_.direct_access_enabled) +
          L", direct_access_port=" + std::to_wstring(config_.direct_access_port));
  RefreshPassword();
  RefreshServerState();
  StartRendezvousWorker();

  if (!CreateMainWindow()) {
    return false;
  }

  RefreshUiText();
  return true;
}

int PortableHostApp::Run() {
  if (start_hidden_on_launch_) {
    HideMainWindowToTray();
  } else {
    ShowWindow(window_, SW_SHOWDEFAULT);
    UpdateWindow(window_);
  }

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

LRESULT CALLBACK PortableHostApp::WindowProcStatic(
    HWND hwnd,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
  auto* app = reinterpret_cast<PortableHostApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
    app = reinterpret_cast<PortableHostApp*>(create_struct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    app->window_ = hwnd;
  }

  if (app != nullptr) {
    return app->WindowProc(hwnd, message, w_param, l_param);
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT PortableHostApp::WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
  if (taskbar_created_message_ != 0 && message == taskbar_created_message_) {
    tray_icon_added_ = false;
    AddTrayIcon();
    return 0;
  }

  switch (message) {
    case WM_SIZE: {
      LayoutControls(LOWORD(l_param), HIWORD(l_param));
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_COMMAND: {
      const UINT control_id = LOWORD(w_param);
      if (control_id == kTrayMenuShowWindow) {
        ShowMainWindow();
        return 0;
      }
      if (control_id == kTrayMenuExit) {
        DestroyWindow(hwnd);
        return 0;
      }
      if (control_id == kOptionsButton) {
        ShowOptionsMenu();
        return 0;
      }
      if (control_id == kRefreshPasswordButton) {
        RefreshPassword();
        RefreshUiText();
        return 0;
      }
      if (control_id == kDisconnectButton) {
        StopActiveSession(true);
        RefreshUiText();
        return 0;
      }
      if (control_id == kOptionsMenuLaunchOnStartup) {
        ToggleLaunchOnStartup();
        return 0;
      }
      if (control_id == kOptionsMenuSetFixedPassword) {
        ConfigureFixedPassword();
        return 0;
      }
      if (control_id == kOptionsMenuDisableRandomPassword) {
        ToggleRandomPassword();
        return 0;
      }
      if (control_id == kOptionsMenuChangeId) {
        ConfigureHostId();
        return 0;
      }
      if (control_id == kOptionsMenuLanguage) {
        ConfigureLanguage();
        return 0;
      }
      if (control_id == kOptionsMenuAbout) {
        ShowAboutDialog();
        return 0;
      }
      break;
    }
    case WM_TIMER: {
      if (w_param == kUiRefreshTimerId) {
        ++ui_refresh_tick_;
        if ((ui_refresh_tick_ % 15U) == 0U) {
          RefreshServerState();
        }
        RefreshUiText();
        return 0;
      }
      break;
    }
    case kAppRendezvousStatus: {
      RefreshUiText();
      return 0;
    }
    case kAppIncomingApprovalUpdated: {
      UpdateIncomingApprovalUi();
      return 0;
    }
    case kAppInstallRemoteFileClipboard: {
      std::unique_ptr<InstallRemoteFileClipboardRequest> request(
          reinterpret_cast<InstallRemoteFileClipboardRequest*>(l_param));
      if (!ole_ready_ || request == nullptr || request->bridge == nullptr) {
        return 0;
      }
      auto* data_object = new (std::nothrow) VirtualFileDataObject(request->bridge);
      if (data_object == nullptr) {
        SetRendezvousStatus(
            L"failed to allocate virtual remote file clipboard object",
            IsRendezvousRegistered());
        return 0;
      }
      const HRESULT result = OleSetClipboard(data_object);
      data_object->Release();
      if (FAILED(result)) {
        SetRendezvousStatus(
            L"failed to install virtual remote file clipboard: " + HResultToText(result),
            IsRendezvousRegistered());
      } else {
        SetRendezvousStatus(
            L"remote file clipboard ready; files will transfer on paste",
            IsRendezvousRegistered());
      }
      return 0;
    }
    case kAppTrayIcon: {
      switch (static_cast<UINT>(l_param)) {
        case WM_LBUTTONDBLCLK:
          ShowMainWindow();
          return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
          ShowTrayMenu();
          return 0;
        default:
          break;
      }
      break;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint = {};
      HDC dc = BeginPaint(hwnd, &paint);
      RECT client_rect = {};
      GetClientRect(hwnd, &client_rect);
      FillRect(
          dc,
          &client_rect,
          dark_brush_ != nullptr ? dark_brush_ : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
      DrawConnectionStatusCard(dc);

      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(w_param);
      SetBkMode(dc, TRANSPARENT);
      HWND control = reinterpret_cast<HWND>(l_param);
      if (control == id_value_ || control == password_value_) {
        SetBkColor(dc, kPanelColor);
        SetTextColor(dc, kTextColor);
        return reinterpret_cast<INT_PTR>(panel_brush_);
      }
      if (control == id_accent_ || control == password_accent_) {
        SetBkColor(dc, kAccentColor);
        return reinterpret_cast<INT_PTR>(button_brush_);
      }
      if (control == server_status_label_) {
        SetBkColor(dc, kWindowColor);
        if (IsRendezvousRegistered()) {
          SetTextColor(dc, kGoodColor);
        } else if (server_state_ == ServerState::kReachable) {
          SetTextColor(dc, RGB(120, 200, 255));
        } else {
          SetTextColor(dc, kBadColor);
        }
        return reinterpret_cast<INT_PTR>(dark_brush_);
      }
      if (control == server_value_label_) {
        SetBkColor(dc, kWindowColor);
        SetTextColor(dc, kTextColor);
        return reinterpret_cast<INT_PTR>(dark_brush_);
      } else if (control == subtitle_label_ ||
                 control == hint_label_ ||
                 control == id_label_ ||
                 control == password_label_ ||
                 control == config_path_label_) {
        SetTextColor(dc, kMutedTextColor);
      } else {
        SetTextColor(dc, kTextColor);
      }
      return reinterpret_cast<INT_PTR>(dark_brush_);
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(w_param);
      SetBkColor(dc, kPanelColor);
      SetTextColor(dc, kTextColor);
      return reinterpret_cast<INT_PTR>(panel_brush_);
    }
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(w_param);
      HWND control = reinterpret_cast<HWND>(l_param);
      SetTextColor(dc, kTextColor);
      if (control == options_button_ || control == refresh_password_button_) {
        SetBkColor(dc, kWindowColor);
        return reinterpret_cast<INT_PTR>(dark_brush_);
      }
      if (control == incoming_approval_dismiss_button_) {
        SetBkColor(dc, kSecondaryButtonColor);
        return reinterpret_cast<INT_PTR>(card_brush_ != nullptr ? card_brush_ : dark_brush_);
      }
      if (control == incoming_approval_accept_button_) {
        SetBkColor(dc, kAccentColor);
        return reinterpret_cast<INT_PTR>(button_brush_);
      }
      SetBkColor(dc, kAccentColor);
      return reinterpret_cast<INT_PTR>(button_brush_);
    }
    case WM_DRAWITEM:
      if (DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(l_param))) {
        return TRUE;
      }
      break;
    case WM_CLOSE:
      HideMainWindowToTray();
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kUiRefreshTimerId);
      DestroyIncomingApprovalWindow();
      RemoveTrayIcon();
      if (tray_menu_ != nullptr) {
        DestroyMenu(tray_menu_);
        tray_menu_ = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

bool PortableHostApp::CreateMainWindow() {
  CreateFonts();

  window_icon_large_ = static_cast<HICON>(LoadImageW(
      instance_,
      MAKEINTRESOURCEW(IDI_APP_ICON),
      IMAGE_ICON,
      GetSystemMetrics(SM_CXICON),
      GetSystemMetrics(SM_CYICON),
      0));
  if (window_icon_large_ == nullptr) {
    window_icon_large_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
  }

  window_icon_small_ = static_cast<HICON>(LoadImageW(
      instance_,
      MAKEINTRESOURCEW(IDI_APP_ICON),
      IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON),
      GetSystemMetrics(SM_CYSMICON),
      0));
  if (window_icon_small_ == nullptr) {
    window_icon_small_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
  }

  tray_icon_ = static_cast<HICON>(LoadImageW(
      instance_,
      MAKEINTRESOURCEW(IDI_TRAY_ICON),
      IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON),
      GetSystemMetrics(SM_CYSMICON),
      0));
  if (tray_icon_ == nullptr) {
    tray_icon_ = static_cast<HICON>(LoadImageW(
        instance_,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        0));
  }
  if (tray_icon_ == nullptr) {
    tray_icon_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
  }

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &PortableHostApp::WindowProcStatic;
  window_class.hInstance = instance_;
  window_class.hIcon = window_icon_large_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = dark_brush_;
  window_class.lpszClassName = kAppWindowClassName;
  window_class.hIconSm = window_icon_small_;

  if (RegisterClassExW(&window_class) == 0) {
    return false;
  }

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT rect = {0, 0, ScaleForSystemDpi(kWindowWidth), ScaleForSystemDpi(kWindowHeight)};
  AdjustWindowRect(&rect, style, FALSE);

  window_ = CreateWindowExW(
      0,
      window_class.lpszClassName,
      GetText(L"app_window_title", kAppWindowTitle).c_str(),
      style,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      rect.right - rect.left,
      rect.bottom - rect.top,
      nullptr,
      nullptr,
      instance_,
      this);

  if (window_ == nullptr) {
    return false;
  }

  SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(window_icon_large_));
  SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(window_icon_small_));

  tray_menu_ = CreatePopupMenu();
  if (tray_menu_ != nullptr) {
    AppendMenuW(
        tray_menu_,
        MF_STRING,
        kTrayMenuShowWindow,
        GetText(L"tray_show_main", L"\u986f\u793a\u4e3b\u9801").c_str());
    AppendMenuW(tray_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        tray_menu_,
        MF_STRING,
        kTrayMenuExit,
        GetText(L"tray_exit", L"\u96e2\u958b").c_str());
  }
  AddTrayIcon();

  CreateControls();
  ApplyFonts();
  SetTimer(window_, kUiRefreshTimerId, 1000, nullptr);

  RECT client_rect = {};
  GetClientRect(window_, &client_rect);
  LayoutControls(client_rect.right - client_rect.left, client_rect.bottom - client_rect.top);
  return true;
}

void PortableHostApp::DestroyIcons() {
  if (tray_icon_ != nullptr) {
    DestroyIcon(tray_icon_);
    tray_icon_ = nullptr;
  }
  if (window_icon_small_ != nullptr) {
    DestroyIcon(window_icon_small_);
    window_icon_small_ = nullptr;
  }
  if (window_icon_large_ != nullptr) {
    DestroyIcon(window_icon_large_);
    window_icon_large_ = nullptr;
  }
}

bool PortableHostApp::AddTrayIcon() {
  if (window_ == nullptr) {
    return false;
  }

  NOTIFYICONDATAW notify_data = {};
  notify_data.cbSize = sizeof(notify_data);
  notify_data.hWnd = window_;
  notify_data.uID = kTrayIconId;
  notify_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  notify_data.uCallbackMessage = kAppTrayIcon;
  notify_data.hIcon = tray_icon_ != nullptr ? tray_icon_ : window_icon_small_;
  const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
  lstrcpynW(notify_data.szTip, app_title.c_str(), ARRAYSIZE(notify_data.szTip));

  if (tray_icon_added_) {
    if (Shell_NotifyIconW(NIM_MODIFY, &notify_data)) {
      return true;
    }
    tray_icon_added_ = false;
  }

  if (!Shell_NotifyIconW(NIM_ADD, &notify_data)) {
    return false;
  }

  tray_icon_added_ = true;
  notify_data.uVersion = NOTIFYICON_VERSION;
  Shell_NotifyIconW(NIM_SETVERSION, &notify_data);
  return true;
}

void PortableHostApp::RemoveTrayIcon() {
  if (!tray_icon_added_ || window_ == nullptr) {
    return;
  }

  NOTIFYICONDATAW notify_data = {};
  notify_data.cbSize = sizeof(notify_data);
  notify_data.hWnd = window_;
  notify_data.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &notify_data);
  tray_icon_added_ = false;
}

void PortableHostApp::ShowMainWindow() {
  if (window_ == nullptr) {
    return;
  }

  ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(window_);
}

void PortableHostApp::HideMainWindowToTray() {
  if (window_ == nullptr) {
    return;
  }

  if (!tray_icon_added_) {
    AddTrayIcon();
  }
  ShowWindow(window_, SW_HIDE);
}

void PortableHostApp::ShowTrayMenu() {
  if (window_ == nullptr || tray_menu_ == nullptr) {
    return;
  }

  POINT cursor_position = {};
  GetCursorPos(&cursor_position);
  SetForegroundWindow(window_);
  TrackPopupMenu(
      tray_menu_,
      TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
      cursor_position.x,
      cursor_position.y,
      0,
      window_,
      nullptr);
  PostMessageW(window_, WM_NULL, 0, 0);
}

unsigned long PortableHostApp::BeginIncomingApproval(
    const std::wstring& remote_id,
    const std::wstring& remote_name) {
  unsigned long token = 0;
  {
    Win32LockGuard guard(incoming_approval_mutex_);
    token = ++incoming_approval_token_seed_;
    incoming_approval_token_ = token;
    incoming_approval_decision_ = IncomingApprovalDecision::kPending;
    incoming_approval_remote_id_ = remote_id;
    incoming_approval_remote_name_ = remote_name;
  }
  if (window_ != nullptr) {
    PostMessageW(window_, kAppIncomingApprovalUpdated, 0, 0);
  }
  return token;
}

PortableHostApp::IncomingApprovalDecision PortableHostApp::GetIncomingApprovalDecision(
    unsigned long token) const {
  Win32LockGuard guard(incoming_approval_mutex_);
  if (token != 0 && token == incoming_approval_token_) {
    return incoming_approval_decision_;
  }
  return IncomingApprovalDecision::kPending;
}

void PortableHostApp::ResolveIncomingApproval(IncomingApprovalDecision decision) {
  {
    Win32LockGuard guard(incoming_approval_mutex_);
    if (incoming_approval_token_ == 0 ||
        incoming_approval_decision_ != IncomingApprovalDecision::kPending) {
      return;
    }
    incoming_approval_decision_ = decision;
  }
  if (window_ != nullptr) {
    PostMessageW(window_, kAppIncomingApprovalUpdated, 0, 0);
  }
}

void PortableHostApp::CompleteIncomingApproval(unsigned long token) {
  bool changed = false;
  {
    Win32LockGuard guard(incoming_approval_mutex_);
    if (token != 0 && token == incoming_approval_token_) {
      incoming_approval_token_ = 0;
      incoming_approval_decision_ = IncomingApprovalDecision::kPending;
      incoming_approval_remote_id_.clear();
      incoming_approval_remote_name_.clear();
      changed = true;
    }
  }
  if (changed && window_ != nullptr) {
    PostMessageW(window_, kAppIncomingApprovalUpdated, 0, 0);
  }
}

void PortableHostApp::ResetIncomingApproval() {
  {
    Win32LockGuard guard(incoming_approval_mutex_);
    incoming_approval_token_ = 0;
    incoming_approval_decision_ = IncomingApprovalDecision::kPending;
    incoming_approval_remote_id_.clear();
    incoming_approval_remote_name_.clear();
  }
  if (window_ != nullptr) {
    PostMessageW(window_, kAppIncomingApprovalUpdated, 0, 0);
  }
}

void PortableHostApp::UpdateIncomingApprovalUi() {
  unsigned long token = 0;
  IncomingApprovalDecision decision = IncomingApprovalDecision::kPending;
  {
    Win32LockGuard guard(incoming_approval_mutex_);
    token = incoming_approval_token_;
    decision = incoming_approval_decision_;
  }

  if (token == 0 || decision != IncomingApprovalDecision::kPending) {
    if (incoming_approval_window_ != nullptr) {
      ShowWindow(incoming_approval_window_, SW_HIDE);
    }
    return;
  }

  if (!EnsureIncomingApprovalWindow()) {
    return;
  }

  RECT work_area = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const int width = ScaleForSystemDpi(408);
  const int height = ScaleForSystemDpi(244);
  const int x = work_area.left + ((work_area.right - work_area.left - width) / 2);
  const int y = work_area.top + ((work_area.bottom - work_area.top - height) / 2);
  SetWindowPos(
      incoming_approval_window_,
      HWND_TOPMOST,
      x,
      y,
      width,
      height,
      SWP_SHOWWINDOW);
  RECT client_rect = {};
  GetClientRect(incoming_approval_window_, &client_rect);
  LayoutIncomingApprovalWindow(
      client_rect.right - client_rect.left,
      client_rect.bottom - client_rect.top);
  InvalidateRect(incoming_approval_window_, nullptr, TRUE);
  ShowWindow(incoming_approval_window_, SW_SHOWNORMAL);
  SetForegroundWindow(incoming_approval_window_);
}

bool PortableHostApp::EnsureIncomingApprovalWindow() {
  if (incoming_approval_window_ != nullptr) {
    return true;
  }

  const wchar_t kApprovalClassName[] = L"RustDeskCppIncomingApprovalWindow";
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &PortableHostApp::IncomingApprovalWindowProcStatic;
  window_class.hInstance = instance_;
  window_class.hIcon = window_icon_small_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = dark_brush_;
  window_class.lpszClassName = kApprovalClassName;
  window_class.hIconSm = window_icon_small_;
  if (RegisterClassExW(&window_class) == 0) {
    const DWORD register_error = GetLastError();
    if (register_error != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
  }

  incoming_approval_window_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
      kApprovalClassName,
      GetText(L"incoming_window_title", L"\u9060\u7aef\u9023\u5165").c_str(),
      WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      ScaleForSystemDpi(408),
      ScaleForSystemDpi(264),
      nullptr,
      nullptr,
      instance_,
      this);
  if (incoming_approval_window_ == nullptr) {
    return false;
  }

  incoming_approval_dismiss_button_ = CreateWindowExW(
      0,
      L"BUTTON",
      GetText(L"incoming_dismiss", L"\u95dc\u9589").c_str(),
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
      0,
      0,
      0,
      0,
      incoming_approval_window_,
      reinterpret_cast<HMENU>(kIncomingApprovalDismissButton),
      instance_,
      nullptr);
  incoming_approval_accept_button_ = CreateWindowExW(
      0,
      L"BUTTON",
      GetText(L"incoming_accept", L"\u63a5\u53d7").c_str(),
      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
      0,
      0,
      0,
      0,
      incoming_approval_window_,
      reinterpret_cast<HMENU>(kIncomingApprovalAcceptButton),
      instance_,
      nullptr);

  if (font_body_ != nullptr) {
    if (incoming_approval_dismiss_button_ != nullptr) {
      SendMessageW(
          incoming_approval_dismiss_button_,
          WM_SETFONT,
          reinterpret_cast<WPARAM>(font_button_ != nullptr ? font_button_ : font_body_),
          TRUE);
    }
    if (incoming_approval_accept_button_ != nullptr) {
      SendMessageW(
          incoming_approval_accept_button_,
          WM_SETFONT,
          reinterpret_cast<WPARAM>(font_button_ != nullptr ? font_button_ : font_body_),
          TRUE);
    }
  }

  return true;
}

void PortableHostApp::DestroyIncomingApprovalWindow() {
  if (incoming_approval_window_ != nullptr) {
    DestroyWindow(incoming_approval_window_);
    incoming_approval_window_ = nullptr;
  }
  incoming_approval_accept_button_ = nullptr;
  incoming_approval_dismiss_button_ = nullptr;
}

void PortableHostApp::LayoutIncomingApprovalWindow(int client_width, int client_height) {
  if (incoming_approval_window_ == nullptr) {
    return;
  }

  const int button_width = ScaleForSystemDpi(88);
  const int button_height = ScaleForSystemDpi(34);
  const int button_gap = ScaleForSystemDpi(10);
  const int bottom = ScaleForSystemDpi(16);
  const int right = ScaleForSystemDpi(18);
  const int accept_left = client_width - right - button_width;
  const int dismiss_left = accept_left - button_gap - button_width;
  const int top = client_height - bottom - button_height;

  if (incoming_approval_dismiss_button_ != nullptr) {
    MoveWindow(
        incoming_approval_dismiss_button_,
        dismiss_left,
        top,
        button_width,
        button_height,
        TRUE);
  }
  if (incoming_approval_accept_button_ != nullptr) {
    MoveWindow(
        incoming_approval_accept_button_,
        accept_left,
        top,
        button_width,
        button_height,
        TRUE);
  }
}

void PortableHostApp::CaptureIncomingApprovalRemoteIdentity(
    std::wstring* remote_id,
    std::wstring* remote_name) const {
  Win32LockGuard guard(incoming_approval_mutex_);
  if (remote_id != nullptr) {
    *remote_id = incoming_approval_remote_id_;
  }
  if (remote_name != nullptr) {
    *remote_name = incoming_approval_remote_name_;
  }
}

std::wstring PortableHostApp::GetIncomingApprovalDisplayName() const {
  Win32LockGuard guard(incoming_approval_mutex_);
  const std::wstring remote_name = Trim(incoming_approval_remote_name_);
  const std::wstring remote_id = Trim(incoming_approval_remote_id_);
  const bool generic_system_name =
      !remote_name.empty() &&
      (_wcsicmp(remote_name.c_str(), L"administrator") == 0 ||
       _wcsicmp(remote_name.c_str(), L"admin") == 0 ||
       _wcsicmp(remote_name.c_str(), L"system") == 0);
  if (!remote_name.empty() && !generic_system_name) {
    return remote_name;
  }
  if (!remote_id.empty()) {
    return remote_id;
  }
  if (!remote_name.empty()) {
    return remote_name;
  }
  return GetText(L"incoming_unknown_device", L"\u672a\u77e5\u88dd\u7f6e");
}

std::wstring PortableHostApp::GetIncomingApprovalSecondaryText() const {
  Win32LockGuard guard(incoming_approval_mutex_);
  const std::wstring remote_name = Trim(incoming_approval_remote_name_);
  const std::wstring remote_id = Trim(incoming_approval_remote_id_);
  const bool generic_system_name =
      !remote_name.empty() &&
      (_wcsicmp(remote_name.c_str(), L"administrator") == 0 ||
       _wcsicmp(remote_name.c_str(), L"admin") == 0 ||
       _wcsicmp(remote_name.c_str(), L"system") == 0);
  if (remote_id.empty()) {
    return L"";
  }
  if (!remote_name.empty() && !generic_system_name &&
      _wcsicmp(remote_name.c_str(), remote_id.c_str()) != 0) {
    return GetText(L"incoming_rustdesk_id_prefix", L"RustDesk ID") +
           L"  " +
           FormatDisplayHostId(remote_id);
  }
  return L"";
}

void PortableHostApp::StoreActiveSessionIdentity(
    const std::wstring& remote_id,
    const std::wstring& remote_name) {
  {
    Win32LockGuard guard(active_session_mutex_);
    active_session_remote_id_ = Trim(remote_id);
    active_session_remote_name_ = Trim(remote_name);
  }
  InvalidateConnectionStatusCard();
  if (window_ != nullptr) {
    PostMessageW(window_, kAppRendezvousStatus, 0, 0);
  }
}

void PortableHostApp::ClearActiveSessionIdentity() {
  {
    Win32LockGuard guard(active_session_mutex_);
    active_session_remote_id_.clear();
    active_session_remote_name_.clear();
  }
  InvalidateConnectionStatusCard();
  if (window_ != nullptr) {
    PostMessageW(window_, kAppRendezvousStatus, 0, 0);
  }
}

std::wstring PortableHostApp::GetActiveSessionDisplayName() const {
  std::wstring remote_id;
  std::wstring remote_name;
  {
    Win32LockGuard guard(active_session_mutex_);
    remote_id = Trim(active_session_remote_id_);
    remote_name = Trim(active_session_remote_name_);
  }

  const bool generic_system_name =
      !remote_name.empty() &&
      (_wcsicmp(remote_name.c_str(), L"administrator") == 0 ||
       _wcsicmp(remote_name.c_str(), L"admin") == 0 ||
       _wcsicmp(remote_name.c_str(), L"system") == 0);
  if (!remote_name.empty() && !generic_system_name) {
    return remote_name;
  }
  if (!remote_id.empty()) {
    return FormatDisplayHostId(remote_id);
  }
  if (!remote_name.empty()) {
    return remote_name;
  }
  return GetText(L"incoming_unknown_device", L"\u672a\u77e5\u88dd\u7f6e");
}

void PortableHostApp::InvalidateConnectionStatusCard() const {
  if (window_ == nullptr || IsRectEmpty(&connection_status_card_rect_)) {
    return;
  }
  InvalidateRect(window_, &connection_status_card_rect_, TRUE);
  if (disconnect_button_ != nullptr) {
    InvalidateRect(disconnect_button_, nullptr, TRUE);
  }
}

void PortableHostApp::DrawConnectionStatusCard(HDC dc) const {
  if (IsRectEmpty(&connection_status_card_rect_)) {
    return;
  }

  const bool connected = active_session_connected_.load();
  const std::wstring status_text = GetText(
      connected ? L"connection_status_authorized" : L"connection_status_disconnected",
      connected ? L"\u5df2\u9023\u5165 (\u5df2\u6388\u6b0a)" : L"\u672a\u9023\u7dda");
  const std::wstring remote_user_prefix =
      GetText(L"connection_status_remote_user", L"\u9023\u7dda\u4eba\u54e1") + L": ";
  const std::wstring display_name =
      connected ? GetActiveSessionDisplayName() : L"----------";

  RECT card_rect = connection_status_card_rect_;
  DrawRoundedBlock(
      dc,
      card_rect,
      ScaleForSystemDpi(12),
      connected ? kConnectionCardConnectedFillColor : kConnectionCardIdleFillColor,
      connected ? kConnectionCardConnectedBorderColor : kConnectionCardIdleBorderColor);

  RECT content_rect = card_rect;
  InflateRect(&content_rect, -ScaleForSystemDpi(16), -ScaleForSystemDpi(14));
  SetBkMode(dc, TRANSPARENT);

  const int dot_size = ScaleForSystemDpi(10);
  RECT dot_rect = {
      content_rect.left,
      content_rect.top + ScaleForSystemDpi(3),
      content_rect.left + dot_size,
      content_rect.top + ScaleForSystemDpi(3) + dot_size};
  HBRUSH dot_brush = CreateSolidBrush(connected ? kGoodColor : RGB(165, 168, 174));
  if (dot_brush != nullptr) {
    HGDIOBJ old_brush = SelectObject(dc, dot_brush);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dot_rect.left, dot_rect.top, dot_rect.right, dot_rect.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(dot_brush);
  }

  HFONT status_font =
      font_button_ != nullptr ? font_button_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HFONT small_font =
      font_small_ != nullptr ? font_small_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ old_font = SelectObject(dc, status_font);

  RECT status_rect = content_rect;
  status_rect.left = dot_rect.right + ScaleForSystemDpi(8);
  status_rect.top -= ScaleForSystemDpi(1);
  status_rect.bottom = status_rect.top + ScaleForSystemDpi(20);
  SetTextColor(dc, connected ? kConnectionCardConnectedTextColor : RGB(244, 245, 247));
  DrawTextW(dc, status_text.c_str(), -1, &status_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  auto draw_labeled_value = [&](int top,
                                const std::wstring& prefix,
                                const std::wstring& value,
                                COLORREF prefix_color,
                                COLORREF value_color) {
    RECT prefix_rect = content_rect;
    prefix_rect.top = top;
    prefix_rect.bottom = top + ScaleForSystemDpi(20);
    SetTextColor(dc, prefix_color);
    SelectObject(dc, small_font);
    DrawTextW(dc, prefix.c_str(), -1, &prefix_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SIZE prefix_size = {};
    GetTextExtentPoint32W(dc, prefix.c_str(), static_cast<int>(prefix.size()), &prefix_size);
    RECT value_rect = prefix_rect;
    value_rect.left += prefix_size.cx;
    SetTextColor(dc, value_color);
    DrawTextW(dc, value.c_str(), -1, &value_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  };

  draw_labeled_value(
      content_rect.top + ScaleForSystemDpi(24),
      remote_user_prefix,
      display_name,
      connected ? RGB(224, 228, 234) : RGB(198, 201, 208),
      connected ? kConnectionNameAccentColor : RGB(214, 217, 223));

  SelectObject(dc, old_font);
}

bool PortableHostApp::DrawOwnerButton(const DRAWITEMSTRUCT* draw_item) const {
  if (draw_item == nullptr || draw_item->CtlType != ODT_BUTTON) {
    return false;
  }

  const UINT control_id = draw_item->CtlID;
  const bool primary = control_id == kIncomingApprovalAcceptButton;
  const bool icon_button =
      control_id == kOptionsButton || control_id == kRefreshPasswordButton;
  const bool secondary = control_id == kIncomingApprovalDismissButton;
  const bool disconnect_button = control_id == kDisconnectButton;
  const bool disabled = (draw_item->itemState & ODS_DISABLED) != 0;
  const bool selected = (draw_item->itemState & ODS_SELECTED) != 0;
  COLORREF fill_color = primary
      ? kAccentColor
      : (secondary
            ? kSecondaryButtonColor
            : (disconnect_button ? kDisconnectButtonColor : kWindowColor));
  COLORREF border_color = primary
      ? kAccentColor
      : (secondary
            ? kSecondaryButtonBorderColor
            : (disconnect_button ? kDisconnectButtonColor : kWindowColor));
  COLORREF text_color = icon_button ? RGB(222, 224, 228) : kTextColor;

  if (disabled) {
    fill_color = icon_button ? kWindowColor : kDisabledButtonFillColor;
    border_color = icon_button ? kWindowColor : kDisabledButtonBorderColor;
    text_color = kDisabledButtonTextColor;
  } else if (selected) {
    if (icon_button) {
      fill_color = kWindowColor;
      border_color = kWindowColor;
    } else if (primary) {
      fill_color = RGB(32, 118, 244);
      border_color = RGB(32, 118, 244);
    } else if (disconnect_button) {
      fill_color = kDisconnectButtonPressedColor;
      border_color = kDisconnectButtonPressedColor;
    } else if (secondary) {
      fill_color = RGB(61, 64, 72);
      border_color = RGB(77, 80, 88);
    } else {
      fill_color = RGB(55, 57, 64);
      border_color = RGB(70, 72, 79);
    }
  }

  DrawRoundedBlock(
      draw_item->hDC,
      draw_item->rcItem,
      ScaleForSystemDpi(icon_button ? 6 : 9),
      fill_color,
      border_color);

  if (icon_button) {
    const HBITMAP bitmap =
        control_id == kOptionsButton ? options_icon_bitmap_ : refresh_icon_bitmap_;
    if (bitmap != nullptr) {
      DrawBitmapCentered(draw_item->hDC, draw_item->rcItem, bitmap);
    }
    return true;
  }

  wchar_t text[128] = {};
  GetWindowTextW(draw_item->hwndItem, text, static_cast<int>(_countof(text)));
  SetBkMode(draw_item->hDC, TRANSPARENT);
  SetTextColor(draw_item->hDC, text_color);
  HFONT font =
      font_button_ != nullptr ? font_button_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ old_font = SelectObject(draw_item->hDC, font);
  RECT text_rect = draw_item->rcItem;
  DrawTextW(
      draw_item->hDC,
      text,
      -1,
      &text_rect,
      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(draw_item->hDC, old_font);
  return true;
}

LRESULT CALLBACK PortableHostApp::IncomingApprovalWindowProcStatic(
    HWND hwnd,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
  auto* app =
      reinterpret_cast<PortableHostApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
    app = reinterpret_cast<PortableHostApp*>(create_struct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }

  if (app != nullptr) {
    return app->IncomingApprovalWindowProc(hwnd, message, w_param, l_param);
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT PortableHostApp::IncomingApprovalWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
  switch (message) {
    case WM_COMMAND: {
      const UINT control_id = LOWORD(w_param);
      if (control_id == kIncomingApprovalAcceptButton) {
        ResolveIncomingApproval(IncomingApprovalDecision::kAccepted);
        return 0;
      }
      if (control_id == kIncomingApprovalDismissButton) {
        ResolveIncomingApproval(IncomingApprovalDecision::kRejected);
        return 0;
      }
      break;
    }
    case WM_SIZE:
      LayoutIncomingApprovalWindow(LOWORD(l_param), HIWORD(l_param));
      return 0;
    case WM_DRAWITEM:
      if (DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(l_param))) {
        return TRUE;
      }
      break;
    case WM_CLOSE:
      ResolveIncomingApproval(IncomingApprovalDecision::kRejected);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint = {};
      HDC dc = BeginPaint(hwnd, &paint);
      RECT client_rect = {};
      GetClientRect(hwnd, &client_rect);
      HBRUSH dialog_brush = CreateSolidBrush(kDialogColor);
      FillRect(
          dc,
          &client_rect,
          dialog_brush != nullptr ? dialog_brush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
      if (dialog_brush != nullptr) {
        DeleteObject(dialog_brush);
      }
      SetBkMode(dc, TRANSPARENT);

      RECT draw_rect = client_rect;
      draw_rect.left += ScaleForSystemDpi(26);
      draw_rect.top += ScaleForSystemDpi(18);
      draw_rect.right -= ScaleForSystemDpi(26);

      SelectObject(
          dc,
          font_dialog_title_ != nullptr ? font_dialog_title_ : GetStockObject(DEFAULT_GUI_FONT));
      SetTextColor(dc, kTextColor);
      RECT question_rect = draw_rect;
      question_rect.bottom = question_rect.top + ScaleForSystemDpi(34);
      DrawTextW(
          dc,
          GetText(L"incoming_question", L"\u662f\u5426\u63a5\u53d7\uff1f").c_str(),
          -1,
          &question_rect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER);

      const std::wstring display_name = GetIncomingApprovalDisplayName();
      const std::wstring secondary = GetIncomingApprovalSecondaryText();
      const int avatar_size = ScaleForSystemDpi(54);
      RECT avatar_rect = {
          draw_rect.left,
          question_rect.bottom + ScaleForSystemDpi(18),
          draw_rect.left + avatar_size,
          question_rect.bottom + ScaleForSystemDpi(18) + avatar_size};
      HBRUSH avatar_brush = CreateSolidBrush(kAvatarColor);
      if (avatar_brush != nullptr) {
        HGDIOBJ old_brush = SelectObject(dc, avatar_brush);
        HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, avatar_rect.left, avatar_rect.top, avatar_rect.right, avatar_rect.bottom);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(avatar_brush);
      }

      wchar_t avatar_text[2] = {GetAvatarGlyph(display_name), L'\0'};
      HGDIOBJ old_avatar_font = SelectObject(
          dc,
          font_avatar_ != nullptr ? font_avatar_ : GetStockObject(DEFAULT_GUI_FONT));
      SetTextColor(dc, RGB(252, 252, 252));
      DrawTextW(
          dc,
          avatar_text,
          -1,
          &avatar_rect,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old_avatar_font);

      RECT name_rect = draw_rect;
      name_rect.left = avatar_rect.right + ScaleForSystemDpi(16);
      name_rect.top = avatar_rect.top + ScaleForSystemDpi(6);
      name_rect.bottom = name_rect.top + ScaleForSystemDpi(32);
      SelectObject(
          dc,
          font_dialog_name_ != nullptr ? font_dialog_name_ : GetStockObject(DEFAULT_GUI_FONT));
      SetTextColor(dc, RGB(255, 255, 255));
      DrawTextW(
          dc,
          display_name.c_str(),
          -1,
          &name_rect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

      if (!secondary.empty()) {
        SelectObject(dc, font_small_ != nullptr ? font_small_ : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(198, 201, 208));
        RECT secondary_rect = name_rect;
        secondary_rect.top = name_rect.bottom + ScaleForSystemDpi(3);
        secondary_rect.bottom = secondary_rect.top + ScaleForSystemDpi(24);
        DrawTextW(
            dc,
            secondary.c_str(),
            -1,
            &secondary_rect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        draw_rect.top = (std::max)(avatar_rect.bottom, secondary_rect.bottom) + ScaleForSystemDpi(16);
      } else {
        draw_rect.top = avatar_rect.bottom + ScaleForSystemDpi(16);
      }

      SelectObject(
          dc,
          font_dialog_body_ != nullptr ? font_dialog_body_ : GetStockObject(DEFAULT_GUI_FONT));
      SetTextColor(dc, RGB(244, 246, 249));
      RECT body_rect = draw_rect;
      body_rect.bottom = client_rect.bottom - ScaleForSystemDpi(56);
      DrawTextW(
          dc,
          GetText(
              L"incoming_body",
              L"\u6536\u5230\u9060\u7aef\u9023\u5165\u8acb\u6c42\uff0c\u662f\u5426\u63a5\u53d7\uff1f")
              .c_str(),
          -1,
          &body_rect,
          DT_LEFT | DT_WORDBREAK);

      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_NCDESTROY:
      if (incoming_approval_window_ == hwnd) {
        incoming_approval_window_ = nullptr;
        incoming_approval_accept_button_ = nullptr;
        incoming_approval_dismiss_button_ = nullptr;
      }
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

void PortableHostApp::CreateFonts() {
  font_title_ = CreateFontW(
      ScaleForSystemDpi(23), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_body_ = CreateFontW(
      ScaleForSystemDpi(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_value_ = CreateFontW(
      ScaleForSystemDpi(23), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_small_ = CreateFontW(
      ScaleForSystemDpi(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_button_ = CreateFontW(
      ScaleForSystemDpi(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_dialog_title_ = CreateFontW(
      ScaleForSystemDpi(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_dialog_name_ = CreateFontW(
      ScaleForSystemDpi(21), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_dialog_body_ = CreateFontW(
      ScaleForSystemDpi(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  font_avatar_ = CreateFontW(
      ScaleForSystemDpi(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void PortableHostApp::DestroyFonts() {
  if (font_avatar_ != nullptr) {
    DeleteObject(font_avatar_);
    font_avatar_ = nullptr;
  }
  if (font_dialog_body_ != nullptr) {
    DeleteObject(font_dialog_body_);
    font_dialog_body_ = nullptr;
  }
  if (font_dialog_name_ != nullptr) {
    DeleteObject(font_dialog_name_);
    font_dialog_name_ = nullptr;
  }
  if (font_dialog_title_ != nullptr) {
    DeleteObject(font_dialog_title_);
    font_dialog_title_ = nullptr;
  }
  if (font_button_ != nullptr) {
    DeleteObject(font_button_);
    font_button_ = nullptr;
  }
  if (font_small_ != nullptr) {
    DeleteObject(font_small_);
    font_small_ = nullptr;
  }
  if (font_value_ != nullptr) {
    DeleteObject(font_value_);
    font_value_ = nullptr;
  }
  if (font_body_ != nullptr) {
    DeleteObject(font_body_);
    font_body_ = nullptr;
  }
  if (font_title_ != nullptr) {
    DeleteObject(font_title_);
    font_title_ = nullptr;
  }
}

void PortableHostApp::CreateControls() {
  const DWORD static_style = WS_CHILD | WS_VISIBLE;
  const DWORD edit_style = WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL;
  const DWORD button_style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW;
  const DWORD icon_button_style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW;

  if (logo_bitmap_ != nullptr) {
    DeleteObject(logo_bitmap_);
    logo_bitmap_ = nullptr;
  }
  if (options_icon_bitmap_ != nullptr) {
    DeleteObject(options_icon_bitmap_);
    options_icon_bitmap_ = nullptr;
  }
  if (refresh_icon_bitmap_ != nullptr) {
    DeleteObject(refresh_icon_bitmap_);
    refresh_icon_bitmap_ = nullptr;
  }
  if (gdiplus_ready_) {
    logo_bitmap_ = LoadBitmapFromResource(
        instance_,
        IDB_QS_LOGO,
        RT_RCDATA,
        ScaleForSystemDpi(kLogoTargetWidth),
        ScaleForSystemDpi(kLogoTargetHeight));
    options_icon_bitmap_ = LoadTintedBitmapFromResource(
        instance_,
        IDB_ICON_MORE_VERT,
        RT_RCDATA,
        RGB(216, 218, 222),
        kWindowColor,
        ScaleForSystemDpi(18),
        ScaleForSystemDpi(18));
    refresh_icon_bitmap_ = LoadTintedBitmapFromResource(
        instance_,
        IDB_ICON_REFRESH,
        RT_RCDATA,
        RGB(232, 234, 237),
        kWindowColor,
        ScaleForSystemDpi(18),
        ScaleForSystemDpi(18));
  }

  logo_icon_ = CreateWindowExW(
      0,
      L"STATIC",
      nullptr,
      static_style | SS_BITMAP | SS_CENTERIMAGE,
      0,
      0,
      0,
      0,
      window_,
      nullptr,
      instance_,
      nullptr);
  if (logo_icon_ != nullptr && logo_bitmap_ != nullptr) {
    SendMessageW(
        logo_icon_,
        STM_SETIMAGE,
        IMAGE_BITMAP,
        reinterpret_cast<LPARAM>(logo_bitmap_));
  }

  title_label_ = CreateWindowExW(
      0, L"STATIC", kAppWindowTitle, static_style,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  subtitle_label_ = CreateWindowExW(
      0, L"STATIC",
      L"\u6b61\u8fce\u4f7f\u7528\u9060\u7aef\u5354\u52a9",
      static_style,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  hint_label_ = CreateWindowExW(
      0,
      L"STATIC",
      L"\u8acb\u63d0\u4f9b\u4e0b\u9762\u7684 ID \u53ca\u5bc6\u78bc",
      static_style,
      0,
      0,
      0,
      0,
      window_,
      nullptr,
      instance_,
      nullptr);

  id_accent_ = CreateWindowExW(
      0,
      L"STATIC",
      L"",
      static_style,
      0,
      0,
      0,
      0,
      window_,
      nullptr,
      instance_,
      nullptr);

  id_label_ = CreateWindowExW(
      0, L"STATIC", L"ID", static_style,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  id_value_ = CreateWindowExW(
      0, L"EDIT", L"",
      edit_style,
      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kIdValue), instance_, nullptr);

  options_button_ = CreateWindowExW(
      0,
      L"BUTTON",
      L"",
      icon_button_style,
      0,
      0,
      0,
      0,
      window_,
      reinterpret_cast<HMENU>(kOptionsButton),
      instance_,
      nullptr);
  if (options_button_ != nullptr) {
    SetWindowLongPtrW(
        options_button_,
        GWL_STYLE,
        GetWindowLongPtrW(options_button_, GWL_STYLE) & ~WS_TABSTOP);
  }

  password_accent_ = CreateWindowExW(
      0,
      L"STATIC",
      L"",
      static_style,
      0,
      0,
      0,
      0,
      window_,
      nullptr,
      instance_,
      nullptr);

  password_label_ = CreateWindowExW(
      0, L"STATIC", L"\u4e00\u6b21\u6027\u5bc6\u78bc", static_style,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  password_value_ = CreateWindowExW(
      0, L"EDIT", L"",
      edit_style,
      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kPasswordValue), instance_, nullptr);

  refresh_password_button_ = CreateWindowExW(
      0, L"BUTTON", L"",
      icon_button_style,
      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kRefreshPasswordButton), instance_, nullptr);
  if (refresh_password_button_ != nullptr) {
    SetWindowLongPtrW(
        refresh_password_button_,
        GWL_STYLE,
        GetWindowLongPtrW(refresh_password_button_, GWL_STYLE) & ~WS_TABSTOP);
  }

  disconnect_button_ = CreateWindowExW(
      0,
      L"BUTTON",
      L"",
      button_style,
      0,
      0,
      0,
      0,
      window_,
      reinterpret_cast<HMENU>(kDisconnectButton),
      instance_,
      nullptr);

  server_status_label_ = CreateWindowExW(
      0, L"STATIC", L"\u4f3a\u670d\u5668\u72c0\u614b", static_style | SS_CENTERIMAGE,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  server_value_label_ = CreateWindowExW(
      0, L"STATIC", L"", static_style | SS_CENTERIMAGE,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  config_path_label_ = CreateWindowExW(
      0, L"STATIC", L"", static_style,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);

  const LPARAM edit_margins = MAKELPARAM(ScaleForSystemDpi(4), ScaleForSystemDpi(4));
  if (id_value_ != nullptr) {
    SendMessageW(id_value_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, edit_margins);
  }
  if (password_value_ != nullptr) {
    SendMessageW(
        password_value_,
        EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        edit_margins);
  }
  if (subtitle_label_ != nullptr) {
    ShowWindow(subtitle_label_, SW_HIDE);
  }
  if (config_path_label_ != nullptr) {
    ShowWindow(config_path_label_, SW_HIDE);
  }
  if (window_ != nullptr) {
    SendMessageW(window_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
  }
}

void PortableHostApp::ApplyFonts() {
  const HWND controls[] = {
      logo_icon_,
      title_label_,
      subtitle_label_,
      hint_label_,
      id_accent_,
      id_label_,
      id_value_,
      options_button_,
      password_accent_,
      password_label_,
      password_value_,
      refresh_password_button_,
      disconnect_button_,
      server_status_label_,
      server_value_label_,
      config_path_label_,
  };

  for (HWND control : controls) {
    if (control != nullptr) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
    }
  }

  SendMessageW(title_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_title_), TRUE);
  SendMessageW(id_value_, WM_SETFONT, reinterpret_cast<WPARAM>(font_value_), TRUE);
  SendMessageW(password_value_, WM_SETFONT, reinterpret_cast<WPARAM>(font_value_), TRUE);
  SendMessageW(id_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
  SendMessageW(password_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
  SendMessageW(subtitle_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_small_), TRUE);
  SendMessageW(hint_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_small_), TRUE);
  SendMessageW(disconnect_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_button_), TRUE);
  SendMessageW(
      server_status_label_,
      WM_SETFONT,
      reinterpret_cast<WPARAM>(font_body_),
      TRUE);
  SendMessageW(server_value_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
  SendMessageW(config_path_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_small_), TRUE);
}

void PortableHostApp::LayoutControls(int client_width, int client_height) {
  if (window_ == nullptr) {
    return;
  }

  const bool use_xp_status_layout = IsWindowsXpOrEarlier();
  const int margin = ScaleForSystemDpi(kControlMargin);
  const int edit_height = ScaleForSystemDpi(kEditHeight);
  const int left = margin;
  const int width = client_width - (margin * 2);
  const int label_inset = ScaleForSystemDpi(9);
  const int icon_button_width = ScaleForSystemDpi(24);
  const int icon_button_height = ScaleForSystemDpi(24);
  const int icon_gap = ScaleForSystemDpi(6);
  const int block_height = ScaleForSystemDpi(58);
  int top = ScaleForSystemDpi(13);

  MoveWindow(logo_icon_, left, top, width, ScaleForSystemDpi(64), TRUE);
  top += ScaleForSystemDpi(68);

  MoveWindow(title_label_, left, top, width, ScaleForSystemDpi(33), TRUE);
  top += ScaleForSystemDpi(34);

  MoveWindow(hint_label_, left, top, width, ScaleForSystemDpi(23), TRUE);
  top += ScaleForSystemDpi(24);

  MoveWindow(id_accent_, left, top, ScaleForSystemDpi(3), block_height, TRUE);
  MoveWindow(
      id_label_,
      left + label_inset,
      top,
      width - label_inset,
      ScaleForSystemDpi(22),
      TRUE);

  const int id_value_left = left + label_inset;
  const int id_value_top = top + ScaleForSystemDpi(21);
  const int id_value_width = width - label_inset - icon_button_width - icon_gap;
  MoveWindow(id_value_, id_value_left, id_value_top, id_value_width, edit_height, TRUE);
  MoveWindow(
      options_button_,
      id_value_left + id_value_width + icon_gap,
      id_value_top + ((edit_height - icon_button_height) / 2),
      icon_button_width,
      icon_button_height,
      TRUE);
  top += block_height + ScaleForSystemDpi(6);

  MoveWindow(password_accent_, left, top, ScaleForSystemDpi(3), block_height, TRUE);
  MoveWindow(
      password_label_,
      left + label_inset,
      top,
      width - label_inset,
      ScaleForSystemDpi(22),
      TRUE);

  const int password_value_left = left + label_inset;
  const int password_value_top = top + ScaleForSystemDpi(21);
  const int password_value_width = width - label_inset - icon_button_width - icon_gap;
  MoveWindow(
      password_value_,
      password_value_left,
      password_value_top,
      password_value_width,
      edit_height,
      TRUE);
  MoveWindow(
      refresh_password_button_,
      password_value_left + password_value_width + icon_gap,
      password_value_top + ((edit_height - icon_button_height) / 2),
      icon_button_width,
      icon_button_height,
      TRUE);
  top += block_height + ScaleForSystemDpi(8);

  const int connection_card_height = ScaleForSystemDpi(102);
  SetRect(
      &connection_status_card_rect_,
      left,
      top,
      left + width,
      top + connection_card_height);
  MoveWindow(
      disconnect_button_,
      left + ScaleForSystemDpi(16),
      connection_status_card_rect_.bottom - ScaleForSystemDpi(40),
      width - ScaleForSystemDpi(32),
      ScaleForSystemDpi(32),
      TRUE);
  top = connection_status_card_rect_.bottom + ScaleForSystemDpi(5);

  const int status_top = top;
  const int status_label_left = left + ScaleForSystemDpi(use_xp_status_layout ? 8 : 10);
  const int status_label_top =
      status_top + ScaleForSystemDpi(use_xp_status_layout ? 2 : 0);
  const int status_label_width = ScaleForSystemDpi(use_xp_status_layout ? 18 : 16);
  const int status_label_height = ScaleForSystemDpi(use_xp_status_layout ? 17 : 18);
  const int status_text_left =
      status_label_left + status_label_width + ScaleForSystemDpi(use_xp_status_layout ? 8 : 3);
  const int status_text_width =
      width - (status_text_left - left) - ScaleForSystemDpi(10);
  MoveWindow(
      server_status_label_,
      status_label_left,
      status_label_top,
      status_label_width,
      status_label_height,
      TRUE);
  MoveWindow(
      server_value_label_,
      status_text_left,
      status_label_top,
      status_text_width,
      status_label_height,
      TRUE);
  MoveWindow(config_path_label_, 0, client_height, 0, 0, FALSE);
}

void PortableHostApp::LoadOrCreateConfig() {
  config_.exe_dir = GetExecutableDirectory();
  config_.config_path = BuildConfigPath(config_.exe_dir);
  EnsureDefaultLanguageFiles();
  config_.random_password_enabled = true;
  config_.fixed_password_protected.clear();
  fixed_password_.clear();

  if (!FileExists(config_.config_path)) {
    config_.id_server = L"rs-ny.rustdesk.com:21116";
    config_.relay_server = L"rs-ny.rustdesk.com:21117";
    config_.api_server.clear();
    config_.key = L"OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw=";
    config_.direct_access_enabled = true;
    config_.direct_access_port = kDefaultDirectAccessPort;
    config_.preferred_codec = NormalizePreferredCodec(L"h264");
    config_.video_fps = kDefaultTargetVideoFps;
    config_.video_bitrate_kbps = kDefaultVideoBitrateKbps;
    config_.host_id = GenerateRustDeskStyleId();
    config_.language_file = GetDefaultLanguageFile();
    LoadLanguageStrings();
    SaveConfig();
    return;
  }

  config_.id_server = ReadIniString(L"server", L"id_server", L"rs-ny.rustdesk.com:21116");
  config_.relay_server = ReadIniString(L"server", L"relay_server", L"rs-ny.rustdesk.com:21117");
  config_.api_server = ReadIniString(L"server", L"api_server", L"");
  config_.key = ReadIniString(L"server", L"key", L"");
  config_.host_id = ReadIniString(L"host", L"id", L"");
  config_.language_file = NormalizeLanguageFileSelection(
      ReadIniString(L"host", L"language_file", L""));
  config_.preferred_codec = NormalizePreferredCodec(
      ReadIniString(L"host", L"preferred_codec", L"h264"));
  public_key_hex_ = ReadIniString(L"host", L"public_key_hex", L"");
  secret_key_hex_ = ReadIniString(L"host", L"secret_key_hex", L"");
  device_uuid_text_ = ReadIniString(L"host", L"device_uuid", L"");
  config_.random_password_enabled =
      ReadIniString(L"host", L"random_password_enabled", L"1") != L"0";
  config_.fixed_password_protected =
      ReadIniString(L"host", L"fixed_password_protected", L"");
  fixed_password_ = UnprotectLocalMachineString(config_.fixed_password_protected);
  if (!config_.fixed_password_protected.empty() && fixed_password_.empty()) {
    config_.fixed_password_protected.clear();
    config_.random_password_enabled = true;
  }
  if (config_.preferred_codec.empty()) {
    config_.preferred_codec = L"h264";
  }

  const ParsedHostPort id_server = ParseHostPort(config_.id_server, kDefaultIdServerPort);
  const ParsedHostPort relay_server = ParseHostPort(config_.relay_server, kDefaultRelayServerPort);

  if (config_.id_server.empty() || id_server.host.empty()) {
    config_.id_server = L"rs-ny.rustdesk.com:21116";
  } else {
    config_.id_server = BuildDisplayEndpoint(id_server.host, id_server.port);
  }

  if (config_.relay_server.empty() || relay_server.host.empty()) {
    config_.relay_server = L"rs-ny.rustdesk.com:21117";
  } else {
    config_.relay_server = BuildDisplayEndpoint(relay_server.host, relay_server.port);
  }

  if (config_.api_server.empty() && IsRustDeskPublicHost(id_server.host)) {
    config_.api_server.clear();
  }

  if (config_.key.empty() && IsRustDeskPublicHost(id_server.host)) {
    config_.key = L"OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw=";
  }

  if (config_.host_id.empty()) {
    config_.host_id = GenerateRustDeskStyleId();
  }
  if (Trim(config_.language_file).empty()) {
    config_.language_file = GetDefaultLanguageFile();
  }

  const std::wstring password_length_text =
      ReadIniString(L"host", L"temporary_password_length", L"6");
  const int parsed_length = _wtoi(password_length_text.c_str());
  if (parsed_length == 8 || parsed_length == 10) {
    config_.temporary_password_length = parsed_length;
  } else {
    config_.temporary_password_length = 6;
  }

  const std::wstring force_relay_text = ReadIniString(L"host", L"force_relay", L"1");
  config_.force_relay = force_relay_text != L"0";

  const std::wstring direct_access_enabled_text =
      ReadIniString(L"host", L"direct_access_enabled", L"");
  if (!Trim(direct_access_enabled_text).empty()) {
    config_.direct_access_enabled =
        ParseIniBoolValue(direct_access_enabled_text, true);
  } else {
    config_.direct_access_enabled = ParseIniBoolValue(
        ReadIniString(L"host", L"direct-server", L""),
        true);
  }

  std::wstring direct_access_port_text =
      ReadIniString(L"host", L"direct_access_port", L"");
  if (Trim(direct_access_port_text).empty()) {
    direct_access_port_text =
        ReadIniString(L"host", L"direct-access-port", L"");
  }
  const int parsed_direct_access_port =
      _wtoi(direct_access_port_text.c_str());
  if (parsed_direct_access_port >= 1 &&
      parsed_direct_access_port <= 65535) {
    config_.direct_access_port = parsed_direct_access_port;
  } else {
    config_.direct_access_port = kDefaultDirectAccessPort;
  }

  const std::wstring video_fps_text =
      ReadIniString(L"host", L"video_fps", std::to_wstring(kDefaultTargetVideoFps).c_str());
  const int parsed_video_fps = _wtoi(video_fps_text.c_str());
  if (parsed_video_fps >= 1 && parsed_video_fps <= 60) {
    config_.video_fps = parsed_video_fps;
  } else {
    config_.video_fps = kDefaultTargetVideoFps;
  }

  const std::wstring video_bitrate_text =
      ReadIniString(L"host", L"video_bitrate_kbps", std::to_wstring(kDefaultVideoBitrateKbps).c_str());
  const int parsed_video_bitrate = _wtoi(video_bitrate_text.c_str());
  if (parsed_video_bitrate >= 500 && parsed_video_bitrate <= 50000) {
    config_.video_bitrate_kbps = parsed_video_bitrate;
  } else {
    config_.video_bitrate_kbps = kDefaultVideoBitrateKbps;
  }

  LoadLanguageStrings();
  SaveConfig();
}

void PortableHostApp::SaveConfig() const {
  WriteIniString(
      L"server",
      L"id_server",
      config_.id_server.empty() ? L"rs-ny.rustdesk.com:21116" : config_.id_server);
  WriteIniString(
      L"server",
      L"relay_server",
      config_.relay_server.empty() ? L"rs-ny.rustdesk.com:21117" : config_.relay_server);
  WriteIniString(L"server", L"api_server", config_.api_server);
  WriteIniString(L"server", L"key", config_.key);

  WriteIniString(L"host", L"id", config_.host_id);
  WriteIniString(L"host", L"language_file", config_.language_file);
  WriteIniString(L"host", L"temporary_password_length", std::to_wstring(config_.temporary_password_length));
  WriteIniString(L"host", L"random_password_enabled", config_.random_password_enabled ? L"1" : L"0");
  WriteIniString(L"host", L"fixed_password_protected", config_.fixed_password_protected);
  WriteIniString(L"host", L"force_relay", config_.force_relay ? L"1" : L"0");
  WriteIniString(L"host", L"direct_access_enabled", config_.direct_access_enabled ? L"1" : L"0");
  WriteIniString(L"host", L"direct_access_port", std::to_wstring(config_.direct_access_port));
  WriteIniString(L"host", L"preferred_codec", config_.preferred_codec.empty() ? L"h264" : config_.preferred_codec);
  WriteIniString(L"host", L"video_fps", std::to_wstring(config_.video_fps));
  WriteIniString(L"host", L"video_bitrate_kbps", std::to_wstring(config_.video_bitrate_kbps));
  WriteIniString(L"host", L"public_key_hex", public_key_hex_);
  WriteIniString(L"host", L"secret_key_hex", secret_key_hex_);
  WriteIniString(L"host", L"device_uuid", device_uuid_text_);
  WritePrivateProfileStringW(L"host", L"placeholder_public_key_hex", nullptr, config_.config_path.c_str());
}

void PortableHostApp::EnsureDefaultLanguageFiles() const {
  const std::wstring tw_path = config_.exe_dir + L"\\tw.txt";
  if (!FileExists(tw_path)) {
    WriteUtf8TextFile(tw_path, BuildDefaultLanguageFileContent(true));
  }

  const std::wstring en_path = config_.exe_dir + L"\\en.txt";
  if (!FileExists(en_path)) {
    WriteUtf8TextFile(en_path, BuildDefaultLanguageFileContent(false));
  }
}

void PortableHostApp::LoadLanguageStrings() {
  language_strings_.clear();
  language_base_is_traditional_ =
      _wcsicmp(GetFileNamePart(config_.language_file).c_str(), L"en.txt") != 0;

  std::wstring language_text;
  if (ReadUtf8TextFile(BuildLanguageFilePath(config_.language_file), &language_text)) {
    ParseLanguageFileText(
        language_text,
        &language_strings_,
        &language_base_is_traditional_);
  }
}

std::wstring PortableHostApp::GetText(
    const wchar_t* key,
    const wchar_t* fallback) const {
  if (key != nullptr) {
    const auto found = language_strings_.find(key);
    if (found != language_strings_.end() && !found->second.empty()) {
      return found->second;
    }
    const wchar_t* builtin = LookupBuiltinLanguageEntry(language_base_is_traditional_, key);
    if (builtin != nullptr && *builtin != L'\0') {
      return builtin;
    }
  }
  return fallback != nullptr ? std::wstring(fallback) : std::wstring();
}

std::wstring PortableHostApp::FormatText(
    const wchar_t* key,
    const wchar_t* fallback,
    const std::wstring& arg0) const {
  std::wstring text = GetText(key, fallback);
  ReplaceAllSubstrings(&text, L"{0}", arg0);
  return text;
}

std::wstring PortableHostApp::LocalizeRendezvousStatusText(
    const std::wstring& text) const {
  if (text.empty()) {
    return text;
  }

  auto localize_exact = [&](const wchar_t* source,
                            const wchar_t* key,
                            const wchar_t* fallback,
                            std::wstring* localized) -> bool {
    if (_wcsicmp(text.c_str(), source) != 0) {
      return false;
    }
    *localized = GetText(key, fallback);
    return true;
  };

  auto localize_prefix = [&](const wchar_t* source_prefix,
                             const wchar_t* key,
                             const wchar_t* fallback,
                             std::wstring* localized) -> bool {
    const std::wstring prefix = source_prefix;
    if (!StartsWithInsensitive(text, prefix)) {
      return false;
    }
    *localized = FormatText(key, fallback, text.substr(prefix.size()));
    return true;
  };

  auto localize_wrapped_suffix = [&](const wchar_t* source_prefix,
                                     const wchar_t* key,
                                     const wchar_t* fallback,
                                     std::wstring* localized) -> bool {
    const std::wstring prefix = source_prefix;
    if (!StartsWithInsensitive(text, prefix) || text.size() <= prefix.size() ||
        text.back() != L')') {
      return false;
    }
    *localized =
        FormatText(key, fallback, text.substr(prefix.size(), text.size() - prefix.size() - 1));
    return true;
  };

  std::wstring localized;
  if (localize_exact(
          L"failed to start rendezvous worker thread",
          L"status_rendezvous_worker_start_failed",
          L"Failed to start the rendezvous worker thread.",
          &localized) ||
      localize_exact(
          L"failed to start active session worker thread",
          L"status_active_session_worker_start_failed",
          L"Failed to start the active session worker thread.",
          &localized) ||
      localize_exact(
          L"id_server is empty",
          L"status_id_server_empty",
          L"id_server is empty",
          &localized) ||
      localize_exact(
          L"hbbs tcp timeout",
          L"status_hbbs_tcp_timeout",
          L"hbbs TCP timeout",
          &localized) ||
      localize_exact(
          L"hbbs tcp closed",
          L"status_hbbs_tcp_closed",
          L"hbbs TCP closed the connection",
          &localized) ||
      localize_exact(
          L"hbbs requested RegisterPk over tcp",
          L"status_hbbs_request_pk_tcp",
          L"hbbs requested RegisterPk over TCP",
          &localized) ||
      localize_exact(
          L"registered to hbbs via tcp",
          L"status_registered_tcp",
          L"Registered to hbbs via TCP",
          &localized) ||
      localize_exact(
          L"RegisterPeerResponse received over tcp",
          L"status_register_peer_response_tcp",
          L"RegisterPeerResponse received over TCP",
          &localized) ||
      localize_exact(
          L"UUID_MISMATCH from hbbs over tcp",
          L"status_uuid_mismatch_tcp",
          L"UUID_MISMATCH from hbbs over TCP",
          &localized) ||
      localize_exact(
          L"ID_EXISTS from hbbs over tcp",
          L"status_id_exists_tcp",
          L"ID_EXISTS from hbbs over TCP",
          &localized) ||
      localize_exact(
          L"NOT_DEPLOYED from hbbs over tcp",
          L"status_not_deployed_tcp",
          L"NOT_DEPLOYED from hbbs over TCP",
          &localized) ||
      localize_exact(
          L"failed to queue RequestRelay tcp session",
          L"status_request_relay_tcp_queue_failed",
          L"Failed to queue the RequestRelay TCP session",
          &localized) ||
      localize_exact(
          L"failed to queue PunchHole tcp relay session",
          L"status_punch_hole_tcp_queue_failed",
          L"Failed to queue the PunchHole TCP relay session",
          &localized) ||
      localize_exact(
          L"failed to queue FetchLocalAddr tcp relay fallback",
          L"status_fetch_local_addr_tcp_relay_queue_failed",
          L"Failed to queue the FetchLocalAddr TCP relay fallback",
          &localized) ||
      localize_exact(
          L"failed to queue FetchLocalAddr tcp direct/relay session",
          L"status_fetch_local_addr_tcp_direct_queue_failed",
          L"Failed to queue the FetchLocalAddr TCP direct/relay session",
          &localized) ||
      localize_exact(
          L"hbbs udp no response before ready; reconnecting",
          L"status_hbbs_udp_no_response_reconnecting",
          L"hbbs gave no UDP response before ready; reconnecting",
          &localized) ||
      localize_exact(
          L"hbbs udp timeout",
          L"status_hbbs_udp_timeout",
          L"hbbs UDP timeout",
          &localized) ||
      localize_exact(
          L"hbbs requested RegisterPk over udp",
          L"status_hbbs_request_pk_udp",
          L"hbbs requested RegisterPk over UDP",
          &localized) ||
      localize_exact(
          L"registered to hbbs via udp",
          L"status_registered_udp",
          L"Registered to hbbs via UDP",
          &localized) ||
      localize_exact(
          L"RegisterPeerResponse received over udp",
          L"status_register_peer_response_udp",
          L"RegisterPeerResponse received over UDP",
          &localized) ||
      localize_exact(
          L"UUID_MISMATCH from hbbs",
          L"status_uuid_mismatch_udp",
          L"UUID_MISMATCH from hbbs",
          &localized) ||
      localize_exact(
          L"ID_EXISTS from hbbs",
          L"status_id_exists_udp",
          L"ID_EXISTS from hbbs",
          &localized) ||
      localize_exact(
          L"NOT_DEPLOYED from hbbs",
          L"status_not_deployed_udp",
          L"NOT_DEPLOYED from hbbs",
          &localized) ||
      localize_exact(
          L"failed to queue relay session",
          L"status_relay_session_queue_failed",
          L"Failed to queue the relay session",
          &localized) ||
      localize_exact(
          L"failed to queue punch-hole relay fallback",
          L"status_punch_hole_relay_queue_failed",
          L"Failed to queue the punch-hole relay fallback",
          &localized) ||
      localize_exact(
          L"failed to queue FetchLocalAddr relay fallback",
          L"status_fetch_local_addr_relay_queue_failed",
          L"Failed to queue the FetchLocalAddr relay fallback",
          &localized) ||
      localize_exact(
          L"failed to queue FetchLocalAddr direct/relay session",
          L"status_fetch_local_addr_direct_queue_failed",
          L"Failed to queue the FetchLocalAddr direct/relay session",
          &localized) ||
      localize_exact(
          L"online, FetchLocalAddr over tcp requested relay fallback",
          L"status_online_fetch_local_addr_tcp_relay",
          L"Online, FetchLocalAddr over TCP requested relay fallback",
          &localized)) {
    return localized;
  }

  if (localize_prefix(
          L"identity setup failed: ",
          L"status_identity_setup_failed",
          L"Identity setup failed: {0}",
          &localized) ||
      localize_prefix(
          L"connecting udp ",
          L"status_connecting_udp",
          L"Connecting over UDP to {0}",
          &localized) ||
      localize_prefix(
          L"udp connect failed: ",
          L"status_udp_connect_failed",
          L"UDP connection failed: {0}",
          &localized) ||
      localize_prefix(
          L"RegisterPk udp send failed: ",
          L"status_register_pk_udp_send_failed",
          L"RegisterPk UDP send failed: {0}",
          &localized) ||
      localize_prefix(
          L"RegisterPeer udp send failed: ",
          L"status_register_peer_udp_send_failed",
          L"RegisterPeer UDP send failed: {0}",
          &localized) ||
      localize_prefix(
          L"connecting tcp ",
          L"status_connecting_tcp",
          L"Connecting over TCP to {0}",
          &localized) ||
      localize_prefix(
          L"tcp connect failed: ",
          L"status_tcp_connect_failed",
          L"TCP connection failed: {0}",
          &localized) ||
      localize_prefix(
          L"tcp secure handshake failed: ",
          L"status_tcp_secure_handshake_failed",
          L"TCP secure handshake failed: {0}",
          &localized) ||
      localize_prefix(
          L"RegisterPk tcp send failed: ",
          L"status_register_pk_tcp_send_failed",
          L"RegisterPk TCP send failed: {0}",
          &localized) ||
      localize_prefix(
          L"tcp receive failed: ",
          L"status_tcp_receive_failed",
          L"TCP receive failed: {0}",
          &localized) ||
      localize_prefix(
          L"UUID_MISMATCH from hbbs over tcp; retrying with new ID ",
          L"status_uuid_mismatch_tcp_retry",
          L"UUID_MISMATCH from hbbs over TCP; retrying with new ID {0}",
          &localized) ||
      localize_prefix(
          L"RegisterPkResponse over tcp ",
          L"status_register_pk_response_tcp",
          L"RegisterPkResponse over TCP: {0}",
          &localized) ||
      localize_prefix(
          L"received unhandled hbbs tcp message fields=",
          L"status_unhandled_hbbs_tcp_message",
          L"Received an unhandled hbbs TCP message: {0}",
          &localized) ||
      localize_prefix(
          L"udp receive failed: ",
          L"status_udp_receive_failed",
          L"UDP receive failed: {0}",
          &localized) ||
      localize_prefix(
          L"UUID_MISMATCH from hbbs; retrying with new ID ",
          L"status_uuid_mismatch_udp_retry",
          L"UUID_MISMATCH from hbbs; retrying with new ID {0}",
          &localized) ||
      localize_prefix(
          L"RegisterPkResponse ",
          L"status_register_pk_response_udp",
          L"RegisterPkResponse: {0}",
          &localized) ||
      localize_prefix(
          L"received unhandled hbbs udp message fields=",
          L"status_unhandled_hbbs_udp_message",
          L"Received an unhandled hbbs UDP message: {0}",
          &localized) ||
      localize_prefix(
          L"RequestRelay tcp session failed: ",
          L"status_request_relay_tcp_session_failed",
          L"RequestRelay TCP session failed: {0}",
          &localized) ||
      localize_prefix(
          L"PunchHole tcp relay failed: ",
          L"status_punch_hole_tcp_relay_failed",
          L"PunchHole TCP relay failed: {0}",
          &localized) ||
      localize_prefix(
          L"FetchLocalAddr tcp relay failed: ",
          L"status_fetch_local_addr_tcp_relay_failed",
          L"FetchLocalAddr TCP relay failed: {0}",
          &localized) ||
      localize_prefix(
          L"FetchLocalAddr tcp direct+relay failed: ",
          L"status_fetch_local_addr_tcp_direct_failed",
          L"FetchLocalAddr TCP direct/relay failed: {0}",
          &localized) ||
      localize_prefix(
          L"relay session setup failed: ",
          L"status_relay_session_setup_failed",
          L"Relay session setup failed: {0}",
          &localized) ||
      localize_prefix(
          L"punch-hole relay fallback failed: ",
          L"status_punch_hole_relay_fallback_failed",
          L"Punch-hole relay fallback failed: {0}",
          &localized) ||
      localize_prefix(
          L"FetchLocalAddr relay fallback failed: ",
          L"status_fetch_local_addr_relay_fallback_failed",
          L"FetchLocalAddr relay fallback failed: {0}",
          &localized) ||
      localize_prefix(
          L"FetchLocalAddr direct+relay failed: ",
          L"status_fetch_local_addr_direct_failed",
          L"FetchLocalAddr direct/relay failed: {0}",
          &localized)) {
    return localized;
  }

  if (localize_wrapped_suffix(
          L"online, relay request received from hbbs over tcp; opening secure relay session (",
          L"status_online_request_relay_tcp_secure",
          L"Online, relay request received from hbbs over TCP; opening secure relay session ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, relay request received from hbbs over tcp; opening plain relay session (",
          L"status_online_request_relay_tcp_plain",
          L"Online, relay request received from hbbs over TCP; opening plain relay session ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, PunchHole requested relay over tcp (",
          L"status_online_punch_hole_tcp_force_relay",
          L"Online, PunchHole requested relay over TCP ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, PunchHole received from hbbs over tcp; opening relay path (",
          L"status_online_punch_hole_tcp_relay",
          L"Online, PunchHole received from hbbs over TCP; opening relay path ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, FetchLocalAddr received from hbbs over tcp; trying direct intranet path (",
          L"status_online_fetch_local_addr_tcp_direct",
          L"Online, FetchLocalAddr received from hbbs over TCP; trying direct intranet path ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, relay request received from hbbs; opening secure relay session (",
          L"status_online_request_relay_udp_secure",
          L"Online, relay request received from hbbs; opening secure relay session ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, relay request received from hbbs; opening plain relay session (",
          L"status_online_request_relay_udp_plain",
          L"Online, relay request received from hbbs; opening plain relay session ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, punch hole request received from hbbs; switching to relay fallback (",
          L"status_online_punch_hole_udp_relay",
          L"Online, punch-hole request received from hbbs; switching to relay fallback ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, FetchLocalAddr received from hbbs; force relay enabled, switching directly to relay (",
          L"status_online_fetch_local_addr_udp_relay",
          L"Online, FetchLocalAddr received from hbbs; force relay enabled, switching directly to relay ({0})",
          &localized) ||
      localize_wrapped_suffix(
          L"online, FetchLocalAddr received from hbbs; trying direct intranet path (",
          L"status_online_fetch_local_addr_udp_direct",
          L"Online, FetchLocalAddr received from hbbs; trying direct intranet path ({0})",
          &localized)) {
    return localized;
  }

  return text;
}

std::wstring PortableHostApp::BuildLanguageFilePath(
    const std::wstring& language_file) const {
  std::wstring normalized = NormalizePathSeparators(Trim(language_file));
  if (normalized.empty()) {
    normalized = GetDefaultLanguageFile();
  }
  if (IsAbsoluteWindowsPath(normalized)) {
    return normalized;
  }
  return config_.exe_dir + L"\\" + GetFileNamePart(normalized);
}

std::wstring PortableHostApp::GetDefaultLanguageFile() const {
  LANGID language_id = GetUserDefaultUILanguage();
  if (language_id == 0) {
    language_id = GetSystemDefaultUILanguage();
  }
  return IsTraditionalChineseLanguageId(language_id) ? L"tw.txt" : L"en.txt";
}

std::wstring PortableHostApp::NormalizeLanguageFileSelection(
    const std::wstring& selected_path) const {
  std::wstring normalized = NormalizePathSeparators(Trim(selected_path));
  if (normalized.empty()) {
    return std::wstring();
  }
  if (!IsAbsoluteWindowsPath(normalized)) {
    return GetFileNamePart(normalized);
  }

  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD size = 0;
  while (true) {
    size = GetFullPathNameW(
        normalized.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (size == 0) {
      break;
    }
    if (size < buffer.size()) {
      normalized.assign(buffer.data(), size);
      break;
    }
    buffer.resize(size + 1);
  }

  const std::wstring exe_prefix = NormalizePathSeparators(config_.exe_dir) + L"\\";
  if (StartsWithInsensitive(normalized, exe_prefix)) {
    return GetFileNamePart(normalized);
  }
  return normalized;
}

bool PortableHostApp::RestartApplication() {
  const std::wstring executable_path = GetExecutablePath();
  if (executable_path.empty()) {
    return false;
  }

  wchar_t system_directory[MAX_PATH] = {};
  UINT system_directory_length = GetSystemDirectoryW(
      system_directory, static_cast<UINT>(_countof(system_directory)));
  if (system_directory_length == 0 ||
      system_directory_length >= _countof(system_directory)) {
    return false;
  }

  const std::wstring cmd_path = std::wstring(system_directory) + L"\\cmd.exe";
  const std::wstring command_line =
      L"cmd.exe /c ping 127.0.0.1 -n 2 >nul && start \"\" \"" +
      executable_path + L"\"";
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW;
  startup_info.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process_info = {};
  const BOOL started = CreateProcessW(
      cmd_path.c_str(),
      mutable_command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      config_.exe_dir.empty() ? nullptr : config_.exe_dir.c_str(),
      &startup_info,
      &process_info);
  if (!started) {
    return false;
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  if (window_ != nullptr) {
    DestroyWindow(window_);
  }
  return true;
}

void PortableHostApp::RefreshPassword() {
  if (!config_.random_password_enabled) {
    temporary_password_.clear();
    return;
  }
  temporary_password_ = GenerateNumericPassword(config_.temporary_password_length);
}

void PortableHostApp::RefreshServerState() {
  const ParsedHostPort id_server = ParseHostPort(config_.id_server, kDefaultIdServerPort);
  if (!winsock_ready_ || id_server.host.empty()) {
    server_state_ = ServerState::kUnknown;
    return;
  }

  server_state_ =
      CanReachTcpHost(id_server.host, id_server.port, 1800) ? ServerState::kReachable : ServerState::kUnreachable;
}

void PortableHostApp::RefreshUiText() {
  if (window_ == nullptr) {
    return;
  }

  const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
  SetWindowTextW(window_, app_title.c_str());
  SetWindowTextW(title_label_, GetText(L"welcome_title", L"\u6b61\u8fce\u4f7f\u7528\u9060\u7aef\u5354\u52a9").c_str());
  SetWindowTextW(subtitle_label_, L"");
  SetWindowTextW(
      hint_label_,
      GetText(L"hint_provide_id_password", L"\u8acb\u63d0\u4f9b\u4e0b\u9762\u7684 ID \u53ca\u5bc6\u78bc").c_str());
  SetWindowTextW(id_label_, GetText(L"label_id", L"ID").c_str());
  SetWindowTextW(id_value_, FormatDisplayHostId(config_.host_id).c_str());
  SetWindowTextW(password_label_, GetText(L"label_one_time_password", L"\u4e00\u6b21\u6027\u5bc6\u78bc").c_str());
  SetWindowTextW(
      password_value_,
      config_.random_password_enabled ? temporary_password_.c_str() : L"------");
  SetWindowTextW(refresh_password_button_, L"");
  EnableWindow(refresh_password_button_, config_.random_password_enabled ? TRUE : FALSE);
  SetWindowTextW(
      disconnect_button_,
      GetText(L"button_disconnect_session", L"\u4e2d\u65b7\u9023\u7dda").c_str());
  EnableWindow(disconnect_button_, active_session_connected_.load() ? TRUE : FALSE);
  SetWindowTextW(server_status_label_, L"\u25cf");
  const bool registered = IsRendezvousRegistered();
  const std::wstring rendezvous_status = GetRendezvousStatusText();
  std::wstring server_status;
  if (registered) {
    server_status = GetText(L"status_server_ready", L"\u4f3a\u670d\u5668\u5df2\u5c31\u7dd2");
  } else if (!rendezvous_status.empty()) {
    server_status = rendezvous_status;
  } else if (server_state_ == ServerState::kReachable) {
    server_status = GetText(L"status_server_connecting", L"\u6b63\u5728\u9023\u7dda\u4f3a\u670d\u5668");
  } else if (server_state_ == ServerState::kUnreachable) {
    server_status = GetText(L"status_server_unreachable", L"\u4f3a\u670d\u5668\u7121\u6cd5\u9023\u7dda");
  } else {
    server_status = GetText(L"status_server_checking", L"\u6b63\u5728\u6aa2\u67e5\u4f3a\u670d\u5668");
  }
  SetWindowTextW(server_value_label_, server_status.c_str());
  SetWindowTextW(config_path_label_, L"");

  if (tray_menu_ != nullptr) {
    const std::wstring tray_show = GetText(L"tray_show_main", L"\u986f\u793a\u4e3b\u9801");
    const std::wstring tray_exit = GetText(L"tray_exit", L"\u96e2\u958b");
    ModifyMenuW(
        tray_menu_,
        kTrayMenuShowWindow,
        MF_BYCOMMAND | MF_STRING,
        kTrayMenuShowWindow,
        tray_show.c_str());
    ModifyMenuW(
        tray_menu_,
        kTrayMenuExit,
        MF_BYCOMMAND | MF_STRING,
        kTrayMenuExit,
        tray_exit.c_str());
  }
  if (tray_icon_added_) {
    AddTrayIcon();
  }
  if (incoming_approval_window_ != nullptr) {
    SetWindowTextW(
        incoming_approval_window_,
        GetText(L"incoming_window_title", L"\u9060\u7aef\u9023\u5165").c_str());
    if (incoming_approval_dismiss_button_ != nullptr) {
      SetWindowTextW(
          incoming_approval_dismiss_button_,
          GetText(L"incoming_dismiss", L"\u95dc\u9589").c_str());
    }
    if (incoming_approval_accept_button_ != nullptr) {
      SetWindowTextW(
          incoming_approval_accept_button_,
          GetText(L"incoming_accept", L"\u63a5\u53d7").c_str());
    }
    InvalidateRect(incoming_approval_window_, nullptr, TRUE);
  }
  InvalidateConnectionStatusCard();
}

void PortableHostApp::ShowOptionsMenu() {
  if (window_ == nullptr || options_button_ == nullptr) {
    return;
  }

  HMENU options_menu = CreatePopupMenu();
  if (options_menu == nullptr) {
    return;
  }

  AppendMenuW(
      options_menu,
      MF_STRING | (IsLaunchOnStartupEnabled() ? MF_CHECKED : 0),
      kOptionsMenuLaunchOnStartup,
      GetText(L"menu_launch_on_startup", L"\u958b\u6a5f\u555f\u52d5").c_str());
  AppendMenuW(
      options_menu,
      MF_STRING,
      kOptionsMenuSetFixedPassword,
      GetText(L"menu_set_fixed_password", L"\u8a2d\u5b9a\u56fa\u5b9a\u5bc6\u78bc").c_str());
  AppendMenuW(
      options_menu,
      MF_STRING | (!config_.random_password_enabled ? MF_CHECKED : 0),
      kOptionsMenuDisableRandomPassword,
      GetText(L"menu_disable_random_password", L"\u505c\u7528\u96a8\u6a5f\u5bc6\u78bc").c_str());
  AppendMenuW(
      options_menu,
      MF_STRING,
      kOptionsMenuChangeId,
      GetText(L"menu_change_id", L"\u66f4\u6539 ID").c_str());
  AppendMenuW(
      options_menu,
      MF_STRING,
      kOptionsMenuLanguage,
      GetText(L"menu_language", L"\u8a9e\u8a00").c_str());
  AppendMenuW(
      options_menu,
      MF_STRING,
      kOptionsMenuAbout,
      GetText(L"menu_about", L"\u95dc\u65bc").c_str());

  RECT button_rect = {};
  GetWindowRect(options_button_, &button_rect);
  SetForegroundWindow(window_);
  TrackPopupMenu(
      options_menu,
      TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
      button_rect.left,
      button_rect.bottom,
      0,
      window_,
      nullptr);
  DestroyMenu(options_menu);
  PostMessageW(window_, WM_NULL, 0, 0);
}

void PortableHostApp::ToggleLaunchOnStartup() {
  const bool enabled = IsLaunchOnStartupEnabled();
  if (!SetLaunchOnStartupEnabled(!enabled)) {
    const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
    MessageBoxW(
        window_,
        GetText(L"msg_startup_update_failed", L"\u7121\u6cd5\u66f4\u65b0\u958b\u6a5f\u555f\u52d5\u8a2d\u5b9a\u3002").c_str(),
        app_title.c_str(),
        MB_OK | MB_ICONWARNING);
  }
}

void PortableHostApp::ToggleRandomPassword() {
  config_.random_password_enabled = !config_.random_password_enabled;
  SaveConfig();
  RefreshPassword();
  StartRendezvousWorker();
  RefreshUiText();
}

void PortableHostApp::ConfigureFixedPassword() {
  std::wstring password;
  if (!PromptForTextInput(
          instance_,
          window_,
          font_body_,
          GetText(L"dialog_fixed_password_title", L"\u8a2d\u5b9a\u56fa\u5b9a\u5bc6\u78bc"),
          GetText(L"dialog_fixed_password_prompt", L"\u8acb\u8f38\u5165\u8981\u5132\u5b58\u7684\u56fa\u5b9a\u5bc6\u78bc"),
          L"",
          true,
          64,
          GetText(L"dialog_ok", L"\u78ba\u5b9a"),
          GetText(L"dialog_cancel", L"\u53d6\u6d88"),
          &password)) {
    return;
  }

  if (Trim(password).empty()) {
    const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
    MessageBoxW(
        window_,
        GetText(L"msg_fixed_password_empty", L"\u56fa\u5b9a\u5bc6\u78bc\u4e0d\u53ef\u70ba\u7a7a\u767d\u3002").c_str(),
        app_title.c_str(),
        MB_OK | MB_ICONWARNING);
    return;
  }

  if (!SetFixedPassword(password)) {
    const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
    MessageBoxW(
        window_,
        GetText(L"msg_fixed_password_save_failed", L"\u56fa\u5b9a\u5bc6\u78bc\u5132\u5b58\u5931\u6557\u3002").c_str(),
        app_title.c_str(),
        MB_OK | MB_ICONWARNING);
    return;
  }

  if (!config_.random_password_enabled) {
    RefreshPassword();
  }
  StartRendezvousWorker();
  RefreshUiText();
}

void PortableHostApp::ConfigureHostId() {
  std::wstring input;
  if (!PromptForTextInput(
          instance_,
          window_,
          font_body_,
          GetText(L"dialog_change_id_title", L"\u66f4\u6539 ID"),
          GetText(L"dialog_change_id_prompt", L"\u8acb\u8f38\u5165\u65b0\u7684 ID\uff08\u50c5\u9650\u6578\u5b57\uff09"),
          config_.host_id,
          false,
          18,
          GetText(L"dialog_ok", L"\u78ba\u5b9a"),
          GetText(L"dialog_cancel", L"\u53d6\u6d88"),
          &input)) {
    return;
  }

  std::wstring normalized;
  normalized.reserve(input.size());
  for (wchar_t c : input) {
    if (c >= L'0' && c <= L'9') {
      normalized.push_back(c);
    } else if (c == L' ' || c == L'-') {
      continue;
    } else {
      const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
      MessageBoxW(
          window_,
          GetText(L"msg_id_digits_only", L"ID \u50c5\u80fd\u8f38\u5165\u6578\u5b57\u3002").c_str(),
          app_title.c_str(),
          MB_OK | MB_ICONWARNING);
      return;
    }
  }

  if (normalized.size() < 6 || normalized.size() > 16) {
    const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
    MessageBoxW(
        window_,
        GetText(L"msg_id_length_invalid", L"ID \u9577\u5ea6\u9700\u8981\u5728 6 \u5230 16 \u78bc\u4e4b\u9593\u3002").c_str(),
        app_title.c_str(),
        MB_OK | MB_ICONWARNING);
    return;
  }

  if (normalized == config_.host_id) {
    return;
  }

  config_.host_id = normalized;
  SaveConfig();
  StartRendezvousWorker();
  RefreshUiText();
}

void PortableHostApp::ConfigureLanguage() {
  wchar_t selected_path[MAX_PATH] = {};
  OPENFILENAMEW open_file = {};
  open_file.lStructSize = sizeof(open_file);
  open_file.hwndOwner = window_;
  open_file.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
  open_file.lpstrFile = selected_path;
  open_file.nMaxFile = static_cast<DWORD>(_countof(selected_path));
  open_file.lpstrInitialDir = config_.exe_dir.c_str();
  const std::wstring dialog_title =
      GetText(L"dialog_language_title", L"\u9078\u64c7\u8a9e\u8a00\u6a94");
  open_file.lpstrTitle = dialog_title.c_str();
  open_file.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  open_file.lpstrDefExt = L"txt";
  if (!GetOpenFileNameW(&open_file)) {
    return;
  }

  const std::wstring normalized = NormalizeLanguageFileSelection(selected_path);
  if (normalized.empty() ||
      _wcsicmp(normalized.c_str(), config_.language_file.c_str()) == 0) {
    return;
  }

  config_.language_file = normalized;
  LoadLanguageStrings();
  SaveConfig();
  if (!RestartApplication()) {
    RefreshUiText();
    const std::wstring app_title = GetText(L"app_window_title", kAppWindowTitle);
    MessageBoxW(
        window_,
        GetText(L"msg_language_restart_failed", L"\u8a9e\u8a00\u6a94\u5df2\u5957\u7528\uff0c\u4f46\u7a0b\u5f0f\u7121\u6cd5\u81ea\u52d5\u91cd\u65b0\u555f\u52d5\u3002").c_str(),
        app_title.c_str(),
        MB_OK | MB_ICONWARNING);
  }
}

void PortableHostApp::ShowAboutDialog() const {
  const std::wstring dialog_title = GetText(L"menu_about", L"\u95dc\u65bc");
  const std::wstring about_text = language_base_is_traditional_
      ? std::wstring(kAppWindowTitle) +
            L"\r\n\u7248\u672c\uff1a" + kAboutDisplayVersion +
            L"\r\n\r\n"
            L"\u8f15\u91cf\u5316 RustDesk \u76f8\u5bb9 Host-only \u88ab\u63a7\u7aef\u5de5\u5177\r\n"
            L"\u652f\u63f4 Windows XP / 7 / 10 / 11 \u8207 WinPE \u74b0\u5883\r\n\r\n"
            L"Copyright \u00A9 2025-2026 Terence0816\r\n\r\n"
            L"\u6388\u6b0a\uff1aGPL-3.0\r\n"
            L"\u5c08\u6848\u7db2\u5740\uff1a\r\n" +
            std::wstring(kProjectUrl) + L"\r\n\r\n" +
            L"\u672c\u5c08\u6848\u70ba\u7368\u7acb\u958b\u767c\u7684 RustDesk \u76f8\u5bb9\u5be6\u4f5c\uff0c\r\n"
            L"\u4e26\u975e RustDesk \u5b98\u65b9\u7528\u6236\u7aef\u3002"
      : std::wstring(kAppWindowTitle) +
            L"\r\nVersion " + kAboutDisplayVersion +
            L"\r\n\r\n"
            L"A lightweight RustDesk-compatible host-only client for Windows XP / 7 / 10 / 11 and WinPE.\r\n\r\n"
            L"Copyright \u00A9 2025-2026 Terence0816\r\n\r\n"
            L"License: GPL-3.0\r\n"
            L"Project:\r\n" +
            std::wstring(kProjectUrl) + L"\r\n\r\n" +
            L"This project is an independent RustDesk-compatible implementation.\r\n"
            L"It is not an official RustDesk client.";
  ShowAboutDialogModal(
      instance_,
      window_,
      font_body_ != nullptr ? font_body_ : font_button_,
      window_icon_small_ != nullptr ? window_icon_small_ : window_icon_large_,
      dialog_title,
      about_text,
      GetText(L"dialog_ok", L"\u78ba\u5b9a"),
      GetText(L"button_check_updates", L"\u6aa2\u67e5\u66f4\u65b0"),
      GetText(L"msg_open_project_failed", L"\u7121\u6cd5\u958b\u555f\u5c08\u6848\u7db2\u5740\u3002"),
      kProjectUrl);
}

void PortableHostApp::StartRendezvousWorker() {
  AppendPortableHostLog(L"session", L"StartRendezvousWorker requested");
  StopRendezvousWorker();
  stop_rendezvous_.store(false);
  stop_active_session_.store(false);
  stop_auxiliary_session_.store(false);
  active_session_manual_close_requested_.store(false);
  active_session_running_.store(false);
  auxiliary_session_running_.store(false);
  active_session_connected_.store(false);
  desktop_session_count_.store(0);
  active_session_generation_.store(0);
  {
    Win32LockGuard guard(active_session_mutex_);
    active_session_connection_ = nullptr;
    pending_session_runner_ = std::function<bool(std::wstring*)>();
    pending_session_starting_status_.clear();
    pending_session_failure_prefix_.clear();
    pending_session_registered_ = false;
    pending_session_requested_ = false;
    pending_session_generation_ = 0;
  }
  {
    Win32LockGuard guard(auxiliary_session_mutex_);
    auxiliary_session_connection_ = nullptr;
  }
  ClearActiveSessionIdentity();
  if (!rendezvous_thread_.Start([this]() { RendezvousWorker(); })) {
    AppendPortableHostLog(L"session", L"failed to start rendezvous worker thread");
    SetRendezvousStatus(L"failed to start rendezvous worker thread", false);
  } else {
    AppendPortableHostLog(L"session", L"rendezvous worker thread started");
  }
}

void PortableHostApp::StopRendezvousWorker() {
  AppendPortableHostLog(L"session", L"StopRendezvousWorker requested");
  stop_rendezvous_.store(true);
  StopActiveSession();
  StopAuxiliarySession();
  {
    Win32LockGuard guard(active_session_mutex_);
    pending_session_runner_ = std::function<bool(std::wstring*)>();
    pending_session_starting_status_.clear();
    pending_session_failure_prefix_.clear();
    pending_session_registered_ = false;
    pending_session_requested_ = false;
    pending_session_generation_ = 0;
  }
  {
    Win32LockGuard guard(active_session_thread_mutex_);
    if (active_session_thread_.Joinable()) {
      active_session_thread_.Join();
    }
  }
  if (rendezvous_thread_.Joinable()) {
    rendezvous_thread_.Join();
  }
  AppendPortableHostLog(L"session", L"StopRendezvousWorker completed");
}

void PortableHostApp::StopActiveSession(bool notify_peer) {
  AppendPortableHostLog(
      L"session",
      L"StopActiveSession notify_peer=" + BoolToLogText(notify_peer) +
          L", active_running=" + BoolToLogText(active_session_running_.load()) +
          L", active_connected=" + BoolToLogText(active_session_connected_.load()));
  stop_active_session_.store(true);
  active_session_manual_close_requested_.store(notify_peer);
  active_session_connected_.store(false);
  ClearActiveSessionIdentity();
  StopAuxiliarySession();

  if (notify_peer) {
    return;
  }

  TcpFramedConnection* connection = nullptr;
  {
    Win32LockGuard guard(active_session_mutex_);
    connection = reinterpret_cast<TcpFramedConnection*>(active_session_connection_);
  }
  if (connection != nullptr) {
    AppendPortableHostLog(L"session", L"aborting active session connection");
    connection->Abort();
  }
}

void PortableHostApp::StopAuxiliarySession() {
  const bool was_running = auxiliary_session_running_.load();
  if (!was_running) {
    Win32LockGuard thread_guard(auxiliary_session_thread_mutex_);
    if (!auxiliary_session_thread_.Joinable()) {
      return;
    }
  }

  AppendPortableHostLog(
      L"session",
      L"StopAuxiliarySession requested, running=" +
          BoolToLogText(was_running));
  stop_auxiliary_session_.store(true);

  TcpFramedConnection* connection = nullptr;
  {
    Win32LockGuard guard(auxiliary_session_mutex_);
    connection = reinterpret_cast<TcpFramedConnection*>(auxiliary_session_connection_);
  }
  if (connection != nullptr) {
    AppendPortableHostLog(L"session", L"aborting auxiliary session connection");
    connection->Abort();
  }

  {
    Win32LockGuard thread_guard(auxiliary_session_thread_mutex_);
    if (auxiliary_session_thread_.Joinable()) {
      auxiliary_session_thread_.Join();
    }
  }
  auxiliary_session_running_.store(false);
  ClearAuxiliarySessionConnection(nullptr);
}

void PortableHostApp::ActiveSessionWorker() {
  AppendPortableHostLog(L"session", L"ActiveSessionWorker started");
  while (!stop_rendezvous_.load()) {
    std::function<bool(std::wstring*)> runner;
    std::wstring starting_status;
    std::wstring failure_prefix;
    bool registered = false;
    unsigned long generation = 0;

    {
      Win32LockGuard guard(active_session_mutex_);
      if (pending_session_requested_) {
        runner = std::move(pending_session_runner_);
        pending_session_runner_ = std::function<bool(std::wstring*)>();
        starting_status = pending_session_starting_status_;
        failure_prefix = pending_session_failure_prefix_;
        registered = pending_session_registered_;
        generation = pending_session_generation_;
        pending_session_requested_ = false;
        stop_active_session_.store(false);
        active_session_manual_close_requested_.store(false);
        active_session_running_.store(true);
        active_session_connected_.store(false);
      }
    }

    if (!runner) {
      Sleep(20);
      continue;
    }

    if (generation == active_session_generation_.load()) {
      SetRendezvousStatus(starting_status, registered);
    }

    AppendPortableHostLog(
        L"session",
        L"runner starting generation=" + std::to_wstring(generation) +
            L", registered=" + BoolToLogText(registered) +
            L", starting_status=" + starting_status);
    std::wstring session_status;
    const bool session_ok = runner(&session_status);
    AppendPortableHostLog(
        L"session",
        L"runner finished generation=" + std::to_wstring(generation) +
            L", ok=" + BoolToLogText(session_ok) +
            L", status=" + session_status);

    active_session_running_.store(false);
    if (desktop_session_count_.load() <= 0) {
      active_session_connected_.store(false);
    }
    ClearActiveSessionConnection(nullptr);
    if (desktop_session_count_.load() <= 0) {
      ClearActiveSessionIdentity();
    }

    bool has_pending_session = false;
    {
      Win32LockGuard guard(active_session_mutex_);
      has_pending_session = pending_session_requested_;
    }

    if (!has_pending_session &&
        generation == active_session_generation_.load() &&
        !stop_rendezvous_.load()) {
      if (session_ok) {
        SetRendezvousStatus(
            session_status.empty() ? L"session ended" : session_status,
            registered);
      } else {
        SetRendezvousStatus(
            failure_prefix + (session_status.empty() ? L"unknown session error" : session_status),
            registered);
      }
    }
  }
  AppendPortableHostLog(L"session", L"ActiveSessionWorker exited");
}

void PortableHostApp::SetRendezvousStatus(const std::wstring& text, bool registered) {
  {
    Win32LockGuard guard(rendezvous_mutex_);
    rendezvous_status_text_ = LocalizeRendezvousStatusText(text);
    rendezvous_registered_ = registered;
  }
  if (window_ != nullptr) {
    PostMessageW(window_, kAppRendezvousStatus, 0, 0);
  }
}

std::wstring PortableHostApp::GetRendezvousStatusText() const {
  Win32LockGuard guard(rendezvous_mutex_);
  return rendezvous_status_text_;
}

bool PortableHostApp::IsRendezvousRegistered() const {
  Win32LockGuard guard(rendezvous_mutex_);
  return rendezvous_registered_;
}

bool PortableHostApp::HasActiveSession() const {
  return active_session_running_.load();
}

bool PortableHostApp::IsActiveSessionStopRequested() const {
  return stop_rendezvous_.load() || stop_active_session_.load();
}

bool PortableHostApp::HasAuxiliarySession() const {
  return auxiliary_session_running_.load();
}

bool PortableHostApp::IsAuxiliarySessionStopRequested() const {
  return stop_rendezvous_.load() || stop_active_session_.load() ||
         stop_auxiliary_session_.load();
}

void PortableHostApp::RegisterActiveSessionConnection(void* connection) {
  Win32LockGuard guard(active_session_mutex_);
  active_session_connection_ = connection;
}

void PortableHostApp::ClearActiveSessionConnection(void* connection) {
  Win32LockGuard guard(active_session_mutex_);
  if (connection == nullptr || active_session_connection_ == connection) {
    active_session_connection_ = nullptr;
  }
}

void PortableHostApp::RegisterAuxiliarySessionConnection(void* connection) {
  Win32LockGuard guard(auxiliary_session_mutex_);
  auxiliary_session_connection_ = connection;
}

void PortableHostApp::ClearAuxiliarySessionConnection(void* connection) {
  Win32LockGuard guard(auxiliary_session_mutex_);
  if (connection == nullptr || auxiliary_session_connection_ == connection) {
    auxiliary_session_connection_ = nullptr;
  }
}

bool PortableHostApp::StartActiveSessionThread(
    const std::wstring& starting_status,
    bool registered,
    const std::wstring& failure_prefix,
    std::function<bool(std::wstring*)> runner) {
  if (!runner) {
    return false;
  }

  const bool had_active_session = active_session_running_.load();
  bool had_pending_session = false;
  {
    Win32LockGuard guard(active_session_mutex_);
    had_pending_session = pending_session_requested_;
  }
  AppendPortableHostLog(
      L"session",
      L"StartActiveSessionThread starting_status=" + starting_status +
          L", registered=" + BoolToLogText(registered) +
          L", had_active_session=" + BoolToLogText(had_active_session) +
          L", had_pending_session=" + BoolToLogText(had_pending_session));

  const unsigned long generation = active_session_generation_.fetch_add(1) + 1;
  {
    Win32LockGuard guard(active_session_mutex_);
    pending_session_runner_ = std::move(runner);
    pending_session_starting_status_ = starting_status;
    pending_session_failure_prefix_ = failure_prefix;
    pending_session_registered_ = registered;
    pending_session_requested_ = true;
    pending_session_generation_ = generation;
  }
  SetRendezvousStatus(starting_status, registered);
  StopActiveSession();

  Win32LockGuard guard(active_session_thread_mutex_);
  if (!active_session_thread_.Joinable() &&
      !active_session_thread_.Start([this]() { ActiveSessionWorker(); })) {
    Win32LockGuard session_guard(active_session_mutex_);
    pending_session_runner_ = std::function<bool(std::wstring*)>();
    pending_session_starting_status_.clear();
    pending_session_failure_prefix_.clear();
    pending_session_registered_ = false;
    pending_session_requested_ = false;
    pending_session_generation_ = 0;
    SetRendezvousStatus(L"failed to start active session worker thread", registered);
    AppendPortableHostLog(L"session", L"failed to start active session worker thread");
    return false;
  }
  AppendPortableHostLog(
      L"session",
      L"StartActiveSessionThread queued generation=" + std::to_wstring(generation));
  return true;
}

bool PortableHostApp::StartAuxiliarySessionThread(
    const std::wstring& starting_status,
    bool registered,
    const std::wstring& failure_prefix,
    std::function<bool(std::wstring*)> runner) {
  if (!runner) {
    return false;
  }

  if (HasAuxiliarySession()) {
    AppendPortableHostLog(
        L"session",
        L"auxiliary session already running; ignoring new request: " + starting_status);
    return true;
  }

  AppendPortableHostLog(
      L"session",
      L"StartAuxiliarySessionThread starting_status=" + starting_status +
          L", registered=" + BoolToLogText(registered));
  StopAuxiliarySession();
  stop_auxiliary_session_.store(false);
  auxiliary_session_running_.store(true);

  Win32LockGuard guard(auxiliary_session_thread_mutex_);
  if (!auxiliary_session_thread_.Start(
          [this, starting_status, registered, failure_prefix, runner = std::move(runner)]() mutable {
            AppendPortableHostLog(
                L"session",
                L"auxiliary runner starting, registered=" +
                    BoolToLogText(registered) +
                    L", starting_status=" + starting_status);
            std::wstring session_status;
            const bool session_ok = runner(&session_status);
            AppendPortableHostLog(
                L"session",
                L"auxiliary runner finished, ok=" + BoolToLogText(session_ok) +
                    L", status=" + session_status);
            auxiliary_session_running_.store(false);
            ClearAuxiliarySessionConnection(nullptr);
            if (!stop_rendezvous_.load() &&
                !stop_active_session_.load() &&
                !stop_auxiliary_session_.load() &&
                !session_ok) {
              SetRendezvousStatus(
                  failure_prefix + (session_status.empty() ? L"unknown auxiliary session error"
                                                           : session_status),
                  registered);
            }
          })) {
    auxiliary_session_running_.store(false);
    ClearAuxiliarySessionConnection(nullptr);
    AppendPortableHostLog(L"session", L"failed to start auxiliary session worker thread");
    return false;
  }
  AppendPortableHostLog(L"session", L"auxiliary session thread started");
  return true;
}

std::wstring PortableHostApp::ReadIniString(
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* default_value) const {
  wchar_t buffer[1024] = {};
  GetPrivateProfileStringW(
      section,
      key,
      default_value,
      buffer,
      static_cast<DWORD>(_countof(buffer)),
      config_.config_path.c_str());
  return buffer;
}

void PortableHostApp::WriteIniString(
    const wchar_t* section,
    const wchar_t* key,
    const std::wstring& value) const {
  WritePrivateProfileStringW(section, key, value.c_str(), config_.config_path.c_str());
}

std::wstring PortableHostApp::GetExecutableDirectory() const {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD size = 0;

  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return L".";
    }
    if (size < buffer.size() - 1) {
      break;
    }
    buffer.resize(buffer.size() * 2);
  }

  std::wstring path(buffer.data(), size);
  const size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return L".";
  }
  return path.substr(0, separator);
}

std::wstring PortableHostApp::GetExecutablePath() const {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD size = 0;

  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return std::wstring();
    }
    if (size < buffer.size() - 1) {
      break;
    }
    buffer.resize(buffer.size() * 2);
  }

  return std::wstring(buffer.data(), size);
}

std::wstring PortableHostApp::BuildConfigPath(const std::wstring& exe_dir) const {
  return exe_dir + L"\\rustdesk_cpp_host.ini";
}

std::wstring PortableHostApp::GenerateRustDeskStyleId() const {
  // Mirrors the desktop ID logic in RustDesk:
  // libs/hbb_common/src/config.rs::get_auto_id()
  ULONG buffer_length = 0;
  if (GetAdaptersInfo(nullptr, &buffer_length) != ERROR_BUFFER_OVERFLOW || buffer_length == 0) {
    return GenerateFallbackDesktopId();
  }

  std::vector<unsigned char> buffer(buffer_length);
  auto* adapter = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
  if (GetAdaptersInfo(adapter, &buffer_length) != ERROR_SUCCESS) {
    return GenerateFallbackDesktopId();
  }

  for (IP_ADAPTER_INFO* current = adapter; current != nullptr; current = current->Next) {
    if (current->Type == MIB_IF_TYPE_LOOPBACK || current->AddressLength < 6) {
      continue;
    }

    bool all_zero = true;
    for (UINT index = 0; index < current->AddressLength; ++index) {
      if (current->Address[index] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      continue;
    }

    unsigned int id = 0;
    for (UINT index = 2; index < 6; ++index) {
      id = (id << 8U) | current->Address[index];
    }
    id &= 0x1FFFFFFFU;
    if (id != 0) {
      return std::to_wstring(id);
    }
  }

  return GenerateFallbackDesktopId();
}

std::wstring PortableHostApp::GenerateNumericPassword(int length) const {
  std::wstring value;
  value.reserve(length);
  for (int index = 0; index < length; ++index) {
    const unsigned int random_value = GetRandomUint32();
    value.push_back(static_cast<wchar_t>(L'0' + (random_value % 10)));
  }
  return value;
}

std::wstring PortableHostApp::GetActivePassword() const {
  if (!config_.random_password_enabled && !fixed_password_.empty()) {
    return fixed_password_;
  }
  return temporary_password_;
}

bool PortableHostApp::IsLaunchOnStartupEnabled() const {
  HKEY key_handle = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          kStartupRunKeyPath,
          0,
          KEY_READ,
          &key_handle) != ERROR_SUCCESS) {
    return false;
  }

  wchar_t buffer[1024] = {};
  DWORD value_type = 0;
  DWORD value_bytes = sizeof(buffer);
  const LONG query_status = RegQueryValueExW(
      key_handle,
      kStartupRunValueName,
      nullptr,
      &value_type,
      reinterpret_cast<LPBYTE>(buffer),
      &value_bytes);
  RegCloseKey(key_handle);
  if (query_status != ERROR_SUCCESS || (value_type != REG_SZ && value_type != REG_EXPAND_SZ)) {
    return false;
  }

  const std::wstring executable_path = GetExecutablePath();
  const std::wstring stored = buffer;
  const std::wstring hidden_command =
      BuildLaunchOnStartupCommand(executable_path, true);
  if (!hidden_command.empty() &&
      _wcsicmp(stored.c_str(), hidden_command.c_str()) == 0) {
    return true;
  }

  const std::wstring legacy_command =
      BuildLaunchOnStartupCommand(executable_path, false);
  return !legacy_command.empty() &&
         _wcsicmp(stored.c_str(), legacy_command.c_str()) == 0;
}

bool PortableHostApp::SetLaunchOnStartupEnabled(bool enabled) {
  HKEY key_handle = nullptr;
  DWORD disposition = 0;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER,
          kStartupRunKeyPath,
          0,
          nullptr,
          REG_OPTION_NON_VOLATILE,
          KEY_SET_VALUE,
          nullptr,
          &key_handle,
          &disposition) != ERROR_SUCCESS) {
    return false;
  }

  bool ok = true;
  if (enabled) {
    const std::wstring command =
        BuildLaunchOnStartupCommand(GetExecutablePath(), true);
    const DWORD bytes =
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    ok = RegSetValueExW(
             key_handle,
             kStartupRunValueName,
             0,
             REG_SZ,
             reinterpret_cast<const BYTE*>(command.c_str()),
             bytes) == ERROR_SUCCESS;
  } else {
    const LONG status = RegDeleteValueW(key_handle, kStartupRunValueName);
    ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }
  RegCloseKey(key_handle);
  return ok;
}

bool PortableHostApp::SetFixedPassword(const std::wstring& password) {
  const std::wstring trimmed = Trim(password);
  if (trimmed.empty()) {
    fixed_password_.clear();
    config_.fixed_password_protected.clear();
    SaveConfig();
    return true;
  }

  const std::wstring protected_value = ProtectLocalMachineString(trimmed);
  if (protected_value.empty()) {
    return false;
  }

  fixed_password_ = trimmed;
  config_.fixed_password_protected = protected_value;
  SaveConfig();
  return true;
}

std::wstring PortableHostApp::BuildStableDeviceUuid() const {
  wchar_t machine_guid[256] = {};
  DWORD machine_guid_bytes = sizeof(machine_guid);
  HKEY key_handle = nullptr;
  LONG open_status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Cryptography",
      0,
      KEY_READ | KEY_WOW64_64KEY,
      &key_handle);
  if (open_status != ERROR_SUCCESS) {
    open_status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Cryptography",
        0,
        KEY_READ,
        &key_handle);
  }
  if (open_status == ERROR_SUCCESS) {
    const LONG query_status = RegQueryValueExW(
        key_handle,
        L"MachineGuid",
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(machine_guid),
        &machine_guid_bytes);
    RegCloseKey(key_handle);
    if (query_status == ERROR_SUCCESS && machine_guid[0] != L'\0') {
      return machine_guid;
    }
  }

  HW_PROFILE_INFOW profile = {};
  if (GetCurrentHwProfileW(&profile) && profile.szHwProfileGuid[0] != L'\0') {
    return profile.szHwProfileGuid;
  }

  GUID guid = {};
  if (CoCreateGuid(&guid) == S_OK) {
    wchar_t buffer[64] = {};
    if (StringFromGUID2(guid, buffer, static_cast<int>(_countof(buffer))) > 0) {
      return buffer;
    }
  }

  return L"cpp-host-fallback-uuid";
}

bool PortableHostApp::GetOrCreateIdentity(
    std::vector<unsigned char>* public_key,
    std::vector<unsigned char>* secret_key,
    std::vector<unsigned char>* device_uuid,
    std::wstring* error_text) {
  if (!EnsureSodiumInitialized(error_text)) {
    return false;
  }

  *public_key = HexToBytes(public_key_hex_);
  *secret_key = HexToBytes(secret_key_hex_);
  if (public_key->size() != crypto_sign_ed25519_PUBLICKEYBYTES ||
      secret_key->size() != crypto_sign_ed25519_SECRETKEYBYTES) {
    public_key->assign(crypto_sign_ed25519_PUBLICKEYBYTES, 0);
    secret_key->assign(crypto_sign_ed25519_SECRETKEYBYTES, 0);
    if (crypto_sign_ed25519_keypair(public_key->data(), secret_key->data()) != 0) {
      if (error_text != nullptr) {
        *error_text = L"crypto_sign_ed25519_keypair failed";
      }
      return false;
    }
    public_key_hex_ = BytesToHex(*public_key);
    secret_key_hex_ = BytesToHex(*secret_key);
    SaveConfig();
  }

  if (device_uuid_text_.empty()) {
    device_uuid_text_ = BuildStableDeviceUuid();
    SaveConfig();
  }
  const std::string uuid_utf8 = WideToUtf8(device_uuid_text_);
  device_uuid->assign(uuid_utf8.begin(), uuid_utf8.end());
  if (device_uuid->empty()) {
    if (error_text != nullptr) {
      *error_text = L"device uuid is empty";
    }
    return false;
  }
  return true;
}

bool DecodeSymmetricKeyFromPublicKey(
    const std::vector<unsigned char>& their_curve_public_key,
    const std::vector<unsigned char>& sealed_key,
    const std::array<unsigned char, crypto_box_SECRETKEYBYTES>& our_curve_secret_key,
    std::array<unsigned char, crypto_secretbox_KEYBYTES>* symmetric_key,
    std::wstring* error_text) {
  if (their_curve_public_key.size() != crypto_box_PUBLICKEYBYTES) {
    if (error_text != nullptr) {
      *error_text = L"unexpected controller curve25519 public key length";
    }
    return false;
  }
  if (sealed_key.size() != crypto_secretbox_KEYBYTES + crypto_box_MACBYTES) {
    if (error_text != nullptr) {
      *error_text = L"unexpected controller sealed key length";
    }
    return false;
  }

  std::array<unsigned char, crypto_box_PUBLICKEYBYTES> their_public = {};
  std::memcpy(their_public.data(), their_curve_public_key.data(), their_curve_public_key.size());
  const std::array<unsigned char, crypto_box_NONCEBYTES> zero_nonce = {};

  std::array<unsigned char, crypto_secretbox_KEYBYTES> decrypted_key = {};
  if (crypto_box_open_easy(
          decrypted_key.data(),
          sealed_key.data(),
          static_cast<unsigned long long>(sealed_key.size()),
          zero_nonce.data(),
          their_public.data(),
          our_curve_secret_key.data()) != 0) {
    if (error_text != nullptr) {
      *error_text = L"crypto_box_open_easy failed";
    }
    return false;
  }

  *symmetric_key = decrypted_key;
  return true;
}

bool SecureTcpRendezvousConnection(
    TcpFramedConnection* connection,
    const std::wstring& rendezvous_key_base64,
    std::wstring* error_text) {
  if (connection == nullptr) {
    if (error_text != nullptr) {
      *error_text = L"tcp rendezvous connection is null";
    }
    return false;
  }

  const std::vector<unsigned char> rendezvous_sign_public_key =
      DecodeBase64(WideToUtf8(rendezvous_key_base64));
  if (rendezvous_sign_public_key.empty()) {
    if (error_text != nullptr) {
      *error_text = L"invalid rendezvous signing public key";
    }
    return false;
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    std::vector<unsigned char> frame;
    const TcpFramedConnection::ReceiveState state =
        connection->ReceiveFrame(&frame, kConnectTimeoutMs, error_text);
    if (state != TcpFramedConnection::ReceiveState::kFrame) {
      if (error_text != nullptr && error_text->empty()) {
        switch (state) {
          case TcpFramedConnection::ReceiveState::kTimeout:
            *error_text = L"rendezvous tcp handshake timed out";
            break;
          case TcpFramedConnection::ReceiveState::kClosed:
            *error_text = L"rendezvous tcp closed before KeyExchange";
            break;
          case TcpFramedConnection::ReceiveState::kError:
            *error_text = L"rendezvous tcp handshake receive failed";
            break;
          case TcpFramedConnection::ReceiveState::kFrame:
          default:
            break;
        }
      }
      return false;
    }

    if (frame.empty()) {
      continue;
    }

    ParsedServerFrame parsed = ParseServerFrame(frame);
    if (!parsed.has_key_exchange || parsed.key_exchange_keys.size() != 1U) {
      if (error_text != nullptr) {
        *error_text =
            std::wstring(L"expected KeyExchange over rendezvous tcp, fields=") +
            FormatObservedFields(parsed.observed_fields);
      }
      return false;
    }

    std::vector<unsigned char> their_curve_public_key;
    if (!VerifySignedServerKey(
            parsed.key_exchange_keys[0],
            rendezvous_sign_public_key,
            &their_curve_public_key,
            error_text)) {
      return false;
    }

    std::vector<unsigned char> our_curve_public_key;
    std::vector<unsigned char> sealed_key;
    std::array<unsigned char, crypto_secretbox_KEYBYTES> symmetric_key = {};
    if (!CreateSymmetricKeyResponse(
            their_curve_public_key,
            &our_curve_public_key,
            &sealed_key,
            &symmetric_key,
            error_text)) {
      return false;
    }

    const std::vector<unsigned char> key_exchange =
        EncodeKeyExchangeMessage(our_curve_public_key, sealed_key);
    if (!connection->SendRawFrame(key_exchange, error_text)) {
      if (error_text != nullptr && !error_text->empty()) {
        *error_text = L"rendezvous tcp KeyExchange send failed: " + *error_text;
      }
      return false;
    }

    connection->SetSymmetricKey(symmetric_key);
    return true;
  }

  if (error_text != nullptr) {
    *error_text = L"rendezvous tcp did not provide KeyExchange";
  }
  return false;
}

void PortableHostApp::RendezvousWorker() {
  int uuid_mismatch_auto_id_retry_count = 0;
  SOCKET direct_access_listener = INVALID_SOCKET;
  unsigned short direct_access_listener_port = 0;
  std::wstring direct_access_listener_error;
  auto close_direct_access_listener = [&]() {
    if (direct_access_listener != INVALID_SOCKET) {
      closesocket(direct_access_listener);
      direct_access_listener = INVALID_SOCKET;
    }
    direct_access_listener_port = 0;
  };
  ScopeExit direct_access_listener_scope([&]() {
    close_direct_access_listener();
  });
  while (!stop_rendezvous_.load()) {
    std::vector<unsigned char> public_key_bytes;
    std::vector<unsigned char> secret_key_bytes;
    std::vector<unsigned char> uuid_bytes;
    std::wstring error_text;
    if (!GetOrCreateIdentity(&public_key_bytes, &secret_key_bytes, &uuid_bytes, &error_text)) {
      SetRendezvousStatus(L"identity setup failed: " + error_text, false);
      for (int wait_count = 0; wait_count < 3 && !stop_rendezvous_.load(); ++wait_count) {
        Sleep(1000);
      }
      continue;
    }

    const ParsedHostPort id_server = ParseHostPort(config_.id_server, kDefaultIdServerPort);
    if (id_server.host.empty()) {
      SetRendezvousStatus(L"id_server is empty", false);
      Sleep(2000);
      continue;
    }

    const std::wstring endpoint = BuildDisplayEndpoint(id_server.host, id_server.port);
    SetRendezvousStatus(std::wstring(L"connecting udp ") + endpoint, false);

    auto retry_with_new_host_id = [this, &uuid_mismatch_auto_id_retry_count](
                                      const std::wstring& reason) -> bool {
      if (uuid_mismatch_auto_id_retry_count >= 3) {
        return false;
      }

      ++uuid_mismatch_auto_id_retry_count;
      const std::wstring previous_host_id = config_.host_id;
      std::wstring new_host_id = GenerateFallbackDesktopId();
      for (int attempt = 0; attempt < 4 && new_host_id == previous_host_id; ++attempt) {
        new_host_id = GenerateFallbackDesktopId();
      }
      config_.host_id = new_host_id;
      SaveConfig();
      SetRendezvousStatus(reason + L"; retrying with new ID " + config_.host_id, false);
      return true;
    };

    const bool allow_tcp_fallback = true;
    UdpMessageSocket socket;
    const bool udp_connected = socket.Connect(id_server.host, id_server.port, &error_text);
    if (!udp_connected) {
      SetRendezvousStatus(L"udp connect failed: " + error_text, false);
    }

    auto send_register_pk = [&]() -> bool {
      std::vector<unsigned char> register_pk =
          EncodeRegisterPkMessage(config_.host_id, uuid_bytes, public_key_bytes);
      if (!socket.SendMessage(register_pk, &error_text)) {
        SetRendezvousStatus(L"RegisterPk udp send failed: " + error_text, false);
        return false;
      }
      return true;
    };

    auto send_register_peer = [&]() -> bool {
      const std::vector<unsigned char> register_peer =
          EncodeRegisterPeerMessage(config_.host_id, 0);
      if (!socket.SendMessage(register_peer, &error_text)) {
        SetRendezvousStatus(L"RegisterPeer udp send failed: " + error_text, false);
        return false;
      }
      return true;
    };

    const HostConfig session_config = config_;
    const std::wstring session_temporary_password_snapshot = temporary_password_;
    const std::wstring session_fixed_password_snapshot = fixed_password_;
    const std::wstring session_device_uuid_snapshot = device_uuid_text_;
    const std::vector<unsigned char> session_secret_key_bytes = secret_key_bytes;
    const ParsedHostPort session_id_server = id_server;

    auto generate_numeric_password = [](int length) -> std::wstring {
      std::wstring value;
      value.reserve(length);
      for (int index = 0; index < length; ++index) {
        const unsigned int random_value = GetRandomUint32();
        value.push_back(static_cast<wchar_t>(L'0' + (random_value % 10)));
      }
      return value;
    };

    auto describe_receive_state = [](TcpFramedConnection::ReceiveState state) -> std::wstring {
      switch (state) {
        case TcpFramedConnection::ReceiveState::kTimeout:
          return L"timeout";
        case TcpFramedConnection::ReceiveState::kClosed:
          return L"closed";
        case TcpFramedConnection::ReceiveState::kError:
          return L"error";
        case TcpFramedConnection::ReceiveState::kFrame:
        default:
          return L"frame";
      }
    };

    auto should_auto_accept_secondary_file_transfer =
        [this](const LoginRequestData& login_request,
               const std::wstring& display_remote_id_override) -> bool {
      bool accepted = false;
      std::wstring reason = L"not a secondary file-transfer request";
      std::wstring expected_remote_id;
      std::wstring active_remote_id;
      if (!login_request.has_file_transfer) {
        reason = L"login_request.has_file_transfer=false";
      } else if (!login_request.password.empty()) {
        reason = L"login_request.password is not empty";
      } else if (!active_session_connected_.load()) {
        reason = L"active_session_connected=false";
      } else {
        expected_remote_id = Trim(
            display_remote_id_override.empty()
                ? login_request.my_id
                : display_remote_id_override);
        if (expected_remote_id.empty()) {
          reason = L"expected_remote_id is empty";
        } else {
          {
            Win32LockGuard guard(active_session_mutex_);
            active_remote_id = Trim(active_session_remote_id_);
          }
          accepted = !active_remote_id.empty() &&
                     _wcsicmp(active_remote_id.c_str(), expected_remote_id.c_str()) == 0;
          reason = accepted ? L"matched active remote id" : L"active remote id mismatch";
        }
      }
      AppendPortableHostLog(
          L"file-transfer",
          L"secondary auto-accept check: result=" + BoolToLogText(accepted) +
              L", reason=" + reason +
              L", expected_remote_id=" + expected_remote_id +
              L", active_remote_id=" + active_remote_id +
              L", login=" + DescribeLoginRequestForLog(login_request));
      return accepted;
    };

    auto is_file_transfer_stop_requested = [this]() -> bool {
      return stop_rendezvous_.load() ||
             stop_active_session_.load() ||
             stop_auxiliary_session_.load();
    };

    auto run_minimal_plain_session_over_connection = [this, describe_receive_state](
                                                     TcpFramedConnection* connection,
                                                     const std::wstring& channel_name,
                                                     std::wstring* session_status) -> bool {
      RegisterActiveSessionConnection(connection);
      std::shared_ptr<void> connection_scope(
          connection,
          [this](void* registered_connection) {
            ClearActiveSessionConnection(registered_connection);
          });
      std::vector<unsigned char> frame;
      const TcpFramedConnection::ReceiveState login_state =
          connection->ReceiveFrame(&frame, kConnectTimeoutMs, session_status);
      if (login_state != TcpFramedConnection::ReceiveState::kFrame) {
        if (session_status != nullptr) {
          const std::wstring previous = *session_status;
          *session_status = channel_name + L" plain receive " + describe_receive_state(login_state);
          if (!previous.empty()) {
            *session_status += L": ";
            *session_status += previous;
          }
        }
        return false;
      }

      if (!FrameContainsMessageField(frame, 7U)) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" received plain message, but it was not LoginRequest";
        }
        return false;
      }

      const std::vector<unsigned char> login_response =
          EncodeLoginResponseErrorMessage(
              L"C++ host transport is online, but desktop session protocol is not ported yet.");
      if (!connection->SendRawFrame(login_response, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" plain login response send failed: " + *session_status;
        }
        return false;
      }

      if (session_status != nullptr) {
        *session_status =
            channel_name +
            L" reached plain LoginRequest; relay/direct transport path works, desktop session protocol still pending";
      }
      return true;
    };

    auto run_file_transfer_session_loop =
        [this, describe_receive_state, &is_file_transfer_stop_requested](
            TcpFramedConnection* connection,
            const std::wstring& channel_name,
            const std::wstring& connected_remote_id,
            const std::wstring& connected_remote_name,
            const std::wstring& initial_dir_request,
            bool initial_show_hidden,
            std::wstring* session_status) -> bool {
      active_session_connected_.store(true);
      StoreActiveSessionIdentity(connected_remote_id, connected_remote_name);
      AppendPortableHostLog(
          L"file-transfer",
          channel_name + L" starting; remote_id=" + connected_remote_id +
              L", remote_name=" + connected_remote_name +
              L", initial_dir=" + initial_dir_request +
              L", show_hidden=" + BoolToLogText(initial_show_hidden));
      ScopeExit log_exit([&]() {
        AppendPortableHostLog(
            L"file-transfer",
            channel_name + L" exiting; status=" +
                (session_status == nullptr ? std::wstring() : *session_status));
      });

      std::unordered_map<int, std::shared_ptr<FileTransferReadJob>> read_jobs;
      std::unordered_map<int, std::shared_ptr<FileTransferWriteJob>> write_jobs;
      ScopeExit cleanup_jobs([&read_jobs, &write_jobs]() {
        for (auto& entry : read_jobs) {
          if (entry.second != nullptr) {
            CloseFileTransferHandle(&entry.second->file);
          }
        }
        for (auto& entry : write_jobs) {
          if (entry.second != nullptr) {
            DiscardFileTransferWriteCurrentFile(entry.second.get());
          }
        }
      });

      auto erase_read_job = [&](int id) {
        auto cursor = read_jobs.find(id);
        if (cursor == read_jobs.end()) {
          return;
        }
        if (cursor->second != nullptr) {
          CloseFileTransferHandle(&cursor->second->file);
        }
        read_jobs.erase(cursor);
      };
      auto erase_write_job = [&](int id, bool discard_current_file) {
        auto cursor = write_jobs.find(id);
        if (cursor == write_jobs.end()) {
          return;
        }
        if (cursor->second != nullptr) {
          if (discard_current_file) {
            DiscardFileTransferWriteCurrentFile(cursor->second.get());
          } else {
            std::wstring ignored_error;
            FinalizeFileTransferWriteCurrentFile(cursor->second.get(), &ignored_error);
          }
          CloseFileTransferHandle(&cursor->second->file);
        }
        write_jobs.erase(cursor);
      };

      auto send_file_transfer_error =
          [&](int id, int file_num, const std::wstring& error_text) -> bool {
        AppendPortableHostLog(
            L"file-transfer",
            channel_name + L" sending error response; id=" + std::to_wstring(id) +
                L", file_num=" + std::to_wstring(file_num) +
                L", error=" + error_text);
        if (!connection->SendFrame(
                EncodeFileTransferErrorResponseMessage(id, error_text, file_num),
                session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" file-transfer error send failed: " + *session_status;
          }
          return false;
        }
        return true;
      };

      auto pump_read_jobs = [&]() -> bool {
        for (auto cursor = read_jobs.begin(); cursor != read_jobs.end();) {
          const std::shared_ptr<FileTransferReadJob>& job = cursor->second;
          if (job == nullptr) {
            cursor = read_jobs.erase(cursor);
            continue;
          }
          if (IsFileTransferReadJobComplete(*job)) {
            if (!connection->SendFrame(
                    EncodeFileTransferDoneResponseMessage(job->id, job->file_num),
                    session_status)) {
              if (session_status != nullptr && !session_status->empty()) {
                *session_status =
                    channel_name + L" file-transfer final done send failed: " + *session_status;
              }
              return false;
            }
            CloseFileTransferHandle(&job->file);
            cursor = read_jobs.erase(cursor);
            continue;
          }

          if (!job->sent_digest) {
            const FileTransferEntryData& entry = job->files[job->file_num];
            if (!connection->SendFrame(
                    EncodeFileTransferDigestResponseMessage(
                        job->id,
                        job->file_num,
                        entry.modified_time,
                        entry.size,
                        false,
                        false,
                        0,
                        false),
                    session_status)) {
              if (session_status != nullptr && !session_status->empty()) {
                *session_status =
                    channel_name + L" file-transfer digest send failed: " + *session_status;
              }
              return false;
            }
            job->sent_digest = true;
            job->waiting_for_confirm = true;
            ++cursor;
            continue;
          }
          if (job->waiting_for_confirm && !job->file_confirmed) {
            ++cursor;
            continue;
          }

          std::wstring io_error;
          if (!OpenFileTransferReadHandle(job.get(), &io_error)) {
            if (!send_file_transfer_error(job->id, job->file_num, io_error)) {
              return false;
            }
            AdvanceFileTransferReadJob(job.get());
            if (IsFileTransferReadJobComplete(*job)) {
              cursor = read_jobs.erase(cursor);
            } else {
              ++cursor;
            }
            continue;
          }

          std::vector<unsigned char> data(kFileTransferBlockSize);
          DWORD read = 0;
          const BOOL ok = ReadFile(
              job->file,
              data.data(),
              static_cast<DWORD>(data.size()),
              &read,
              nullptr);
          if (!ok) {
            if (!send_file_transfer_error(
                    job->id,
                    job->file_num,
                    L"ReadFile failed while streaming file transfer data")) {
              return false;
            }
            AdvanceFileTransferReadJob(job.get());
            if (IsFileTransferReadJobComplete(*job)) {
              cursor = read_jobs.erase(cursor);
            } else {
              ++cursor;
            }
            continue;
          }

          data.resize(read);
          if (read == 0) {
            if (!connection->SendFrame(
                    EncodeFileTransferBlockResponseMessage(
                        job->id,
                        job->file_num,
                        std::vector<unsigned char>(),
                        false,
                        0U),
                    session_status)) {
              if (session_status != nullptr && !session_status->empty()) {
                *session_status =
                    channel_name + L" file-transfer eof block send failed: " + *session_status;
              }
              return false;
            }
            AdvanceFileTransferReadJob(job.get());
            ++cursor;
            continue;
          }

          if (!connection->SendFrame(
                  EncodeFileTransferBlockResponseMessage(
                      job->id,
                      job->file_num,
                      data,
                      false,
                      0U),
                  session_status)) {
            if (session_status != nullptr && !session_status->empty()) {
              *session_status =
                  channel_name + L" file-transfer block send failed: " + *session_status;
            }
            return false;
          }
          ++cursor;
        }
        return true;
      };

      std::wstring initial_dir =
          initial_dir_request.empty() ? L"/" : ResolveFileTransferFilesystemPath(initial_dir_request);
      std::vector<FileTransferEntryData> initial_entries;
      std::wstring initial_error;
      if (!CollectFileTransferDirectoryEntries(
              initial_dir,
              initial_show_hidden,
              &initial_entries,
              &initial_error)) {
        initial_dir = L"/";
        initial_entries.clear();
        initial_error.clear();
        if (!CollectFileTransferDirectoryEntries(initial_dir, true, &initial_entries, &initial_error)) {
          if (!send_file_transfer_error(0, 0, initial_error.empty()
                                                  ? L"failed to enumerate initial file-transfer directory"
                                                  : initial_error)) {
            return false;
          }
        }
      }
      if (!connection->SendFrame(
              EncodeFileTransferDirectoryResponseMessage(0, initial_dir, initial_entries),
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" initial file-transfer listing send failed: " + *session_status;
        }
        return false;
      }
      AppendPortableHostLog(
          L"file-transfer",
          channel_name + L" initial listing sent; path=" + initial_dir +
              L", entries=" + std::to_wstring(initial_entries.size()));

      if (session_status != nullptr) {
        *session_status = channel_name + L" file-transfer session ready";
      }

      std::vector<unsigned char> frame;
      while (!is_file_transfer_stop_requested()) {
        if (!pump_read_jobs()) {
          return false;
        }

        frame.clear();
        const TcpFramedConnection::ReceiveState receive_state =
            connection->ReceiveFrame(&frame, kSessionPollMs, session_status);
        if (receive_state == TcpFramedConnection::ReceiveState::kTimeout) {
          if (session_status != nullptr) {
            if (!read_jobs.empty()) {
              *session_status = channel_name + L" file-transfer session streaming files";
            } else if (!write_jobs.empty()) {
              *session_status = channel_name + L" file-transfer session receiving files";
            } else {
              *session_status = channel_name + L" file-transfer session connected; waiting for action";
            }
          }
          continue;
        }
        if (receive_state != TcpFramedConnection::ReceiveState::kFrame) {
          if (session_status != nullptr) {
            const std::wstring previous = *session_status;
            *session_status =
                channel_name + L" file-transfer receive " + describe_receive_state(receive_state);
            if (!previous.empty()) {
              *session_status += L": ";
              *session_status += previous;
            }
          }
          AppendPortableHostLog(
              L"file-transfer",
              channel_name + L" receive state=" + describe_receive_state(receive_state));
          return true;
        }

        FileTransferActionData action;
        if (ParseFileTransferActionMessage(frame, &action)) {
          AppendPortableHostLog(
              L"file-transfer",
              channel_name + L" received action: " + DescribeFileTransferActionForLog(action));
          switch (action.kind) {
            case FileTransferActionKind::kReadDir: {
              const std::wstring listing_path =
                  action.read_dir.path.empty()
                      ? L"/"
                      : ResolveFileTransferFilesystemPath(action.read_dir.path);
              std::vector<FileTransferEntryData> entries;
              std::wstring listing_error;
              if (!CollectFileTransferDirectoryEntries(
                      listing_path,
                      action.read_dir.include_hidden,
                      &entries,
                      &listing_error)) {
                if (!send_file_transfer_error(0, 0, listing_error)) {
                  return false;
                }
              } else if (!connection->SendFrame(
                             EncodeFileTransferDirectoryResponseMessage(0, listing_path, entries),
                             session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" file-transfer directory send failed: " + *session_status;
                }
                return false;
              }
              break;
            }
            case FileTransferActionKind::kAllFiles: {
              const std::wstring listing_path =
                  action.all_files.path.empty()
                      ? L"/"
                      : ResolveFileTransferFilesystemPath(action.all_files.path);
              std::vector<FileTransferEntryData> entries;
              std::wstring listing_error;
              if (!CollectFileTransferRecursiveFiles(
                      listing_path,
                      action.all_files.include_hidden,
                      &entries,
                      &listing_error)) {
                if (!send_file_transfer_error(action.all_files.id, -1, listing_error)) {
                  return false;
                }
              } else if (!connection->SendFrame(
                             EncodeFileTransferDirectoryResponseMessage(
                                 action.all_files.id,
                                 listing_path,
                                 entries),
                             session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" file-transfer recursive listing send failed: " +
                      *session_status;
                }
                return false;
              }
              break;
            }
            case FileTransferActionKind::kSend: {
              if (action.send.file_type != 0) {
                if (!send_file_transfer_error(
                        action.send.id,
                        action.send.file_num,
                        L"only generic file-transfer send is supported")) {
                  return false;
                }
                break;
              }
              std::vector<FileTransferEntryData> files;
              std::wstring listing_error;
              if (!CollectFileTransferRecursiveFiles(
                      action.send.path,
                      action.send.include_hidden,
                      &files,
                      &listing_error)) {
                if (!send_file_transfer_error(action.send.id, action.send.file_num, listing_error)) {
                  return false;
                }
                break;
              }
              std::shared_ptr<FileTransferReadJob> job = std::make_shared<FileTransferReadJob>();
              job->id = action.send.id;
              job->source_path = ResolveFileTransferFilesystemPath(action.send.path);
              job->files = files;
              job->file_num = action.send.file_num >= 0 &&
                                      action.send.file_num < static_cast<int>(files.size())
                                  ? action.send.file_num
                                  : 0;
              read_jobs[job->id] = job;
              if (!connection->SendFrame(
                      EncodeFileTransferDirectoryResponseMessage(
                          action.send.id,
                          ResolveFileTransferFilesystemPath(action.send.path),
                          files),
                      session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" file-transfer send listing failed: " + *session_status;
                }
                return false;
              }
              break;
            }
            case FileTransferActionKind::kReceive: {
              if (action.receive.files.empty()) {
                if (!send_file_transfer_error(action.receive.id, action.receive.file_num, L"no files")) {
                  return false;
                }
                break;
              }
              bool valid_files = true;
              std::wstring validation_error;
              for (size_t index = 0; index < action.receive.files.size(); ++index) {
                if (!ValidateFileTransferRelativePath(
                        action.receive.files[index].name,
                        action.receive.files.size() == 1 &&
                            action.receive.files[index].name.empty(),
                        &validation_error)) {
                  valid_files = false;
                  break;
                }
              }
              if (!valid_files) {
                if (!send_file_transfer_error(
                        action.receive.id,
                        action.receive.file_num,
                        validation_error.empty()
                            ? L"invalid incoming file-transfer file list"
                            : validation_error)) {
                  return false;
                }
                break;
              }

              std::shared_ptr<FileTransferWriteJob> job = std::make_shared<FileTransferWriteJob>();
              job->id = action.receive.id;
              job->target_path = ResolveFileTransferFilesystemPath(action.receive.path);
              job->files = action.receive.files;
              write_jobs[job->id] = job;
              break;
            }
            case FileTransferActionKind::kCancel:
              erase_read_job(action.cancel.id);
              erase_write_job(action.cancel.id, true);
              break;
            case FileTransferActionKind::kSendConfirm: {
              auto job_cursor = read_jobs.find(action.send_confirm.id);
              if (job_cursor != read_jobs.end() && job_cursor->second != nullptr) {
                FileTransferReadJob* job = job_cursor->second.get();
                if (action.send_confirm.file_num == job->file_num) {
                  if (action.send_confirm.has_skip && action.send_confirm.skip) {
                    AdvanceFileTransferReadJob(job);
                  } else {
                    job->waiting_for_confirm = false;
                    job->file_confirmed = true;
                    if (action.send_confirm.has_offset_blk) {
                      job->resume_offset = action.send_confirm.offset_blk;
                    }
                  }
                }
                if (IsFileTransferReadJobComplete(*job)) {
                  erase_read_job(action.send_confirm.id);
                }
              }
              break;
            }
            case FileTransferActionKind::kReadEmptyDirs:
              if (!connection->SendFrame(
                      EncodeFileTransferEmptyDirsResponseMessage(
                          ResolveFileTransferFilesystemPath(action.read_dir.path)),
                      session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" file-transfer empty-dirs send failed: " + *session_status;
                }
                return false;
              }
              break;
            case FileTransferActionKind::kCreate:
              {
                std::wstring create_path;
                std::wstring create_error;
                if (!ResolveFileTransferMutablePath(
                        action.create.path,
                        &create_path,
                        &create_error)) {
                  if (!send_file_transfer_error(
                          action.create.id,
                          -1,
                          create_error.empty() ? L"invalid directory path" : create_error)) {
                    return false;
                  }
                  break;
                }

                const DWORD existing_attributes = GetFileAttributesW(create_path.c_str());
                bool created = false;
                if (existing_attributes != INVALID_FILE_ATTRIBUTES) {
                  if ((existing_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    created = true;
                  } else {
                    create_error = L"target path already exists as a file";
                  }
                } else {
                  created = EnsureDirectoryExistsRecursive(create_path, &create_error);
                }

                if (!created) {
                  if (!send_file_transfer_error(
                          action.create.id,
                          -1,
                          create_error.empty() ? L"failed to create directory" : create_error)) {
                    return false;
                  }
                  break;
                }

                if (!connection->SendFrame(
                        EncodeFileTransferDoneResponseMessage(action.create.id, -1),
                        session_status)) {
                  if (session_status != nullptr && !session_status->empty()) {
                    *session_status =
                        channel_name + L" file-transfer create-dir done send failed: " +
                        *session_status;
                  }
                  return false;
                }
              }
              break;
            case FileTransferActionKind::kRemoveDir:
              {
                std::wstring remove_path;
                std::wstring remove_error;
                if (!ResolveFileTransferMutablePath(
                        action.remove_dir.path,
                        &remove_path,
                        &remove_error)) {
                  if (!send_file_transfer_error(
                          action.remove_dir.id,
                          -1,
                          remove_error.empty() ? L"invalid directory path" : remove_error)) {
                    return false;
                  }
                  break;
                }

                bool removed = false;
                const DWORD attributes = GetFileAttributesW(remove_path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                  remove_error = L"directory not found";
                } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                  remove_error = L"path is not a directory";
                } else if (action.remove_dir.recursive) {
                  removed = RemoveFileTransferDirectoryTree(remove_path, &remove_error);
                } else {
                  TryRemoveReadOnlyAttribute(remove_path, attributes);
                  removed = RemoveDirectoryW(remove_path.c_str()) != FALSE;
                  if (!removed) {
                    remove_error =
                        L"RemoveDirectoryW failed, error=" + std::to_wstring(GetLastError());
                  }
                }

                if (!removed) {
                  if (!send_file_transfer_error(
                          action.remove_dir.id,
                          -1,
                          remove_error.empty() ? L"failed to remove directory" : remove_error)) {
                    return false;
                  }
                  break;
                }

                if (!connection->SendFrame(
                        EncodeFileTransferDoneResponseMessage(action.remove_dir.id, -1),
                        session_status)) {
                  if (session_status != nullptr && !session_status->empty()) {
                    *session_status =
                        channel_name + L" file-transfer remove-dir done send failed: " +
                        *session_status;
                  }
                  return false;
                }
              }
              break;
            case FileTransferActionKind::kRemoveFile:
              {
                std::wstring remove_path;
                std::wstring remove_error;
                if (!ResolveFileTransferMutablePath(
                        action.remove_file.path,
                        &remove_path,
                        &remove_error)) {
                  if (!send_file_transfer_error(
                          action.remove_file.id,
                          action.remove_file.file_num,
                          remove_error.empty() ? L"invalid file path" : remove_error)) {
                    return false;
                  }
                  break;
                }

                const DWORD attributes = GetFileAttributesW(remove_path.c_str());
                bool removed = false;
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                  remove_error = L"file not found";
                } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                  remove_error = L"path is a directory";
                } else {
                  TryRemoveReadOnlyAttribute(remove_path, attributes);
                  removed = DeleteFileW(remove_path.c_str()) != FALSE;
                  if (!removed) {
                    remove_error =
                        L"DeleteFileW failed, error=" + std::to_wstring(GetLastError());
                  }
                }

                if (!removed) {
                  if (!send_file_transfer_error(
                          action.remove_file.id,
                          action.remove_file.file_num,
                          remove_error.empty() ? L"failed to remove file" : remove_error)) {
                    return false;
                  }
                  break;
                }

                if (!connection->SendFrame(
                        EncodeFileTransferDoneResponseMessage(
                            action.remove_file.id,
                            action.remove_file.file_num),
                        session_status)) {
                  if (session_status != nullptr && !session_status->empty()) {
                    *session_status =
                        channel_name + L" file-transfer remove-file done send failed: " +
                        *session_status;
                  }
                  return false;
                }
              }
              break;
            case FileTransferActionKind::kRename:
              if (!send_file_transfer_error(
                      action.rename.id,
                      0,
                      L"renaming through file-transfer is not supported yet")) {
                return false;
              }
              break;
            default:
              if (!send_file_transfer_error(0, 0, L"file-transfer operation is not supported yet")) {
                return false;
              }
              break;
          }

          if (!pump_read_jobs()) {
            return false;
          }
          continue;
        }

        FileTransferResponseData response;
        if (ParseFileTransferResponseMessage(frame, &response)) {
          AppendPortableHostLog(
              L"file-transfer",
              channel_name + L" received response: " + DescribeFileTransferResponseForLog(response));
          switch (response.kind) {
            case FileTransferResponseKind::kBlock: {
              auto job_cursor = write_jobs.find(response.block.id);
              if (job_cursor == write_jobs.end() || job_cursor->second == nullptr) {
                break;
              }
              std::wstring write_error;
              if (!PrepareFileTransferWriteTarget(
                      job_cursor->second.get(),
                      response.block.file_num,
                      &write_error)) {
                if (!send_file_transfer_error(response.block.id, response.block.file_num, write_error)) {
                  return false;
                }
                erase_write_job(response.block.id, true);
                break;
              }

              std::vector<unsigned char> plain;
              const std::vector<unsigned char>* payload = &response.block.data;
              if (response.block.compressed) {
                if (!DecompressZstdBytes(response.block.data, &plain, &write_error)) {
                  if (!send_file_transfer_error(
                          response.block.id,
                          response.block.file_num,
                          write_error.empty()
                              ? L"failed to decompress file-transfer block"
                              : write_error)) {
                    return false;
                  }
                  erase_write_job(response.block.id, true);
                  break;
                }
                payload = &plain;
              }

              DWORD written = 0;
              const BOOL ok = payload->empty()
                                  ? TRUE
                                  : WriteFile(
                                        job_cursor->second->file,
                                        payload->data(),
                                        static_cast<DWORD>(payload->size()),
                                        &written,
                                        nullptr);
              if (!ok || written != payload->size()) {
                if (!send_file_transfer_error(
                        response.block.id,
                        response.block.file_num,
                        L"WriteFile failed while receiving file-transfer block")) {
                  return false;
                }
                erase_write_job(response.block.id, true);
              }
              break;
            }
            case FileTransferResponseKind::kDone: {
              auto job_cursor = write_jobs.find(response.done.id);
              if (job_cursor == write_jobs.end() || job_cursor->second == nullptr) {
                break;
              }
              FileTransferWriteJob* const job = job_cursor->second.get();
              const int file_count = static_cast<int>(job->files.size());
              const bool whole_job_done =
                  response.done.file_num < 0 || response.done.file_num >= file_count ||
                  job->current_file_num == -1 ||
                  response.done.file_num != job->current_file_num;
              std::wstring write_error;
              if ((!whole_job_done &&
                   (!PrepareFileTransferWriteTarget(job, response.done.file_num, &write_error) ||
                    !FinalizeFileTransferWriteCurrentFile(job, &write_error))) ||
                  (whole_job_done &&
                   !FinalizeFileTransferWriteCurrentFile(job, &write_error))) {
                if (!send_file_transfer_error(response.done.id, response.done.file_num, write_error)) {
                  return false;
                }
                erase_write_job(response.done.id, true);
                break;
              }
              if (!connection->SendFrame(
                      EncodeFileTransferDoneResponseMessage(
                          response.done.id,
                          response.done.file_num),
                      session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" file-transfer completion ack failed: " + *session_status;
                }
                return false;
              }
              if (whole_job_done || response.done.file_num + 1 >= file_count) {
                erase_write_job(response.done.id, false);
              }
              break;
            }
            case FileTransferResponseKind::kDigest:
              if (write_jobs.find(response.digest.id) != write_jobs.end()) {
                if (!connection->SendFrame(
                        EncodeFileTransferSendConfirmActionMessage(
                            response.digest.id,
                            response.digest.file_num,
                            false,
                            0U),
                        session_status)) {
                  if (session_status != nullptr && !session_status->empty()) {
                    *session_status =
                        channel_name + L" file-transfer confirm send failed: " + *session_status;
                  }
                  return false;
                }
              }
              break;
            case FileTransferResponseKind::kError:
              erase_read_job(response.error.id);
              erase_write_job(response.error.id, true);
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" peer file-transfer error: " + response.error.error;
              }
              break;
            default:
              break;
          }

          if (!pump_read_jobs()) {
            return false;
          }
          continue;
        }

        const SessionMessageType session_message = ParseSessionMessage(frame);
        if (session_message.has_close_reason) {
          if (session_status != nullptr) {
            *session_status = channel_name + L" file-transfer closed by peer";
            if (!session_message.close_reason.empty()) {
              *session_status += L": ";
              *session_status += session_message.close_reason;
            }
          }
          AppendPortableHostLog(
              L"file-transfer",
              channel_name + L" peer closed session; reason=" + session_message.close_reason);
          return true;
        }

        if (session_status != nullptr) {
          *session_status =
              channel_name +
              L" file-transfer session ignoring follow-up fields=" +
              FormatObservedFields(ExtractTopLevelMessageFields(frame));
        }
        AppendPortableHostLog(
            L"file-transfer",
            channel_name +
                L" ignored follow-up message fields=" +
                FormatObservedFields(ExtractTopLevelMessageFields(frame)));
      }

      return true;
    };

    auto run_desktop_session_loop =
        [this, session_config, describe_receive_state](
            TcpFramedConnection* connection,
            const std::wstring& channel_name,
            const std::wstring& connected_remote_id,
            const std::wstring& connected_remote_name,
            bool use_auxiliary_stop_path,
            std::wstring* session_status) -> bool {
      auto is_desktop_session_stop_requested = [this, use_auxiliary_stop_path]() -> bool {
        return use_auxiliary_stop_path
            ? IsAuxiliarySessionStopRequested()
            : IsActiveSessionStopRequested();
      };
      desktop_session_count_.fetch_add(1);
      active_session_connected_.store(true);
      StoreActiveSessionIdentity(connected_remote_id, connected_remote_name);
      ScopeExit desktop_session_scope([this]() {
        const long remaining = desktop_session_count_.fetch_sub(1) - 1;
        if (remaining <= 0) {
          active_session_connected_.store(false);
          ClearActiveSessionIdentity();
        }
      });

      std::atomic<int> active_clipboard_file_requests{0};
      std::atomic<int> next_clipboard_stream_id{1};
      std::wstring last_local_clipboard_text;
      std::wstring last_remote_clipboard_text;
      GetClipboardUnicodeText(&last_local_clipboard_text, nullptr);
      std::wstring last_local_formatted_clipboard_signature;
      std::wstring last_remote_formatted_clipboard_signature;
      FormattedTextClipboardContent initial_formatted_clipboard;
      CaptureFormattedTextClipboardContent(
          &initial_formatted_clipboard,
          &last_local_formatted_clipboard_signature,
          nullptr);
      auto next_clipboard_poll = std::chrono::steady_clock::now();
      DWORD last_local_clipboard_sequence = GetClipboardSequenceNumber();
      bool peer_cliprdr_ready = false;
      const int local_file_descriptor_format_id =
          static_cast<int>(GetFileDescriptorClipboardFormat());
      const int local_file_contents_format_id =
          static_cast<int>(GetFileContentsClipboardFormat());
      std::wstring last_local_file_clipboard_signature;
      std::vector<LocalClipboardFileDescriptor> local_file_clipboard_entries;
      std::vector<unsigned char> local_file_clipboard_descriptor_payload;
      std::vector<std::weak_ptr<RemoteFileClipboardBridge>> file_clipboard_brokers;
      auto describe_file_clipboard_progress = [&]() -> std::wstring {
        return channel_name + L" serving on-demand clipboard file transfer";
      };
      auto collect_live_file_clipboard_brokers =
          [&]() -> std::vector<std::shared_ptr<RemoteFileClipboardBridge>> {
        std::vector<std::shared_ptr<RemoteFileClipboardBridge>> live_brokers;
        auto cursor = file_clipboard_brokers.begin();
        while (cursor != file_clipboard_brokers.end()) {
          std::shared_ptr<RemoteFileClipboardBridge> broker = cursor->lock();
          if (broker == nullptr) {
            cursor = file_clipboard_brokers.erase(cursor);
            continue;
          }
          live_brokers.push_back(broker);
          ++cursor;
        }
        return live_brokers;
      };
      auto close_file_clipboard_brokers = [&](const std::wstring& reason) {
        for (const std::shared_ptr<RemoteFileClipboardBridge>& broker :
             collect_live_file_clipboard_brokers()) {
          broker->Close(reason);
        }
      };
      auto install_remote_file_clipboard =
          [&](const std::vector<unsigned char>& descriptor_payload,
              std::wstring* transfer_status) -> bool {
        auto broker = std::make_shared<RemoteFileClipboardBridge>(
            connection,
            &active_clipboard_file_requests,
            &next_clipboard_stream_id,
            channel_name);
        if (!broker->InitializeFromFileGroupDescriptorPayload(
                descriptor_payload,
                transfer_status)) {
          if (transfer_status != nullptr && !transfer_status->empty()) {
            *transfer_status =
                channel_name + L" failed to parse remote clipboard file descriptors: " +
                *transfer_status;
          }
          return false;
        }
        file_clipboard_brokers.push_back(broker);
        if (window_ != nullptr) {
          auto* request = new (std::nothrow) InstallRemoteFileClipboardRequest();
          if (request == nullptr) {
            if (transfer_status != nullptr) {
              *transfer_status = channel_name + L" failed to allocate clipboard install request";
            }
            return false;
          }
          request->bridge = broker;
          if (!PostMessageW(
                  window_,
                  kAppInstallRemoteFileClipboard,
                  0,
                  reinterpret_cast<LPARAM>(request))) {
            delete request;
            if (transfer_status != nullptr) {
              *transfer_status = channel_name + L" failed to post clipboard install request";
            }
            return false;
          }
        }
        if (transfer_status != nullptr) {
          *transfer_status =
              channel_name + L" received remote file clipboard; file contents will stream on paste";
        }
        return true;
      };
      auto handle_file_contents_response = [&](const CliprdrMessageData& cliprdr) -> bool {
        for (const std::shared_ptr<RemoteFileClipboardBridge>& broker :
             collect_live_file_clipboard_brokers()) {
          if (broker->HandleFileContentsResponse(cliprdr)) {
            return true;
          }
        }
        return false;
      };

      std::atomic<bool> stop_video_thread{false};
      std::atomic<bool> request_immediate_video{true};
      std::atomic<bool> video_thread_failed{false};
      std::atomic<bool> sent_video_frame{false};
      std::atomic<bool> video_force_keyframe{true};
      std::atomic<int> active_display_width{0};
      std::atomic<int> active_display_height{0};
      const std::wstring effective_codec = GetEffectivePreferredCodec(session_config.preferred_codec);
      const bool start_with_vp8 = _wcsicmp(effective_codec.c_str(), L"vp8-software") == 0;
      std::atomic<bool> active_codec_is_vp8{start_with_vp8};
      std::wstring video_thread_error;
      Win32Mutex video_thread_error_mutex;
      Win32Thread video_thread;
      const bool video_thread_started = video_thread.Start([&]() {
        GdiScreenCapturer screen_capturer;
        MinimalH264Encoder h264_encoder;
        MinimalVp8Encoder vp8_encoder;
        DesktopFrameBgra desktop_frame;
        std::vector<unsigned char> nv12_frame;
        std::vector<unsigned char> i420_frame;
        const auto video_epoch = std::chrono::steady_clock::now();
        auto next_video_deadline = video_epoch;
        auto next_cursor_probe = video_epoch;
        HCURSOR last_sent_cursor = nullptr;
        bool sent_cursor_data = false;
        bool use_vp8_runtime = start_with_vp8;

        while (!stop_video_thread.load() && !is_desktop_session_stop_requested()) {
          if (active_clipboard_file_requests.load() > 0) {
            Sleep(20);
            continue;
          }
          const auto now = std::chrono::steady_clock::now();
          if (now >= next_cursor_probe) {
            HCURSOR current_cursor = nullptr;
            if (TryGetVisibleRustDeskCursor(&current_cursor) &&
                (!sent_cursor_data || current_cursor != last_sent_cursor)) {
              std::vector<unsigned char> cursor_message;
              std::wstring cursor_error;
              if (EncodeRustDeskCursorDataMessage(
                      current_cursor, &cursor_message, &cursor_error)) {
                if (!connection->SendFrame(cursor_message, nullptr)) {
                  Win32LockGuard error_lock(video_thread_error_mutex);
                  video_thread_error = L"cursor data send failed";
                  video_thread_failed.store(true);
                  return;
                }
                last_sent_cursor = current_cursor;
                sent_cursor_data = true;
              }
            }
            next_cursor_probe = now + std::chrono::milliseconds(50);
          }
          if (!request_immediate_video.load() && now < next_video_deadline) {
            Sleep(2);
            continue;
          }
          request_immediate_video.store(false);

          std::wstring capture_error;
          if (!screen_capturer.Capture(&desktop_frame, &capture_error)) {
            Win32LockGuard error_lock(video_thread_error_mutex);
            video_thread_error = L"screen capture failed: " + capture_error;
            video_thread_failed.store(true);
            return;
          }

          active_display_width.store(desktop_frame.width);
          active_display_height.store(desktop_frame.height);
          if (desktop_frame.width < 1 || desktop_frame.height < 1) {
            Win32LockGuard error_lock(video_thread_error_mutex);
            video_thread_error = L"captured invalid display dimensions";
            video_thread_failed.store(true);
            return;
          }

          const int64_t pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - video_epoch).count();
          std::vector<std::vector<unsigned char>> encoded_frames;
          std::vector<bool> key_flags;
          const bool request_keyframe = video_force_keyframe.exchange(false);

          auto encode_vp8_frame = [&](bool keyframe_requested, std::wstring* error_text) -> bool {
            if (!vp8_encoder.IsInitializedFor(desktop_frame.width, desktop_frame.height)) {
              std::wstring vp8_error;
              if (!vp8_encoder.Initialize(
                      desktop_frame.width,
                      desktop_frame.height,
                      session_config.video_fps,
                      session_config.video_bitrate_kbps,
                      &vp8_error)) {
                if (error_text != nullptr) {
                  *error_text = L"VP8 encoder init failed: " + vp8_error;
                }
                return false;
              }
              video_force_keyframe.store(true);
            }

            std::wstring vp8_error;
            if (!ConvertBgraToI420(desktop_frame, &i420_frame, &vp8_error)) {
              if (error_text != nullptr) {
                *error_text = L"BGRA to I420 failed: " + vp8_error;
              }
              return false;
            }

            if (!vp8_encoder.EncodeFrame(
                    i420_frame,
                    keyframe_requested,
                    pts_ms,
                    &encoded_frames,
                    &key_flags,
                    &vp8_error)) {
              if (error_text != nullptr) {
                *error_text = L"VP8 encode failed: " + vp8_error;
              }
              return false;
            }
            return true;
          };

          if (use_vp8_runtime) {
            std::wstring vp8_error;
            if (!encode_vp8_frame(request_keyframe, &vp8_error)) {
              Win32LockGuard error_lock(video_thread_error_mutex);
              video_thread_error = vp8_error;
              video_thread_failed.store(true);
              return;
            }
          } else {
            if (!h264_encoder.IsInitializedFor(desktop_frame.width, desktop_frame.height)) {
              if (!h264_encoder.Initialize(
                      desktop_frame.width,
                      desktop_frame.height,
                      session_config.video_fps,
                      session_config.video_bitrate_kbps,
                      &capture_error)) {
                const std::wstring h264_error = L"H264 encoder init failed: " + capture_error;
                use_vp8_runtime = true;
                active_codec_is_vp8.store(true);
                video_force_keyframe.store(true);
                encoded_frames.clear();
                key_flags.clear();
                std::wstring vp8_error;
                if (!encode_vp8_frame(true, &vp8_error)) {
                  Win32LockGuard error_lock(video_thread_error_mutex);
                  video_thread_error = h264_error + L"; VP8 fallback failed: " + vp8_error;
                  video_thread_failed.store(true);
                  return;
                }
              }
              video_force_keyframe.store(true);
            }

            if (!use_vp8_runtime) {
              ConvertBgraToNv12(desktop_frame, &nv12_frame);
              if (!h264_encoder.EncodeFrame(
                      nv12_frame,
                      request_keyframe,
                      pts_ms,
                      &encoded_frames,
                      &key_flags,
                      &capture_error)) {
                const std::wstring h264_error = L"H264 encode failed: " + capture_error;
                use_vp8_runtime = true;
                active_codec_is_vp8.store(true);
                video_force_keyframe.store(true);
                encoded_frames.clear();
                key_flags.clear();
                std::wstring vp8_error;
                if (!encode_vp8_frame(true, &vp8_error)) {
                  Win32LockGuard error_lock(video_thread_error_mutex);
                  video_thread_error = h264_error + L"; VP8 fallback failed: " + vp8_error;
                  video_thread_failed.store(true);
                  return;
                }
              }
            }
          }

          if (!encoded_frames.empty()) {
            const std::vector<unsigned char> video_message = use_vp8_runtime
                ? EncodeVp8VideoFrameMessage(encoded_frames, key_flags, pts_ms, 0)
                : EncodeH264VideoFrameMessage(encoded_frames, key_flags, pts_ms, 0);
            if (!connection->SendFrame(video_message, nullptr)) {
              Win32LockGuard error_lock(video_thread_error_mutex);
              video_thread_error = L"video frame send failed";
              video_thread_failed.store(true);
              return;
            }
            sent_video_frame.store(true);
          }

          next_video_deadline =
              now + std::chrono::milliseconds(
                        1000 / (session_config.video_fps > 0 ? session_config.video_fps : 1));
        }
      });
      if (!video_thread_started) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" failed to start video thread";
        }
        return false;
      }
      auto stop_video_session = [&]() {
        stop_video_thread.store(true);
        request_immediate_video.store(true);
        connection->Abort();
        if (video_thread.Joinable()) {
          video_thread.Join();
        }
      };
      auto try_forward_local_clipboard =
          [&](std::wstring* clipboard_status) -> bool {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_clipboard_poll) {
          return true;
        }
        next_clipboard_poll = now + std::chrono::milliseconds(350);

        const DWORD clipboard_sequence = GetClipboardSequenceNumber();
        if (clipboard_sequence == last_local_clipboard_sequence) {
          return true;
        }
        last_local_clipboard_sequence = clipboard_sequence;

        FormattedTextClipboardContent local_formatted_clipboard;
        std::wstring local_formatted_clipboard_signature;
        if (CaptureFormattedTextClipboardContent(
                &local_formatted_clipboard,
                &local_formatted_clipboard_signature,
                nullptr)) {
          if (local_formatted_clipboard_signature !=
              last_local_formatted_clipboard_signature) {
            if (local_formatted_clipboard_signature ==
                last_remote_formatted_clipboard_signature) {
              last_local_formatted_clipboard_signature =
                  local_formatted_clipboard_signature;
              last_remote_formatted_clipboard_signature.clear();
            } else {
              std::vector<ClipboardMessageData> outbound_clipboards;
              if (BuildFormattedTextClipboardMessages(
                      local_formatted_clipboard,
                      &outbound_clipboards)) {
                const bool sending_rich_text =
                    outbound_clipboards.size() > 1U ||
                    (outbound_clipboards.size() == 1U &&
                     outbound_clipboards.front().format !=
                         kClipboardFormatText);
                const std::vector<unsigned char> clipboard_message =
                    outbound_clipboards.size() == 1U
                        ? EncodeSingleClipboardMessage(
                              outbound_clipboards.front())
                        : EncodeMultiClipboardMessage(outbound_clipboards);
                std::wstring clipboard_error;
                if (!connection->SendFrame(
                        clipboard_message,
                        &clipboard_error)) {
                  if (clipboard_status != nullptr) {
                    *clipboard_status =
                        channel_name + L" local clipboard send failed: " +
                        clipboard_error;
                  }
                  return false;
                }
                if (clipboard_status != nullptr) {
                  *clipboard_status =
                      sending_rich_text
                          ? channel_name +
                                L" forwarded local formatted clipboard to controller"
                          : channel_name +
                                L" forwarded local text clipboard to controller";
                }
              }
              last_local_formatted_clipboard_signature =
                  local_formatted_clipboard_signature;
            }
          }
          last_local_clipboard_text =
              local_formatted_clipboard.has_text
                  ? local_formatted_clipboard.text
                  : std::wstring();
        } else {
          last_local_formatted_clipboard_signature.clear();
          last_local_clipboard_text.clear();
        }

        if (peer_cliprdr_ready) {
          std::vector<LocalClipboardFileDescriptor> file_entries;
          std::vector<unsigned char> descriptor_payload;
          std::wstring signature;
          if (CaptureLocalClipboardFileDescriptors(
                  &file_entries,
                  &descriptor_payload,
                  &signature,
                  nullptr)) {
            if (signature != last_local_file_clipboard_signature ||
                descriptor_payload != local_file_clipboard_descriptor_payload) {
              std::vector<CliprdrFormatData> formats;
              CliprdrFormatData file_descriptor_format;
              file_descriptor_format.id = local_file_descriptor_format_id;
              file_descriptor_format.format_name = L"FileGroupDescriptorW";
              formats.push_back(file_descriptor_format);
              CliprdrFormatData file_contents_format;
              file_contents_format.id = local_file_contents_format_id;
              file_contents_format.format_name = L"FileContents";
              formats.push_back(file_contents_format);

              std::wstring clipboard_error;
              if (!connection->SendFrame(
                      EncodeCliprdrFormatListMessage(formats),
                      &clipboard_error)) {
                if (clipboard_status != nullptr) {
                  *clipboard_status =
                      channel_name + L" local file clipboard send failed: " + clipboard_error;
                }
                return false;
              }
              local_file_clipboard_entries = std::move(file_entries);
              local_file_clipboard_descriptor_payload =
                  std::move(descriptor_payload);
              last_local_file_clipboard_signature = std::move(signature);
              if (clipboard_status != nullptr) {
                *clipboard_status =
                    channel_name + L" forwarded local file clipboard to controller";
              }
            }
          }
        }
        return true;
      };

      bool echoed_test_delay = false;
      std::vector<unsigned char> frame;
      while (true) {
        if (is_desktop_session_stop_requested()) {
          const bool manual_close_requested =
              !use_auxiliary_stop_path &&
              active_session_manual_close_requested_.exchange(false);
          if (manual_close_requested) {
            connection->SendFrame(
                EncodeCloseReasonMessage(kLoginMsgClosedManuallyByPeer),
                nullptr);
          }
          close_file_clipboard_brokers(channel_name + L" session stopped");
          stop_video_session();
          if (session_status != nullptr) {
            *session_status = manual_close_requested
                ? channel_name + L" closed locally"
                : (use_auxiliary_stop_path
                       ? channel_name + L" stopping auxiliary desktop session"
                       : channel_name + L" stopping active session");
          }
          return true;
        }

        if (video_thread_failed.load()) {
          close_file_clipboard_brokers(channel_name + L" video thread failed");
          stop_video_session();
          if (session_status != nullptr) {
            Win32LockGuard error_lock(video_thread_error_mutex);
            *session_status = channel_name + L" " + video_thread_error;
          }
          return false;
        }

        frame.clear();
        const TcpFramedConnection::ReceiveState post_login_state =
            connection->ReceiveFrame(&frame, kSessionPollMs, session_status);
        if (post_login_state == TcpFramedConnection::ReceiveState::kTimeout) {
          if (!try_forward_local_clipboard(session_status)) {
            close_file_clipboard_brokers(channel_name + L" local clipboard send failed");
            stop_video_session();
            return false;
          }
          if (session_status != nullptr) {
            if (active_clipboard_file_requests.load() > 0) {
              *session_status = describe_file_clipboard_progress();
            } else {
              const std::wstring active_codec_label =
                  active_codec_is_vp8.load() ? L"VP8" : L"H264";
              *session_status =
                  channel_name +
                  (sent_video_frame.load()
                       ? std::wstring(L" session established; streaming ") + active_codec_label +
                             L" desktop frames"
                       : (echoed_test_delay
                              ? std::wstring(L" session kept alive; preparing first ") +
                                    active_codec_label + L" desktop frame"
                              : L" session established; waiting for controller follow-up"));
            }
          }
          continue;
        }
        if (post_login_state != TcpFramedConnection::ReceiveState::kFrame) {
          close_file_clipboard_brokers(
              channel_name + L" receive " + describe_receive_state(post_login_state));
          stop_video_session();
          if (session_status != nullptr) {
            const std::wstring previous = *session_status;
            *session_status = channel_name + L" post-login receive " + describe_receive_state(post_login_state);
            if (!previous.empty()) {
              *session_status += L": ";
              *session_status += previous;
            }
          }
          return true;
        }

        bool found_test_delay = false;
        bool test_delay_from_client = false;
        if (TryParseTestDelayMessage(frame, &found_test_delay, &test_delay_from_client) &&
            found_test_delay) {
          if (test_delay_from_client) {
            if (!connection->SendFrame(frame, session_status)) {
              close_file_clipboard_brokers(channel_name + L" test-delay echo send failed");
              stop_video_session();
              if (session_status != nullptr && !session_status->empty()) {
                *session_status = channel_name + L" test-delay echo send failed: " + *session_status;
              }
              return false;
            }
            echoed_test_delay = true;
            if (session_status != nullptr) {
              *session_status = channel_name + L" session established; echoed controller TestDelay";
            }
          } else if (session_status != nullptr) {
            echoed_test_delay = true;
            *session_status = channel_name + L" session test-delay acknowledged";
          }
          continue;
        }

        const SessionMessageType session_message = ParseSessionMessage(frame);
        bool handled_follow_up = false;
        if (session_message.has_close_reason) {
          close_file_clipboard_brokers(channel_name + L" controller closed session");
          stop_video_session();
          if (session_status != nullptr) {
            *session_status = channel_name + L" closed by controller";
            if (!session_message.close_reason.empty()) {
              *session_status += L": ";
              *session_status += session_message.close_reason;
            }
          }
          return true;
        }
        if (session_message.wants_refresh_video) {
          video_force_keyframe.store(true);
          request_immediate_video.store(true);
          handled_follow_up = true;
          if (session_status != nullptr) {
            *session_status = channel_name + L" received refresh_video; scheduling keyframe";
          }
        }
        if (session_message.has_mouse) {
          HandleMouseEvent(
              session_message.mouse,
              active_display_width.load() > 0 ? active_display_width.load() : GetDesktopCaptureBounds().width,
              active_display_height.load() > 0 ? active_display_height.load() : GetDesktopCaptureBounds().height);
          request_immediate_video.store(true);
          handled_follow_up = true;
        }
        if (session_message.has_key) {
          HandleKeyEvent(session_message.key);
          request_immediate_video.store(true);
          handled_follow_up = true;
        }
        if (!session_message.clipboards.empty()) {
          handled_follow_up = true;
          FormattedTextClipboardContent remote_formatted_clipboard;
          std::wstring remote_formatted_clipboard_signature;
          std::wstring clipboard_error;
          if (DecodeFormattedTextClipboardContent(
                  session_message.clipboards,
                  &remote_formatted_clipboard,
                  &remote_formatted_clipboard_signature,
                  &clipboard_error)) {
            if (SetClipboardFormattedTextContent(
                    remote_formatted_clipboard,
                    &clipboard_error)) {
              last_remote_clipboard_text =
                  remote_formatted_clipboard.has_text
                      ? remote_formatted_clipboard.text
                      : std::wstring();
              last_local_clipboard_text = last_remote_clipboard_text;
              last_remote_formatted_clipboard_signature =
                  remote_formatted_clipboard_signature;
              last_local_formatted_clipboard_signature =
                  remote_formatted_clipboard_signature;
              if (session_status != nullptr) {
                *session_status =
                    (remote_formatted_clipboard.has_html ||
                     remote_formatted_clipboard.has_rtf ||
                     remote_formatted_clipboard.has_excel_xml)
                        ? channel_name +
                              L" updated remote formatted clipboard"
                        : channel_name + L" updated remote text clipboard";
              }
            } else if (session_status != nullptr) {
              *session_status =
                  channel_name + L" clipboard text update failed: " +
                  clipboard_error;
            }
          } else if (session_status != nullptr && !clipboard_error.empty()) {
            *session_status =
                channel_name + L" clipboard text update failed: " +
                clipboard_error;
          }
        }
        if (session_message.has_cliprdr) {
          handled_follow_up = true;
          switch (session_message.cliprdr.kind) {
            case CliprdrMessageKind::kReady:
              peer_cliprdr_ready = true;
              last_local_clipboard_sequence = 0;
              next_clipboard_poll = std::chrono::steady_clock::time_point();
              if (session_status != nullptr) {
                *session_status = channel_name + L" cliprdr monitor-ready acknowledged";
              }
              break;
            case CliprdrMessageKind::kFiles:
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" received clipboard file audit (" +
                    std::to_wstring(session_message.cliprdr.files.size()) + L" item(s))";
              }
              break;
            case CliprdrMessageKind::kFormatList: {
              int file_descriptor_format_id = 0;
              int file_contents_format_id = 0;
              for (const CliprdrFormatData& format : session_message.cliprdr.formats) {
                if (_wcsicmp(format.format_name.c_str(), L"FileGroupDescriptorW") == 0) {
                  file_descriptor_format_id = format.id;
                } else if (_wcsicmp(format.format_name.c_str(), L"FileContents") == 0) {
                  file_contents_format_id = format.id;
                }
              }
              if (file_descriptor_format_id != 0 &&
                  file_contents_format_id != 0) {
                std::wstring clipboard_error;
                if (!connection->SendFrame(
                        EncodeCliprdrFormatDataRequestMessage(
                            file_descriptor_format_id),
                        &clipboard_error)) {
                  if (session_status != nullptr) {
                    *session_status = channel_name + L" cliprdr format-data request failed: " + clipboard_error;
                  }
                } else if (session_status != nullptr) {
                  *session_status = channel_name + L" requesting remote clipboard file descriptors";
                }
              } else if (session_status != nullptr) {
                *session_status = channel_name + L" controller clipboard formats did not include FileGroupDescriptorW/FileContents";
              }
              break;
            }
            case CliprdrMessageKind::kFormatDataRequest:
              if (session_message.cliprdr.requested_format_id ==
                      local_file_descriptor_format_id &&
                  !local_file_clipboard_descriptor_payload.empty()) {
                std::wstring clipboard_error;
                if (!connection->SendFrame(
                        EncodeCliprdrFormatDataResponseMessage(
                            kCliprdrResponseOk,
                            local_file_clipboard_descriptor_payload),
                        &clipboard_error)) {
                  if (session_status != nullptr) {
                    *session_status =
                        channel_name + L" local clipboard format-data response failed: " +
                        clipboard_error;
                  }
                  close_file_clipboard_brokers(
                      channel_name + L" local clipboard format-data response send failed");
                  stop_video_session();
                  return false;
                }
                if (session_status != nullptr) {
                  *session_status =
                      channel_name + L" sent local clipboard file descriptors to controller";
                }
              }
              break;
            case CliprdrMessageKind::kFormatDataResponse:
              if (session_message.cliprdr.msg_flags != kCliprdrResponseOk) {
                if (session_status != nullptr) {
                  *session_status = channel_name + L" controller rejected clipboard format-data request";
                }
              } else if (!install_remote_file_clipboard(
                             session_message.cliprdr.payload,
                             session_status)) {
                if (session_status != nullptr && session_status->empty()) {
                  *session_status = channel_name + L" clipboard virtual file install failed";
                }
              }
              break;
            case CliprdrMessageKind::kFileContentsResponse:
              if (!handle_file_contents_response(session_message.cliprdr) &&
                  session_status != nullptr && session_status->empty()) {
                *session_status = channel_name + L" clipboard file response could not be applied";
              }
              break;
            case CliprdrMessageKind::kFileContentsRequest:
              if (!local_file_clipboard_entries.empty()) {
                active_clipboard_file_requests.fetch_add(1);
                ScopeExit local_file_request_scope([&]() {
                  active_clipboard_file_requests.fetch_sub(1);
                });
                std::vector<unsigned char> payload;
                std::wstring build_error;
                const bool build_ok = BuildLocalClipboardFileContentsPayload(
                    local_file_clipboard_entries,
                    session_message.cliprdr,
                    &payload,
                    &build_error);
                std::wstring send_error;
                if (!connection->SendFrame(
                        EncodeCliprdrFileContentsResponseMessage(
                            build_ok ? kCliprdrResponseOk : 0,
                            session_message.cliprdr.stream_id,
                            payload),
                        &send_error)) {
                  if (session_status != nullptr) {
                    *session_status =
                        channel_name + L" local clipboard file response send failed: " +
                        send_error;
                  }
                  close_file_clipboard_brokers(
                      channel_name + L" local clipboard file response send failed");
                  stop_video_session();
                  return false;
                }
                if (session_status != nullptr) {
                  *session_status = build_ok
                      ? (((session_message.cliprdr.dw_flags &
                           kCliprdrFileContentsSizeFlag) != 0)
                             ? channel_name +
                                   L" reported local clipboard file size to controller"
                             : channel_name +
                                   L" streamed local clipboard file block to controller")
                      : channel_name +
                            L" local clipboard file request failed: " + build_error;
                }
              }
              break;
            case CliprdrMessageKind::kTryEmpty:
              close_file_clipboard_brokers(
                  channel_name + L" remote controller cleared file clipboard");
              file_clipboard_brokers.clear();
              if (session_status != nullptr) {
                *session_status = channel_name + L" remote controller cleared file clipboard state";
              }
              break;
            default:
              if (session_status != nullptr) {
                *session_status =
                    channel_name +
                    L" received cliprdr follow-up kind=" +
                    std::to_wstring(static_cast<int>(session_message.cliprdr.kind));
              }
              break;
          }
        }
        if (handled_follow_up) {
          if (session_status != nullptr && session_status->empty() && sent_video_frame.load()) {
            *session_status = channel_name + L" session established; streaming desktop and handling input";
          }
          continue;
        }

        if (session_status != nullptr) {
          *session_status =
              channel_name +
              L" session established; ignoring follow-up fields=" +
              FormatObservedFields(ExtractTopLevelMessageFields(frame));
        }
        continue;
      }
    };

    auto run_minimal_direct_ip_session_over_connection =
        [this,
         session_config,
         session_temporary_password_snapshot,
         session_fixed_password_snapshot,
         session_device_uuid_snapshot,
         generate_numeric_password,
         describe_receive_state,
         &should_auto_accept_secondary_file_transfer,
         &run_file_transfer_session_loop,
         &run_desktop_session_loop](
             TcpFramedConnection* connection,
             const std::wstring& channel_name,
             const std::wstring& display_remote_id_override,
             bool use_auxiliary_stop_path,
             std::wstring* session_status) -> bool {
      if (use_auxiliary_stop_path) {
        RegisterAuxiliarySessionConnection(connection);
      } else {
        RegisterActiveSessionConnection(connection);
      }
      std::shared_ptr<void> connection_scope(
          connection,
          [this, use_auxiliary_stop_path](void* registered_connection) {
            if (use_auxiliary_stop_path) {
              ClearAuxiliarySessionConnection(registered_connection);
            } else {
              ClearActiveSessionConnection(registered_connection);
            }
          });
      const std::wstring display_remote_id =
          Trim(display_remote_id_override);
      const auto is_direct_session_stop_requested =
          [this, use_auxiliary_stop_path]() -> bool {
        return use_auxiliary_stop_path ? IsAuxiliarySessionStopRequested()
                                       : IsActiveSessionStopRequested();
      };

      const std::string hash_salt = session_device_uuid_snapshot.empty()
                                        ? WideToUtf8(session_config.host_id)
                                        : WideToUtf8(session_device_uuid_snapshot);
      const std::string hash_challenge =
          WideToUtf8(generate_numeric_password(session_config.temporary_password_length));
      const std::vector<unsigned char> hash_message =
          EncodeHashMessage(hash_salt, hash_challenge);
      if (!connection->SendFrame(hash_message, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" hash send failed: " + *session_status;
        }
        return false;
      }

      const std::vector<unsigned char> expected_temporary_password =
          (!session_temporary_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_temporary_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const std::vector<unsigned char> expected_fixed_password =
          (!session_fixed_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_fixed_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const auto session_start = std::chrono::steady_clock::now();
      bool saw_empty_login = false;
      std::chrono::steady_clock::time_point manual_password_deadline;
      unsigned long incoming_approval_token = 0;
      ScopeExit incoming_approval_scope([this, &incoming_approval_token]() {
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }
      });

      while (true) {
        if (is_direct_session_stop_requested()) {
          const bool manual_close_requested =
              !use_auxiliary_stop_path &&
              active_session_manual_close_requested_.exchange(false);
          if (manual_close_requested) {
            connection->SendFrame(
                EncodeCloseReasonMessage(kLoginMsgClosedManuallyByPeer),
                nullptr);
          }
          if (session_status != nullptr) {
            *session_status = manual_close_requested
                ? channel_name + L" closed locally before login"
                : (use_auxiliary_stop_path
                       ? channel_name + L" auxiliary session stop requested"
                       : channel_name + L" session stop requested");
          }
          connection->Abort();
          return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          if (now >= manual_password_deadline) {
            if (session_status != nullptr) {
              *session_status = incoming_approval_token != 0
                  ? channel_name + L" incoming approval timed out"
                  : channel_name + L" manual password entry timed out";
            }
            return true;
          }
        } else {
          const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - session_start).count();
          if (elapsed_ms >= static_cast<long long>(kLoginWaitTimeoutMs)) {
            if (session_status != nullptr) {
              *session_status = channel_name + L" login wait timed out";
            }
            return false;
          }
        }

        unsigned long remaining_ms = 1000;
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              manual_password_deadline - now).count();
          remaining_ms = remaining > 1000
              ? 1000
              : (remaining > 0 ? static_cast<unsigned long>(remaining) : 1);
        } else {
          const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - session_start).count();
          const auto wait_left = static_cast<long long>(kLoginWaitTimeoutMs) - elapsed_ms;
          remaining_ms = wait_left > 1000
              ? 1000
              : (wait_left > 0 ? static_cast<unsigned long>(wait_left) : 1);
        }
        bool accepted_temporary_password = false;
        bool accepted_fixed_password = false;
        bool accepted_click_approval = false;
        bool accepted_secondary_file_transfer = false;
        if (incoming_approval_token != 0) {
          const IncomingApprovalDecision approval_decision =
              GetIncomingApprovalDecision(incoming_approval_token);
          if (approval_decision == IncomingApprovalDecision::kRejected) {
            const std::vector<unsigned char> rejected =
                EncodeLoginResponseErrorMessage(kLoginMsgClosedManuallyByPeer);
            connection->SendFrame(rejected, nullptr);
            if (session_status != nullptr) {
              *session_status = channel_name + L" rejected incoming request locally";
            }
            return true;
          }
          if (approval_decision == IncomingApprovalDecision::kAccepted) {
            accepted_click_approval = true;
          }
        }

        LoginRequestData login_request;
        if (!accepted_click_approval) {
          std::vector<unsigned char> frame;
          const TcpFramedConnection::ReceiveState login_state =
              connection->ReceiveFrame(&frame, remaining_ms, session_status);
          if (login_state != TcpFramedConnection::ReceiveState::kFrame) {
            if (login_state == TcpFramedConnection::ReceiveState::kTimeout &&
                manual_password_deadline.time_since_epoch().count() != 0) {
              if (session_status != nullptr) {
                *session_status = incoming_approval_token != 0
                    ? channel_name + L" waiting for local approval or password entry"
                    : channel_name + L" waiting for manual password entry";
              }
              continue;
            }
            if (session_status != nullptr) {
              const std::wstring previous = *session_status;
              *session_status = channel_name + L" receive " + describe_receive_state(login_state);
              if (!previous.empty()) {
                *session_status += L": ";
                *session_status += previous;
              }
            }
            return login_state == TcpFramedConnection::ReceiveState::kClosed ||
                   manual_password_deadline.time_since_epoch().count() != 0;
          }

          if (!ParseLoginRequestMessage(frame, &login_request)) {
            const SessionMessageType session_message = ParseSessionMessage(frame);
            if (session_message.has_close_reason) {
              if (session_status != nullptr) {
                *session_status = channel_name + L" controller closed before login";
                if (!session_message.close_reason.empty()) {
                  *session_status += L": ";
                  *session_status += session_message.close_reason;
                }
              }
              return true;
            }
            if (session_status != nullptr) {
              *session_status = channel_name + L" received plain message, but it was not LoginRequest";
            }
            return false;
          }
          AppendPortableHostLog(
              L"login",
              channel_name + L" parsed LoginRequest: " + DescribeLoginRequestForLog(login_request));

          if (login_request.password.empty()) {
            if (should_auto_accept_secondary_file_transfer(login_request, display_remote_id)) {
              accepted_secondary_file_transfer = true;
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" auto-accepted secondary file-transfer login");
            } else {
              saw_empty_login = true;
              if (incoming_approval_token == 0) {
                incoming_approval_token = BeginIncomingApproval(
                    display_remote_id.empty()
                        ? login_request.my_id
                        : display_remote_id,
                    display_remote_id.empty()
                        ? login_request.my_name
                        : L"");
              }
              manual_password_deadline =
                  std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(kIncomingApprovalTimeoutMs);
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" received incoming remote request; waiting for approval or password entry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" empty-password login is waiting for local approval/password entry");
              continue;
            }
          }

          if (!accepted_secondary_file_transfer) {
            accepted_temporary_password =
                !expected_temporary_password.empty() &&
                login_request.password == expected_temporary_password;
            accepted_fixed_password =
                !expected_fixed_password.empty() &&
                login_request.password == expected_fixed_password;
            if (!accepted_temporary_password &&
                !accepted_fixed_password) {
              const std::vector<unsigned char> wrong_password =
                  EncodeLoginResponseErrorMessage(kLoginMsgWrongPassword);
              if (!connection->SendFrame(wrong_password, session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" wrong password response send failed: " + *session_status;
                }
                return false;
              }
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" rejected LoginRequest with Wrong Password; waiting for retry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" rejected LoginRequest with wrong password");
              continue;
            }
          }
        }

        const std::vector<unsigned char> login_response =
            EncodeLoginResponsePeerInfoMessage(session_config);
        if (!connection->SendFrame(login_response, session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" login response send failed: " + *session_status;
          }
          return false;
        }

        if (session_status != nullptr) {
          if (accepted_secondary_file_transfer) {
            *session_status =
                channel_name +
                L" accepted secondary file-transfer request from active session and sent PeerInfo login response";
          } else if (accepted_fixed_password) {
            *session_status =
                channel_name + L" accepted fixed password and sent PeerInfo login response";
          } else if (accepted_click_approval) {
            *session_status =
                channel_name + L" accepted incoming remote request by local confirmation and sent PeerInfo login response";
          } else {
            *session_status = saw_empty_login
                                  ? channel_name + L" accepted manual LoginRequest and sent PeerInfo login response"
                                  : channel_name + L" accepted LoginRequest and sent PeerInfo login response";
          }
        }
        AppendPortableHostLog(
            L"login",
            channel_name + L" sent PeerInfo login response; secondary_file_transfer=" +
                BoolToLogText(accepted_secondary_file_transfer) +
                L", fixed_password=" + BoolToLogText(accepted_fixed_password) +
                L", click_approval=" + BoolToLogText(accepted_click_approval) +
                L", manual_login=" + BoolToLogText(saw_empty_login));
        std::wstring connected_remote_id;
        std::wstring connected_remote_name;
        if (accepted_click_approval) {
          CaptureIncomingApprovalRemoteIdentity(
              &connected_remote_id,
              &connected_remote_name);
        } else if (!display_remote_id.empty()) {
          connected_remote_id = display_remote_id;
          connected_remote_name.clear();
        } else {
          connected_remote_id = login_request.my_id;
          connected_remote_name = login_request.my_name;
        }
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }

        if (login_request.has_file_transfer) {
          return run_file_transfer_session_loop(
              connection,
              channel_name,
              connected_remote_id,
              connected_remote_name,
              login_request.file_transfer_dir,
              login_request.file_transfer_show_hidden,
              session_status);
        }

        if (!connection->SendFrame(EncodeCliprdrMonitorReadyMessage(), session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" cliprdr monitor-ready send failed: " + *session_status;
          }
          return false;
        }
        return run_desktop_session_loop(
            connection,
            channel_name,
            connected_remote_id,
            connected_remote_name,
            use_auxiliary_stop_path,
            session_status);
      }
    };

    auto run_minimal_secure_session_over_connection =
        [this,
         session_config,
         session_temporary_password_snapshot,
         session_fixed_password_snapshot,
         session_device_uuid_snapshot,
         session_secret_key_bytes,
         generate_numeric_password,
         describe_receive_state,
         &should_auto_accept_secondary_file_transfer,
         &run_file_transfer_session_loop](
                                                      TcpFramedConnection* connection,
                                                      const std::wstring& channel_name,
                                                      const std::wstring& display_remote_id_override,
                                                      std::wstring* session_status) -> bool {
      RegisterActiveSessionConnection(connection);
      std::shared_ptr<void> connection_scope(
          connection,
          [this](void* registered_connection) {
            ClearActiveSessionConnection(registered_connection);
          });
      const std::wstring display_remote_id =
          Trim(display_remote_id_override);
      std::array<unsigned char, crypto_box_PUBLICKEYBYTES> session_curve_public = {};
      std::array<unsigned char, crypto_box_SECRETKEYBYTES> session_curve_secret = {};
      std::vector<unsigned char> signed_id = EncodeSignedIdMessage(
          session_config.host_id,
          session_secret_key_bytes,
          &session_curve_public,
          &session_curve_secret,
          session_status);
      if (signed_id.empty()) {
        return false;
      }
      if (!connection->SendRawFrame(signed_id, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" signed_id send failed: " + *session_status;
        }
        return false;
      }

      std::vector<unsigned char> frame;
      const TcpFramedConnection::ReceiveState handshake_state =
          connection->ReceiveFrame(&frame, kConnectTimeoutMs, session_status);
      if (handshake_state != TcpFramedConnection::ReceiveState::kFrame) {
        if (session_status != nullptr) {
          const std::wstring previous = *session_status;
          *session_status = channel_name + L" handshake receive " + describe_receive_state(handshake_state);
          if (!previous.empty()) {
            *session_status += L": ";
            *session_status += previous;
          }
        }
        return false;
      }

      PublicKeyMessageData public_key_message;
      if (!ParsePublicKeyMessage(frame, &public_key_message)) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" handshake parse failed";
        }
        return false;
      }
      if (public_key_message.asymmetric_value.empty()) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" controller requested insecure fallback";
        }
        return false;
      }

      std::array<unsigned char, crypto_secretbox_KEYBYTES> symmetric_key = {};
      if (!DecodeSymmetricKeyFromPublicKey(
              public_key_message.asymmetric_value,
              public_key_message.symmetric_value,
              session_curve_secret,
              &symmetric_key,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" handshake key decode failed: " + *session_status;
        }
        return false;
      }
      connection->SetSymmetricKey(symmetric_key);

      const std::string hash_salt = session_device_uuid_snapshot.empty()
                                        ? WideToUtf8(session_config.host_id)
                                        : WideToUtf8(session_device_uuid_snapshot);
      const std::string hash_challenge =
          WideToUtf8(generate_numeric_password(session_config.temporary_password_length));
      const std::vector<unsigned char> hash_message =
          EncodeHashMessage(hash_salt, hash_challenge);
      if (!connection->SendFrame(hash_message, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" hash send failed: " + *session_status;
        }
        return false;
      }

      const std::vector<unsigned char> expected_temporary_password =
          (!session_temporary_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_temporary_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const std::vector<unsigned char> expected_fixed_password =
          (!session_fixed_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_fixed_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const auto session_start = std::chrono::steady_clock::now();
      bool saw_empty_login = false;
      std::chrono::steady_clock::time_point manual_password_deadline;
      unsigned long incoming_approval_token = 0;
      ScopeExit incoming_approval_scope([this, &incoming_approval_token]() {
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }
      });

      while (true) {
        if (IsActiveSessionStopRequested()) {
          const bool manual_close_requested =
              active_session_manual_close_requested_.exchange(false);
          if (manual_close_requested) {
            connection->SendFrame(
                EncodeCloseReasonMessage(kLoginMsgClosedManuallyByPeer),
                nullptr);
          }
          if (session_status != nullptr) {
            *session_status = manual_close_requested
                ? channel_name + L" closed locally before login"
                : channel_name + L" session stop requested";
          }
          connection->Abort();
          return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          if (now >= manual_password_deadline) {
            if (session_status != nullptr) {
              *session_status = incoming_approval_token != 0
                  ? channel_name + L" incoming approval timed out"
                  : channel_name + L" manual password entry timed out";
            }
            return true;
          }
        } else {
          const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - session_start).count();
          if (elapsed_ms >= static_cast<long long>(kLoginWaitTimeoutMs)) {
            if (session_status != nullptr) {
              *session_status = channel_name + L" login wait timed out";
            }
            return false;
          }
        }

        unsigned long remaining_ms = 1000;
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              manual_password_deadline - now).count();
          remaining_ms = remaining > 1000
              ? 1000
              : (remaining > 0 ? static_cast<unsigned long>(remaining) : 1);
        } else {
          const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - session_start).count();
          const auto wait_left = static_cast<long long>(kLoginWaitTimeoutMs) - elapsed_ms;
          remaining_ms = wait_left > 1000
              ? 1000
              : (wait_left > 0 ? static_cast<unsigned long>(wait_left) : 1);
        }
        bool accepted_temporary_password = false;
        bool accepted_fixed_password = false;
        bool accepted_click_approval = false;
        bool accepted_secondary_file_transfer = false;
        if (incoming_approval_token != 0) {
          const IncomingApprovalDecision approval_decision =
              GetIncomingApprovalDecision(incoming_approval_token);
          if (approval_decision == IncomingApprovalDecision::kRejected) {
            const std::vector<unsigned char> rejected =
                EncodeLoginResponseErrorMessage(kLoginMsgClosedManuallyByPeer);
            connection->SendFrame(rejected, nullptr);
            if (session_status != nullptr) {
              *session_status = channel_name + L" rejected incoming request locally";
            }
            return true;
          }
          if (approval_decision == IncomingApprovalDecision::kAccepted) {
            accepted_click_approval = true;
          }
        }

        LoginRequestData login_request;
        if (!accepted_click_approval) {
          frame.clear();
          const TcpFramedConnection::ReceiveState login_state =
              connection->ReceiveFrame(&frame, remaining_ms, session_status);
          if (login_state != TcpFramedConnection::ReceiveState::kFrame) {
            if (login_state == TcpFramedConnection::ReceiveState::kTimeout &&
                manual_password_deadline.time_since_epoch().count() != 0) {
              if (session_status != nullptr) {
                *session_status = incoming_approval_token != 0
                    ? channel_name + L" waiting for local approval or password entry"
                    : channel_name + L" waiting for manual password entry";
              }
              continue;
            }
            if (session_status != nullptr) {
              const std::wstring previous = *session_status;
              *session_status = channel_name + L" encrypted receive " + describe_receive_state(login_state);
              if (!previous.empty()) {
                *session_status += L": ";
                *session_status += previous;
              }
            }
            return login_state == TcpFramedConnection::ReceiveState::kClosed ||
                   manual_password_deadline.time_since_epoch().count() != 0;
          }

          if (!ParseLoginRequestMessage(frame, &login_request)) {
            const SessionMessageType session_message = ParseSessionMessage(frame);
            if (session_message.has_close_reason) {
              if (session_status != nullptr) {
                *session_status = channel_name + L" controller closed before login";
                if (!session_message.close_reason.empty()) {
                  *session_status += L": ";
                  *session_status += session_message.close_reason;
                }
              }
              return true;
            }
            if (session_status != nullptr) {
              *session_status = channel_name + L" received encrypted message, but it was not LoginRequest";
            }
            return false;
          }
          AppendPortableHostLog(
              L"login",
              channel_name + L" parsed encrypted LoginRequest: " +
                  DescribeLoginRequestForLog(login_request));

          if (login_request.password.empty()) {
            if (should_auto_accept_secondary_file_transfer(login_request, display_remote_id)) {
              accepted_secondary_file_transfer = true;
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" auto-accepted secondary encrypted file-transfer login");
            } else {
              saw_empty_login = true;
              if (incoming_approval_token == 0) {
                incoming_approval_token = BeginIncomingApproval(
                    display_remote_id.empty()
                        ? login_request.my_id
                        : display_remote_id,
                    display_remote_id.empty()
                        ? login_request.my_name
                        : L"");
              }
              manual_password_deadline =
                  std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(kIncomingApprovalTimeoutMs);
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" received incoming remote request; waiting for approval or password entry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" empty-password encrypted login is waiting for local approval/password entry");
              continue;
            }
          }

          if (!accepted_secondary_file_transfer) {
            accepted_temporary_password =
                !expected_temporary_password.empty() &&
                login_request.password == expected_temporary_password;
            accepted_fixed_password =
                !expected_fixed_password.empty() &&
                login_request.password == expected_fixed_password;
            if (!accepted_temporary_password &&
                !accepted_fixed_password) {
              const std::vector<unsigned char> wrong_password =
                  EncodeLoginResponseErrorMessage(kLoginMsgWrongPassword);
              if (!connection->SendFrame(wrong_password, session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" wrong password response send failed: " + *session_status;
                }
                return false;
              }
              if (session_status != nullptr) {
                *session_status =
                    channel_name + L" rejected encrypted LoginRequest with Wrong Password; waiting for retry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name + L" rejected encrypted LoginRequest with wrong password");
              continue;
            }
          }
        }

        const std::vector<unsigned char> login_response =
            EncodeLoginResponsePeerInfoMessage(session_config);
        if (!connection->SendFrame(login_response, session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" login response send failed: " + *session_status;
          }
          return false;
        }

        if (session_status != nullptr) {
          if (accepted_secondary_file_transfer) {
            *session_status =
                channel_name +
                L" accepted secondary encrypted file-transfer request from active session and sent PeerInfo login response";
          } else if (accepted_fixed_password) {
            *session_status =
                channel_name + L" accepted fixed password and sent PeerInfo login response";
          } else if (accepted_click_approval) {
            *session_status =
                channel_name + L" accepted incoming remote request by local confirmation and sent PeerInfo login response";
          } else {
            *session_status = saw_empty_login
                                  ? channel_name + L" accepted manual encrypted LoginRequest and sent PeerInfo login response"
                                  : channel_name + L" accepted encrypted LoginRequest and sent PeerInfo login response";
          }
        }
        AppendPortableHostLog(
            L"login",
            channel_name + L" sent encrypted PeerInfo login response; secondary_file_transfer=" +
                BoolToLogText(accepted_secondary_file_transfer) +
                L", fixed_password=" + BoolToLogText(accepted_fixed_password) +
                L", click_approval=" + BoolToLogText(accepted_click_approval) +
                L", manual_login=" + BoolToLogText(saw_empty_login));
        std::wstring connected_remote_id;
        std::wstring connected_remote_name;
        if (accepted_click_approval) {
          CaptureIncomingApprovalRemoteIdentity(
              &connected_remote_id,
              &connected_remote_name);
        } else if (!display_remote_id.empty()) {
          connected_remote_id = display_remote_id;
          connected_remote_name.clear();
        } else {
          connected_remote_id = login_request.my_id;
          connected_remote_name = login_request.my_name;
        }
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }

        if (login_request.has_file_transfer) {
          return run_file_transfer_session_loop(
              connection,
              channel_name,
              connected_remote_id,
              connected_remote_name,
              login_request.file_transfer_dir,
              login_request.file_transfer_show_hidden,
              session_status);
        }

        if (!connection->SendFrame(EncodeCliprdrMonitorReadyMessage(), session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" cliprdr monitor-ready send failed: " + *session_status;
          }
          return false;
        }

        desktop_session_count_.fetch_add(1);
        active_session_connected_.store(true);
        StoreActiveSessionIdentity(connected_remote_id, connected_remote_name);
        ScopeExit desktop_session_scope([this]() {
          const long remaining = desktop_session_count_.fetch_sub(1) - 1;
          if (remaining <= 0) {
            active_session_connected_.store(false);
            ClearActiveSessionIdentity();
          }
        });

        std::atomic<int> active_clipboard_file_requests{0};
        std::atomic<int> next_clipboard_stream_id{1};
        std::wstring last_local_clipboard_text;
        std::wstring last_remote_clipboard_text;
        GetClipboardUnicodeText(&last_local_clipboard_text, nullptr);
        std::wstring last_local_formatted_clipboard_signature;
        std::wstring last_remote_formatted_clipboard_signature;
        FormattedTextClipboardContent initial_formatted_clipboard;
        CaptureFormattedTextClipboardContent(
            &initial_formatted_clipboard,
            &last_local_formatted_clipboard_signature,
            nullptr);
        auto next_clipboard_poll = std::chrono::steady_clock::now();
        DWORD last_local_clipboard_sequence = GetClipboardSequenceNumber();
        bool peer_cliprdr_ready = false;
        const int local_file_descriptor_format_id =
            static_cast<int>(GetFileDescriptorClipboardFormat());
        const int local_file_contents_format_id =
            static_cast<int>(GetFileContentsClipboardFormat());
        std::wstring last_local_file_clipboard_signature;
        std::vector<LocalClipboardFileDescriptor> local_file_clipboard_entries;
        std::vector<unsigned char> local_file_clipboard_descriptor_payload;
        std::vector<std::weak_ptr<RemoteFileClipboardBridge>> file_clipboard_brokers;
        auto describe_file_clipboard_progress = [&]() -> std::wstring {
          return channel_name + L" serving on-demand clipboard file transfer";
        };
        auto collect_live_file_clipboard_brokers =
            [&]() -> std::vector<std::shared_ptr<RemoteFileClipboardBridge>> {
          std::vector<std::shared_ptr<RemoteFileClipboardBridge>> live_brokers;
          auto cursor = file_clipboard_brokers.begin();
          while (cursor != file_clipboard_brokers.end()) {
            std::shared_ptr<RemoteFileClipboardBridge> broker = cursor->lock();
            if (broker == nullptr) {
              cursor = file_clipboard_brokers.erase(cursor);
              continue;
            }
            live_brokers.push_back(broker);
            ++cursor;
          }
          return live_brokers;
        };
        auto close_file_clipboard_brokers = [&](const std::wstring& reason) {
          for (const std::shared_ptr<RemoteFileClipboardBridge>& broker :
               collect_live_file_clipboard_brokers()) {
            broker->Close(reason);
          }
        };
        auto install_remote_file_clipboard =
            [&](const std::vector<unsigned char>& descriptor_payload,
                std::wstring* transfer_status) -> bool {
          auto broker = std::make_shared<RemoteFileClipboardBridge>(
              connection,
              &active_clipboard_file_requests,
              &next_clipboard_stream_id,
              channel_name);
          if (!broker->InitializeFromFileGroupDescriptorPayload(
                  descriptor_payload,
                  transfer_status)) {
            if (transfer_status != nullptr && !transfer_status->empty()) {
              *transfer_status =
                  channel_name + L" failed to parse remote clipboard file descriptors: " +
                  *transfer_status;
            }
            return false;
          }
          file_clipboard_brokers.push_back(broker);
          if (window_ != nullptr) {
            auto* request = new (std::nothrow) InstallRemoteFileClipboardRequest();
            if (request == nullptr) {
              if (transfer_status != nullptr) {
                *transfer_status = channel_name + L" failed to allocate clipboard install request";
              }
              return false;
            }
            request->bridge = broker;
            if (!PostMessageW(
                    window_,
                    kAppInstallRemoteFileClipboard,
                    0,
                    reinterpret_cast<LPARAM>(request))) {
              delete request;
              if (transfer_status != nullptr) {
                *transfer_status = channel_name + L" failed to post clipboard install request";
              }
              return false;
            }
          }
          if (transfer_status != nullptr) {
            *transfer_status =
                channel_name + L" received remote file clipboard; file contents will stream on paste";
          }
          return true;
        };
        auto handle_file_contents_response = [&](const CliprdrMessageData& cliprdr) -> bool {
          for (const std::shared_ptr<RemoteFileClipboardBridge>& broker :
               collect_live_file_clipboard_brokers()) {
            if (broker->HandleFileContentsResponse(cliprdr)) {
              return true;
            }
          }
          return false;
        };

        std::atomic<bool> stop_video_thread{false};
        std::atomic<bool> request_immediate_video{true};
        std::atomic<bool> video_thread_failed{false};
        std::atomic<bool> sent_video_frame{false};
        std::atomic<bool> video_force_keyframe{true};
        std::atomic<int> active_display_width{0};
        std::atomic<int> active_display_height{0};
        const std::wstring effective_codec = GetEffectivePreferredCodec(session_config.preferred_codec);
        const bool start_with_vp8 = _wcsicmp(effective_codec.c_str(), L"vp8-software") == 0;
        std::atomic<bool> active_codec_is_vp8{start_with_vp8};
        std::wstring video_thread_error;
        Win32Mutex video_thread_error_mutex;
        Win32Thread video_thread;
        const bool video_thread_started = video_thread.Start([&]() {
          GdiScreenCapturer screen_capturer;
          MinimalH264Encoder h264_encoder;
          MinimalVp8Encoder vp8_encoder;
          DesktopFrameBgra desktop_frame;
          std::vector<unsigned char> nv12_frame;
          std::vector<unsigned char> i420_frame;
          const auto video_epoch = std::chrono::steady_clock::now();
          auto next_video_deadline = video_epoch;
          auto next_cursor_probe = video_epoch;
          HCURSOR last_sent_cursor = nullptr;
          bool sent_cursor_data = false;
          bool use_vp8_runtime = start_with_vp8;

          while (!stop_video_thread.load() && !IsActiveSessionStopRequested()) {
            if (active_clipboard_file_requests.load() > 0) {
              Sleep(20);
              continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_cursor_probe) {
              HCURSOR current_cursor = nullptr;
              if (TryGetVisibleRustDeskCursor(&current_cursor) &&
                  (!sent_cursor_data || current_cursor != last_sent_cursor)) {
                std::vector<unsigned char> cursor_message;
                std::wstring cursor_error;
                if (EncodeRustDeskCursorDataMessage(
                        current_cursor, &cursor_message, &cursor_error)) {
                  if (!connection->SendFrame(cursor_message, nullptr)) {
                    Win32LockGuard error_lock(video_thread_error_mutex);
                    video_thread_error = L"cursor data send failed";
                    video_thread_failed.store(true);
                    return;
                  }
                  last_sent_cursor = current_cursor;
                  sent_cursor_data = true;
                }
              }
              next_cursor_probe = now + std::chrono::milliseconds(50);
            }
            if (!request_immediate_video.load() && now < next_video_deadline) {
              Sleep(2);
              continue;
            }
            request_immediate_video.store(false);

            std::wstring capture_error;
            if (!screen_capturer.Capture(&desktop_frame, &capture_error)) {
              Win32LockGuard error_lock(video_thread_error_mutex);
              video_thread_error = L"screen capture failed: " + capture_error;
              video_thread_failed.store(true);
              return;
            }

            active_display_width.store(desktop_frame.width);
            active_display_height.store(desktop_frame.height);
            if (desktop_frame.width < 1 || desktop_frame.height < 1) {
              Win32LockGuard error_lock(video_thread_error_mutex);
              video_thread_error = L"captured invalid display dimensions";
              video_thread_failed.store(true);
              return;
            }

            const int64_t pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - video_epoch).count();
            std::vector<std::vector<unsigned char>> encoded_frames;
            std::vector<bool> key_flags;
            const bool request_keyframe = video_force_keyframe.exchange(false);

            auto encode_vp8_frame = [&](bool keyframe_requested, std::wstring* error_text) -> bool {
              if (!vp8_encoder.IsInitializedFor(desktop_frame.width, desktop_frame.height)) {
                std::wstring vp8_error;
                if (!vp8_encoder.Initialize(
                        desktop_frame.width,
                        desktop_frame.height,
                        session_config.video_fps,
                        session_config.video_bitrate_kbps,
                        &vp8_error)) {
                  if (error_text != nullptr) {
                    *error_text = L"VP8 encoder init failed: " + vp8_error;
                  }
                  return false;
                }
                video_force_keyframe.store(true);
              }

              std::wstring vp8_error;
              if (!ConvertBgraToI420(desktop_frame, &i420_frame, &vp8_error)) {
                if (error_text != nullptr) {
                  *error_text = L"BGRA to I420 failed: " + vp8_error;
                }
                return false;
              }

              if (!vp8_encoder.EncodeFrame(
                      i420_frame,
                      keyframe_requested,
                      pts_ms,
                      &encoded_frames,
                      &key_flags,
                      &vp8_error)) {
                if (error_text != nullptr) {
                  *error_text = L"VP8 encode failed: " + vp8_error;
                }
                return false;
              }
              return true;
            };

            if (use_vp8_runtime) {
              std::wstring vp8_error;
              if (!encode_vp8_frame(request_keyframe, &vp8_error)) {
                Win32LockGuard error_lock(video_thread_error_mutex);
                video_thread_error = vp8_error;
                video_thread_failed.store(true);
                return;
              }
            } else {
              if (!h264_encoder.IsInitializedFor(desktop_frame.width, desktop_frame.height)) {
                if (!h264_encoder.Initialize(
                        desktop_frame.width,
                        desktop_frame.height,
                        session_config.video_fps,
                        session_config.video_bitrate_kbps,
                        &capture_error)) {
                  const std::wstring h264_error = L"H264 encoder init failed: " + capture_error;
                  use_vp8_runtime = true;
                  active_codec_is_vp8.store(true);
                  video_force_keyframe.store(true);
                  encoded_frames.clear();
                  key_flags.clear();
                  std::wstring vp8_error;
                  if (!encode_vp8_frame(true, &vp8_error)) {
                    Win32LockGuard error_lock(video_thread_error_mutex);
                    video_thread_error = h264_error + L"; VP8 fallback failed: " + vp8_error;
                    video_thread_failed.store(true);
                    return;
                  }
                }
                video_force_keyframe.store(true);
              }

              if (!use_vp8_runtime) {
                ConvertBgraToNv12(desktop_frame, &nv12_frame);
                if (!h264_encoder.EncodeFrame(
                        nv12_frame,
                        request_keyframe,
                        pts_ms,
                        &encoded_frames,
                        &key_flags,
                        &capture_error)) {
                  const std::wstring h264_error = L"H264 encode failed: " + capture_error;
                  use_vp8_runtime = true;
                  active_codec_is_vp8.store(true);
                  video_force_keyframe.store(true);
                  encoded_frames.clear();
                  key_flags.clear();
                  std::wstring vp8_error;
                  if (!encode_vp8_frame(true, &vp8_error)) {
                    Win32LockGuard error_lock(video_thread_error_mutex);
                    video_thread_error = h264_error + L"; VP8 fallback failed: " + vp8_error;
                    video_thread_failed.store(true);
                    return;
                  }
                }
              }
            }

            if (!encoded_frames.empty()) {
              const std::vector<unsigned char> video_message = use_vp8_runtime
                  ? EncodeVp8VideoFrameMessage(encoded_frames, key_flags, pts_ms, 0)
                  : EncodeH264VideoFrameMessage(encoded_frames, key_flags, pts_ms, 0);
              if (!connection->SendFrame(video_message, nullptr)) {
                Win32LockGuard error_lock(video_thread_error_mutex);
                video_thread_error = L"video frame send failed";
                video_thread_failed.store(true);
                return;
              }
              sent_video_frame.store(true);
            }

            next_video_deadline =
                now + std::chrono::milliseconds(
                          1000 / (session_config.video_fps > 0 ? session_config.video_fps : 1));
          }
        });
        if (!video_thread_started) {
          if (session_status != nullptr) {
            *session_status = channel_name + L" failed to start video thread";
          }
          return false;
        }
        auto stop_video_session = [&]() {
          stop_video_thread.store(true);
          request_immediate_video.store(true);
          connection->Abort();
          if (video_thread.Joinable()) {
            video_thread.Join();
          }
        };
        auto try_forward_local_clipboard =
            [&](std::wstring* clipboard_status) -> bool {
          const auto now = std::chrono::steady_clock::now();
          if (now < next_clipboard_poll) {
            return true;
          }
          next_clipboard_poll = now + std::chrono::milliseconds(350);

          const DWORD clipboard_sequence = GetClipboardSequenceNumber();
          if (clipboard_sequence == last_local_clipboard_sequence) {
            return true;
          }
          last_local_clipboard_sequence = clipboard_sequence;

          FormattedTextClipboardContent local_formatted_clipboard;
          std::wstring local_formatted_clipboard_signature;
          if (CaptureFormattedTextClipboardContent(
                  &local_formatted_clipboard,
                  &local_formatted_clipboard_signature,
                  nullptr)) {
            if (local_formatted_clipboard_signature !=
                last_local_formatted_clipboard_signature) {
              if (local_formatted_clipboard_signature ==
                  last_remote_formatted_clipboard_signature) {
                last_local_formatted_clipboard_signature =
                    local_formatted_clipboard_signature;
                last_remote_formatted_clipboard_signature.clear();
              } else {
                std::vector<ClipboardMessageData> outbound_clipboards;
                if (BuildFormattedTextClipboardMessages(
                        local_formatted_clipboard,
                        &outbound_clipboards)) {
                  const bool sending_rich_text =
                      outbound_clipboards.size() > 1U ||
                      (outbound_clipboards.size() == 1U &&
                       outbound_clipboards.front().format !=
                           kClipboardFormatText);
                  const std::vector<unsigned char> clipboard_message =
                      outbound_clipboards.size() == 1U
                          ? EncodeSingleClipboardMessage(
                                outbound_clipboards.front())
                          : EncodeMultiClipboardMessage(outbound_clipboards);
                  std::wstring clipboard_error;
                  if (!connection->SendFrame(
                          clipboard_message,
                          &clipboard_error)) {
                    if (clipboard_status != nullptr) {
                      *clipboard_status =
                          channel_name + L" local clipboard send failed: " +
                          clipboard_error;
                    }
                    return false;
                  }
                  if (clipboard_status != nullptr) {
                    *clipboard_status =
                        sending_rich_text
                            ? channel_name +
                                  L" forwarded local formatted clipboard to controller"
                            : channel_name +
                                  L" forwarded local text clipboard to controller";
                  }
                }
                last_local_formatted_clipboard_signature =
                    local_formatted_clipboard_signature;
              }
            }
            last_local_clipboard_text =
                local_formatted_clipboard.has_text
                    ? local_formatted_clipboard.text
                    : std::wstring();
          } else {
            last_local_formatted_clipboard_signature.clear();
            last_local_clipboard_text.clear();
          }

          if (peer_cliprdr_ready) {
            std::vector<LocalClipboardFileDescriptor> file_entries;
            std::vector<unsigned char> descriptor_payload;
            std::wstring signature;
            if (CaptureLocalClipboardFileDescriptors(
                    &file_entries,
                    &descriptor_payload,
                    &signature,
                    nullptr)) {
              if (signature != last_local_file_clipboard_signature ||
                  descriptor_payload != local_file_clipboard_descriptor_payload) {
                std::vector<CliprdrFormatData> formats;
                CliprdrFormatData file_descriptor_format;
                file_descriptor_format.id = local_file_descriptor_format_id;
                file_descriptor_format.format_name = L"FileGroupDescriptorW";
                formats.push_back(file_descriptor_format);
                CliprdrFormatData file_contents_format;
                file_contents_format.id = local_file_contents_format_id;
                file_contents_format.format_name = L"FileContents";
                formats.push_back(file_contents_format);

                std::wstring clipboard_error;
                if (!connection->SendFrame(
                        EncodeCliprdrFormatListMessage(formats),
                        &clipboard_error)) {
                  if (clipboard_status != nullptr) {
                    *clipboard_status =
                        channel_name + L" local file clipboard send failed: " +
                        clipboard_error;
                  }
                  return false;
                }
                local_file_clipboard_entries = std::move(file_entries);
                local_file_clipboard_descriptor_payload =
                    std::move(descriptor_payload);
                last_local_file_clipboard_signature = std::move(signature);
                if (clipboard_status != nullptr) {
                  *clipboard_status =
                      channel_name + L" forwarded local file clipboard to controller";
                }
              }
            }
          }
          return true;
        };

        bool echoed_test_delay = false;
        while (true) {
          if (IsActiveSessionStopRequested()) {
            const bool manual_close_requested =
                active_session_manual_close_requested_.exchange(false);
            if (manual_close_requested) {
              connection->SendFrame(
                  EncodeCloseReasonMessage(kLoginMsgClosedManuallyByPeer),
                  nullptr);
            }
            close_file_clipboard_brokers(channel_name + L" session stopped");
            stop_video_session();
            if (session_status != nullptr) {
              *session_status = manual_close_requested
                  ? channel_name + L" closed locally"
                  : channel_name + L" stopping active session";
            }
            return true;
          }

          if (video_thread_failed.load()) {
            close_file_clipboard_brokers(channel_name + L" video thread failed");
            stop_video_session();
            if (session_status != nullptr) {
              Win32LockGuard error_lock(video_thread_error_mutex);
              *session_status = channel_name + L" " + video_thread_error;
            }
            return false;
          }

          frame.clear();
          const TcpFramedConnection::ReceiveState post_login_state =
              connection->ReceiveFrame(&frame, kSessionPollMs, session_status);
          if (post_login_state == TcpFramedConnection::ReceiveState::kTimeout) {
            if (!try_forward_local_clipboard(session_status)) {
              close_file_clipboard_brokers(channel_name + L" local clipboard send failed");
              stop_video_session();
              return false;
            }
            if (session_status != nullptr) {
              if (active_clipboard_file_requests.load() > 0) {
                *session_status = describe_file_clipboard_progress();
              } else {
                const std::wstring active_codec_label =
                    active_codec_is_vp8.load() ? L"VP8" : L"H264";
                *session_status =
                    channel_name +
                    (sent_video_frame.load()
                         ? std::wstring(L" session established; streaming ") + active_codec_label +
                               L" desktop frames"
                         : (echoed_test_delay
                                ? std::wstring(L" session kept alive; preparing first ") +
                                      active_codec_label + L" desktop frame"
                                : L" session established; waiting for controller follow-up"));
              }
            }
            continue;
          }
          if (post_login_state != TcpFramedConnection::ReceiveState::kFrame) {
            close_file_clipboard_brokers(
                channel_name + L" receive " + describe_receive_state(post_login_state));
            stop_video_session();
            if (session_status != nullptr) {
              const std::wstring previous = *session_status;
              *session_status = channel_name + L" post-login receive " + describe_receive_state(post_login_state);
              if (!previous.empty()) {
                *session_status += L": ";
                *session_status += previous;
              }
            }
            return true;
          }

          bool found_test_delay = false;
          bool test_delay_from_client = false;
          if (TryParseTestDelayMessage(frame, &found_test_delay, &test_delay_from_client) &&
              found_test_delay) {
            if (test_delay_from_client) {
              if (!connection->SendFrame(frame, session_status)) {
                close_file_clipboard_brokers(channel_name + L" test-delay echo send failed");
                stop_video_session();
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status = channel_name + L" test-delay echo send failed: " + *session_status;
                }
                return false;
              }
              echoed_test_delay = true;
              if (session_status != nullptr) {
                *session_status = channel_name + L" session established; echoed controller TestDelay";
              }
            } else if (session_status != nullptr) {
              echoed_test_delay = true;
              *session_status = channel_name + L" session test-delay acknowledged";
            }
            continue;
          }

          const SessionMessageType session_message = ParseSessionMessage(frame);
          bool handled_follow_up = false;
          if (session_message.has_close_reason) {
            close_file_clipboard_brokers(channel_name + L" controller closed session");
            stop_video_session();
            if (session_status != nullptr) {
              *session_status = channel_name + L" closed by controller";
              if (!session_message.close_reason.empty()) {
                *session_status += L": ";
                *session_status += session_message.close_reason;
              }
            }
            return true;
          }
          if (session_message.wants_refresh_video) {
            video_force_keyframe.store(true);
            request_immediate_video.store(true);
            handled_follow_up = true;
            if (session_status != nullptr) {
              *session_status = channel_name + L" received refresh_video; scheduling keyframe";
            }
          }
          if (session_message.has_mouse) {
            HandleMouseEvent(
                session_message.mouse,
                active_display_width.load() > 0 ? active_display_width.load() : GetDesktopCaptureBounds().width,
                active_display_height.load() > 0 ? active_display_height.load() : GetDesktopCaptureBounds().height);
            request_immediate_video.store(true);
            handled_follow_up = true;
          }
          if (session_message.has_key) {
            HandleKeyEvent(session_message.key);
            request_immediate_video.store(true);
            handled_follow_up = true;
          }
          if (!session_message.clipboards.empty()) {
            handled_follow_up = true;
            FormattedTextClipboardContent remote_formatted_clipboard;
            std::wstring remote_formatted_clipboard_signature;
            std::wstring clipboard_error;
            if (DecodeFormattedTextClipboardContent(
                    session_message.clipboards,
                    &remote_formatted_clipboard,
                    &remote_formatted_clipboard_signature,
                    &clipboard_error)) {
              if (SetClipboardFormattedTextContent(
                      remote_formatted_clipboard,
                      &clipboard_error)) {
                last_remote_clipboard_text =
                    remote_formatted_clipboard.has_text
                        ? remote_formatted_clipboard.text
                        : std::wstring();
                last_local_clipboard_text = last_remote_clipboard_text;
                last_remote_formatted_clipboard_signature =
                    remote_formatted_clipboard_signature;
                last_local_formatted_clipboard_signature =
                    remote_formatted_clipboard_signature;
                if (session_status != nullptr) {
                  *session_status =
                      (remote_formatted_clipboard.has_html ||
                       remote_formatted_clipboard.has_rtf ||
                       remote_formatted_clipboard.has_excel_xml)
                          ? channel_name +
                                L" updated remote formatted clipboard"
                          : channel_name + L" updated remote text clipboard";
                }
              } else if (session_status != nullptr) {
                *session_status =
                    channel_name + L" clipboard text update failed: " +
                    clipboard_error;
              }
            } else if (session_status != nullptr && !clipboard_error.empty()) {
              *session_status =
                  channel_name + L" clipboard text update failed: " +
                  clipboard_error;
            }
          }
          if (session_message.has_cliprdr) {
            handled_follow_up = true;
            switch (session_message.cliprdr.kind) {
              case CliprdrMessageKind::kReady:
                peer_cliprdr_ready = true;
                last_local_clipboard_sequence = 0;
                next_clipboard_poll = std::chrono::steady_clock::time_point();
                if (session_status != nullptr) {
                  *session_status = channel_name + L" cliprdr monitor-ready acknowledged";
                }
                break;
              case CliprdrMessageKind::kFiles:
                if (session_status != nullptr) {
                  *session_status =
                      channel_name + L" received clipboard file audit (" +
                      std::to_wstring(session_message.cliprdr.files.size()) + L" item(s))";
                }
                break;
              case CliprdrMessageKind::kFormatList: {
                int file_descriptor_format_id = 0;
                int file_contents_format_id = 0;
                for (const CliprdrFormatData& format : session_message.cliprdr.formats) {
                  if (_wcsicmp(format.format_name.c_str(), L"FileGroupDescriptorW") == 0) {
                    file_descriptor_format_id = format.id;
                  } else if (_wcsicmp(format.format_name.c_str(), L"FileContents") == 0) {
                    file_contents_format_id = format.id;
                  }
                }
                if (file_descriptor_format_id != 0 &&
                    file_contents_format_id != 0) {
                  std::wstring clipboard_error;
                  if (!connection->SendFrame(
                          EncodeCliprdrFormatDataRequestMessage(
                              file_descriptor_format_id),
                          &clipboard_error)) {
                    if (session_status != nullptr) {
                      *session_status = channel_name + L" cliprdr format-data request failed: " + clipboard_error;
                    }
                  } else if (session_status != nullptr) {
                    *session_status = channel_name + L" requesting remote clipboard file descriptors";
                  }
                } else if (session_status != nullptr) {
                  *session_status = channel_name + L" controller clipboard formats did not include FileGroupDescriptorW/FileContents";
                }
                break;
              }
              case CliprdrMessageKind::kFormatDataRequest:
                if (session_message.cliprdr.requested_format_id ==
                        local_file_descriptor_format_id &&
                    !local_file_clipboard_descriptor_payload.empty()) {
                  std::wstring clipboard_error;
                  if (!connection->SendFrame(
                          EncodeCliprdrFormatDataResponseMessage(
                              kCliprdrResponseOk,
                              local_file_clipboard_descriptor_payload),
                          &clipboard_error)) {
                    if (session_status != nullptr) {
                      *session_status =
                          channel_name +
                          L" local clipboard format-data response failed: " +
                          clipboard_error;
                    }
                    close_file_clipboard_brokers(
                        channel_name +
                        L" local clipboard format-data response send failed");
                    stop_video_session();
                    return false;
                  }
                  if (session_status != nullptr) {
                    *session_status =
                        channel_name +
                        L" sent local clipboard file descriptors to controller";
                  }
                }
                break;
              case CliprdrMessageKind::kFormatDataResponse:
                if (session_message.cliprdr.msg_flags != kCliprdrResponseOk) {
                  if (session_status != nullptr) {
                    *session_status = channel_name + L" controller rejected clipboard format-data request";
                  }
                } else if (!install_remote_file_clipboard(
                               session_message.cliprdr.payload,
                               session_status)) {
                  if (session_status != nullptr && session_status->empty()) {
                    *session_status = channel_name + L" clipboard virtual file install failed";
                  }
                }
                break;
              case CliprdrMessageKind::kFileContentsResponse:
                if (!handle_file_contents_response(session_message.cliprdr) &&
                    session_status != nullptr && session_status->empty()) {
                  *session_status = channel_name + L" clipboard file response could not be applied";
                }
                break;
              case CliprdrMessageKind::kFileContentsRequest:
                if (!local_file_clipboard_entries.empty()) {
                  active_clipboard_file_requests.fetch_add(1);
                  ScopeExit local_file_request_scope([&]() {
                    active_clipboard_file_requests.fetch_sub(1);
                  });
                  std::vector<unsigned char> payload;
                  std::wstring build_error;
                  const bool build_ok = BuildLocalClipboardFileContentsPayload(
                      local_file_clipboard_entries,
                      session_message.cliprdr,
                      &payload,
                      &build_error);
                  std::wstring send_error;
                  if (!connection->SendFrame(
                          EncodeCliprdrFileContentsResponseMessage(
                              build_ok ? kCliprdrResponseOk : 0,
                              session_message.cliprdr.stream_id,
                              payload),
                          &send_error)) {
                    if (session_status != nullptr) {
                      *session_status =
                          channel_name +
                          L" local clipboard file response send failed: " +
                          send_error;
                    }
                    close_file_clipboard_brokers(
                        channel_name +
                        L" local clipboard file response send failed");
                    stop_video_session();
                    return false;
                  }
                  if (session_status != nullptr) {
                    *session_status = build_ok
                        ? (((session_message.cliprdr.dw_flags &
                             kCliprdrFileContentsSizeFlag) != 0)
                               ? channel_name +
                                     L" reported local clipboard file size to controller"
                               : channel_name +
                                     L" streamed local clipboard file block to controller")
                        : channel_name +
                              L" local clipboard file request failed: " +
                              build_error;
                  }
                }
                break;
              case CliprdrMessageKind::kTryEmpty:
                close_file_clipboard_brokers(
                    channel_name + L" remote controller cleared file clipboard");
                file_clipboard_brokers.clear();
                if (session_status != nullptr) {
                  *session_status = channel_name + L" remote controller cleared file clipboard state";
                }
                break;
              default:
                if (session_status != nullptr) {
                  *session_status =
                      channel_name +
                      L" received cliprdr follow-up kind=" +
                      std::to_wstring(static_cast<int>(session_message.cliprdr.kind));
                }
                break;
            }
          }
          if (handled_follow_up) {
            if (session_status != nullptr && session_status->empty() && sent_video_frame.load()) {
              *session_status = channel_name + L" session established; streaming desktop and handling input";
            }
            continue;
          }

          if (session_status != nullptr) {
            *session_status =
                channel_name +
                L" session established; ignoring follow-up fields=" +
                FormatObservedFields(ExtractTopLevelMessageFields(frame));
          }
          continue;
        }
      }
    };

    auto run_auxiliary_secure_session_over_connection =
        [this,
         session_config,
         session_temporary_password_snapshot,
         session_fixed_password_snapshot,
         session_device_uuid_snapshot,
         session_secret_key_bytes,
         generate_numeric_password,
         describe_receive_state,
         &should_auto_accept_secondary_file_transfer,
         &run_file_transfer_session_loop,
         &run_desktop_session_loop](
            TcpFramedConnection* connection,
            const std::wstring& channel_name,
            const std::wstring& display_remote_id_override,
            std::wstring* session_status) -> bool {
      RegisterAuxiliarySessionConnection(connection);
      std::shared_ptr<void> connection_scope(
          connection,
          [this](void* registered_connection) {
            ClearAuxiliarySessionConnection(registered_connection);
          });
      const std::wstring display_remote_id = Trim(display_remote_id_override);
      std::array<unsigned char, crypto_box_PUBLICKEYBYTES> session_curve_public = {};
      std::array<unsigned char, crypto_box_SECRETKEYBYTES> session_curve_secret = {};
      std::vector<unsigned char> signed_id = EncodeSignedIdMessage(
          session_config.host_id,
          session_secret_key_bytes,
          &session_curve_public,
          &session_curve_secret,
          session_status);
      if (signed_id.empty()) {
        return false;
      }
      if (!connection->SendRawFrame(signed_id, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" signed_id send failed: " + *session_status;
        }
        return false;
      }

      std::vector<unsigned char> frame;
      const TcpFramedConnection::ReceiveState handshake_state =
          connection->ReceiveFrame(&frame, kConnectTimeoutMs, session_status);
      if (handshake_state != TcpFramedConnection::ReceiveState::kFrame) {
        if (session_status != nullptr) {
          const std::wstring previous = *session_status;
          *session_status =
              channel_name + L" handshake receive " + describe_receive_state(handshake_state);
          if (!previous.empty()) {
            *session_status += L": ";
            *session_status += previous;
          }
        }
        return false;
      }

      PublicKeyMessageData public_key_message;
      if (!ParsePublicKeyMessage(frame, &public_key_message)) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" handshake parse failed";
        }
        return false;
      }
      if (public_key_message.asymmetric_value.empty()) {
        if (session_status != nullptr) {
          *session_status = channel_name + L" controller requested insecure fallback";
        }
        return false;
      }

      std::array<unsigned char, crypto_secretbox_KEYBYTES> symmetric_key = {};
      if (!DecodeSymmetricKeyFromPublicKey(
              public_key_message.asymmetric_value,
              public_key_message.symmetric_value,
              session_curve_secret,
              &symmetric_key,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" handshake key decode failed: " + *session_status;
        }
        return false;
      }
      connection->SetSymmetricKey(symmetric_key);

      const std::string hash_salt = session_device_uuid_snapshot.empty()
                                        ? WideToUtf8(session_config.host_id)
                                        : WideToUtf8(session_device_uuid_snapshot);
      const std::string hash_challenge =
          WideToUtf8(generate_numeric_password(session_config.temporary_password_length));
      if (!connection->SendFrame(
              EncodeHashMessage(hash_salt, hash_challenge),
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = channel_name + L" hash send failed: " + *session_status;
        }
        return false;
      }

      const std::vector<unsigned char> expected_temporary_password =
          (!session_temporary_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_temporary_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const std::vector<unsigned char> expected_fixed_password =
          (!session_fixed_password_snapshot.empty())
              ? ComputeRustDeskLoginPasswordDigest(
                    session_fixed_password_snapshot,
                    hash_salt,
                    hash_challenge)
              : std::vector<unsigned char>();
      const auto session_start = std::chrono::steady_clock::now();
      bool saw_empty_login = false;
      std::chrono::steady_clock::time_point manual_password_deadline;
      unsigned long incoming_approval_token = 0;
      ScopeExit incoming_approval_scope([this, &incoming_approval_token]() {
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }
      });

      while (true) {
        if (IsAuxiliarySessionStopRequested()) {
          if (session_status != nullptr) {
            *session_status = channel_name + L" auxiliary session stop requested";
          }
          connection->Abort();
          return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          if (now >= manual_password_deadline) {
            if (session_status != nullptr) {
              *session_status = incoming_approval_token != 0
                  ? channel_name + L" auxiliary incoming approval timed out"
                  : channel_name + L" auxiliary manual password entry timed out";
            }
            return true;
          }
        } else {
          const auto elapsed_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start).count();
          if (elapsed_ms >= static_cast<long long>(kLoginWaitTimeoutMs)) {
            if (session_status != nullptr) {
              *session_status = channel_name + L" auxiliary login wait timed out";
            }
            return false;
          }
        }

        unsigned long remaining_ms = 1000;
        if (manual_password_deadline.time_since_epoch().count() != 0) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              manual_password_deadline - now).count();
          remaining_ms = remaining > 1000
              ? 1000
              : (remaining > 0 ? static_cast<unsigned long>(remaining) : 1);
        } else {
          const auto elapsed_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start).count();
          const auto wait_left =
              static_cast<long long>(kLoginWaitTimeoutMs) - elapsed_ms;
          remaining_ms = wait_left > 1000
              ? 1000
              : (wait_left > 0 ? static_cast<unsigned long>(wait_left) : 1);
        }

        bool accepted_secondary_file_transfer = false;
        bool accepted_temporary_password = false;
        bool accepted_fixed_password = false;
        bool accepted_click_approval = false;
        if (incoming_approval_token != 0) {
          const IncomingApprovalDecision approval_decision =
              GetIncomingApprovalDecision(incoming_approval_token);
          if (approval_decision == IncomingApprovalDecision::kRejected) {
            const std::vector<unsigned char> rejected =
                EncodeLoginResponseErrorMessage(kLoginMsgClosedManuallyByPeer);
            connection->SendFrame(rejected, nullptr);
            if (session_status != nullptr) {
              *session_status = channel_name + L" rejected auxiliary incoming request locally";
            }
            return true;
          }
          if (approval_decision == IncomingApprovalDecision::kAccepted) {
            accepted_click_approval = true;
          }
        }

        LoginRequestData login_request;
        if (!accepted_click_approval) {
          frame.clear();
          const TcpFramedConnection::ReceiveState login_state =
              connection->ReceiveFrame(&frame, remaining_ms, session_status);
          if (login_state != TcpFramedConnection::ReceiveState::kFrame) {
            if (session_status != nullptr) {
              if (login_state == TcpFramedConnection::ReceiveState::kTimeout &&
                  manual_password_deadline.time_since_epoch().count() != 0) {
                *session_status = incoming_approval_token != 0
                    ? channel_name + L" waiting for local approval or password entry"
                    : channel_name + L" waiting for manual password entry";
                continue;
              }
              const std::wstring previous = *session_status;
              *session_status =
                  channel_name + L" auxiliary encrypted receive " +
                  describe_receive_state(login_state);
              if (!previous.empty()) {
                *session_status += L": ";
                *session_status += previous;
              }
            }
            return login_state == TcpFramedConnection::ReceiveState::kClosed ||
                   manual_password_deadline.time_since_epoch().count() != 0;
          }

          if (!ParseLoginRequestMessage(frame, &login_request)) {
            const SessionMessageType session_message = ParseSessionMessage(frame);
            if (session_message.has_close_reason) {
              if (session_status != nullptr) {
                *session_status = channel_name + L" controller closed before auxiliary login";
                if (!session_message.close_reason.empty()) {
                  *session_status += L": ";
                  *session_status += session_message.close_reason;
                }
              }
              return true;
            }
            if (session_status != nullptr) {
              *session_status =
                  channel_name + L" received encrypted message, but it was not LoginRequest";
            }
            return false;
          }
          AppendPortableHostLog(
              L"login",
              channel_name + L" parsed auxiliary encrypted LoginRequest: " +
                  DescribeLoginRequestForLog(login_request));

          if (login_request.password.empty()) {
            if (should_auto_accept_secondary_file_transfer(login_request, display_remote_id)) {
              accepted_secondary_file_transfer = true;
              AppendPortableHostLog(
                  L"login",
                  channel_name +
                      L" auto-accepted auxiliary encrypted file-transfer login");
            } else {
              saw_empty_login = true;
              if (incoming_approval_token == 0) {
                incoming_approval_token = BeginIncomingApproval(
                    display_remote_id.empty()
                        ? login_request.my_id
                        : display_remote_id,
                    display_remote_id.empty()
                        ? login_request.my_name
                        : L"");
              }
              manual_password_deadline =
                  std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(kIncomingApprovalTimeoutMs);
              if (session_status != nullptr) {
                *session_status =
                    channel_name +
                    L" received incoming auxiliary remote request; waiting for approval or password entry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name +
                      L" empty-password auxiliary encrypted login is waiting for local approval/password entry");
              continue;
            }
          }

          if (!accepted_secondary_file_transfer) {
            accepted_temporary_password =
                !expected_temporary_password.empty() &&
                login_request.password == expected_temporary_password;
            accepted_fixed_password =
                !expected_fixed_password.empty() &&
                login_request.password == expected_fixed_password;
            if (!accepted_temporary_password && !accepted_fixed_password) {
              const std::vector<unsigned char> wrong_password =
                  EncodeLoginResponseErrorMessage(kLoginMsgWrongPassword);
              if (!connection->SendFrame(wrong_password, session_status)) {
                if (session_status != nullptr && !session_status->empty()) {
                  *session_status =
                      channel_name + L" wrong password response send failed: " + *session_status;
                }
                return false;
              }
              if (session_status != nullptr) {
                *session_status =
                    channel_name +
                    L" rejected auxiliary encrypted LoginRequest with wrong password; waiting for retry";
              }
              AppendPortableHostLog(
                  L"login",
                  channel_name +
                      L" rejected auxiliary encrypted LoginRequest with wrong password");
              continue;
            }
          }
        }

        const std::vector<unsigned char> login_response =
            EncodeLoginResponsePeerInfoMessage(session_config);
        if (!connection->SendFrame(login_response, session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" login response send failed: " + *session_status;
          }
          return false;
        }
        if (session_status != nullptr) {
          if (accepted_secondary_file_transfer) {
            *session_status =
                channel_name +
                L" accepted secondary auxiliary file-transfer request and sent PeerInfo login response";
          } else if (accepted_fixed_password) {
            *session_status =
                channel_name + L" accepted auxiliary fixed password and sent PeerInfo login response";
          } else if (accepted_click_approval) {
            *session_status =
                channel_name +
                L" accepted auxiliary incoming remote request by local confirmation and sent PeerInfo login response";
          } else {
            *session_status = saw_empty_login
                                  ? channel_name + L" accepted auxiliary manual LoginRequest and sent PeerInfo login response"
                                  : channel_name + L" accepted auxiliary LoginRequest and sent PeerInfo login response";
          }
        }
        AppendPortableHostLog(
            L"login",
            channel_name +
                L" sent auxiliary encrypted PeerInfo login response; secondary_file_transfer=" +
                BoolToLogText(accepted_secondary_file_transfer) +
                L", fixed_password=" + BoolToLogText(accepted_fixed_password) +
                L", temporary_password=" + BoolToLogText(accepted_temporary_password) +
                L", click_approval=" + BoolToLogText(accepted_click_approval) +
                L", manual_login=" + BoolToLogText(saw_empty_login));

        std::wstring connected_remote_id;
        std::wstring connected_remote_name;
        if (accepted_click_approval) {
          CaptureIncomingApprovalRemoteIdentity(
              &connected_remote_id,
              &connected_remote_name);
        } else if (!display_remote_id.empty()) {
          connected_remote_id = display_remote_id;
        } else {
          connected_remote_id = login_request.my_id;
          connected_remote_name = login_request.my_name;
        }
        if (incoming_approval_token != 0) {
          CompleteIncomingApproval(incoming_approval_token);
          incoming_approval_token = 0;
        }
        if (login_request.has_file_transfer) {
          return run_file_transfer_session_loop(
              connection,
              channel_name,
              connected_remote_id,
              connected_remote_name,
              login_request.file_transfer_dir,
              login_request.file_transfer_show_hidden,
              session_status);
        }
        if (!connection->SendFrame(EncodeCliprdrMonitorReadyMessage(), session_status)) {
          if (session_status != nullptr && !session_status->empty()) {
            *session_status = channel_name + L" cliprdr monitor-ready send failed: " + *session_status;
          }
          return false;
        }
        return run_desktop_session_loop(
            connection,
            channel_name,
            connected_remote_id,
            connected_remote_name,
            true,
            session_status);
      }
    };

    auto open_relay_session =
        [this,
         session_config,
         run_minimal_secure_session_over_connection,
         run_minimal_plain_session_over_connection](
            const std::wstring& relay_endpoint,
            const std::wstring& relay_uuid,
            bool secure,
            std::wstring* session_status) -> bool {
      if (IsActiveSessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = L"session stop requested before relay connect";
        }
        return true;
      }
      if (relay_uuid.empty()) {
        if (session_status != nullptr) {
          *session_status = L"relay uuid is empty";
        }
        return false;
      }
      const ParsedHostPort relay_server = ParseHostPort(relay_endpoint, kDefaultRelayServerPort);
      if (relay_server.host.empty()) {
        if (session_status != nullptr) {
          *session_status = L"relay_server is empty";
        }
        return false;
      }

      TcpFramedConnection relay_connection;
      if (!relay_connection.Connect(relay_server.host, relay_server.port, kConnectTimeoutMs, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"relay tcp connect failed: " + *session_status;
        }
        return false;
      }
      if (!relay_connection.SendRawFrame(
              EncodeRequestRelayMessage(relay_uuid, session_config.key),
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"relay RequestRelay send failed: " + *session_status;
        }
        return false;
      }
      if (secure) {
        return run_minimal_secure_session_over_connection(
            &relay_connection, L"relay session", L"", session_status);
      }
      return run_minimal_plain_session_over_connection(
          &relay_connection, L"relay session", session_status);
    };

    auto open_auxiliary_relay_session =
        [this,
         session_config,
         run_auxiliary_secure_session_over_connection](
            const std::wstring& relay_endpoint,
            const std::wstring& relay_uuid,
            bool secure,
            std::wstring* session_status) -> bool {
      if (IsAuxiliarySessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = L"auxiliary session stop requested before relay connect";
        }
        return true;
      }
      if (relay_uuid.empty()) {
        if (session_status != nullptr) {
          *session_status = L"auxiliary relay uuid is empty";
        }
        return false;
      }
      if (!secure) {
        if (session_status != nullptr) {
          *session_status = L"plain auxiliary relay session is not supported";
        }
        return false;
      }
      const ParsedHostPort relay_server = ParseHostPort(relay_endpoint, kDefaultRelayServerPort);
      if (relay_server.host.empty()) {
        if (session_status != nullptr) {
          *session_status = L"auxiliary relay_server is empty";
        }
        return false;
      }

      TcpFramedConnection relay_connection;
      if (!relay_connection.Connect(
              relay_server.host,
              relay_server.port,
              kConnectTimeoutMs,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"auxiliary relay tcp connect failed: " + *session_status;
        }
        return false;
      }
      if (!relay_connection.SendRawFrame(
              EncodeRequestRelayMessage(relay_uuid, session_config.key),
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"auxiliary relay RequestRelay send failed: " + *session_status;
        }
        return false;
      }
      return run_auxiliary_secure_session_over_connection(
          &relay_connection,
          L"relay auxiliary session",
          L"",
          session_status);
    };

    auto open_relay_session_with_sideband =
        [this, session_id_server, session_config, open_relay_session](
            const std::vector<unsigned char>& socket_addr,
            const std::wstring& relay_endpoint,
            const std::wstring& relay_uuid,
            bool initiate,
            bool secure,
            std::wstring* session_status) -> bool {
      if (IsActiveSessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = L"session stop requested before sideband connect";
        }
        return true;
      }
      TcpFramedConnection hbbs_sideband;
      if (!hbbs_sideband.Connect(
              session_id_server.host,
              session_id_server.port,
              kConnectTimeoutMs,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"hbbs tcp sideband connect failed: " + *session_status;
        }
        return false;
      }
      const std::vector<unsigned char> relay_response = EncodeRelayResponseMessage(
          socket_addr,
          relay_endpoint,
          relay_uuid,
          session_config.host_id,
          initiate);
      if (!hbbs_sideband.SendRawFrame(relay_response, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"RelayResponse send failed: " + *session_status;
        }
        return false;
      }
      return open_relay_session(relay_endpoint, relay_uuid, secure, session_status);
    };

    auto open_auxiliary_relay_session_with_sideband =
        [this, session_id_server, session_config, open_auxiliary_relay_session](
            const std::vector<unsigned char>& socket_addr,
            const std::wstring& relay_endpoint,
            const std::wstring& relay_uuid,
            bool initiate,
            bool secure,
            std::wstring* session_status) -> bool {
      if (IsAuxiliarySessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = L"auxiliary session stop requested before sideband connect";
        }
        return true;
      }
      TcpFramedConnection hbbs_sideband;
      if (!hbbs_sideband.Connect(
              session_id_server.host,
              session_id_server.port,
              kConnectTimeoutMs,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"auxiliary hbbs tcp sideband connect failed: " + *session_status;
        }
        return false;
      }
      const std::vector<unsigned char> relay_response = EncodeRelayResponseMessage(
          socket_addr,
          relay_endpoint,
          relay_uuid,
          session_config.host_id,
          initiate);
      if (!hbbs_sideband.SendRawFrame(relay_response, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"auxiliary RelayResponse send failed: " + *session_status;
        }
        return false;
      }
      return open_auxiliary_relay_session(relay_endpoint, relay_uuid, secure, session_status);
    };

    auto accept_direct_intranet_session =
        [this, session_id_server, session_config, run_minimal_secure_session_over_connection](
            const FetchLocalAddrData& fetch_local_addr,
            const std::wstring& requested_relay_server,
            std::wstring* session_status) -> bool {
      if (IsActiveSessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = L"session stop requested before direct intranet setup";
        }
        return true;
      }
      if (fetch_local_addr.socket_addr.empty()) {
        if (session_status != nullptr) {
          *session_status = L"FetchLocalAddr socket_addr is empty";
        }
        return false;
      }

      std::wstring relay_endpoint = requested_relay_server;
      if (relay_endpoint.empty()) {
        relay_endpoint = session_config.relay_server;
      }

      TcpFramedConnection hbbs_sideband;
      if (!hbbs_sideband.Connect(
              session_id_server.host,
              session_id_server.port,
              kConnectTimeoutMs,
              session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"LocalAddr sideband connect failed: " + *session_status;
        }
        return false;
      }

      sockaddr_storage local_storage = {};
      int local_storage_length = 0;
      if (!hbbs_sideband.GetLocalSocketAddress(&local_storage, &local_storage_length) ||
          local_storage.ss_family != AF_INET) {
        if (session_status != nullptr) {
          *session_status = L"unable to obtain IPv4 local rendezvous socket";
        }
        return false;
      }

      const sockaddr_in local_address =
          *reinterpret_cast<const sockaddr_in*>(&local_storage);
      const std::vector<unsigned char> local_socket_addr =
          EncodeMangledIpv4SocketAddress(local_address);
      const std::vector<unsigned char> local_addr_message = EncodeLocalAddrMessage(
          fetch_local_addr.socket_addr,
          local_socket_addr,
          relay_endpoint,
          session_config.host_id);
      if (!hbbs_sideband.SendRawFrame(local_addr_message, session_status)) {
        if (session_status != nullptr && !session_status->empty()) {
          *session_status = L"LocalAddr send failed: " + *session_status;
        }
        return false;
      }

      const std::wstring local_endpoint = hbbs_sideband.LocalEndpointText();
      hbbs_sideband.Close();

      SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listener == INVALID_SOCKET) {
        if (session_status != nullptr) {
          *session_status = L"listen socket create failed, WSA=" + std::to_wstring(WSAGetLastError());
        }
        return false;
      }

      BOOL reuse_addr = TRUE;
      setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_addr), sizeof(reuse_addr));

      if (bind(listener, reinterpret_cast<const sockaddr*>(&local_address), sizeof(local_address)) != 0) {
        if (session_status != nullptr) {
          *session_status = L"listen bind failed on " + local_endpoint + L", WSA=" + std::to_wstring(WSAGetLastError());
        }
        closesocket(listener);
        return false;
      }

      if (listen(listener, 1) != 0) {
        if (session_status != nullptr) {
          *session_status = L"listen failed on " + local_endpoint + L", WSA=" + std::to_wstring(WSAGetLastError());
        }
        closesocket(listener);
        return false;
      }

      int select_result = 0;
      unsigned long waited_ms = 0;
      while (!IsActiveSessionStopRequested() && waited_ms < kConnectTimeoutMs) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener, &read_set);
        TIMEVAL timeout = {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;
        select_result = select(0, &read_set, nullptr, nullptr, &timeout);
        if (select_result > 0) {
          break;
        }
        if (select_result < 0) {
          break;
        }
        waited_ms += 200;
      }
      if (select_result <= 0) {
        if (session_status != nullptr) {
          if (IsActiveSessionStopRequested()) {
            *session_status = L"direct intranet accept cancelled";
          } else if (select_result == 0) {
            *session_status = L"direct intranet accept timeout on " + local_endpoint;
          } else {
            *session_status = L"direct intranet select failed, WSA=" + std::to_wstring(WSAGetLastError());
          }
        }
        closesocket(listener);
        return IsActiveSessionStopRequested();
      }

      sockaddr_storage peer_storage = {};
      int peer_storage_length = sizeof(peer_storage);
      SOCKET accepted = accept(listener, reinterpret_cast<sockaddr*>(&peer_storage), &peer_storage_length);
      closesocket(listener);
      if (accepted == INVALID_SOCKET) {
        if (session_status != nullptr) {
          *session_status = L"direct intranet accept failed, WSA=" + std::to_wstring(WSAGetLastError());
        }
        return false;
      }

      BOOL nodelay = TRUE;
      setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

      TcpFramedConnection direct_connection;
      direct_connection.AttachSocket(accepted);
      if (!run_minimal_secure_session_over_connection(
              &direct_connection,
              L"direct intranet session",
              L"",
              session_status)) {
        return false;
      }
      return true;
    };

    auto start_initiated_relay =
        [this, session_config, open_relay_session_with_sideband](
            const std::vector<unsigned char>& socket_addr,
            const std::wstring& requested_relay_server,
            const std::wstring& trigger_label,
            std::wstring* session_status) -> bool {
      if (IsActiveSessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": session stop requested";
        }
        return true;
      }
      if (socket_addr.empty()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": socket_addr is empty";
        }
        return false;
      }
      std::wstring relay_endpoint = requested_relay_server;
      if (relay_endpoint.empty()) {
        relay_endpoint = session_config.relay_server;
      }
      if (relay_endpoint.empty()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": relay server is empty";
        }
        return false;
      }
      const std::wstring relay_uuid = GenerateSessionUuidText();
      return open_relay_session_with_sideband(
          socket_addr,
          relay_endpoint,
          relay_uuid,
          true,
          true,
          session_status);
    };

    auto start_initiated_auxiliary_relay =
        [this, session_config, open_auxiliary_relay_session_with_sideband](
            const std::vector<unsigned char>& socket_addr,
            const std::wstring& requested_relay_server,
            const std::wstring& trigger_label,
            std::wstring* session_status) -> bool {
      if (IsAuxiliarySessionStopRequested()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": auxiliary session stop requested";
        }
        return true;
      }
      if (socket_addr.empty()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": socket_addr is empty";
        }
        return false;
      }
      std::wstring relay_endpoint = requested_relay_server;
      if (relay_endpoint.empty()) {
        relay_endpoint = session_config.relay_server;
      }
      if (relay_endpoint.empty()) {
        if (session_status != nullptr) {
          *session_status = trigger_label + L": relay server is empty";
        }
        return false;
      }
      const std::wstring relay_uuid = GenerateSessionUuidText();
      return open_auxiliary_relay_session_with_sideband(
          socket_addr,
          relay_endpoint,
          relay_uuid,
          true,
          true,
          session_status);
    };

    auto has_primary_session_or_pending = [this]() -> bool {
      if (HasActiveSession()) {
        return true;
      }
      Win32LockGuard guard(active_session_mutex_);
      return pending_session_requested_;
    };

    auto start_session_or_auxiliary =
        [this, &has_primary_session_or_pending](
            const std::wstring& starting_status,
            bool registered,
            const std::wstring& failure_prefix,
            std::function<bool(std::wstring*)> primary_runner,
            std::function<bool(std::wstring*)> auxiliary_runner) -> bool {
      if (has_primary_session_or_pending()) {
        if (HasAuxiliarySession()) {
          AppendPortableHostLog(
              L"session",
              L"rejecting incoming session request because auxiliary path is already occupied: " +
                  starting_status);
          return false;
        }
        AppendPortableHostLog(
            L"session",
            L"routing incoming session request to auxiliary path: " + starting_status);
        return StartAuxiliarySessionThread(
            starting_status,
            registered,
            failure_prefix,
            std::move(auxiliary_runner));
      }
      return StartActiveSessionThread(
          starting_status,
          registered,
          failure_prefix,
          std::move(primary_runner));
    };

    auto ensure_direct_access_listener = [&](bool registered) -> bool {
      if (!session_config.direct_access_enabled) {
        close_direct_access_listener();
        direct_access_listener_error.clear();
        return true;
      }

      const unsigned short desired_port =
          (session_config.direct_access_port >= 1 &&
           session_config.direct_access_port <= 65535)
              ? static_cast<unsigned short>(session_config.direct_access_port)
              : static_cast<unsigned short>(kDefaultDirectAccessPort);

      if (direct_access_listener != INVALID_SOCKET &&
          direct_access_listener_port == desired_port) {
        return true;
      }

      close_direct_access_listener();

      SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listener == INVALID_SOCKET) {
        const std::wstring status_text =
            L"direct IP listen socket create failed, WSA=" +
            std::to_wstring(WSAGetLastError());
        if (status_text != direct_access_listener_error) {
          SetRendezvousStatus(status_text, registered);
          direct_access_listener_error = status_text;
        }
        return false;
      }

      BOOL reuse_addr = TRUE;
      setsockopt(
          listener,
          SOL_SOCKET,
          SO_REUSEADDR,
          reinterpret_cast<const char*>(&reuse_addr),
          sizeof(reuse_addr));

      sockaddr_in bind_address = {};
      bind_address.sin_family = AF_INET;
      bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
      bind_address.sin_port = htons(desired_port);
      if (bind(
              listener,
              reinterpret_cast<const sockaddr*>(&bind_address),
              sizeof(bind_address)) != 0) {
        const std::wstring status_text =
            L"direct IP listen bind failed on port " +
            std::to_wstring(desired_port) + L", WSA=" +
            std::to_wstring(WSAGetLastError());
        closesocket(listener);
        if (status_text != direct_access_listener_error) {
          SetRendezvousStatus(status_text, registered);
          direct_access_listener_error = status_text;
        }
        return false;
      }

      if (listen(listener, SOMAXCONN) != 0) {
        const std::wstring status_text =
            L"direct IP listen failed on port " +
            std::to_wstring(desired_port) + L", WSA=" +
            std::to_wstring(WSAGetLastError());
        closesocket(listener);
        if (status_text != direct_access_listener_error) {
          SetRendezvousStatus(status_text, registered);
          direct_access_listener_error = status_text;
        }
        return false;
      }

      direct_access_listener = listener;
      direct_access_listener_port = desired_port;
      direct_access_listener_error.clear();
      return true;
    };

    auto try_accept_direct_access_session = [&](bool registered) {
      if (!ensure_direct_access_listener(registered) ||
          direct_access_listener == INVALID_SOCKET) {
        return;
      }

      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(direct_access_listener, &read_set);
      TIMEVAL timeout = {};
      const int select_result =
          select(0, &read_set, nullptr, nullptr, &timeout);
      if (select_result <= 0) {
        return;
      }

      sockaddr_storage peer_storage = {};
      int peer_storage_length = sizeof(peer_storage);
      SOCKET accepted = accept(
          direct_access_listener,
          reinterpret_cast<sockaddr*>(&peer_storage),
          &peer_storage_length);
      if (accepted == INVALID_SOCKET) {
        const int accept_error = WSAGetLastError();
        if (accept_error != WSAEWOULDBLOCK) {
          const std::wstring status_text =
              L"direct IP accept failed, WSA=" +
              std::to_wstring(accept_error);
          if (status_text != direct_access_listener_error) {
            SetRendezvousStatus(status_text, registered);
            direct_access_listener_error = status_text;
          }
        }
        return;
      }

      BOOL nodelay = TRUE;
      setsockopt(
          accepted,
          IPPROTO_TCP,
          TCP_NODELAY,
          reinterpret_cast<const char*>(&nodelay),
          sizeof(nodelay));

      std::wstring peer_host;
      std::array<wchar_t, NI_MAXHOST> host = {};
      if (GetNameInfoW(
              reinterpret_cast<sockaddr*>(&peer_storage),
              peer_storage_length,
              host.data(),
              static_cast<DWORD>(host.size()),
              nullptr,
              0,
              NI_NUMERICHOST) == 0) {
        peer_host = host.data();
      }
      AppendPortableHostLog(
          L"direct-ip",
          L"accepted socket from " +
              (peer_host.empty() ? std::wstring(L"<unknown>") : peer_host) +
              L" on port " + std::to_wstring(direct_access_listener_port));
      std::wstring starting_status =
          L"online, direct IP access request received";
      if (!peer_host.empty()) {
        starting_status += L" from ";
        starting_status += peer_host;
      }
      starting_status += L" (";
      starting_status += std::to_wstring(direct_access_listener_port);
      starting_status += L")";

      if (!start_session_or_auxiliary(
              starting_status,
              registered,
              L"direct IP session failed: ",
              [run_minimal_direct_ip_session_over_connection,
               accepted,
               peer_host](std::wstring* session_status) -> bool {
                 TcpFramedConnection direct_connection;
                 direct_connection.AttachSocket(accepted);
                 return run_minimal_direct_ip_session_over_connection(
                     &direct_connection,
                     L"direct IP session",
                     peer_host,
                     false,
                     session_status);
               },
               [run_minimal_direct_ip_session_over_connection,
                accepted,
                peer_host](std::wstring* session_status) -> bool {
                 TcpFramedConnection direct_connection;
                 direct_connection.AttachSocket(accepted);
                 return run_minimal_direct_ip_session_over_connection(
                     &direct_connection,
                     L"direct IP auxiliary session",
                     peer_host,
                     true,
                     session_status);
               })) {
        AppendPortableHostLog(
            L"direct-ip",
            L"failed to dispatch accepted socket into direct session thread; peer=" +
                (peer_host.empty() ? std::wstring(L"<unknown>") : peer_host));
        closesocket(accepted);
      } else {
        AppendPortableHostLog(
            L"direct-ip",
            L"accepted socket dispatched into primary/auxiliary direct session thread; peer=" +
                (peer_host.empty() ? std::wstring(L"<unknown>") : peer_host));
      }
    };

    auto wait_with_direct_access =
        [&](unsigned long total_ms, bool registered) {
          unsigned long waited_ms = 0;
          while (!stop_rendezvous_.load() && waited_ms < total_ms) {
            try_accept_direct_access_session(registered);
            const unsigned long remaining_ms = total_ms - waited_ms;
            const unsigned long sleep_ms =
                remaining_ms > kReceivePollMs ? kReceivePollMs : remaining_ms;
            if (sleep_ms == 0) {
              break;
            }
            Sleep(sleep_ms);
            waited_ms += sleep_ms;
          }
        };

    auto run_tcp_rendezvous = [&]() -> bool {
      std::wstring tcp_error;
      TcpFramedConnection connection;
      SetRendezvousStatus(std::wstring(L"connecting tcp ") + endpoint, false);
      if (!connection.Connect(
              id_server.host,
              id_server.port,
              kConnectTimeoutMs,
              &tcp_error)) {
        SetRendezvousStatus(L"tcp connect failed: " + tcp_error, false);
        return false;
      }
      if (!SecureTcpRendezvousConnection(&connection, config_.key, &tcp_error)) {
        SetRendezvousStatus(L"tcp secure handshake failed: " + tcp_error, false);
        return false;
      }

      bool tcp_ready = true;
      bool key_confirmed = false;
      bool registered = false;
      int keep_alive_ms = kDefaultKeepAliveMs;

      auto send_register_pk_tcp = [&]() -> bool {
        const std::vector<unsigned char> register_pk =
            EncodeRegisterPkMessage(config_.host_id, uuid_bytes, public_key_bytes);
        if (!connection.SendFrame(register_pk, &tcp_error)) {
          SetRendezvousStatus(L"RegisterPk tcp send failed: " + tcp_error, false);
          return false;
        }
        return true;
      };

      if (!send_register_pk_tcp()) {
        return false;
      }

      auto last_register = std::chrono::steady_clock::now();
      auto last_recv = std::chrono::steady_clock::now();
      std::vector<unsigned char> tcp_frame;

      while (!stop_rendezvous_.load()) {
        try_accept_direct_access_session(registered);
        tcp_frame.clear();
        const TcpFramedConnection::ReceiveState state =
            connection.ReceiveFrame(&tcp_frame, kReceivePollMs, &tcp_error);

        if (state == TcpFramedConnection::ReceiveState::kTimeout) {
          const auto now = std::chrono::steady_clock::now();
          if (!key_confirmed &&
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_register).count() >=
                  kRegisterIntervalMs) {
            if (!send_register_pk_tcp()) {
              break;
            }
            last_register = now;
          }
          if (registered &&
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_recv).count() >
                  (keep_alive_ms * 3 / 2)) {
            SetRendezvousStatus(L"hbbs tcp timeout", registered);
            break;
          }
          continue;
        }

        if (state == TcpFramedConnection::ReceiveState::kClosed) {
          SetRendezvousStatus(L"hbbs tcp closed", registered);
          break;
        }

        if (state == TcpFramedConnection::ReceiveState::kError) {
          SetRendezvousStatus(L"tcp receive failed: " + tcp_error, registered);
          break;
        }

        last_recv = std::chrono::steady_clock::now();
        if (tcp_frame.empty()) {
          continue;
        }

        ParsedServerFrame parsed = ParseServerFrame(tcp_frame);
        bool handled_frame = false;
        bool restart_registration_after_config_change = false;
        if (parsed.has_register_peer_response) {
          handled_frame = true;
          if (parsed.register_peer_response.request_pk) {
            SetRendezvousStatus(L"hbbs requested RegisterPk over tcp", registered);
            key_confirmed = false;
            if (!send_register_pk_tcp()) {
              break;
            }
            last_register = std::chrono::steady_clock::now();
          } else if (registered) {
            SetRendezvousStatus(L"registered to hbbs via tcp", true);
          } else {
            SetRendezvousStatus(L"RegisterPeerResponse received over tcp", false);
          }
        }

        if (parsed.has_register_pk_response) {
          handled_frame = true;
          if (parsed.register_pk_response.keep_alive_ms > 0) {
            keep_alive_ms = parsed.register_pk_response.keep_alive_ms;
          }
          switch (parsed.register_pk_response.result) {
            case 0:
              uuid_mismatch_auto_id_retry_count = 0;
              key_confirmed = true;
              registered = true;
              SetRendezvousStatus(L"registered to hbbs via tcp", true);
              break;
            case 2:
              key_confirmed = false;
              registered = false;
              if (retry_with_new_host_id(L"UUID_MISMATCH from hbbs over tcp")) {
                restart_registration_after_config_change = true;
                break;
              }
              SetRendezvousStatus(L"UUID_MISMATCH from hbbs over tcp", false);
              break;
            case 3:
              key_confirmed = false;
              registered = false;
              SetRendezvousStatus(L"ID_EXISTS from hbbs over tcp", false);
              break;
            case 8:
              key_confirmed = false;
              registered = false;
              SetRendezvousStatus(L"NOT_DEPLOYED from hbbs over tcp", false);
              break;
            default:
              key_confirmed = false;
              registered = false;
              SetRendezvousStatus(
                  std::wstring(L"RegisterPkResponse over tcp ") +
                      RegisterPkResultText(parsed.register_pk_response.result) +
                      L" (" + std::to_wstring(parsed.register_pk_response.result) + L")",
                  false);
              break;
          }
        }

        if (restart_registration_after_config_change) {
          break;
        }

        if (parsed.has_request_relay) {
          handled_frame = true;
          const RequestRelayData request_relay = parsed.request_relay;
          std::wstring relay = request_relay.relay_server;
          if (relay.empty()) {
            relay = session_config.relay_server;
          }
          const std::wstring starting_status =
              std::wstring(L"online, relay request received from hbbs over tcp; opening ") +
              (request_relay.secure ? L"secure" : L"plain") +
              L" relay session (" + relay + L")";
          if (!start_session_or_auxiliary(
                  starting_status,
                  registered,
                  L"RequestRelay tcp session failed: ",
                  [open_relay_session_with_sideband, request_relay, relay](
                      std::wstring* session_status) -> bool {
                    const std::wstring relay_uuid =
                        request_relay.uuid.empty() ? GenerateSessionUuidText() : request_relay.uuid;
                    return open_relay_session_with_sideband(
                        request_relay.socket_addr,
                        relay,
                        relay_uuid,
                        false,
                        request_relay.secure,
                        session_status);
                  },
                  [open_auxiliary_relay_session_with_sideband, request_relay, relay](
                      std::wstring* session_status) -> bool {
                    const std::wstring relay_uuid =
                        request_relay.uuid.empty() ? GenerateSessionUuidText() : request_relay.uuid;
                    return open_auxiliary_relay_session_with_sideband(
                        request_relay.socket_addr,
                        relay,
                        relay_uuid,
                        false,
                        request_relay.secure,
                        session_status);
                  })) {
            SetRendezvousStatus(L"failed to queue RequestRelay tcp session", registered);
          }
        }

        if (parsed.has_punch_hole) {
          handled_frame = true;
          const PunchHoleData punch_hole = parsed.punch_hole;
          std::wstring relay = punch_hole.relay_server;
          if (relay.empty()) {
            relay = session_config.relay_server;
          }
          const std::wstring starting_status = punch_hole.force_relay
              ? std::wstring(L"online, PunchHole requested relay over tcp (") + relay + L")"
              : std::wstring(L"online, PunchHole received from hbbs over tcp; opening relay path (") +
                    relay + L")";
          if (!start_session_or_auxiliary(
                  starting_status,
                  registered,
                  L"PunchHole tcp relay failed: ",
                  [start_initiated_relay, punch_hole, relay](
                      std::wstring* session_status) -> bool {
                    return start_initiated_relay(
                        punch_hole.socket_addr,
                        relay,
                        punch_hole.force_relay
                            ? L"PunchHole tcp forced relay"
                            : L"PunchHole tcp relay",
                        session_status);
                  },
                  [start_initiated_auxiliary_relay, punch_hole, relay](
                      std::wstring* session_status) -> bool {
                    return start_initiated_auxiliary_relay(
                        punch_hole.socket_addr,
                        relay,
                        punch_hole.force_relay
                            ? L"PunchHole tcp forced relay"
                            : L"PunchHole tcp relay",
                        session_status);
                  })) {
            SetRendezvousStatus(L"failed to queue PunchHole tcp relay session", registered);
          }
        }

        if (parsed.has_fetch_local_addr) {
          handled_frame = true;
          const FetchLocalAddrData fetch_local_addr = parsed.fetch_local_addr;
          std::wstring relay = fetch_local_addr.relay_server;
          if (relay.empty()) {
            relay = session_config.relay_server;
          }
          if (relay.empty()) {
            if (!start_session_or_auxiliary(
                    L"online, FetchLocalAddr over tcp requested relay fallback",
                    registered,
                    L"FetchLocalAddr tcp relay failed: ",
                    [start_initiated_relay, fetch_local_addr, relay](
                        std::wstring* session_status) -> bool {
                      return start_initiated_relay(
                          fetch_local_addr.socket_addr,
                          relay,
                          L"FetchLocalAddr tcp relay",
                          session_status);
                    },
                    [start_initiated_auxiliary_relay, fetch_local_addr, relay](
                        std::wstring* session_status) -> bool {
                      return start_initiated_auxiliary_relay(
                          fetch_local_addr.socket_addr,
                          relay,
                          L"FetchLocalAddr tcp relay",
                          session_status);
                    })) {
              SetRendezvousStatus(L"failed to queue FetchLocalAddr tcp relay fallback", registered);
            }
          } else {
            const std::wstring starting_status =
                std::wstring(L"online, FetchLocalAddr received from hbbs over tcp; trying direct intranet path (") +
                relay + L")";
            if (!start_session_or_auxiliary(
                    starting_status,
                    registered,
                    L"FetchLocalAddr tcp direct+relay failed: ",
                    [accept_direct_intranet_session,
                     start_initiated_relay,
                     fetch_local_addr,
                     relay](std::wstring* session_status) -> bool {
                      if (accept_direct_intranet_session(fetch_local_addr, relay, session_status)) {
                        return true;
                      }
                      return start_initiated_relay(
                          fetch_local_addr.socket_addr,
                          relay,
                          L"FetchLocalAddr tcp",
                          session_status);
                    },
                    [start_initiated_auxiliary_relay, fetch_local_addr, relay](
                        std::wstring* session_status) -> bool {
                      return start_initiated_auxiliary_relay(
                          fetch_local_addr.socket_addr,
                          relay,
                          L"FetchLocalAddr tcp",
                          session_status);
                    })) {
              SetRendezvousStatus(
                  L"failed to queue FetchLocalAddr tcp direct/relay session",
                  registered);
            }
          }
        }

        if (!handled_frame) {
          SetRendezvousStatus(
              std::wstring(L"received unhandled hbbs tcp message fields=") +
                  FormatObservedFields(parsed.observed_fields) +
                  L" (" + std::to_wstring(tcp_frame.size()) + L" bytes)",
              registered);
        }
      }

      return tcp_ready;
    };

    bool should_try_tcp_fallback = !udp_connected && allow_tcp_fallback;
    try_accept_direct_access_session(false);
    if (udp_connected) {
      bool key_confirmed = false;
      bool registered = false;
      SetRendezvousStatus(
          std::wstring(L"udp connected to ") + socket.PeerEndpointText() + L", sending RegisterPk",
          false);
      if (!send_register_pk()) {
        should_try_tcp_fallback = allow_tcp_fallback;
      } else {
        auto last_register = std::chrono::steady_clock::now();
        auto last_recv = std::chrono::steady_clock::now();
        const bool restart_unregistered_udp = IsWindowsPreinstallationEnvironment();
        int keep_alive_ms = kDefaultKeepAliveMs;
        std::vector<unsigned char> frame;

        while (!stop_rendezvous_.load()) {
          try_accept_direct_access_session(registered);
          frame.clear();
          const UdpMessageSocket::ReceiveState state =
              socket.ReceiveMessage(&frame, kReceivePollMs, &error_text);

          if (state == UdpMessageSocket::ReceiveState::kTimeout) {
            const auto now = std::chrono::steady_clock::now();
            const int register_retry_interval_ms =
                registered ? kRegisterIntervalMs : kRegisterRetryBeforeReadyMs;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_register).count() >=
                register_retry_interval_ms) {
              const bool sent = key_confirmed ? send_register_peer() : send_register_pk();
              if (!sent) {
                should_try_tcp_fallback = allow_tcp_fallback && !registered;
                break;
              }
              last_register = now;
            }
            if (!registered &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_recv).count() >=
                    kRegisterTcpFallbackBeforeReadyMs) {
              should_try_tcp_fallback = allow_tcp_fallback;
              if (!should_try_tcp_fallback && restart_unregistered_udp) {
                SetRendezvousStatus(L"hbbs udp no response before ready; reconnecting", false);
              }
              if (should_try_tcp_fallback || restart_unregistered_udp) {
                break;
              }
            }
            if (registered &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_recv).count() >
                    (keep_alive_ms * 3 / 2)) {
              SetRendezvousStatus(L"hbbs udp timeout", registered);
              break;
            }
            continue;
          }

          if (state == UdpMessageSocket::ReceiveState::kError) {
            SetRendezvousStatus(L"udp receive failed: " + error_text, registered);
            should_try_tcp_fallback = allow_tcp_fallback && !registered;
            break;
          }

          last_recv = std::chrono::steady_clock::now();
          if (frame.empty()) {
            continue;
          }

          ParsedServerFrame parsed = ParseServerFrame(frame);
          bool handled_frame = false;
          bool restart_registration_after_config_change = false;
          if (parsed.has_register_peer_response) {
            handled_frame = true;
            if (parsed.register_peer_response.request_pk) {
              SetRendezvousStatus(L"hbbs requested RegisterPk over udp", registered);
              key_confirmed = false;
              if (!send_register_pk()) {
                should_try_tcp_fallback = allow_tcp_fallback && !registered;
                break;
              }
              last_register = std::chrono::steady_clock::now();
            } else if (registered) {
              SetRendezvousStatus(L"registered to hbbs via udp", true);
            } else {
              SetRendezvousStatus(L"RegisterPeerResponse received over udp", false);
            }
          }

          if (parsed.has_register_pk_response) {
            handled_frame = true;
            if (parsed.register_pk_response.keep_alive_ms > 0) {
              keep_alive_ms = parsed.register_pk_response.keep_alive_ms;
            }
            switch (parsed.register_pk_response.result) {
              case 0:
                uuid_mismatch_auto_id_retry_count = 0;
                key_confirmed = true;
                registered = true;
                SetRendezvousStatus(L"registered to hbbs via udp", true);
                if (!send_register_peer()) {
                  should_try_tcp_fallback = allow_tcp_fallback && !registered;
                  break;
                }
                last_register = std::chrono::steady_clock::now();
                break;
              case 2:
                key_confirmed = false;
                registered = false;
                if (retry_with_new_host_id(L"UUID_MISMATCH from hbbs")) {
                  restart_registration_after_config_change = true;
                  break;
                }
                SetRendezvousStatus(L"UUID_MISMATCH from hbbs", false);
                break;
              case 3:
                key_confirmed = false;
                registered = false;
                SetRendezvousStatus(L"ID_EXISTS from hbbs", false);
                break;
              case 8:
                key_confirmed = false;
                registered = false;
                SetRendezvousStatus(L"NOT_DEPLOYED from hbbs", false);
                break;
              default:
                key_confirmed = false;
                registered = false;
                SetRendezvousStatus(
                    std::wstring(L"RegisterPkResponse ") +
                        RegisterPkResultText(parsed.register_pk_response.result) +
                        L" (" + std::to_wstring(parsed.register_pk_response.result) + L")",
                    false);
                break;
            }
          }

          if (restart_registration_after_config_change) {
            break;
          }

          if (parsed.has_request_relay) {
            handled_frame = true;
            const RequestRelayData request_relay = parsed.request_relay;
            std::wstring relay = request_relay.relay_server;
            if (relay.empty()) {
              relay = session_config.relay_server;
            }
            const std::wstring starting_status =
                std::wstring(L"online, relay request received from hbbs; opening ") +
                (request_relay.secure ? L"secure" : L"plain") +
                L" relay session (" + relay + L")";
            if (!start_session_or_auxiliary(
                    starting_status,
                    registered,
                    L"relay session setup failed: ",
                    [open_relay_session_with_sideband, request_relay, relay](
                        std::wstring* session_status) -> bool {
                      return open_relay_session_with_sideband(
                          request_relay.socket_addr,
                          relay,
                          request_relay.uuid,
                          false,
                          request_relay.secure,
                          session_status);
                    },
                    [open_auxiliary_relay_session_with_sideband, request_relay, relay](
                        std::wstring* session_status) -> bool {
                      return open_auxiliary_relay_session_with_sideband(
                          request_relay.socket_addr,
                          relay,
                          request_relay.uuid,
                          false,
                          request_relay.secure,
                          session_status);
                    })) {
              SetRendezvousStatus(L"failed to queue relay session", registered);
            }
          }

          if (parsed.has_punch_hole) {
            handled_frame = true;
            const PunchHoleData punch_hole = parsed.punch_hole;
            std::wstring relay = punch_hole.relay_server;
            if (relay.empty()) {
              relay = session_config.relay_server;
            }
            const std::wstring starting_status =
                std::wstring(L"online, punch hole request received from hbbs; switching to relay fallback (") +
                relay + L")";
            if (!start_session_or_auxiliary(
                    starting_status,
                    registered,
                    L"punch-hole relay fallback failed: ",
                    [start_initiated_relay, punch_hole, relay](std::wstring* session_status) -> bool {
                      return start_initiated_relay(
                          punch_hole.socket_addr,
                          relay,
                          L"PunchHole",
                          session_status);
                    },
                    [start_initiated_auxiliary_relay, punch_hole, relay](
                        std::wstring* session_status) -> bool {
                      return start_initiated_auxiliary_relay(
                          punch_hole.socket_addr,
                          relay,
                          L"PunchHole",
                          session_status);
                    })) {
              SetRendezvousStatus(L"failed to queue punch-hole relay fallback", registered);
            }
          }

          if (parsed.has_fetch_local_addr) {
            handled_frame = true;
            const FetchLocalAddrData fetch_local_addr = parsed.fetch_local_addr;
            std::wstring relay = fetch_local_addr.relay_server;
            if (relay.empty()) {
              relay = session_config.relay_server;
            }
            if (session_config.force_relay) {
              const std::wstring starting_status =
                  std::wstring(L"online, FetchLocalAddr received from hbbs; force relay enabled, switching directly to relay (") +
                  relay + L")";
              if (!start_session_or_auxiliary(
                      starting_status,
                      registered,
                      L"FetchLocalAddr relay fallback failed: ",
                      [start_initiated_relay, fetch_local_addr, relay](std::wstring* session_status) -> bool {
                        return start_initiated_relay(
                            fetch_local_addr.socket_addr,
                            relay,
                            L"FetchLocalAddr",
                            session_status);
                      },
                      [start_initiated_auxiliary_relay, fetch_local_addr, relay](
                          std::wstring* session_status) -> bool {
                        return start_initiated_auxiliary_relay(
                            fetch_local_addr.socket_addr,
                            relay,
                            L"FetchLocalAddr",
                            session_status);
                      })) {
                SetRendezvousStatus(L"failed to queue FetchLocalAddr relay fallback", registered);
              }
            } else {
              const std::wstring starting_status =
                  std::wstring(L"online, FetchLocalAddr received from hbbs; trying direct intranet path (") +
                  relay + L")";
              if (!start_session_or_auxiliary(
                      starting_status,
                      registered,
                      L"FetchLocalAddr direct+relay failed: ",
                      [accept_direct_intranet_session,
                       start_initiated_relay,
                       fetch_local_addr,
                       relay](std::wstring* session_status) -> bool {
                        if (accept_direct_intranet_session(fetch_local_addr, relay, session_status)) {
                          return true;
                        }
                        return start_initiated_relay(
                            fetch_local_addr.socket_addr,
                            relay,
                            L"FetchLocalAddr",
                            session_status);
                      },
                      [start_initiated_auxiliary_relay, fetch_local_addr, relay](
                          std::wstring* session_status) -> bool {
                        return start_initiated_auxiliary_relay(
                            fetch_local_addr.socket_addr,
                            relay,
                            L"FetchLocalAddr",
                            session_status);
                      })) {
                SetRendezvousStatus(L"failed to queue FetchLocalAddr direct/relay session", registered);
              }
            }
          }

          if (!handled_frame) {
            SetRendezvousStatus(
                std::wstring(L"received unhandled hbbs udp message fields=") +
                    FormatObservedFields(parsed.observed_fields) +
                    L" (" + std::to_wstring(frame.size()) + L" bytes)",
                registered);
          }
        }
      }
    }

    if (should_try_tcp_fallback && !stop_rendezvous_.load() && run_tcp_rendezvous()) {
      wait_with_direct_access(2000, IsRendezvousRegistered());
      continue;
    }

    if (!udp_connected) {
      wait_with_direct_access(5000, IsRendezvousRegistered());
      continue;
    }

    wait_with_direct_access(2000, IsRendezvousRegistered());
  }
}

bool PortableHostApp::CanReachTcpHost(
    const std::wstring& host,
    unsigned short port,
    unsigned long timeout_ms) const {
  if (!winsock_ready_ || host.empty()) {
    return false;
  }

  addrinfoW hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfoW* results = nullptr;
  const std::wstring port_text = std::to_wstring(port);
  if (GetAddrInfoW(host.c_str(), port_text.c_str(), &hints, &results) != 0) {
    return false;
  }

  bool reachable = false;
  for (addrinfoW* current = results; current != nullptr && !reachable; current = current->ai_next) {
    SOCKET socket_handle = socket(
        current->ai_family,
        current->ai_socktype,
        current->ai_protocol);
    if (socket_handle == INVALID_SOCKET) {
      continue;
    }

    u_long non_blocking = 1;
    ioctlsocket(socket_handle, FIONBIO, &non_blocking);

    const int connect_result = connect(
        socket_handle,
        current->ai_addr,
        static_cast<int>(current->ai_addrlen));

    if (connect_result == 0) {
      reachable = true;
    } else {
      const int error_code = WSAGetLastError();
      if (error_code == WSAEWOULDBLOCK || error_code == WSAEINPROGRESS || error_code == WSAEINVAL) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_handle, &write_set);

        TIMEVAL timeout = {};
        timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
        timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

        const int select_result = select(0, nullptr, &write_set, nullptr, &timeout);
        if (select_result > 0) {
          int socket_error = 0;
          int option_length = sizeof(socket_error);
          if (getsockopt(
                  socket_handle,
                  SOL_SOCKET,
                  SO_ERROR,
                  reinterpret_cast<char*>(&socket_error),
                  &option_length) == 0 &&
              socket_error == 0) {
            reachable = true;
          }
        }
      }
    }

    closesocket(socket_handle);
  }

  FreeAddrInfoW(results);
  return reachable;
}
