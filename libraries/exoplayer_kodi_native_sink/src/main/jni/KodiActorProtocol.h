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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTOR_PROTOCOL_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTOR_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include "KodiEvent.h"

namespace androidx_media3 {
namespace actor {

class CPayloadWrapBase {
 public:
  virtual ~CPayloadWrapBase() = default;
};

template <typename Payload>
class CPayloadWrap : public CPayloadWrapBase {
 public:
  explicit CPayloadWrap(Payload* data) : payload_(data) {}
  explicit CPayloadWrap(const Payload& data) : payload_(std::make_unique<Payload>(data)) {}

  Payload* GetPayload() { return payload_.get(); }

 private:
  std::unique_ptr<Payload> payload_;
};

class Protocol;

class Message {
  friend class Protocol;

  static constexpr size_t kInternalBufferSize = 32;

 public:
  int signal = 0;
  bool is_sync = false;
  bool is_sync_fini = false;
  bool is_out = false;
  bool is_sync_timeout = false;
  size_t payload_size = 0;
  uint8_t buffer[kInternalBufferSize];
  uint8_t* data = nullptr;
  std::unique_ptr<CPayloadWrapBase> payload_obj;
  Message* reply_message = nullptr;
  Protocol& origin;
  std::unique_ptr<CEvent> event;

  void Release();
  bool Reply(int sig, void* data = nullptr, size_t size = 0);

 private:
  explicit Message(Protocol& origin) : origin(origin) {}
};

class Protocol {
 public:
  Protocol(std::string name, CEvent* in_event, CEvent* out_event)
      : port_name(std::move(name)), container_in_event(in_event), container_out_event(out_event) {}
  explicit Protocol(std::string name) : Protocol(std::move(name), nullptr, nullptr) {}
  ~Protocol();

  Message* GetMessage();
  void ReturnMessage(Message* msg);

  bool SendOutMessage(int signal, const void* data = nullptr, size_t size = 0, Message* out_msg = nullptr);
  bool SendOutMessage(int signal, CPayloadWrapBase* payload, Message* out_msg = nullptr);
  bool SendInMessage(int signal, const void* data = nullptr, size_t size = 0, Message* out_msg = nullptr);
  bool SendInMessage(int signal, CPayloadWrapBase* payload, Message* out_msg = nullptr);
  bool SendOutMessageSync(int signal,
                          Message** ret_msg,
                          std::chrono::milliseconds timeout,
                          const void* data = nullptr,
                          size_t size = 0);
  bool SendOutMessageSync(int signal,
                          Message** ret_msg,
                          std::chrono::milliseconds timeout,
                          CPayloadWrapBase* payload);
  bool ReceiveOutMessage(Message** msg);
  bool ReceiveInMessage(Message** msg);
  void Purge();
  void PurgeIn(int signal);
  void PurgeOut(int signal);
  void DeferIn(bool value) { in_deferred = value; }
  void DeferOut(bool value) { out_deferred = value; }
  void Lock() { mutex.lock(); }
  void Unlock() { mutex.unlock(); }

  std::string port_name;

 private:
  CEvent* container_in_event;
  CEvent* container_out_event;
  std::mutex mutex;
  std::queue<Message*> out_messages;
  std::queue<Message*> in_messages;
  std::queue<Message*> free_messages;
  bool in_deferred = false;
  bool out_deferred = false;
};

}  // namespace actor
}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTOR_PROTOCOL_H_
