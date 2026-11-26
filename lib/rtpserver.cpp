/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019-2023 David Moreno Montero <dmoreno@coralbits.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 */

#include <algorithm>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

#include <rtpmidid/exceptions.hpp>
#include <rtpmidid/iobytes.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/network.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtppeer.hpp>
#include <rtpmidid/rtpserver.hpp>

using namespace rtpmidid;

static uint32_t read_u32(const uint8_t *bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

// V6: both ports, MIDI on control + 1. A dynamic port ("0") tries a new pair
// when control + 1 is taken, as the client does; a fixed one is tried once.
bool rtpserver_t::bind(const std::string &port) {
  static constexpr int MAX_PAIR_RETRIES = 5;
  const bool dynamic = port == "0" || port.empty(); // an export asks for ""
  const int attempts = dynamic ? MAX_PAIR_RETRIES : 1;
  std::string error;
  for (int attempt = 0; attempt < attempts; attempt++) {
    control.open("::", port);
    if (!control.is_open()) {
      error = FMT::format("control port {}: {}", port,
                          strerror(control.get_errno()));
      continue;
    }
    const int base = control.get_address().port();
    midi.open("::", base + 1);
    if (midi.is_open())
      return true;
    error =
        FMT::format("MIDI port {}: {}", base + 1, strerror(midi.get_errno()));
    control.close();
  }
  ERROR("RTP MIDI server '{}' could not bind its ports ({}); it is not "
        "listening",
        name, error);
  return false;
}

// NOLINTNEXTLINE(bugprone-swappable-parameters)
rtpserver_t::rtpserver_t(std::string _name, const std::string &port)
    : name(std::move(_name)) {

  if (!bind(port))
    return;

  on_read_control = control.on_read.connect(
      [&](const packet_t &packet, const network_address_t &from) {
        io_bytes_reader iobytes(packet.get_data(), packet.get_size());
        data_ready(iobytes, from, rtppeer_t::CONTROL_PORT);
      });
  on_read_midi = midi.on_read.connect(
      [&](const packet_t &packet, const network_address_t &from) {
        io_bytes_reader iobytes(packet.get_data(), packet.get_size());
        data_ready(iobytes, from, rtppeer_t::MIDI_PORT);
      });

  INFO("Listening RTP MIDI connections at {} / {}, with name: '{}'",
       control.get_address().to_string(), midi.get_address().to_string(), name);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
rtpserver_t::~rtpserver_t() {
  DEBUG("~rtpserver_t({})", name);

  // Must clear here, to be able to use the control and midi
  // sockets
  // This calls indirectly all the disconnects, that will remove from the
  // list. So we create a shared_ptr copy and use that.
  std::vector<std::shared_ptr<rtppeer_t>> peers_copy;
  peers_copy.reserve(peers.size());
  for (auto &peerinfo : peers) {
    DEBUG("Peer still in the server list: {}", peerinfo.id);
    peers_copy.push_back(peerinfo.peer);
  }

  // We have proper copyes, so we can proceed with the send goodbyes.. and then
  // maybe delete the peers.
  for (auto &peer : peers_copy) {
    if (peer) {
      if (peer->status & rtppeer_t::CONTROL_CONNECTED)
        peer->send_goodbye(rtppeer_t::CONTROL_PORT);
      if (peer->status & rtppeer_t::MIDI_CONNECTED)
        peer->send_goodbye(rtppeer_t::MIDI_PORT);
    }
  }
}

// V1: a session is (source address, base port, SSRC); its control datagrams
// come from the base port, its MIDI ones from base + 1.
rtpserverpeer_t *rtpserver_t::find_session(const network_address_t &from,
                                           rtppeer_t::port_e port,
                                           uint32_t ssrc) {
  const int base = from.port() - (port == rtppeer_t::MIDI_PORT ? 1 : 0);
  for (auto &session : peers) {
    if (session.base_port == base && session.peer->remote_ssrc == ssrc &&
        session.address.same_host(from))
      return &session;
  }
  return nullptr;
}

void rtpserver_t::data_ready(const io_bytes_reader &data,
                             const network_address_t &addr,
                             rtppeer_t::port_e port) {
  io_bytes_reader buffer(data);
  const uint8_t *bytes = buffer.start;
  const size_t size = buffer.size();

  uint32_t ssrc = 0;
  if (rtppeer_t::is_command(buffer)) {
    const uint16_t command = (uint16_t(bytes[2]) << 8) | bytes[3];
    switch (command) {
    case rtppeer_t::IN:
      invitation(buffer, addr, port);
      return;
    case rtppeer_t::OK:
    case rtppeer_t::NO:
    case rtppeer_t::BY:
      ssrc = read_u32(bytes + 12);
      break;
    case rtppeer_t::CK:
    case rtppeer_t::RS:
      ssrc = read_u32(bytes + 4);
      break;
    default:
      DEBUG("Unknown command {:04X} from {}, ignored", command,
            addr.to_string());
      return;
    }
  } else if (rtppeer_t::is_feedback(buffer)) {
    ssrc = read_u32(bytes + 4);
  } else if (port == rtppeer_t::MIDI_PORT && size >= 12 &&
             (bytes[1] & 0x7F) == 0x61) {
    ssrc = read_u32(bytes + 8);
  } else {
    DEBUG("Unknown datagram from {} on the {} port, ignored", addr.to_string(),
          port);
    return;
  }

  auto *session = find_session(addr, port, ssrc);
  if (!session) {
    DEBUG("Datagram from {} on the {} port, ssrc {:08X}: no such session, "
          "ignored",
          addr.to_string(), port, ssrc);
    return;
  }
  // The peer may end its session (a goodbye) and so free it: keep it alive.
  auto peer = session->peer;
  peer->data_ready(std::move(buffer), port);
}

// V2: a repetition, the other port of a handshake, a new session that replaces
// a client's others, or a new client.
void rtpserver_t::invitation(const io_bytes_reader &data,
                             const network_address_t &from,
                             rtppeer_t::port_e port) {
  io_bytes_reader buffer(data);
  const uint32_t token = read_u32(buffer.start + 8);
  const uint32_t ssrc = read_u32(buffer.start + 12);
  // V3: the base port is the source port of a control IN, the source port - 1
  // of a MIDI IN that arrives first.
  const int base = from.port() - (port == rtppeer_t::MIDI_PORT ? 1 : 0);

  for (auto &session : peers) {
    if (session.base_port == base && session.peer->remote_ssrc == ssrc &&
        session.peer->initiator_id == token && session.address.same_host(from)) {
      auto peer = session.peer;
      peer->data_ready(std::move(buffer), port);
      return;
    }
  }

  if (port == rtppeer_t::CONTROL_PORT) {
    // A control IN from a known client (same address, same SSRC) with another
    // base port or token: its new session. The others go, with no goodbye to
    // their dead ports; their disconnect status removes their ALSA ports.
    std::vector<std::shared_ptr<rtppeer_t>> replaced;
    for (auto &session : peers) {
      if (session.peer->remote_ssrc == ssrc && session.address.same_host(from)) {
        INFO("Session replaced: {} by a new invitation from port {}",
             session.label(), base);
        replaced.push_back(session.peer);
      }
    }
    for (auto &peer : replaced)
      peer->drop(rtppeer_t::DISCONNECTED_PEER_DISCONNECTED);
  }
  // A MIDI IN never removes anything: it opens an incomplete session that the
  // control IN completes, or that goes after 5 s (V5).
  create_session(std::move(buffer), from, base, port);
}

void rtpserver_t::sendto(const io_bytes_reader &pb, rtppeer_t::port_e port,
                         network_address_t &address, int remote_base_port) {
  ssize_t res;
  if (port == rtppeer_t::MIDI_PORT) {
    address.set_port(remote_base_port + 1);
    res = midi.sendto(packet_t(pb.start, pb.size()), address);
  } else {
    address.set_port(remote_base_port);
    res = control.sendto(packet_t(pb.start, pb.size()), address);
  }
  if (res < 0) {
    // One line a minute, however many datagrams are lost (C7).
    WARNING_RATE_LIMIT(60, "Server '{}': could not send to {}: {}", name,
                       address.to_string(), strerror(errno));
  }
}

rtpserverpeer_t &rtpserver_t::create_session(io_bytes_reader &&buffer,
                                             const network_address_t &from,
                                             int base_port,
                                             rtppeer_t::port_e port) {
  return peers.emplace_back(std::move(buffer), from, base_port, port, name,
                            this);
}

rtpserverpeer_t *rtpserver_t::find_peer_data_by_id(int id) {
  auto peerdata = std::find_if(
      peers.begin(), peers.end(),
      [id](const rtpserverpeer_t &datapeer) { return datapeer.id == id; });
  if (peerdata == peers.end())
    return nullptr;
  return &(*peerdata);
}

void rtpserver_t::send_midi_to_all_peers(const io_bytes_reader &buffer) {
  for (auto &speers : peers) {
    speers.peer->send_midi(buffer);
  }
}

void rtpserver_t::remove_peer(int peer_id) {
  peers.remove_if(
      [peer_id](const rtpserverpeer_t &datapeer) { return datapeer.id == peer_id; });
}
