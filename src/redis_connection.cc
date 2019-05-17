#include "redis_connection.h"

#include <glog/logging.h>

namespace Redis {

Connection::Connection(bufferevent *bev, Worker *owner)
    : bev_(bev), req_(owner->svr_), owner_(owner) {
  time_t now;
  time(&now);
  create_time_ = now;
  last_interaction_ = now;
}

Connection::~Connection() {
  if (bev_) { bufferevent_free(bev_); }
  // unscribe all channels and patterns if exists
  UnSubscribeAll();
  PUnSubscribeAll();
}

void Connection::OnRead(struct bufferevent *bev, void *ctx) {
  DLOG(INFO) << "[connection] on read: " << bufferevent_getfd(bev);
  auto conn = static_cast<Connection *>(ctx);

  conn->SetLastInteraction();
  conn->req_.Tokenize(conn->Input());
  conn->req_.ExecuteCommands(conn);
}

void Connection::OnWrite(struct bufferevent *bev, void *ctx) {
  auto conn = static_cast<Connection *>(ctx);
  if (conn->IsFlagEnabled(kCloseAfterReply)) {
    conn->owner_->RemoveConnection(conn->GetFD());
  }
}

void Connection::OnEvent(bufferevent *bev, int16_t events, void *ctx) {
  auto conn = static_cast<Connection *>(ctx);
  if (events & BEV_EVENT_ERROR) {
    LOG(ERROR) << "[connection] Going to remove the client: " << conn->GetAddr()
               << ", while encounter error: "
               << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
    conn->owner_->RemoveConnection(conn->GetFD());
    return;
  }
  if (events & BEV_EVENT_EOF) {
    DLOG(INFO) << "[connection] Going to remove the client: " << conn->GetAddr()
               << ", while closed by client";
    conn->owner_->RemoveConnection(conn->GetFD());
    return;
  }
  if (events & BEV_EVENT_TIMEOUT) {
    DLOG(INFO) << "[connection] The client: " << conn->GetAddr()  << "] reached timeout";
    bufferevent_enable(bev, EV_READ | EV_WRITE);
  }
}

void Connection::Reply(const std::string &msg) {
  owner_->svr_->stats_.IncrOutbondBytes(msg.size());
  Redis::Reply(bufferevent_get_output(bev_), msg);
}

void Connection::SendFile(int fd) {
  // NOTE: we don't need to close the fd, the libevent will do that
  auto output = bufferevent_get_output(bev_);
  evbuffer_add_file(output, fd, 0, -1);
}

uint64_t Connection::GetAge() {
  time_t now;
  time(&now);
  return static_cast<uint64_t>(now-create_time_);
}

void Connection::SetLastInteraction() {
  time(&last_interaction_);
}

uint64_t Connection::GetIdleTime() {
  time_t now;
  time(&now);
  return static_cast<uint64_t>(now-last_interaction_);
}

std::string Connection::GetFlags() {
  std::string flags;
  if (owner_->IsRepl()) flags.append("S");
  if (IsFlagEnabled(kCloseAfterReply)) flags.append("c");
  if (IsFlagEnabled(kMonitor)) flags.append("M");
  if (!subscribe_channels_.empty()) flags.append("P");
  if (flags.empty()) flags = "N";
  return flags;
}

void Connection::EnableFlag(Flag flag) {
  flags_ |= flag;
}

bool Connection::IsFlagEnabled(Flag flag) {
  return (flags_ & flag) > 0;
}

void Connection::SubscribeChannel(const std::string &channel) {
  for (const auto &chan : subscribe_channels_) {
    if (channel == chan) return;
  }
  subscribe_channels_.emplace_back(channel);
  owner_->svr_->SubscribeChannel(channel, this);
}

void Connection::UnSubscribeChannel(const std::string &channel) {
  auto iter = subscribe_channels_.begin();
  for (; iter != subscribe_channels_.end(); iter++) {
    if (*iter == channel) {
      subscribe_channels_.erase(iter);
      owner_->svr_->UnSubscribeChannel(channel, this);
      return;
    }
  }
}

void Connection::UnSubscribeAll() {
  if (subscribe_channels_.empty()) return;
  for (const auto &chan : subscribe_channels_) {
    owner_->svr_->UnSubscribeChannel(chan, this);
  }
  subscribe_channels_.clear();
}

int Connection::SubscriptionsCount() {
  return static_cast<int>(subscribe_channels_.size());
}

void Connection::PSubscribeChannel(const std::string &pattern) {
  for (const auto &p : subcribe_patterns_) {
    if (pattern == p) return;
  }
  subcribe_patterns_.emplace_back(pattern);
  owner_->svr_->PSubscribeChannel(pattern, this);
}

void Connection::PUnSubscribeChannel(const std::string &pattern) {
  auto iter = subscribe_channels_.begin();
  for (; iter != subscribe_channels_.end(); iter++) {
    if (*iter == pattern) {
      subscribe_channels_.erase(iter);
      owner_->svr_->PUnSubscribeChannel(pattern, this);
      return;
    }
  }
}

void Connection::PUnSubscribeAll() {
  if (subcribe_patterns_.empty()) return;
  for (const auto &pattern : subcribe_patterns_) {
    owner_->svr_->PUnSubscribeChannel(pattern, this);
  }
  subcribe_patterns_.clear();
}

int Connection::PSubscriptionsCount() {
  return static_cast<int>(subcribe_patterns_.size());
}
}  // namespace Redis