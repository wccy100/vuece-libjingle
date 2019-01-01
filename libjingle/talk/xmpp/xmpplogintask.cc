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

#include <iostream>
#include <string>
#include <vector>
#include "talk/base/base64.h"
#include "talk/base/common.h"
#include "talk/xmllite/xmlelement.h"
#include "talk/xmpp/constants.h"
#include "talk/xmpp/jid.h"
#include "talk/xmpp/saslmechanism.h"
#include "talk/xmpp/xmppengineimpl.h"
#include "talk/xmpp/xmpplogintask.h"

using talk_base::ConstantLabel;

namespace buzz {

#ifdef _DEBUG
const ConstantLabel XmppLoginTask::LOGINTASK_STATES[] = {
  KLABEL(LOGINSTATE_INIT),
  KLABEL(LOGINSTATE_STREAMSTART_SENT),
  KLABEL(LOGINSTATE_STARTED_XMPP),
  KLABEL(LOGINSTATE_TLS_INIT),
  KLABEL(LOGINSTATE_AUTH_INIT),
  KLABEL(LOGINSTATE_BIND_INIT),
  KLABEL(LOGINSTATE_TLS_REQUESTED),
  KLABEL(LOGINSTATE_SASL_RUNNING),
  KLABEL(LOGINSTATE_BIND_REQUESTED),
  KLABEL(LOGINSTATE_SESSION_REQUESTED),
  KLABEL(LOGINSTATE_DONE),
  LASTLABEL
};
#endif  // _DEBUG

XmppLoginTask::XmppLoginTask(XmppEngineImpl * pctx) :
  pctx_(pctx),
  authNeeded_(true),
  state_(LOGINSTATE_INIT),
  pelStanza_(NULL),
  isStart_(false),
  iqId_(STR_EMPTY),
  pelFeatures_(NULL),
  fullJid_(STR_EMPTY),
  streamId_(STR_EMPTY),
  pvecQueuedStanzas_(new std::vector<XmlElement *>()),
  sasl_mech_(NULL) {
}

XmppLoginTask::~XmppLoginTask() {
  for (size_t i = 0; i < pvecQueuedStanzas_->size(); i += 1)
    delete (*pvecQueuedStanzas_)[i];
}

void
XmppLoginTask::IncomingStanza(const XmlElement *element, bool isStart) {
  pelStanza_ = element;
  isStart_ = isStart;
  Advance();
  pelStanza_ = NULL;
  isStart_ = false;
}

const XmlElement *
XmppLoginTask::NextStanza() {
  const XmlElement * result = pelStanza_;
  pelStanza_ = NULL;
  return result;
}

bool
XmppLoginTask::Advance() {

  for (;;) {

    const XmlElement * element = NULL;

#if _DEBUG
    LOG(LS_VERBOSE) << "XmppLoginTask::Advance - "
      << talk_base::ErrorName(state_, LOGINTASK_STATES);
#endif  // _DEBUG

    switch (state_) {

      case LOGINSTATE_INIT: {
        pctx_->RaiseReset();
        pelFeatures_.reset(NULL);

        // The proper domain to verify against is the real underlying
        // domain - i.e., the domain that owns the JID.  Our XmppEngineImpl
        // also allows matching against a proxy domain instead, if it is told
        // to do so - see the implementation of XmppEngineImpl::StartTls and
        // XmppEngine::SetTlsServerDomain to see how you can use that feature
        pctx_->InternalSendStart(pctx_->user_jid_.domain());
        state_ = LOGINSTATE_STREAMSTART_SENT;
        break;
      }

      case LOGINSTATE_STREAMSTART_SENT: {
        if (NULL == (element = NextStanza()))
          return true;

        if (!isStart_ )
        {
        	LOG(LS_ERROR) << "XmppLoginTask:isStart_ is false!";
        	return Failure(XmppEngine::ERROR_VERSION);
        }

        if(!HandleStartStream(element))
        {
        	LOG(LS_ERROR) << "XmppLoginTask:HandleStartStream returned false false!";
        	return Failure(XmppEngine::ERROR_VERSION);
        }


        state_ = LOGINSTATE_STARTED_XMPP;
        return true;
      }

      case LOGINSTATE_STARTED_XMPP: {
        if (NULL == (element = NextStanza()))
          return true;

        if (!HandleFeatures(element))
          return Failure(XmppEngine::ERROR_VERSION);

        // Use TLS if forced, or if available
        if (pctx_->tls_needed_ || GetFeature(QN_TLS_STARTTLS) != NULL) {
          state_ = LOGINSTATE_TLS_INIT;
          continue;
        }

        if (authNeeded_) {
          state_ = LOGINSTATE_AUTH_INIT;
          continue;
        }

        state_ = LOGINSTATE_BIND_INIT;
        continue;
      }

      case LOGINSTATE_TLS_INIT: {
        const XmlElement * pelTls = GetFeature(QN_TLS_STARTTLS);
        if (!pelTls)
          return Failure(XmppEngine::ERROR_TLS);

        XmlElement el(QN_TLS_STARTTLS, true);
        pctx_->InternalSendStanza(&el);
        state_ = LOGINSTATE_TLS_REQUESTED;
        continue;
      }

      case LOGINSTATE_TLS_REQUESTED: {
        if (NULL == (element = NextStanza()))
          return true;
        if (element->Name() != QN_TLS_PROCEED)
          return Failure(XmppEngine::ERROR_TLS);

        // The proper domain to verify against is the real underlying
        // domain - i.e., the domain that owns the JID.  Our XmppEngineImpl
        // also allows matching against a proxy domain instead, if it is told
        // to do so - see the implementation of XmppEngineImpl::StartTls and
        // XmppEngine::SetTlsServerDomain to see how you can use that feature
        pctx_->StartTls(pctx_->user_jid_.domain());
        pctx_->tls_needed_ = false;
        state_ = LOGINSTATE_INIT;
        continue;
      }

      case LOGINSTATE_AUTH_INIT: {
        const XmlElement * pelSaslAuth = GetFeature(QN_SASL_MECHANISMS);
        if (!pelSaslAuth) {
          return Failure(XmppEngine::ERROR_AUTH);
        }

        // Collect together the SASL auth mechanisms presented by the server
        std::vector<std::string> mechanisms;
        for (const XmlElement * pelMech =
             pelSaslAuth->FirstNamed(QN_SASL_MECHANISM);
             pelMech;
             pelMech = pelMech->NextNamed(QN_SASL_MECHANISM)) {

          mechanisms.push_back(pelMech->BodyText());
        }

        // Given all the mechanisms, choose the best
        std::string choice(pctx_->ChooseBestSaslMechanism(mechanisms, pctx_->IsEncrypted()));
        if (choice.empty()) {
          return Failure(XmppEngine::ERROR_AUTH);
        }

        // No recognized auth mechanism - that's an error
        sasl_mech_.reset(pctx_->GetSaslMechanism(choice));
        if (sasl_mech_.get() == NULL) {
          return Failure(XmppEngine::ERROR_AUTH);
        }

        // OK, let's start it.
        XmlElement * auth = sasl_mech_->StartSaslAuth();
        if (auth == NULL) {
          return Failure(XmppEngine::ERROR_AUTH);
        }

        pctx_->InternalSendStanza(auth);
        delete auth;
        state_ = LOGINSTATE_SASL_RUNNING;
        continue;
      }

      case LOGINSTATE_SASL_RUNNING: {
        if (NULL == (element = NextStanza()))
          return true;
        if (element->Name().Namespace() != NS_SASL)
          return Failure(XmppEngine::ERROR_AUTH);
        if (element->Name() == QN_SASL_CHALLENGE) {
          XmlElement * response = sasl_mech_->HandleSaslChallenge(element);
          if (response == NULL) {
            return Failure(XmppEngine::ERROR_AUTH);
          }
          pctx_->InternalSendStanza(response);
          delete response;
          state_ = LOGINSTATE_SASL_RUNNING;
          continue;
        }
        if (element->Name() != QN_SASL_SUCCESS) {
          return Failure(XmppEngine::ERROR_UNAUTHORIZED);
        }

        // Authenticated!
        authNeeded_ = false;
        state_ = LOGINSTATE_INIT;
        continue;
      }

      case LOGINSTATE_BIND_INIT: {
        const XmlElement * pelBindFeature = GetFeature(QN_BIND_BIND);
        const XmlElement * pelSessionFeature = GetFeature(QN_SESSION_SESSION);
        if (!pelBindFeature || !pelSessionFeature)
          return Failure(XmppEngine::ERROR_BIND);

        XmlElement iq(QN_IQ);
        iq.AddAttr(QN_TYPE, "set");

        iqId_ = pctx_->NextId();
        iq.AddAttr(QN_ID, iqId_);
        iq.AddElement(new XmlElement(QN_BIND_BIND, true));

        if (pctx_->requested_resource_ != STR_EMPTY) {
          iq.AddElement(new XmlElement(QN_BIND_RESOURCE), 1);
          iq.AddText(pctx_->requested_resource_, 2);
        }
        pctx_->InternalSendStanza(&iq);
        state_ = LOGINSTATE_BIND_REQUESTED;
        continue;
      }

      case LOGINSTATE_BIND_REQUESTED: {
        if (NULL == (element = NextStanza()))
          return true;

        if (element->Name() != QN_IQ || element->Attr(QN_ID) != iqId_ ||
            element->Attr(QN_TYPE) == "get" || element->Attr(QN_TYPE) == "set")
          return true;

        if (element->Attr(QN_TYPE) != "result" || element->FirstElement() == NULL ||
            element->FirstElement()->Name() != QN_BIND_BIND)
          return Failure(XmppEngine::ERROR_BIND);

        fullJid_ = Jid(element->FirstElement()->TextNamed(QN_BIND_JID));
        if (!fullJid_.IsFull()) {
          return Failure(XmppEngine::ERROR_BIND);
        }

        // now request session
        XmlElement iq(QN_IQ);
        iq.AddAttr(QN_TYPE, "set");

        iqId_ = pctx_->NextId();
        iq.AddAttr(QN_ID, iqId_);
        iq.AddElement(new XmlElement(QN_SESSION_SESSION, true));
        pctx_->InternalSendStanza(&iq);

        state_ = LOGINSTATE_SESSION_REQUESTED;
        continue;
      }

      case LOGINSTATE_SESSION_REQUESTED: {
        if (NULL == (element = NextStanza()))
          return true;
        if (element->Name() != QN_IQ || element->Attr(QN_ID) != iqId_ ||
            element->Attr(QN_TYPE) == "get" || element->Attr(QN_TYPE) == "set")
          return false;

        if (element->Attr(QN_TYPE) != "result")
          return Failure(XmppEngine::ERROR_BIND);

        pctx_->SignalBound(fullJid_);
        FlushQueuedStanzas();
        state_ = LOGINSTATE_DONE;
        return true;
      }

      case LOGINSTATE_DONE:
        return false;
    }
  }
}

bool
XmppLoginTask::HandleStartStream(const XmlElement *element) {

  if (element->Name() != QN_STREAM_STREAM)
  {
	  LOG(LS_ERROR) << "HandleStartStream:Wrong stream name!";
	    return false;
  }


  if (element->Attr(QN_XMLNS) != "jabber:client")
  {
	  LOG(LS_ERROR) << "HandleStartStream:Wrong name space!";
	    return false;
  }

  if (element->Attr(QN_VERSION) != "1.0")
  {
	  LOG(LS_ERROR) << "HandleStartStream:Wrong version number";
	    return false;
  }

  if (!element->HasAttr(QN_ID))
  {
	  LOG(LS_ERROR) << "HandleStartStream:No id!";
	    return false;
  }

  streamId_ = element->Attr(QN_ID);

  return true;
}

bool
XmppLoginTask::HandleFeatures(const XmlElement *element) {
  if (element->Name() != QN_STREAM_FEATURES)
    return false;

  pelFeatures_.reset(new XmlElement(*element));
  return true;
}

const XmlElement *
XmppLoginTask::GetFeature(const QName & name) {
  return pelFeatures_->FirstNamed(name);
}

bool
XmppLoginTask::Failure(XmppEngine::Error reason) {
  state_ = LOGINSTATE_DONE;
  pctx_->SignalError(reason, 0);
  return false;
}

void
XmppLoginTask::OutgoingStanza(const XmlElement * element) {
  XmlElement * pelCopy = new XmlElement(*element);
  pvecQueuedStanzas_->push_back(pelCopy);
}

void
XmppLoginTask::FlushQueuedStanzas() {
  for (size_t i = 0; i < pvecQueuedStanzas_->size(); i += 1) {
    pctx_->InternalSendStanza((*pvecQueuedStanzas_)[i]);
    delete (*pvecQueuedStanzas_)[i];
  }
  pvecQueuedStanzas_->clear();
}

}
