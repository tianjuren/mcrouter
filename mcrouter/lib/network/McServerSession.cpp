/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "McServerSession.h"

#include <memory>

#include <folly/Memory.h>
#include <folly/small_vector.h>

#include "mcrouter/lib/debug/Fifo.h"
#include "mcrouter/lib/network/McServerRequestContext.h"
#include "mcrouter/lib/network/MultiOpParent.h"
#include "mcrouter/lib/network/WriteBuffer.h"

namespace facebook { namespace memcache {

namespace {

constexpr size_t kIovecVectorSize = 64;

/**
 * @return true  If this incoming request is a part of a multiget request.
 */
bool isPartOfMultiget(mc_protocol_t protocol, mc_op_t operation) {
  if (protocol != mc_ascii_protocol) {
    return false;
  }

  if (operation == mc_op_get ||
      operation == mc_op_gets ||
      operation == mc_op_lease_get ||
      operation == mc_op_metaget) {
    return true;
  }

  return false;
}

}  // namespace

McServerSession& McServerSession::create(
  folly::AsyncTransportWrapper::UniquePtr transport,
  std::shared_ptr<McServerOnRequest> cb,
  std::function<void(McServerSession&)> onWriteQuiescence,
  std::function<void(McServerSession&)> onCloseStart,
  std::function<void(McServerSession&)> onCloseFinish,
  std::function<void()> onShutdown,
  AsyncMcServerWorkerOptions options,
  void* userCtxt,
  std::shared_ptr<Fifo> debugFifo) {

  auto ptr = new McServerSession(
    std::move(transport),
    std::move(cb),
    std::move(onWriteQuiescence),
    std::move(onCloseStart),
    std::move(onCloseFinish),
    std::move(onShutdown),
    std::move(options),
    userCtxt,
    std::move(debugFifo)
  );

  assert(ptr->state_ == STREAMING);

  DestructorGuard dg(ptr);
  ptr->transport_->setReadCB(ptr);
  if (ptr->state_ != STREAMING) {
    throw std::runtime_error(
        "Failed to create McServerSession: setReadCB failed");
  }

  return *ptr;
}

McServerSession::McServerSession(
  folly::AsyncTransportWrapper::UniquePtr transport,
  std::shared_ptr<McServerOnRequest> cb,
  std::function<void(McServerSession&)> onWriteQuiescence,
  std::function<void(McServerSession&)> onCloseStart,
  std::function<void(McServerSession&)> onCloseFinish,
  std::function<void()> onShutdown,
  AsyncMcServerWorkerOptions options,
  void* userCtxt,
  std::shared_ptr<Fifo> debugFifo)
    : transport_(std::move(transport)),
      onRequest_(std::move(cb)),
      onWriteQuiescence_(std::move(onWriteQuiescence)),
      onCloseStart_(std::move(onCloseStart)),
      onCloseFinish_(std::move(onCloseFinish)),
      onShutdown_(std::move(onShutdown)),
      options_(std::move(options)),
      userCtxt_(userCtxt),
      debugFifo_(std::move(debugFifo)),
      parser_(*this,
              options_.requestsPerRead,
              options_.minBufferSize,
              options_.maxBufferSize),
      sendWritesCallback_(*this) {

  try {
    transport_->getPeerAddress(&socketAddress_);
  } catch (const std::runtime_error& e) {
    // std::system_error or other exception, leave IP address empty
    LOG(WARNING) << "Failed to get socket address: " << e.what();
  }

  auto socket = transport_->getUnderlyingTransport<folly::AsyncSSLSocket>();
  if (socket != nullptr) {
    socket->sslAccept(this, /* timeout = */ 0);
  }
}

void McServerSession::pause(PauseReason reason) {
  pauseState_ |= static_cast<uint64_t>(reason);

  transport_->setReadCB(nullptr);
}

void McServerSession::resume(PauseReason reason) {
  pauseState_ &= ~static_cast<uint64_t>(reason);

  /* Client can half close the socket and in those cases there is
     no point in enabling reads */
  if (!pauseState_ &&
      state_ == STREAMING &&
      transport_->good()) {
    transport_->setReadCB(this);
  }
}

void McServerSession::onTransactionStarted(bool isSubRequest) {
  DestructorGuard dg(this);

  ++inFlight_;
  if (!isSubRequest) {
    ++realRequestsInFlight_;
  }

  if (options_.maxInFlight > 0 &&
      realRequestsInFlight_ >= options_.maxInFlight) {
    pause(PAUSE_THROTTLED);
  }
}

void McServerSession::checkClosed() {
  if (!inFlight_) {
    assert(pendingWrites_.empty());

    if (state_ == CLOSING) {
      /* It's possible to call close() more than once from the same stack.
         Prevent second close() from doing anything */
      state_ = CLOSED;
      if (transport_) {
        /* prevent readEOF() from being called */
        transport_->setReadCB(nullptr);
        transport_.reset();
      }
      if (onCloseFinish_) {
        onCloseFinish_(*this);
      }
      destroy();
    }
  }
}

void McServerSession::onTransactionCompleted(bool isSubRequest) {
  DestructorGuard dg(this);

  assert(inFlight_ > 0);
  --inFlight_;
  if (!isSubRequest) {
    assert(realRequestsInFlight_ > 0);
    --realRequestsInFlight_;
  }

  if (options_.maxInFlight > 0 &&
      realRequestsInFlight_ < options_.maxInFlight) {
    resume(PAUSE_THROTTLED);
  }

  checkClosed();
}

void McServerSession::reply(std::unique_ptr<WriteBuffer> wb, uint64_t reqid) {
  DestructorGuard dg(this);

  if (parser_.outOfOrder()) {
    queueWrite(std::move(wb));
  } else {
    if (reqid == headReqid_) {
      /* head of line reply, write it and all contiguous blocked replies */
      queueWrite(std::move(wb));
      auto it = blockedReplies_.find(++headReqid_);
      while (it != blockedReplies_.end()) {
        queueWrite(std::move(it->second));
        blockedReplies_.erase(it);
        it = blockedReplies_.find(++headReqid_);
      }
    } else {
      /* can't write this reply now, save for later */
      blockedReplies_.emplace(reqid, std::move(wb));
    }
  }
}

void McServerSession::processMultiOpEnd() {
  currentMultiop_->recordEnd(tailReqid_++);
  currentMultiop_.reset();
}

void McServerSession::close() {
  DestructorGuard dg(this);

  if (currentMultiop_) {
    /* If we got closed in the middle of a multiop request,
       process it as if we saw mc_op_end */
    processMultiOpEnd();
  }

  if (state_ == STREAMING) {
    state_ = CLOSING;
    if (onCloseStart_) {
      onCloseStart_(*this);
    }
  }

  checkClosed();
}

void McServerSession::getReadBuffer(void** bufReturn, size_t* lenReturn) {
  curBuffer_ = parser_.getReadBuffer();
  *bufReturn = curBuffer_.first;
  *lenReturn = curBuffer_.second;
}

void McServerSession::readDataAvailable(size_t len) noexcept {
  DestructorGuard dg(this);

  if (debugFifo_) {
    debugFifo_->writeIfConnected(transport_.get(), MessageDirection::Received,
                                 curBuffer_.first, len);
  }

  if (!parser_.readDataAvailable(len)) {
    close();
  }
}

void McServerSession::readEOF() noexcept {
  DestructorGuard dg(this);

  close();
}

void McServerSession::readErr(const folly::AsyncSocketException& ex) noexcept {
  DestructorGuard dg(this);

  close();
}

void McServerSession::multiOpEnd() {
  DestructorGuard dg(this);

  if (state_ != STREAMING) {
    return;
  }

  processMultiOpEnd();
}

void McServerSession::requestReady(McRequest&& req,
                                   mc_op_t operation,
                                   uint64_t reqid,
                                   mc_res_t result,
                                   bool noreply) {
  DestructorGuard dg(this);

  if (state_ != STREAMING) {
    return;
  }

  if (!parser_.outOfOrder()) {
    if (isPartOfMultiget(parser_.protocol(), operation) &&
        !currentMultiop_) {
      currentMultiop_ = std::make_shared<MultiOpParent>(*this, tailReqid_++);
    }

    reqid = tailReqid_++;
  }

  McServerRequestContext ctx(*this, operation, reqid, noreply, currentMultiop_);

  if (parser_.protocol() == mc_ascii_protocol) {
    ctx.asciiKey().emplace();
    req.key().cloneOneInto(ctx.asciiKey().value());
  }

  if (result == mc_res_bad_key) {
    McServerRequestContext::reply(std::move(ctx), McReply(mc_res_bad_key));
  } else if (ctx.operation_ == mc_op_version &&
             options_.defaultVersionHandler) {
    /* Handle version command only if the user doesn't want to handle it
     * themselves. */
    McServerRequestContext::reply(std::move(ctx),
                                  McReply(mc_res_ok, options_.versionString));
  } else if (ctx.operation_ == mc_op_quit) {
    /* mc_op_quit transaction will have `noreply` set, so this call
       is solely to make sure the transaction is completed and cleaned up */
    McServerRequestContext::reply(std::move(ctx), McReply(mc_res_ok));
    close();
  } else if (ctx.operation_ == mc_op_shutdown) {
    McServerRequestContext::reply(std::move(ctx), McReply(mc_res_ok));
    onShutdown_();
  } else {
    onRequest_->requestReady(std::move(ctx), std::move(req), ctx.operation_);
  }
}

void McServerSession::typedRequestReady(uint32_t typeId,
                                        const folly::IOBuf& reqBody,
                                        uint64_t reqid) {
  DestructorGuard dg(this);

  if (state_ != STREAMING) {
    return;
  }

  assert(parser_.outOfOrder());

  McServerRequestContext ctx(*this, mc_op_unknown, reqid);
  onRequest_->typedRequestReady(typeId, reqBody, std::move(ctx));
}

void McServerSession::parseError(mc_res_t result, folly::StringPiece reason) {
  DestructorGuard dg(this);

  if (state_ != STREAMING) {
    return;
  }

  McServerRequestContext::reply(
    McServerRequestContext(*this, mc_op_unknown, tailReqid_++),
    McReply(result, reason));
  close();
}

bool McServerSession::ensureWriteBufs() {
  if (writeBufs_ == nullptr) {
    try {
      writeBufs_ = folly::make_unique<WriteBufferQueue>(parser_.protocol());
    } catch (const std::runtime_error& e) {
      LOG(ERROR) << "Invalid protocol detected";
      transport_->close();
      return false;
    }
  }
  return true;
}

void McServerSession::queueWrite(std::unique_ptr<WriteBuffer> wb) {
  DestructorGuard dg(this);

  if (wb == nullptr) {
    return;
  }
  if (options_.singleWrite) {
    struct iovec* iovs = wb->getIovsBegin();
    size_t iovCount = wb->getIovsCount();
    writeBufs_->push(std::move(wb));
    transport_->writev(this, iovs, iovCount);
    if (debugFifo_) {
      debugFifo_->writeIfConnected(transport_.get(), MessageDirection::Sent,
                                   iovs, iovCount);
    }
    if (!writeBufs_->empty()) {
      /* We only need to pause if the sendmsg() call didn't write everything
         in one go */
      pause(PAUSE_WRITE);
    }
  } else {
    pendingWrites_.emplace_back(std::move(wb));

    if (!writeScheduled_) {
      auto eventBase = transport_->getEventBase();
      CHECK(eventBase != nullptr);
      eventBase->runInLoop(&sendWritesCallback_, /* thisIteration= */ true);
      writeScheduled_ = true;
    }
  }
}

void McServerSession::SendWritesCallback::runLoopCallback() noexcept {
  session_.sendWrites();
}

void McServerSession::sendWrites() {
  DestructorGuard dg(this);

  writeScheduled_ = false;

  folly::small_vector<struct iovec, kIovecVectorSize> iovs;
  size_t count = 0;
  while (!pendingWrites_.empty()) {
    auto wb = std::move(pendingWrites_.front());
    pendingWrites_.pop_front();
    ++count;
    if (!wb->noReply()) {
      iovs.insert(iovs.end(),
                  wb->getIovsBegin(),
                  wb->getIovsBegin() + wb->getIovsCount());
    }
    writeBufs_->push(std::move(wb));
  }
  writeBatches_.push_back(count);

  if (debugFifo_) {
    debugFifo_->writeIfConnected(transport_.get(), MessageDirection::Sent,
                                 iovs.data(), iovs.size());
  }
  transport_->writev(this, iovs.data(), iovs.size());
}

void McServerSession::completeWrite() {
  size_t count;
  if (options_.singleWrite) {
    count = 1;
  } else {
    assert(!writeBatches_.empty());
    count = writeBatches_.front();
    writeBatches_.pop_front();
  }

  while (count-- > 0) {
    assert(!writeBufs_->empty());
    writeBufs_->pop();
  }
}

void McServerSession::writeSuccess() noexcept {
  DestructorGuard dg(this);
  completeWrite();

  assert(writeBufs_ != nullptr);
  if (writeBufs_->empty() && state_ == STREAMING) {
    if (onWriteQuiescence_) {
      onWriteQuiescence_(*this);
    }
    /* No-op if not paused */
    resume(PAUSE_WRITE);
  }
}

void McServerSession::writeErr(
  size_t bytesWritten,
  const folly::AsyncSocketException& ex) noexcept {

  DestructorGuard dg(this);
  completeWrite();
  close();
}

bool McServerSession::handshakeVer(folly::AsyncSSLSocket*,
                                   bool preverifyOk,
                                   X509_STORE_CTX* ctx) noexcept {
  if (!preverifyOk) {
    return false;
  }
  // XXX I'm assuming that this will be the case as a result of
  // preverifyOk being true
  DCHECK(X509_STORE_CTX_get_error(ctx) == X509_V_OK);

  // So the interesting thing is that this always returns the depth of
  // the cert it's asking you to verify, and the error_ assumes to be
  // just a poorly named function.
  auto certDepth = X509_STORE_CTX_get_error_depth(ctx);

  // Depth is numbered from the peer cert going up.  For anything in the
  // chain, let's just leave it to openssl to figure out it's validity.
  // We may want to limit the chain depth later though.
  if (certDepth != 0) {
    return preverifyOk;
  }

  auto cert = X509_STORE_CTX_get_current_cert(ctx);
  sockaddr_storage addrStorage;
  socklen_t addrLen = 0;
  if (!folly::ssl::OpenSSLUtils::getPeerAddressFromX509StoreCtx(
          ctx, &addrStorage, &addrLen)) {
    return false;
  }
  return folly::ssl::OpenSSLUtils::validatePeerCertNames(
      cert, reinterpret_cast<sockaddr*>(&addrStorage), addrLen);
}

void McServerSession::handshakeSuc(folly::AsyncSSLSocket* sock) noexcept {
  auto cert = sock->getPeerCert();
  if (cert == nullptr) {
    return;
  }
  auto sub = X509_get_subject_name(cert.get());
  if (sub != nullptr) {
    char cn[ub_common_name + 1];
    const auto res =
        X509_NAME_get_text_by_NID(sub, NID_commonName, cn, ub_common_name);
    if (res > 0) {
      clientCommonName_.assign(std::string(cn, res));
    }
  }
}

void McServerSession::handshakeErr(
    folly::AsyncSSLSocket*, const folly::AsyncSocketException&) noexcept {}
}
} // facebook::memcache
