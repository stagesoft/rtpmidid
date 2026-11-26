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

#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtppeer.hpp>
#include <rtpmidid/rtpserverpeer.hpp>
#include <rtpmidid/signal.hpp>
#include <rtpmidid/udppeer.hpp>

namespace rtpmidid {
class rtpserver_t {
  NON_COPYABLE_NOR_MOVABLE(rtpserver_t)
public:
  // Callbacks to call when new connections
  using status_change_event_t =
      signal_t<std::shared_ptr<rtppeer_t>, rtppeer_t::status_e>;
  status_change_event_t status_change_event;

  using midi_event_t = signal_t<const io_bytes_reader &>;
  midi_event_t midi_event;

  int max_peer_data_id = 1;
  // The sessions. A list: an element never moves (V4).
  std::list<rtpserverpeer_t> peers;

  std::string name;
  udppeer_t control;
  udppeer_t midi;
  udppeer_t::on_read_t::connection_t on_read_control;
  udppeer_t::on_read_t::connection_t on_read_midi;

  rtpserver_t(std::string name, const std::string &port);
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~rtpserver_t();

  // V6: both ports bound. A server that is not open must not be announced.
  bool is_open() const { return control.is_open() && midi.is_open(); }
  int port() { return is_open() ? control.get_address().port() : 0; }

  void remove_peer(int peer_id);

  void send_midi_to_all_peers(const io_bytes_reader &bufer);

  void data_ready(const io_bytes_reader &data, const network_address_t &addr,
                  rtppeer_t::port_e port);
  void sendto(const io_bytes_reader &b, rtppeer_t::port_e port,
              network_address_t &address, int remote_base_port);

  rtpserverpeer_t *find_peer_data_by_id(int id);
  // V1: the session a datagram from `from` on `port` with `ssrc` belongs to.
  rtpserverpeer_t *find_session(const network_address_t &from,
                                rtppeer_t::port_e port, uint32_t ssrc);

private:
  bool bind(const std::string &port);
  void invitation(const io_bytes_reader &buffer, const network_address_t &from,
                  rtppeer_t::port_e port);
  rtpserverpeer_t &create_session(io_bytes_reader &&buffer,
                                  const network_address_t &from, int base_port,
                                  rtppeer_t::port_e port);
};
} // namespace rtpmidid
