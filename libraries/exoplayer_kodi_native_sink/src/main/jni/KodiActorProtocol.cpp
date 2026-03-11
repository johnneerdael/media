/*
 * Copyright (C) 2026 Nuvio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "KodiActorProtocol.h"

#include <cstring>

namespace androidx_media3 {
namespace actor {

void Message::Release() {
  bool skip = false;
  origin.Lock();
  if (is_sync) {
    skip = !is_sync_fini;
  }
  is_sync_fini = true;
  origin.Unlock();

  if (skip) {
    return;
  }

  if (data != buffer) {
    delete[] data;
  }
  data = nullptr;
  payload_obj.reset();
  event.reset();
  origin.ReturnMessage(this);
}

bool Message::Reply(int sig, void* reply_data, size_t size) {
  if (!is_sync) {
    if (is_out) {
      return origin.SendInMessage(sig, reply_data, size);
    }
    return origin.SendOutMessage(sig, reply_data, size);
  }

  origin.Lock();
  if (!is_sync_timeout) {
    Message* msg = origin.GetMessage();
    msg->signal = sig;
    msg->is_out = !is_out;
    reply_message = msg;
    if (reply_data != nullptr && size > 0) {
      if (size > sizeof(msg->buffer)) {
        msg->data = new uint8_t[size];
      } else {
        msg->data = msg->buffer;
      }
      std::memcpy(msg->data, reply_data, size);
      msg->payload_size = size;
    }
  }
  origin.Unlock();

  if (event) {
    event->Set();
  }
  return true;
}

Protocol::~Protocol() {
  Message* msg = nullptr;
  Purge();
  while (!free_messages.empty()) {
    msg = free_messages.front();
    free_messages.pop();
    delete msg;
  }
}

Message* Protocol::GetMessage() {
  std::lock_guard<std::mutex> lock(mutex);

  Message* msg = nullptr;
  if (!free_messages.empty()) {
    msg = free_messages.front();
    free_messages.pop();
  } else {
    msg = new Message(*this);
  }

  msg->is_sync = false;
  msg->is_sync_fini = false;
  msg->is_sync_timeout = false;
  msg->event.reset();
  msg->data = nullptr;
  msg->payload_size = 0;
  msg->reply_message = nullptr;
  msg->payload_obj.reset();
  return msg;
}

void Protocol::ReturnMessage(Message* msg) {
  std::lock_guard<std::mutex> lock(mutex);
  free_messages.push(msg);
}

bool Protocol::SendOutMessage(int signal, const void* data, size_t size, Message* out_msg) {
  Message* msg = out_msg != nullptr ? out_msg : GetMessage();
  msg->signal = signal;
  msg->is_out = true;
  if (data != nullptr && size > 0) {
    if (size > sizeof(msg->buffer)) {
      msg->data = new uint8_t[size];
    } else {
      msg->data = msg->buffer;
    }
    std::memcpy(msg->data, data, size);
    msg->payload_size = size;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    out_messages.push(msg);
  }
  if (container_out_event != nullptr) {
    container_out_event->Set();
  }
  return true;
}

bool Protocol::SendOutMessage(int signal, CPayloadWrapBase* payload, Message* out_msg) {
  Message* msg = out_msg != nullptr ? out_msg : GetMessage();
  msg->signal = signal;
  msg->is_out = true;
  msg->payload_obj.reset(payload);
  {
    std::lock_guard<std::mutex> lock(mutex);
    out_messages.push(msg);
  }
  if (container_out_event != nullptr) {
    container_out_event->Set();
  }
  return true;
}

bool Protocol::SendInMessage(int signal, const void* data, size_t size, Message* out_msg) {
  Message* msg = out_msg != nullptr ? out_msg : GetMessage();
  msg->signal = signal;
  msg->is_out = false;
  if (data != nullptr && size > 0) {
    if (size > sizeof(msg->buffer)) {
      msg->data = new uint8_t[size];
    } else {
      msg->data = msg->buffer;
    }
    std::memcpy(msg->data, data, size);
    msg->payload_size = size;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    in_messages.push(msg);
  }
  if (container_in_event != nullptr) {
    container_in_event->Set();
  }
  return true;
}

bool Protocol::SendInMessage(int signal, CPayloadWrapBase* payload, Message* out_msg) {
  Message* msg = out_msg != nullptr ? out_msg : GetMessage();
  msg->signal = signal;
  msg->is_out = false;
  msg->payload_obj.reset(payload);
  {
    std::lock_guard<std::mutex> lock(mutex);
    in_messages.push(msg);
  }
  if (container_in_event != nullptr) {
    container_in_event->Set();
  }
  return true;
}

bool Protocol::SendOutMessageSync(int signal,
                                  Message** ret_msg,
                                  std::chrono::milliseconds timeout,
                                  const void* data,
                                  size_t size) {
  Message* msg = GetMessage();
  msg->is_out = true;
  msg->is_sync = true;
  msg->event = std::make_unique<CEvent>();
  msg->event->Reset();
  SendOutMessage(signal, data, size, msg);

  if (!msg->event->Wait(timeout)) {
    std::lock_guard<std::mutex> lock(mutex);
    if (msg->reply_message != nullptr) {
      *ret_msg = msg->reply_message;
    } else {
      *ret_msg = nullptr;
      msg->is_sync_timeout = true;
    }
  } else {
    *ret_msg = msg->reply_message;
  }

  msg->Release();
  return *ret_msg != nullptr;
}

bool Protocol::SendOutMessageSync(int signal,
                                  Message** ret_msg,
                                  std::chrono::milliseconds timeout,
                                  CPayloadWrapBase* payload) {
  Message* msg = GetMessage();
  msg->is_out = true;
  msg->is_sync = true;
  msg->event = std::make_unique<CEvent>();
  msg->event->Reset();
  SendOutMessage(signal, payload, msg);

  if (!msg->event->Wait(timeout)) {
    std::lock_guard<std::mutex> lock(mutex);
    if (msg->reply_message != nullptr) {
      *ret_msg = msg->reply_message;
    } else {
      *ret_msg = nullptr;
      msg->is_sync_timeout = true;
    }
  } else {
    *ret_msg = msg->reply_message;
  }

  msg->Release();
  return *ret_msg != nullptr;
}

bool Protocol::ReceiveOutMessage(Message** msg) {
  std::lock_guard<std::mutex> lock(mutex);
  if (out_messages.empty() || out_deferred) {
    return false;
  }
  *msg = out_messages.front();
  out_messages.pop();
  return true;
}

bool Protocol::ReceiveInMessage(Message** msg) {
  std::lock_guard<std::mutex> lock(mutex);
  if (in_messages.empty() || in_deferred) {
    return false;
  }
  *msg = in_messages.front();
  in_messages.pop();
  return true;
}

void Protocol::Purge() {
  Message* msg = nullptr;
  while (ReceiveInMessage(&msg)) {
    msg->Release();
  }
  while (ReceiveOutMessage(&msg)) {
    msg->Release();
  }
}

void Protocol::PurgeIn(int signal) {
  std::queue<Message*> kept;
  std::lock_guard<std::mutex> lock(mutex);
  while (!in_messages.empty()) {
    Message* msg = in_messages.front();
    in_messages.pop();
    if (msg->signal != signal) {
      kept.push(msg);
    }
  }
  while (!kept.empty()) {
    in_messages.push(kept.front());
    kept.pop();
  }
}

void Protocol::PurgeOut(int signal) {
  std::queue<Message*> kept;
  std::lock_guard<std::mutex> lock(mutex);
  while (!out_messages.empty()) {
    Message* msg = out_messages.front();
    out_messages.pop();
    if (msg->signal != signal) {
      kept.push(msg);
    }
  }
  while (!kept.empty()) {
    out_messages.push(kept.front());
    kept.pop();
  }
}

}  // namespace actor
}  // namespace androidx_media3
