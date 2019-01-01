

#include "talk/base/macsocketserver.h"

#include "talk/base/common.h"
#include "talk/base/logging.h"
#include "talk/base/macasyncsocket.h"
#include "talk/base/macutils.h"
#include "talk/base/thread.h"

namespace talk_base {

///////////////////////////////////////////////////////////////////////////////
// MacBaseSocketServer
///////////////////////////////////////////////////////////////////////////////

MacBaseSocketServer::MacBaseSocketServer() {
}

MacBaseSocketServer::~MacBaseSocketServer() {
}

AsyncSocket* MacBaseSocketServer::CreateAsyncSocket(int type) {

	LOG(LS_VERBOSE) << "MacBaseSocketServer::CreateAsyncSocket";

  if (SOCK_STREAM != type)
    return NULL;

  MacAsyncSocket* socket = new MacAsyncSocket(this);
  if (!socket->valid()) {
    delete socket;
    return NULL;
  }
  return socket;
}

void MacBaseSocketServer::RegisterSocket(MacAsyncSocket* s) {
  sockets_.insert(s);
}

void MacBaseSocketServer::UnregisterSocket(MacAsyncSocket* s) {
  size_t found = sockets_.erase(s);
  ASSERT(found == 1);
}

void MacBaseSocketServer::EnableSocketCallbacks(bool enable) {
  for (std::set<MacAsyncSocket*>::iterator it = sockets().begin();
       it != sockets().end(); ++it) {
    if (enable) {
      (*it)->EnableCallbacks();
    } else {
      (*it)->DisableCallbacks();
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// MacCFSocketServer
///////////////////////////////////////////////////////////////////////////////

void WakeUpCallback(void* info) {
  MacCFSocketServer* server = static_cast<MacCFSocketServer*>(info);
  ASSERT(NULL != server);
  server->OnWakeUpCallback();
}

MacCFSocketServer::MacCFSocketServer()
    : run_loop_(CFRunLoopGetCurrent()),
      wake_up_(NULL) {
  CFRunLoopSourceContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.info = this;
  ctx.perform = &WakeUpCallback;
  wake_up_ = CFRunLoopSourceCreate(NULL, 0, &ctx);
  ASSERT(NULL != wake_up_);
  if (wake_up_) {
    CFRunLoopAddSource(run_loop_, wake_up_, kCFRunLoopCommonModes);
  }
}

MacCFSocketServer::~MacCFSocketServer() {
  if (wake_up_) {
    CFRunLoopSourceInvalidate(wake_up_);
    CFRelease(wake_up_);
  }
}

bool MacCFSocketServer::Wait(int cms, bool process_io) {
  ASSERT(CFRunLoopGetCurrent() == run_loop_);

  if (!process_io && cms == 0) {
    // No op.
    return true;
  }

  if (!process_io) {
    // No way to listen to common modes and not get socket events, unless
    // we disable each one's callbacks.
    EnableSocketCallbacks(false);
  }

  SInt32 result;
  if (kForever == cms) {
    do {
      // Would prefer to run in a custom mode that only listens to wake_up,
      // but we have qtkit sending work to the main thread which is effectively
      // blocked here, causing deadlock.  Thus listen to the common modes.
      // TODO: If QTKit becomes thread safe, do the above.
      result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 10000000, false);
    } while (result != kCFRunLoopRunFinished && result != kCFRunLoopRunStopped);
  } else {
    // TODO: In the case of 0ms wait, this will only process one event, so we
    // may want to loop until it returns TimedOut.
    CFTimeInterval seconds = cms / 1000.0;
    result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
  }

  if (!process_io) {
    // Reenable them.  Hopefully this won't cause spurious callbacks or
    // missing ones while they were disabled.
    EnableSocketCallbacks(true);
  }

  if (kCFRunLoopRunFinished == result) {
    return false;
  }
  return true;
}

void MacCFSocketServer::WakeUp() {
  if (wake_up_) {
    CFRunLoopSourceSignal(wake_up_);
    CFRunLoopWakeUp(run_loop_);
  }
}

void MacCFSocketServer::OnWakeUpCallback() {
  ASSERT(run_loop_ == CFRunLoopGetCurrent());
  CFRunLoopStop(run_loop_);
}

///////////////////////////////////////////////////////////////////////////////
// MacCarbonSocketServer
///////////////////////////////////////////////////////////////////////////////

const UInt32 kEventClassSocketServer = 'MCSS';
const UInt32 kEventWakeUp = 'WAKE';
const EventTypeSpec kEventWakeUpSpec[] = {
  { kEventClassSocketServer, kEventWakeUp }
};

MacCarbonSocketServer::MacCarbonSocketServer()
    : event_queue_(GetCurrentEventQueue()), wake_up_(NULL) {
  VERIFY(noErr == CreateEvent(NULL, kEventClassSocketServer, kEventWakeUp, 0,
                              kEventAttributeUserEvent, &wake_up_));
}

MacCarbonSocketServer::~MacCarbonSocketServer() {
  if (wake_up_) {
    ReleaseEvent(wake_up_);
  }
}

bool MacCarbonSocketServer::Wait(int cms, bool process_io) {
  ASSERT(GetCurrentEventQueue() == event_queue_);

  // Listen to all events if we're processing I/O.
  // Only listen for our wakeup event if we're not.
  UInt32 num_types = 0;
  const EventTypeSpec* events = NULL;
  if (!process_io) {
    num_types = GetEventTypeCount(kEventWakeUpSpec);
    events = kEventWakeUpSpec;
  }

  EventTargetRef target = GetEventDispatcherTarget();
  EventTimeout timeout =
      (kForever == cms) ? kEventDurationForever : cms / 1000.0;
  EventTimeout end_time = GetCurrentEventTime() + timeout;

  bool done = false;
  while (!done) {
    EventRef event;
    OSStatus result = ReceiveNextEvent(num_types, events, timeout, true,
                                       &event);
    if (noErr == result) {
      if (wake_up_ != event) {
        LOG_F(LS_VERBOSE) << "Dispatching event: " << DecodeEvent(event);
        result = SendEventToEventTarget(event, target);
        if ((noErr != result) && (eventNotHandledErr != result)) {
          LOG_E(LS_ERROR, OS, result) << "SendEventToEventTarget";
        }
      } else {
        done = true;
      }
      ReleaseEvent(event);
    } else if (eventLoopTimedOutErr == result) {
      ASSERT(cms != kForever);
      done = true;
    } else if (eventLoopQuitErr == result) {
      // Ignore this... we get spurious quits for a variety of reasons.
      LOG_E(LS_VERBOSE, OS, result) << "ReceiveNextEvent";
    } else {
      // Some strange error occurred. Log it.
      LOG_E(LS_WARNING, OS, result) << "ReceiveNextEvent";
      return false;
    }
    if (kForever != cms) {
      timeout = end_time - GetCurrentEventTime();
    }
  }
  return true;
}

void MacCarbonSocketServer::WakeUp() {
  if (!IsEventInQueue(event_queue_, wake_up_)) {
    RetainEvent(wake_up_);
    OSStatus result = PostEventToQueue(event_queue_, wake_up_,
                                       kEventPriorityStandard);
    if (noErr != result) {
      LOG_E(LS_ERROR, OS, result) << "PostEventToQueue";
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// MacCarbonAppSocketServer
///////////////////////////////////////////////////////////////////////////////

MacCarbonAppSocketServer::MacCarbonAppSocketServer()
    : event_queue_(GetCurrentEventQueue()) {
  // Install event handler
  VERIFY(noErr == InstallApplicationEventHandler(
      NewEventHandlerUPP(WakeUpEventHandler), 1, kEventWakeUpSpec, this,
      &event_handler_));

  // Install a timer and set it idle to begin with.
  VERIFY(noErr == InstallEventLoopTimer(GetMainEventLoop(),
                                        kEventDurationForever,
                                        kEventDurationForever,
                                        NewEventLoopTimerUPP(TimerHandler),
                                        this,
                                        &timer_));
}

MacCarbonAppSocketServer::~MacCarbonAppSocketServer() {
  RemoveEventLoopTimer(timer_);
  RemoveEventHandler(event_handler_);
}

OSStatus MacCarbonAppSocketServer::WakeUpEventHandler(
    EventHandlerCallRef next, EventRef event, void *data) {
  QuitApplicationEventLoop();
  return noErr;
}

void MacCarbonAppSocketServer::TimerHandler(
    EventLoopTimerRef timer, void *data) {
  QuitApplicationEventLoop();
}

bool MacCarbonAppSocketServer::Wait(int cms, bool process_io) {
  if (!process_io && cms == 0) {
    // No op.
    return true;
  }
  if (kForever != cms) {
    // Start a timer.
    OSStatus error =
        SetEventLoopTimerNextFireTime(timer_, cms / 1000.0);
    if (error != noErr) {
      LOG(LS_ERROR) << "Failed setting next fire time.";
    }
  }
  if (!process_io) {
    // No way to listen to common modes and not get socket events, unless
    // we disable each one's callbacks.
    EnableSocketCallbacks(false);
  }
  RunApplicationEventLoop();
  if (!process_io) {
    // Reenable them.  Hopefully this won't cause spurious callbacks or
    // missing ones while they were disabled.
    EnableSocketCallbacks(true);
  }
  return true;
}

void MacCarbonAppSocketServer::WakeUp() {
  // TODO: No-op if there's already a WakeUp in flight.
  EventRef wake_up;
  VERIFY(noErr == CreateEvent(NULL, kEventClassSocketServer, kEventWakeUp, 0,
                              kEventAttributeUserEvent, &wake_up));
  OSStatus result = PostEventToQueue(event_queue_, wake_up,
                                       kEventPriorityStandard);
  if (noErr != result) {
    LOG_E(LS_ERROR, OS, result) << "PostEventToQueue";
  }
  ReleaseEvent(wake_up);
}

///////////////////////////////////////////////////////////////////////////////
// MacNotificationsSocketServer
///////////////////////////////////////////////////////////////////////////////

static const CFStringRef kNotificationName =
  CFSTR("MacNotificationsSocketServer");

MacNotificationsSocketServer::MacNotificationsSocketServer()
    : sent_notification_(false) {
  CFNotificationCenterRef nc = CFNotificationCenterGetLocalCenter();

  // Passing NULL for the observed object
  CFNotificationCenterAddObserver(
      nc, this, NotificationCallBack, kNotificationName, NULL,
      CFNotificationSuspensionBehaviorDeliverImmediately);
}

MacNotificationsSocketServer::~MacNotificationsSocketServer() {
  CFNotificationCenterRef nc = CFNotificationCenterGetLocalCenter();
  CFNotificationCenterRemoveObserver(nc, this, kNotificationName, NULL);
}

bool MacNotificationsSocketServer::Wait(int cms, bool process_io) {
  return cms == 0;
}

void MacNotificationsSocketServer::WakeUp() {
  // We could be invoked recursively, so this stops the infinite loop
  if (!sent_notification_) {
    sent_notification_ = true;
    CFNotificationCenterRef nc = CFNotificationCenterGetLocalCenter();
    CFNotificationCenterPostNotification(nc, kNotificationName, this, NULL,
                                         true);
    sent_notification_ = false;
  }
}

void MacNotificationsSocketServer::NotificationCallBack(
    CFNotificationCenterRef center, void* observer, CFStringRef name,
    const void* object, CFDictionaryRef userInfo) {

  ASSERT(CFStringCompare(name, kNotificationName, 0) == kCFCompareEqualTo);
  ASSERT(userInfo == NULL);

  // We have thread messages to process.
  Thread* thread = Thread::Current();
  if (thread == NULL) {
    // We're shutting down
    return;
  }

  Message msg;
  while (thread->Get(&msg, 0)) {
    thread->Dispatch(&msg);
  }
}

///////////////////////////////////////////////////////////////////////////////

} // namespace talk_base
