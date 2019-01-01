/*
 * libjingle
 * Copyright 2004--2005, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/base/win32socketserver.h"
#include "talk/base/byteorder.h"
#include "talk/base/common.h"
#include "talk/base/logging.h"
#include "talk/base/winping.h"
#include "talk/base/win32window.h"
#include <ws2tcpip.h>  // NOLINT

namespace talk_base {

///////////////////////////////////////////////////////////////////////////////
// Win32Socket
///////////////////////////////////////////////////////////////////////////////

// TODO: Move this to a common place where PhysicalSocketServer can
// share it.
// Standard MTUs
static const uint16 PACKET_MAXIMUMS[] = {
  65535,    // Theoretical maximum, Hyperchannel
  32000,    // Nothing
  17914,    // 16Mb IBM Token Ring
  8166,     // IEEE 802.4
  // 4464   // IEEE 802.5 (4Mb max)
  4352,     // FDDI
  // 2048,  // Wideband Network
  2002,     // IEEE 802.5 (4Mb recommended)
  // 1536,  // Expermental Ethernet Networks
  // 1500,  // Ethernet, Point-to-Point (default)
  1492,     // IEEE 802.3
  1006,     // SLIP, ARPANET
  // 576,   // X.25 Networks
  // 544,   // DEC IP Portal
  // 512,   // NETBIOS
  508,      // IEEE 802/Source-Rt Bridge, ARCNET
  296,      // Point-to-Point (low delay)
  68,       // Official minimum
  0,        // End of list marker
};

static const uint32 IP_HEADER_SIZE = 20;
static const uint32 ICMP_HEADER_SIZE = 8;

// TODO: Enable for production builds also? Use FormatMessage?
#ifdef _DEBUG
LPCSTR WSAErrorToString(int error, LPCSTR *description_result) {
  LPCSTR string = "Unspecified";
  LPCSTR description = "Unspecified description";
  switch (error) {
    case ERROR_SUCCESS:
      string = "SUCCESS";
      description = "Operation succeeded";
      break;
    case WSAEWOULDBLOCK:
      string = "WSAEWOULDBLOCK";
      description = "Using a non-blocking socket, will notify later";
      break;
    case WSAEACCES:
      string = "WSAEACCES";
      description = "Access denied, or sharing violation";
      break;
    case WSAEADDRNOTAVAIL:
      string = "WSAEADDRNOTAVAIL";
      description = "Address is not valid in this context";
      break;
    case WSAENETDOWN:
      string = "WSAENETDOWN";
      description = "Network is down";
      break;
    case WSAENETUNREACH:
      string = "WSAENETUNREACH";
      description = "Network is up, but unreachable";
      break;
    case WSAENETRESET:
      string = "WSANETRESET";
      description = "Connection has been reset due to keep-alive activity";
      break;
    case WSAECONNABORTED:
      string = "WSAECONNABORTED";
      description = "Aborted by host";
      break;
    case WSAECONNRESET:
      string = "WSAECONNRESET";
      description = "Connection reset by host";
      break;
    case WSAETIMEDOUT:
      string = "WSAETIMEDOUT";
      description = "Timed out, host failed to respond";
      break;
    case WSAECONNREFUSED:
      string = "WSAECONNREFUSED";
      description = "Host actively refused connection";
      break;
    case WSAEHOSTDOWN:
      string = "WSAEHOSTDOWN";
      description = "Host is down";
      break;
    case WSAEHOSTUNREACH:
      string = "WSAEHOSTUNREACH";
      description = "Host is unreachable";
      break;
    case WSAHOST_NOT_FOUND:
      string = "WSAHOST_NOT_FOUND";
      description = "No such host is known";
      break;
  }
  if (description_result) {
    *description_result = description;
  }
  return string;
}

void ReportWSAError(LPCSTR context, int error, const SocketAddress& address) {
  LPCSTR description_string;
  LPCSTR error_string = WSAErrorToString(error, &description_string);
  LOG(LS_INFO) << context << " = " << error
    << " (" << error_string << ":" << description_string << ") ["
    << address.ToString() << "]";
}
#else
void ReportWSAError(LPCSTR context, int error, const SocketAddress& address) {}
#endif

/////////////////////////////////////////////////////////////////////////////
// Win32Socket::EventSink
/////////////////////////////////////////////////////////////////////////////

#define WM_SOCKETNOTIFY  (WM_USER + 50)
#define WM_DNSNOTIFY     (WM_USER + 51)

struct Win32Socket::DnsLookup {
  HANDLE handle;
  uint16 port;
  char buffer[MAXGETHOSTSTRUCT];
};

class Win32Socket::EventSink : public Win32Window {
 public:
  explicit EventSink(Win32Socket * parent) : parent_(parent) { }

  void Dispose();

  virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam,
                         LRESULT& result);
  virtual void OnFinalMessage(HWND hWnd);

 private:
  bool OnSocketNotify(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result);
  bool OnDnsNotify(WPARAM wParam, LPARAM lParam, LRESULT& result);

  Win32Socket * parent_;
};

void Win32Socket::EventSink::Dispose() {
  parent_ = NULL;
  if (::IsWindow(handle())) {
    ::DestroyWindow(handle());
  } else {
    delete this;
  }
}

bool Win32Socket::EventSink::OnMessage(UINT uMsg, WPARAM wParam,
                                       LPARAM lParam, LRESULT& result) {
  switch (uMsg) {
  case WM_SOCKETNOTIFY:
  case WM_TIMER:
    return OnSocketNotify(uMsg, wParam, lParam, result);
  case WM_DNSNOTIFY:
    return OnDnsNotify(wParam, lParam, result);
  }
  return false;
}

bool Win32Socket::EventSink::OnSocketNotify(UINT uMsg, WPARAM wParam,
                                            LPARAM lParam, LRESULT& result) {
  result = 0;

  int wsa_event = WSAGETSELECTEVENT(lParam);
  int wsa_error = WSAGETSELECTERROR(lParam);

  // Treat connect timeouts as close notifications
  if (uMsg == WM_TIMER) {
    wsa_event = FD_CLOSE;
    wsa_error = WSAETIMEDOUT;
  }

  if (parent_)
    parent_->OnSocketNotify(static_cast<SOCKET>(wParam), wsa_event, wsa_error);
  return true;
}

bool Win32Socket::EventSink::OnDnsNotify(WPARAM wParam, LPARAM lParam,
                                         LRESULT& result) {
  result = 0;

  int error = WSAGETASYNCERROR(lParam);
  if (parent_)
    parent_->OnDnsNotify(reinterpret_cast<HANDLE>(wParam), error);
  return true;
}

void Win32Socket::EventSink::OnFinalMessage(HWND hWnd) {
  delete this;
}

/////////////////////////////////////////////////////////////////////////////
// Win32Socket
/////////////////////////////////////////////////////////////////////////////

Win32Socket::Win32Socket()
    : socket_(INVALID_SOCKET), error_(0), state_(CS_CLOSED), connect_time_(0),
      closing_(false), close_error_(0), sink_(NULL), dns_(NULL) {
}

Win32Socket::~Win32Socket() {
  Close();
}

bool Win32Socket::CreateT(int type) {
	LOG(LS_VERBOSE) << "Win32Socket::CreateT - Calling Close()";
  Close();
  int proto = (SOCK_DGRAM == type) ? IPPROTO_UDP : IPPROTO_TCP;

  if(proto == IPPROTO_UDP)
  {
	  LOG(LS_VERBOSE) << "Win32Socket::CreateT - Creating win32 UDP socket";
  }
  else
  {
	  LOG(LS_VERBOSE) << "Win32Socket::CreateT - Creating win32 TCP socket";
  }

  socket_ = ::WSASocket(AF_INET, type, proto, NULL, NULL, 0);
  if (socket_ == INVALID_SOCKET) {

	  LOG(LS_VERBOSE) << "Win32Socket::CreateT - Creating win32 socket returned INVALID_SOCKET";

    UpdateLastError();
    return false;
  }
  if ((SOCK_DGRAM == type) && !SetAsync(FD_READ | FD_WRITE)) {
    return false;
  }
  return true;
}

int Win32Socket::Attach(SOCKET s) {
	LOG(LS_VERBOSE) << "Win32Socket::Attach";

  ASSERT(socket_ == INVALID_SOCKET);
  if (socket_ != INVALID_SOCKET)
    return SOCKET_ERROR;

  ASSERT(s != INVALID_SOCKET);
  if (s == INVALID_SOCKET)
    return SOCKET_ERROR;

  socket_ = s;
  state_ = CS_CONNECTED;

  if (!SetAsync(FD_READ | FD_WRITE | FD_CLOSE))
    return SOCKET_ERROR;

  return 0;
}

void Win32Socket::SetTimeout(int ms) {
  if (sink_)
    ::SetTimer(sink_->handle(), 1, ms, 0);
}

SocketAddress Win32Socket::GetLocalAddress() const {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  int result = ::getsockname(socket_, reinterpret_cast<sockaddr*>(&addr),
                             &addrlen);
  SocketAddress address;
  if (result >= 0) {
    ASSERT(addrlen == sizeof(addr));
    address.FromSockAddr(addr);
  } else {
    LOG(LS_WARNING) << "GetLocalAddress: unable to get local addr, socket="
                    << socket_;
  }
  return address;
}

SocketAddress Win32Socket::GetRemoteAddress() const {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  int result = ::getpeername(socket_, reinterpret_cast<sockaddr*>(&addr),
                             &addrlen);
  ASSERT(addrlen == sizeof(addr));
  SocketAddress address;
  if (result >= 0) {
    ASSERT(addrlen == sizeof(addr));
    address.FromSockAddr(addr);
  } else {
    LOG(LS_WARNING) << "GetRemoteAddress: unable to get remote addr, socket="
                    << socket_;
  }
  return address;
}

int Win32Socket::Bind(const SocketAddress& addr) {
	LOG(LS_VERBOSE) << "Win32Socket::Bind";

  ASSERT(socket_ != INVALID_SOCKET);
  if (socket_ == INVALID_SOCKET)
    return SOCKET_ERROR;

  sockaddr_in saddr;
  addr.ToSockAddr(&saddr);
  int err = ::bind(socket_, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
  UpdateLastError();
  return err;
}

int Win32Socket::Connect(const SocketAddress& addr) {

	LOG(LS_VERBOSE) << "Win32Socket::Connect - " << addr.ToString();

  if ((socket_ == INVALID_SOCKET) && !CreateT(SOCK_STREAM))
    return SOCKET_ERROR;

  if (!sink_ && !SetAsync(FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE))
    return SOCKET_ERROR;

  // If we have an IP address, connect now.
  if (!addr.IsUnresolved()) {
    return DoConnect(addr);
  }

  LOG_F(LS_INFO) << "async dns lookup (" << addr.IPAsString() << ")";
  DnsLookup * dns = new DnsLookup;
  dns->handle = WSAAsyncGetHostByName(sink_->handle(), WM_DNSNOTIFY,
        addr.IPAsString().c_str(), dns->buffer, sizeof(dns->buffer));

  if (!dns->handle) {
    LOG_F(LS_ERROR) << "WSAAsyncGetHostByName error: " << WSAGetLastError();
    delete dns;
    UpdateLastError();
    Close();
    return SOCKET_ERROR;
  }

  dns->port = addr.port();
  dns_ = dns;
  state_ = CS_CONNECTING;
  return 0;
}

int Win32Socket::DoConnect(const SocketAddress& addr) {
  sockaddr_in saddr;
  addr.ToSockAddr(&saddr);
	LOG(LS_VERBOSE) << "Win32Socket::DoConnect - " << addr.ToString();
  connect_time_ = Time();
  int result = connect(socket_, reinterpret_cast<SOCKADDR*>(&saddr),
                       sizeof(saddr));
  if (result != SOCKET_ERROR) {
    state_ = CS_CONNECTED;
  } else {
    int code = WSAGetLastError();
    if (code == WSAEWOULDBLOCK) {
      state_ = CS_CONNECTING;
    } else {
      ReportWSAError("WSAAsync:connect", code, addr);
      error_ = code;
      Close();
      return SOCKET_ERROR;
    }
  }
  addr_ = addr;

  return 0;
}

int Win32Socket::GetError() const {
  return error_;
}

void Win32Socket::SetError(int error) {
  error_ = error;
}

Socket::ConnState Win32Socket::GetState() const {
  return state_;
}

int Win32Socket::GetOption(Option opt, int* value) {
  int slevel;
  int sopt;
  if (TranslateOption(opt, &slevel, &sopt) == -1)
    return -1;

  char* p = reinterpret_cast<char*>(value);
  int optlen = sizeof(value);
  return ::getsockopt(socket_, slevel, sopt, p, &optlen);
}

int Win32Socket::SetOption(Option opt, int value) {
  int slevel;
  int sopt;
  if (TranslateOption(opt, &slevel, &sopt) == -1)
    return -1;

  const char* p = reinterpret_cast<const char*>(&value);
  return ::setsockopt(socket_, slevel, sopt, p, sizeof(value));
}

int Win32Socket::Send(const void *pv, size_t cb) {
  int sent = ::send(socket_, reinterpret_cast<const char*>(pv), cb, 0);
  UpdateLastError();
  return sent;
}

int Win32Socket::SendTo(const void *pv, size_t cb,
                        const SocketAddress& addr) {
  sockaddr_in saddr;
  addr.ToSockAddr(&saddr);

  LOG(LS_VERBOSE) << "Win32Socket::SendTo - " << addr.ToString();

  int sent = ::sendto(socket_, reinterpret_cast<const char*>(pv), cb, 0,
                      reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
  UpdateLastError();
  return sent;
}

int Win32Socket::Recv(void *pv, size_t cb) {
//	LOG(LS_VERBOSE) << "Win32Socket::Recv";
  int received = ::recv(socket_, static_cast<char*>(pv), cb, 0);
  UpdateLastError();
  if (closing_ && received <= static_cast<int>(cb))
    PostClosed();
  return received;
}

int Win32Socket::RecvFrom(void *pv, size_t cb,
                          SocketAddress *paddr) {

  sockaddr_in saddr;
  socklen_t cbAddr = sizeof(saddr);

  LOG(LS_VERBOSE) << "Win32Socket::RecvFrom - " << paddr->ToString();

  int received = ::recvfrom(socket_, static_cast<char*>(pv), cb, 0,
                            reinterpret_cast<sockaddr*>(&saddr), &cbAddr);
  UpdateLastError();
  if (received != SOCKET_ERROR)
    paddr->FromSockAddr(saddr);
  if (closing_ && received <= static_cast<int>(cb))
    PostClosed();
  return received;
}

int Win32Socket::Listen(int backlog) {

	LOG(LS_VERBOSE) << "Win32Socket::Listen";

  int err = ::listen(socket_, backlog);
  if (!SetAsync(FD_ACCEPT))
    return SOCKET_ERROR;

  UpdateLastError();
  if (err == 0)
    state_ = CS_CONNECTING;
  return err;
}

Win32Socket* Win32Socket::Accept(SocketAddress *paddr) {
  sockaddr_in saddr;
  socklen_t cbAddr = sizeof(saddr);

  LOG(LS_VERBOSE) << "Win32SocketServer::Accept - " << paddr->ToString();

  SOCKET s = ::accept(socket_, reinterpret_cast<sockaddr*>(&saddr), &cbAddr);
  UpdateLastError();
  if (s == INVALID_SOCKET)
    return NULL;
  if (paddr)
    paddr->FromSockAddr(saddr);
  Win32Socket* socket = new Win32Socket;
  if (0 == socket->Attach(s))
    return socket;
  delete socket;
  return NULL;
}

int Win32Socket::Close() {
  int err = 0;
  LOG(LS_VERBOSE) << "Win32SocketServer::Close";
  if (socket_ != INVALID_SOCKET) {
    err = ::closesocket(socket_);
    socket_ = INVALID_SOCKET;
    closing_ = false;
    close_error_ = 0;
    UpdateLastError();
  }
  if (dns_) {
    WSACancelAsyncRequest(dns_->handle);
    delete dns_;
    dns_ = NULL;
  }
  if (sink_) {
    sink_->Dispose();
    sink_ = NULL;
  }
  addr_.Clear();
  state_ = CS_CLOSED;
  return err;
}

int Win32Socket::EstimateMTU(uint16* mtu) {
  SocketAddress addr = GetRemoteAddress();

  LOG(LS_VERBOSE) << "Win32Socket::EstimateMTU - remote address: " << addr.ToString();

  if (addr.IsAny()) {
    error_ = ENOTCONN;
    return -1;
  }

  WinPing ping;
  if (!ping.IsValid()) {
    error_ = EINVAL;  // can't think of a better error ID
    return -1;
  }

  for (int level = 0; PACKET_MAXIMUMS[level + 1] > 0; ++level) {
    int32 size = PACKET_MAXIMUMS[level] - IP_HEADER_SIZE - ICMP_HEADER_SIZE;
    WinPing::PingResult result = ping.Ping(addr.ip(), size, 0, 1, false);
    if (result == WinPing::PING_FAIL) {
      error_ = EINVAL;  // can't think of a better error ID
      return -1;
    }
    if (result != WinPing::PING_TOO_LARGE) {
      *mtu = PACKET_MAXIMUMS[level];
      return 0;
    }
  }

  ASSERT(false);
  return 0;
}

bool Win32Socket::SetAsync(int events) {
  ASSERT(NULL == sink_);
  LOG(LS_VERBOSE) << "Win32SocketServer::SetAsync";

  // Create window
  sink_ = new EventSink(this);
  sink_->Create(NULL, L"EventSink", 0, 0, 0, 0, 10, 10);

  // start the async select
  if (WSAAsyncSelect(socket_, sink_->handle(), WM_SOCKETNOTIFY, events)
      == SOCKET_ERROR) {
    UpdateLastError();
    Close();
    return false;
  }

  return true;
}

bool Win32Socket::HandleClosed(int close_error) {
  // WM_CLOSE will be received before all data has been read, so we need to
  // hold on to it until the read buffer has been drained.
  char ch;

  LOG(LS_VERBOSE) << "Win32SocketServer::HandleClosed - code: " << close_error;

  closing_ = true;
  close_error_ = close_error;
  return (::recv(socket_, &ch, 1, MSG_PEEK) <= 0);
}

void Win32Socket::PostClosed() {
  // If we see that the buffer is indeed drained, then send the close.

	  LOG(LS_VERBOSE) << "Win32SocketServer::PostClosed";

  closing_ = false;
  ::PostMessage(sink_->handle(), WM_SOCKETNOTIFY,
                socket_, WSAMAKESELECTREPLY(FD_CLOSE, close_error_));
}

void Win32Socket::UpdateLastError() {
  error_ = WSAGetLastError();
}

int Win32Socket::TranslateOption(Option opt, int* slevel, int* sopt) {
  switch (opt) {
    case OPT_DONTFRAGMENT:
      *slevel = IPPROTO_IP;
      *sopt = IP_DONTFRAGMENT;
      break;
    case OPT_RCVBUF:
      *slevel = SOL_SOCKET;
      *sopt = SO_RCVBUF;
      break;
    case OPT_SNDBUF:
      *slevel = SOL_SOCKET;
      *sopt = SO_SNDBUF;
      break;
    case OPT_NODELAY:
      *slevel = IPPROTO_TCP;
      *sopt = TCP_NODELAY;
      break;
    default:
      ASSERT(false);
      return -1;
  }
  return 0;
}

void Win32Socket::OnSocketNotify(SOCKET socket, int event, int error) {

//	LOG(LS_VERBOSE) << "Win32Socket::OnSocketNotify";

  // Ignore events if we're already closed.
  if (socket != socket_)
    return;

  error_ = error;
  switch (event) {
    case FD_CONNECT:
      if (error != ERROR_SUCCESS) {
        ReportWSAError("WSAAsync:connect notify", error, addr_);
#ifdef _DEBUG
        int32 duration = TimeSince(connect_time_);
        LOG(LS_INFO) << "WSAAsync:connect error (" << duration
                     << " ms), faking close";
#endif
        state_ = CS_CLOSED;
        // If you get an error connecting, close doesn't really do anything
        // and it certainly doesn't send back any close notification, but
        // we really only maintain a few states, so it is easiest to get
        // back into a known state by pretending that a close happened, even
        // though the connect event never did occur.
        SignalCloseEvent(this, error);
      } else {
#ifdef _DEBUG
        int32 duration = TimeSince(connect_time_);
        LOG(LS_INFO) << "WSAAsync:connect (" << duration << " ms)";
#endif
        state_ = CS_CONNECTED;

        LOG(LS_VERBOSE) << "Win32Socket::OnSocketNotify - SignalConnectEvent:CS_CONNECTED";

        SignalConnectEvent(this);
      }
      break;

    case FD_ACCEPT:
    case FD_READ:
      if (error != ERROR_SUCCESS) {
        ReportWSAError("WSAAsync:read notify", error, addr_);
      } else {

//    	  LOG(LS_VERBOSE) << "Win32Socket::OnSocketNotify - SignalReadEvent";

        SignalReadEvent(this);
      }
      break;

    case FD_WRITE:
      if (error != ERROR_SUCCESS) {
        ReportWSAError("WSAAsync:write notify", error, addr_);
      } else {
        SignalWriteEvent(this);
      }
      break;

    case FD_CLOSE:
      if (HandleClosed(error)) {
        ReportWSAError("WSAAsync:close notify", error, addr_);
        state_ = CS_CLOSED;
        SignalCloseEvent(this, error);
      }
      break;
  }
}

void Win32Socket::OnDnsNotify(HANDLE task, int error) {
	  LOG(LS_VERBOSE) << "Win32SocketServer::OnDnsNotify";

  if (!dns_ || dns_->handle != task)
    return;

  uint32 ip = 0;
  if (error == 0) {
    hostent* pHost = reinterpret_cast<hostent*>(dns_->buffer);
    uint32 net_ip = *reinterpret_cast<uint32*>(pHost->h_addr_list[0]);
    ip = NetworkToHost32(net_ip);
  }

  LOG_F(LS_INFO) << "(" << SocketAddress::IPToString(ip)
                 << ", " << error << ")";

  if (error == 0) {
    SocketAddress address(ip, dns_->port);
    error = DoConnect(address);
  } else {
    Close();
  }

  if (error) {
    error_ = error;
    SignalCloseEvent(this, error_);
  } else {
    delete dns_;
    dns_ = NULL;
  }
}

///////////////////////////////////////////////////////////////////////////////
// Win32SocketServer
// Provides cricket base services on top of a win32 gui thread
///////////////////////////////////////////////////////////////////////////////

static UINT s_wm_wakeup_id = 0;
const TCHAR Win32SocketServer::kWindowName[] = L"libjingle Message Window";

Win32SocketServer::Win32SocketServer(MessageQueue *message_queue)
    : message_queue_(message_queue), wnd_(this), posted_(false) {


	LOG(LS_VERBOSE) << "-------------------------Win32SocketServer Constructor ---------------------";
	wakeUpCounter = 0;
	getMsgCounter = 0;
	currentMsgCounter = 0;

  if (s_wm_wakeup_id == 0)
    s_wm_wakeup_id = RegisterWindowMessage(L"WM_WAKEUP");
  if (!wnd_.Create(NULL, kWindowName, 0, 0, 0, 0, 0, 0)) {
    LOG_GLE(LS_ERROR) << "Failed to create message window.";
  }
}

Win32SocketServer::~Win32SocketServer() {

	LOG(LS_VERBOSE) << "-------------------------Win32SocketServer De-Constructor ---------------------";

  if (wnd_.handle() != NULL) {
    KillTimer(wnd_.handle(), 1);
    wnd_.Destroy();
  }
}

Socket* Win32SocketServer::CreateSocket(int type) {
  return CreateAsyncSocket(type);
}

AsyncSocket* Win32SocketServer::CreateAsyncSocket(int type) {

	  LOG(LS_VERBOSE) << "Win32SocketServer::CreateAsyncSocket";

  Win32Socket* socket = new Win32Socket;
  if (socket->CreateT(type)) {
    return socket;
  }
  delete socket;
  return NULL;
}

void Win32SocketServer::SetMessageQueue(MessageQueue* queue) {
  message_queue_ = queue;
}

bool Win32SocketServer::Wait(int cms, bool process_io) {
  BOOL b;

//  LOG(LS_VERBOSE) << "Win32SocketServer::Wait - cms = " << cms << ", process_io = " << process_io;

  if (process_io) {
    // Spin the Win32 message pump at least once, and as long as requested.
    // This is the Thread::ProcessMessages case.
    uint32 start = Time();
    MSG msg;
    do {

//		LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 1";

		SetTimer(wnd_.handle(), 0, cms, NULL);
		b = GetMessage(&msg, NULL, 0, 0);

//		LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 2";

      if (b) {
        TranslateMessage(&msg);
//    	LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 3";
        DispatchMessage(&msg);
//    	LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 4";
      }


      KillTimer(wnd_.handle(), 0);

//      LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 5";
    } while (b && TimeSince(start) < cms);

//	LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 7";
  }
  else if (cms != 0)
  {
    // Sit and wait forever for a WakeUp. This is the Thread::Send case.
    ASSERT(cms == -1);
    MSG msg;

//    LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 8" << ", currentMsgCounter = " << currentMsgCounter
//    		<< ", s_wm_wakeup_id = " << s_wm_wakeup_id;

    b = GetMessage(&msg, NULL, s_wm_wakeup_id, s_wm_wakeup_id);

//    LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 8.1";

    {
      CritScope scope(&cs_);

//      LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 8.2";

      posted_ = false;
    }

	getMsgCounter ++;
	currentMsgCounter --;

//    LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 9, getMsgCounter = " << getMsgCounter
//    		<< ", currentMsgCounter = " << currentMsgCounter;
  }

  else {
    // No-op (cms == 0 && !process_io). This is the Pump case.
    b = TRUE;

//    LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 10";
  }

//  LOG(LS_VERBOSE) << "Win32SocketServer::Wait - 11";

  return (b != FALSE);
}


void Win32SocketServer::WakeUp() {

//	LOG(LS_VERBOSE) << "Win32SocketServer::WakeUp";

  if (wnd_.handle()) {
    // Set the "message pending" flag, if not already set.
    {
      CritScope scope(&cs_);
      if (posted_)
      {
//    	  LOG(LS_VERBOSE) << "Win32SocketServer::WakeUp - Already posted";
    	    return;
      }

      posted_ = true;
    }

    PostMessage(wnd_.handle(), s_wm_wakeup_id, 0, 0);

	wakeUpCounter ++;

	currentMsgCounter++;

//    LOG(LS_VERBOSE) << "Win32SocketServer::WakeUp - PostMessage called and returned, wakeUpCounter = " << wakeUpCounter
//    		<< ", currentMsgCounter = " << currentMsgCounter << ", s_wm_wakeup_id = " << s_wm_wakeup_id;

  }
}

void Win32SocketServer::Pump() {
  // Clear the "message pending" flag.

//	LOG(LS_VERBOSE) << "Win32SocketServer::Pump";

  {
    CritScope scope(&cs_);
    posted_ = false;
  }

  // Dispatch all the messages that are currently in our queue. If new messages
  // are posted during the dispatch, they will be handled in the next Pump.
  // We use max(1, ...) to make sure we try to dispatch at least once, since
  // this allow us to process "sent" messages, not included in the size() count.
  Message msg;
  for (size_t max_messages_to_process = _max<size_t>(1, message_queue_->size());
       max_messages_to_process > 0 && message_queue_->Get(&msg, 0, false);
       --max_messages_to_process) {

//	  LOG(LS_VERBOSE) << "Win32SocketServer::Pump - Dispatch message";

    message_queue_->Dispatch(&msg);
  }

  // Anything remaining?
  int delay = message_queue_->GetDelay();
  if (delay == -1) {
    KillTimer(wnd_.handle(), 1);
  } else {
    SetTimer(wnd_.handle(), 1, delay, NULL);
  }
}

bool Win32SocketServer::MessageWindow::OnMessage(UINT wm, WPARAM wp,
                                                 LPARAM lp, LRESULT& lr) {
  bool handled = false;

//  LOG(LS_VERBOSE) << "Win32SocketServer::MessageWindow::OnMessage";

  if (wm == s_wm_wakeup_id || (wm == WM_TIMER && wp == 1)) {
    ss_->Pump();
    lr = 0;
    handled = true;
  }
  return handled;
}

}  // namespace talk_base
