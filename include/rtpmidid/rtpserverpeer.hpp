/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019-2024 David Moreno Montero <dmoreno@coralbits.com>
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

#pragma once

#include "rtppeer.hpp"
#include <memory>
#include <rtpmidid/networkaddress.hpp>
#include <rtpmidid/utils.hpp>

namespace rtpmidid {
class rtpserver_t;

/**
 * One session of a server: (source address, base port, SSRC), V1.
 *
 * Kept in a container that never moves it (V4), so that its timers stay armed
 * for the object they were armed for.
 */
class rtpserverpeer_t {
  NON_COPYABLE_NOR_MOVABLE(rtpserverpeer_t);

public:
  int id;
  std::shared_ptr<rtppeer_t> peer;
  network_address_t address; // the client's address, with the base port
  int base_port;             // the client's control port; MIDI is base + 1
  rtpserver_t *server;

  rtppeer_t::send_event_t::connection_t send_event_connection;
  rtppeer_t::status_change_event_t::connection_t status_change_event_connection;
  rtppeer_t::ck_event_t::connection_t ck_event_connection;

  rtppeer_t::midi_event_t::connection_t midi_event_connection;

  poller_t::timer_t timer_connection;
  // Lives as long as this session: a deferred action checks it first.
  std::shared_ptr<int> alive = std::make_shared<int>(0);

public:
  rtpserverpeer_t(io_bytes_reader &&buffer, const network_address_t &addr,
                  int base_port, rtppeer_t::port_e port,
                  const std::string &name, rtpserver_t *server);
  ~rtpserverpeer_t();

  void setup_connections();
  void sendto(const io_bytes_reader &buff, rtppeer_t::port_e port);
  void status_change(rtppeer_t::status_e st);
  void rearm_ck_timeout();
  // Run `f` after this turn of the loop, if this session still exists then:
  // a timer callback must not remove the session that owns the timer.
  void later(std::function<void()> f);
  std::string label() const;
};
} // namespace rtpmidid
