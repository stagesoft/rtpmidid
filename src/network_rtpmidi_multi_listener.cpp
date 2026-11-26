/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019-2023 David Moreno Montero <dmoreno@coralbits.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 */

#include "network_rtpmidi_multi_listener.hpp"
#include "factory.hpp"
#include "json.hpp"
#include "midirouter.hpp"
#include "rtpmidid/exceptions.hpp"
#include "rtpmidid/poller.hpp"
#include "rtpmidid/mdns_rtpmidi.hpp"
#include "utils.hpp"

namespace rtpmididns {

extern std::shared_ptr<::rtpmidid::mdns_rtpmidi_t> mdns;

network_rtpmidi_multi_listener_t::network_rtpmidi_multi_listener_t(
    const std::string &name, const std::string &port,
    std::shared_ptr<aseq_t> aseq_)
    : aseq(aseq_), server(name, port) {
  // V6 (1amfwm7fgu7): a server that could not bind is not announced and not
  // made; at start-up that fails the start.
  if (!server.is_open())
    throw rtpmidid::exception("RTP MIDI server {} could not bind port {}", name,
                              port);
  if (mdns)
    mdns->announce_rtpmidi(name, server.port());

  status_change_connection = server.status_change_event.connect(
      [this](std::shared_ptr<rtpmidid::rtppeer_t> peer,
             rtpmidid::rtppeer_t::status_e status) {
        // No exception leaves a status event (M2): the server's own handling
        // of this event (its 60 s timer) runs after this slot.
        try {
          session_status(peer, status);
        } catch (const std::exception &exc) {
          ERROR("Error handling the session of {}: {}", peer->remote_name,
                exc.what());
        }
      });
}

// M1 (D3): one router peer and one ALSA port per session, whatever the library
// fires: a second CONNECTED for a session that has its port makes none.
void network_rtpmidi_multi_listener_t::session_status(
    std::shared_ptr<rtpmidid::rtppeer_t> peer,
    rtpmidid::rtppeer_t::status_e status) {
  if (rtpmidid::rtppeer_t::is_disconnected(status)) {
    // Its router peers remove themselves (network_rtpmidi_peer_t).
    sessions.erase(peer.get());
    return;
  }
  if (status != rtpmidid::rtppeer_t::status_e::CONNECTED)
    return;
  if (sessions.count(peer.get())) {
    DEBUG("Session of {} already has its port", peer->remote_name);
    return;
  }
  DEBUG("Got connection from {}", peer->remote_name);
  std::shared_ptr<midipeer_t> alsapeer;
  try {
    alsapeer = make_local_alsa_peer(peer->remote_name, aseq);
  } catch (const std::exception &exc) {
    // M2: no port, no session. It is refused after this event has returned
    // (the server is still inside its own handling of it).
    ERROR("Refusing the session of {}: {}", peer->remote_name, exc.what());
    rtpmidid::poller.call_later([peer] { peer->disconnect(); });
    return;
  }
  auto alsa_id = router->add_peer(alsapeer);
  auto peer_id = router->add_peer(make_network_rtpmidi_peer(peer));
  router->connect(alsa_id, peer_id);
  router->connect(peer_id, alsa_id);
  sessions[peer.get()] = alsa_id;
}

void network_rtpmidi_multi_listener_t::send_midi(midipeer_id_t from,
                                                 const mididata_t &) {}

json_t network_rtpmidi_multi_listener_t::status() {
  std::vector<json_t> peers;
  for (auto &peer : server.peers) {
    auto &peerpeer = peer.peer;
    peers.push_back(peer_status(*peerpeer));
  }

  return json_t{
      //
      {"peers", peers},      //
      {"name", server.name}, //
      {"listening",
       {
           //
           {"name", server.name},           //
           {"control_port", server.port()}, //
           {"midi_port", server.port() + 1} //
       }} //
  };
}

} // namespace rtpmididns
