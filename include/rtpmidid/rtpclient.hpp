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

#include "./iobytes.hpp"
#include "./poller.hpp"
#include "./rtppeer.hpp"
#include "./signal.hpp"
#include "./udppeer.hpp"
#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <vector>

extern "C" {
struct addrinfo;
}

namespace rtpmidid {
/**
 * @short A RTP Client
 *
 * It connects to a remote address and port, do all the connection parts,
 * and emits midi events or on_disconnect if not valid.
 *
 * Session recovery (1amfwm7fgu7): every try waits for a tick of `tick`, never
 * less (C1, C8); clock sync is repeated each tick and judged in tries (C4);
 * an address is never looked up, a name at most every `name_lookup_interval`
 * (C6); while the peer is away the journal gets one line a minute (C7).
 */
class rtpclient_t {
  NON_COPYABLE_NOR_MOVABLE(rtpclient_t)
public:
  rtppeer_t peer;
  struct endpoint_t {
    std::string hostname;
    std::string port;
  };

#include "rtpclient_statemachine.hpp"

public:
  // Can be changed if required (tests shorten them; nothing reads them from a
  // file: D8 of the 10-26 plan)
  std::chrono::milliseconds tick = std::chrono::seconds(1);
  int control_tries = 10; // IN on the control port, one a tick
  int midi_tries = 5;     // IN on the MIDI port, one a tick
  int ck_tries = 10;      // CK0 of one clock-sync exchange, one a tick (D1)
  // MIDI from the server this recently postpones the clock-sync verdict (D10)
  std::chrono::milliseconds midi_postpones_verdict = std::chrono::seconds(10);
  // A `[connect_to]` name is looked up again no sooner than this (C6)
  std::chrono::milliseconds name_lookup_interval = std::chrono::seconds(40);
  // One "Still no session" line per this while the peer is away (C7)
  std::chrono::milliseconds still_no_session_period = std::chrono::seconds(60);
  std::chrono::milliseconds ck_short_period = std::chrono::seconds(2);
  std::chrono::milliseconds ck_long_period = std::chrono::seconds(25);

  // Looks a name up (blocking): the addresses as text, or none and `error`
  // set. Tests replace it; the default uses getaddrinfo.
  std::function<std::vector<std::string>(
      const std::string &name, const std::string &port, std::string &error)>
      lookup_name;

  poller_t::timer_t timer;

  uint16_t local_base_port = 0;
  std::string local_base_port_str =
      "0"; // This can be changed before connec tto force a port

  uint8_t ck_count = 0;
  int tries = 0;              // tries of the current phase or exchange
  int tries_in_round = 0;     // tries since the walk of endpoints began
  bool in_ck_exchange = false; // a CK0 is out and no reply has come
  rtppeer_t::send_event_t::connection_t send_connection;
  rtppeer_t::ck_event_t::connection_t ck_connection;

  rtppeer_t::status_change_event_t::connection_t
      control_connected_event_connection;
  rtppeer_t::status_change_event_t::connection_t
      midi_connected_event_connection;

  signal_t<const std::string &, rtppeer_t::status_e> connected_event;

  udppeer_t control_peer;
  udppeer_t midi_peer;
  network_address_t control_address;
  network_address_t midi_address;
  udppeer_t::on_read_t::connection_t control_on_read_connection;
  udppeer_t::on_read_t::connection_t midi_on_read_connection;

  std::list<endpoint_t> address_port_known;   // All known address and ports
  std::list<endpoint_t> address_port_pending; // Pending to try to connect to
  // The endpoint of the last session, tried first and after each other (C6)
  bool has_last_connected = false;
  endpoint_t last_connected;

  // Currently conneting / connected endpoint
  endpoint_t resolve_next_dns_endpoint;
  // Its addresses, as text, and the one being tried
  std::vector<std::string> resolve_next_ips;
  size_t resolve_next_ip_index = 0;
  bool resolve_next_ips_valid = false;

  // What a `[connect_to]` name resolved to last time (C6)
  struct name_answer_t {
    std::vector<std::string> ips; // the last good answer, kept on failure
    bool looked_up = false;
    std::chrono::steady_clock::time_point looked_up_at{}; // when it ended
    bool all_failed = false; // every address of it failed an attempt since
  };
  std::map<std::string, name_answer_t> name_answers;

  // C8: when the last try (an IN, a CK0, a bind, a lookup) was made
  std::chrono::steady_clock::time_point last_try{};
  // A send of the current try failed (errno), reported by sendto
  int send_errno = 0;
  std::string last_error; // what failed last, for the once-a-minute line

  // C7: the journal of a session that is away
  bool session_lost = false; // an established session ended
  std::chrono::steady_clock::time_point down_since{};
  std::chrono::steady_clock::time_point last_still_no_session{};
  int invitations_while_down = 0;

public:
  rtpclient_t(std::string name);
  ~rtpclient_t();

  void data_ready(rtppeer_t::port_e port);
  void sendto(const io_bytes &pb, rtppeer_t::port_e port);
  void reset();

  // Public interface
  void add_server_address(const std::string &address, const std::string &port);
  void add_server_addresses(
      const std::vector<rtpmidid::rtpclient_t::endpoint_t> &endpoints);
  // Replace every endpoint (a discovered peer's address changed): a session
  // that is up stays; the next attempt goes to the new endpoints.
  void replace_server_addresses(
      const std::vector<rtpmidid::rtpclient_t::endpoint_t> &endpoints);
  void connect();

private:
  void start_control_attempt();
  void send_invitation(rtppeer_t::port_e port);
  void on_control_tick();
  void on_midi_tick();
  void start_ck_exchange();
  void send_ck0_try();
  void on_ck_tick();
  void on_ck_reply();
  void attempt_failed(const std::string &why);
  void rebuild_pending();
  bool endpoint_known(const endpoint_t &endpoint) const;
  std::vector<std::string> addresses_of(const endpoint_t &endpoint);
  void note_down(const std::string &reason);
  void still_no_session();
  std::string peer_label() const;
};
} // namespace rtpmidid

BASIC_FORMATTER(rtpmidid::rtpclient_t::state_e, "{}",
                rtpmidid::rtpclient_t::to_string(v));