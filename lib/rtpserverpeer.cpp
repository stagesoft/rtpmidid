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

#include <rtpmidid/logger.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpserver.hpp>
#include <rtpmidid/rtpserverpeer.hpp>

namespace rtpmidid {
rtpserverpeer_t::rtpserverpeer_t(io_bytes_reader &&buffer,
                                 const network_address_t &addr, int base_port_,
                                 rtppeer_t::port_e port,
                                 const std::string &name, rtpserver_t *srv)
    : base_port(base_port_) {
  server = srv;
  id = server->max_peer_data_id++;

  peer = std::make_shared<rtppeer_t>(name);

  address = addr.dup();
  address.set_port(base_port); // V3: the session is known by its base port
  DEBUG("Connected from {} (base port {})", addr.to_string(), base_port);
  peer->remote_address = address.dup();
  peer->local_address = srv->control.get_address().dup();

  // Setup some callbacks
  setup_connections();
  // Finally pass the data to the peer
  auto keep = peer;
  peer->data_ready(std::move(buffer), port);

  DEBUG("rtpserverpeer_t::rtpserverpeer_t({});", id);
}

std::string rtpserverpeer_t::label() const {
  return FMT::format("{} ({}:{}, ssrc {:08X})", peer->remote_name,
                     address.ip(), base_port, peer->remote_ssrc);
}

void rtpserverpeer_t::later(std::function<void()> f) {
  std::weak_ptr<int> still = alive;
  poller.call_later([still, f = std::move(f)] {
    if (still.lock())
      f();
  });
}

void rtpserverpeer_t::setup_connections() {
  send_event_connection = peer->send_event.connect(
      [this](const io_bytes_reader &buff, rtppeer_t::port_e port) {
        sendto(buff, port);
      });

  status_change_event_connection = peer->status_change_event.connect(
      [this](rtppeer_t::status_e st) { status_change(st); });

  midi_event_connection = peer->midi_event.connect(
      [this](const io_bytes_reader &data) { server->midi_event(data); });

  // V5: a session not complete 5 s after it was made is removed, whichever
  // port it has (a MIDI-only one used to stay for good, answering clock sync).
  timer_connection = poller.add_timer_event(std::chrono::seconds(5), [this]() {
    later([this] {
      if (peer->status == rtppeer_t::status_e::CONNECTED)
        return;
      INFO("Incomplete session removed after 5 s: {}, status {}", label(),
           peer->status);
      auto keep = peer;
      peer->disconnect();
    });
  });

  ck_event_connection =
      peer->ck_event.connect([this](float) { rearm_ck_timeout(); });
}

rtpserverpeer_t::~rtpserverpeer_t() {
  send_event_connection.disconnect();
  status_change_event_connection.disconnect();
  ck_event_connection.disconnect();
  midi_event_connection.disconnect();
  timer_connection.disable();
  DEBUG("rtpserverpeer_t::~rtpserverpeer_t({})", id);
  id = -2;
}

// V4: no clock sync for 60 s ends the session.
void rtpserverpeer_t::rearm_ck_timeout() {
  timer_connection.disable();

  timer_connection = poller.add_timer_event(std::chrono::seconds(60), [this]() {
    later([this] {
      WARNING("Session expired: {}, no clock sync for 60 s", label());
      auto keep = peer;
      peer->disconnect();
    });
  });
}

void rtpserverpeer_t::sendto(const io_bytes_reader &buff,
                             rtppeer_t::port_e port) {
  server->sendto(buff, port, address, base_port);
}

void rtpserverpeer_t::status_change(rtppeer_t::status_e st) {
  DEBUG("rptserverpeer_t status change to {}", st);

  server->status_change_event(peer, st);
  if (st == rtppeer_t::CONNECTED) {
    rearm_ck_timeout();
  } else if (rtppeer_t::is_disconnected(st)) {
    DEBUG("Remove from server the peer {}, status: {}", id, st);
    server->remove_peer(id); // destroys this object: nothing after this line
  }
}
} // namespace rtpmidid
