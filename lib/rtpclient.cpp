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


#include <rtpmidid/exceptions.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/network.hpp>
#include <rtpmidid/networkaddress.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpclient.hpp>
#include <rtpmidid/rtppeer.hpp>
#include <rtpmidid/udppeer.hpp>
#include <rtpmidid/utils.hpp>

#include <algorithm>
#include <cstring>

using namespace std::chrono_literals;
using namespace rtpmidid;

using steady = std::chrono::steady_clock;

static long seconds_since(steady::time_point when) {
  return long(
      std::chrono::duration_cast<std::chrono::seconds>(steady::now() - when)
          .count());
}

// The blocking lookup of a name (C6: only for a `[connect_to]` name, never for
// an address). Counted by network_address_list_t.
static std::vector<std::string> getaddrinfo_lookup(const std::string &name,
                                                   const std::string &port,
                                                   std::string &error) {
  std::vector<std::string> ips;
  network_address_list_t list(name, port);
  if (!list.is_valid()) {
    error = FMT::format("lookup of {} failed: {}", name, list.error_string());
    return ips;
  }
  for (auto address : list) {
    auto ip = address.ip();
    if (std::find(ips.begin(), ips.end(), ip) == ips.end())
      ips.push_back(ip);
  }
  return ips;
}

/**
 * @short Create the rtp client
 *
 * @param name Name of the client.
 *
 * This client will try to connect to the list of `add_server_address`
 * addresses:ports, one by one.
 *
 * There is an internal state machine that helps try all combinations of name
 * resolves to ensure connection. It can even connect control and then fail.
 */
rtpclient_t::rtpclient_t(const std::string name) : peer(std::move(name)) {
  lookup_name = getaddrinfo_lookup;
  peer.initiator_id = ::rtpmidid::rand_u32_nonzero();
  send_connection = peer.send_event.connect([this](const io_bytes_reader &data,
                                                   rtppeer_t::port_e port) {
    // A send that fails is reported to the try that made it (C8), not turned
    // into a status event from inside the send.
    this->sendto(data, port);
  });
  ck_connection = peer.ck_event.connect([this](float) { on_ck_reply(); });
}

rtpclient_t::~rtpclient_t() {
  timer.disable();
  if (peer.is_connected()) {
    try {
      peer.send_goodbye(rtppeer_t::CONTROL_PORT);
      peer.send_goodbye(rtppeer_t::MIDI_PORT);
    } catch (network_exception &e) {
      ERROR("Removing client without sending proper goodbye. If reconnected "
            "may misbehave.");
    }
  }
}

void rtpclient_t::sendto(const io_bytes &pb, rtppeer_t::port_e port) {
  packet_t packet(pb.start, pb.size());

  ssize_t res;
  if (port == rtppeer_t::MIDI_PORT) {
    res = midi_peer.sendto(packet, midi_address);
  } else if (port == rtppeer_t::CONTROL_PORT) {
    res = control_peer.sendto(packet, control_address);
  } else {
    throw exception("Unknown port {}", port);
  }

  if (res == -1) {
    send_errno = errno;
    last_error = FMT::format(
        "send to {} failed: {}",
        port == rtppeer_t::CONTROL_PORT ? control_address.to_string()
                                        : midi_address.to_string(),
        strerror(send_errno));
    DEBUG("Client: {}", last_error);
  }
}

void rtpclient_t::reset() { peer.reset(); }

void rtpclient_t::add_server_address(const std::string &address,
                                     const std::string &port) {
  address_port_known.push_back({address, port});
  address_port_pending.push_back({address, port});
  connect();
}

void rtpclient_t::add_server_addresses(
    const std::vector<rtpclient_t::endpoint_t> &endpoints) {
  for (auto &endpoint : endpoints) {
    address_port_known.push_back(endpoint);
    address_port_pending.push_back(endpoint);
  }
  connect();
}

void rtpclient_t::replace_server_addresses(
    const std::vector<rtpclient_t::endpoint_t> &endpoints) {
  address_port_known.assign(endpoints.begin(), endpoints.end());
  if (has_last_connected && !endpoint_known(last_connected))
    has_last_connected = false;
  rebuild_pending();
  connect();
}

void rtpclient_t::connect() {
  if (state == WaitToStart) {
    // C7: a client is away from the start until its first session.
    down_since = steady::now();
    last_still_no_session = down_since;
    handle_event(Started);
  }
}

bool rtpclient_t::endpoint_known(const endpoint_t &endpoint) const {
  for (auto &known : address_port_known)
    if (known.hostname == endpoint.hostname && known.port == endpoint.port)
      return true;
  return false;
}

// C6: the endpoint of the last session first, and again after each other one.
void rtpclient_t::rebuild_pending() {
  address_port_pending.clear();
  const bool last = has_last_connected && endpoint_known(last_connected);
  if (last)
    address_port_pending.push_back(last_connected);
  for (auto &endpoint : address_port_known) {
    if (last && endpoint.hostname == last_connected.hostname &&
        endpoint.port == last_connected.port)
      continue;
    address_port_pending.push_back(endpoint);
    if (last)
      address_port_pending.push_back(last_connected);
  }
}

std::string rtpclient_t::peer_label() const {
  const std::string name =
      peer.remote_name.empty() ? std::string("?") : peer.remote_name;
  return FMT::format("{} ({}:{})", name, resolve_next_dns_endpoint.hostname,
                     resolve_next_dns_endpoint.port);
}

// C7: the session is away from now on (or has never been up).
void rtpclient_t::note_down(const std::string &reason) {
  if (peer.is_connected() || state == SendCkShort || state == SendCkLong ||
      state == WaitSendCkShort || state == WaitSendCkLong ||
      state == DisconnectBecauseCKTimeout) {
    WARNING("Session lost: {}, reason: {}", peer_label(), reason);
    session_lost = true;
  }
  down_since = steady::now();
  last_still_no_session = down_since;
  invitations_while_down = 0;
}

void rtpclient_t::still_no_session() {
  if (steady::now() - last_still_no_session < still_no_session_period)
    return;
  last_still_no_session = steady::now();
  WARNING("Still no session: {}, {} invitations in {} s, last error: {}",
          peer_label(), invitations_while_down, seconds_since(down_since),
          last_error.empty() ? std::string("none") : last_error);
}

// Everything that ends the current attempt goes through here: the next one
// waits for a tick (C8, in state_connect_control).
void rtpclient_t::attempt_failed(const std::string &why) {
  DEBUG("Attempt to {} failed: {}", peer_label(), why);
  timer.disable();
  control_connected_event_connection.disconnect();
  midi_connected_event_connection.disconnect();
  if (!why.empty())
    last_error = why;
  handle_event(ConnectFailed);
}

///
/// State mahcine implementation
///

void rtpclient_t::state_wait_to_start() {}

void rtpclient_t::state_prepare_next_dns() {
  if (address_port_pending.empty()) {
    handle_event(ConnectListExhausted);
    return;
  }
  resolve_next_dns_endpoint = address_port_pending.front();
  address_port_pending.pop_front();
  resolve_next_ips_valid = false;

  handle_event(NextReady);
}

// C6: an address is used as it is; a name is looked up only when it has no
// answer, or when every address of its answer has failed an attempt, and then
// no sooner than name_lookup_interval after the last lookup ended. A failed
// lookup keeps the last good answer.
std::vector<std::string> rtpclient_t::addresses_of(const endpoint_t &endpoint) {
  if (network_address_list_t::is_numeric(endpoint.hostname))
    return {endpoint.hostname};

  auto &answer = name_answers[endpoint.hostname];
  const bool due = !answer.looked_up ||
                   steady::now() - answer.looked_up_at >= name_lookup_interval;
  if (due && (answer.ips.empty() || answer.all_failed)) {
    std::string error;
    auto ips = lookup_name(endpoint.hostname, endpoint.port, error);
    answer.looked_up = true;
    answer.looked_up_at = steady::now();
    if (!ips.empty()) {
      // An answer: the attempt uses it in this turn (C1, its IN at once).
      answer.ips = std::move(ips);
      answer.all_failed = false;
    } else {
      // A name with no answer is one lost try (C8).
      last_try = answer.looked_up_at;
      tries_in_round++;
      last_error = error;
      DEBUG("{}; keeping {} address(es) of the last answer", error,
            answer.ips.size());
    }
  }
  return answer.ips;
}

void rtpclient_t::state_resolve_next_ip_port() {
  // First time, get the addresses of this endpoint
  if (!resolve_next_ips_valid) {
    resolve_next_ips = addresses_of(resolve_next_dns_endpoint);
    resolve_next_ip_index = 0;
    resolve_next_ips_valid = true;
  } else {
    // Following use the next address
    resolve_next_ip_index++;
  }

  // If any left, go for it, if not failed resolve this address port pair
  if (resolve_next_ip_index < resolve_next_ips.size()) {
    const auto &ip = resolve_next_ips[resolve_next_ip_index];
    control_address =
        network_address_list_t(ip, resolve_next_dns_endpoint.port)
            .get_first()
            .dup();

    // Guard: a resolved entry with destination port 0 means the endpoint
    // had no port (getaddrinfo with service=NULL/""), or the resolver
    // returned a malformed entry. Sending to :0 is permanently broken
    // (kernel returns EINVAL on every sendto), so refuse and advance to
    // the next resolved entry instead of letting the rtpclient_t enter a
    // permanent retry loop.
    if (!control_address.is_valid() || control_address.port() == 0) {
      WARNING_RATE_LIMIT(60,
                         "Refusing endpoint {}:{} - destination port is 0 "
                         "(missing or invalid port in [connect_to] config)",
                         ip, resolve_next_dns_endpoint.port);
      handle_event(ResolveFailed);
      return;
    }

    midi_address =
        network_address_list_t(control_address.ip(),
                               std::to_string(control_address.port() + 1))
            .get_first()
            .dup();

    DEBUG("Try to connect to address: {}", control_address.to_string());

    handle_event(Resolved);
  } else {
    resolve_next_ips_valid = false;
    if (!network_address_list_t::is_numeric(resolve_next_dns_endpoint.hostname))
      name_answers[resolve_next_dns_endpoint.hostname].all_failed = true;
    handle_event(ConnectListExhausted);
  }
}

// C1, C8: an attempt starts on a tick, never sooner than a tick after the last
// try, whatever failed. The deferral is a whole tick: no timer of this client
// is shorter.
void rtpclient_t::state_connect_control() {
  timer.disable();
  // C6: an endpoint that was replaced meanwhile is not invited again.
  if (!endpoint_known(resolve_next_dns_endpoint)) {
    DEBUG("Endpoint {}:{} is no longer known, skipping it",
          resolve_next_dns_endpoint.hostname, resolve_next_dns_endpoint.port);
    handle_event(ConnectFailed);
    return;
  }
  if (steady::now() - last_try + 5ms < tick) {
    timer = poller.add_timer_event(tick, [this] { start_control_attempt(); });
    return;
  }
  start_control_attempt();
}

void rtpclient_t::start_control_attempt() {
  timer.disable();

  // Defensive: if a previous cycle (CK-timeout reconnect, etc.) left either
  // peer bound, close them before re-allocating. Without this, the kernel
  // could hand us the same ephemeral control port as last cycle, and our own
  // surviving midi_peer would then hold control_port+1 — a false collision.
  control_peer.close();
  midi_peer.close();

  // Speculatively bind both control_peer and midi_peer here, BEFORE telling
  // the remote peer about us via IN. RTP-MIDI convention is
  // midi_port = control_port + 1. The kernel's ephemeral allocator can give
  // us a control_port whose +1 is owned by another process; retry with a
  // fresh ephemeral pair up to MAX_PAIR_RETRIES.
  //
  // Retry only when local_base_port_str == "0" (ephemeral). A fixed user-
  // configured local_udp_port that collides on +1 is a config error — fail
  // after one attempt, and say so in the once-a-minute line.
  static constexpr int MAX_PAIR_RETRIES = 5;
  const bool ephemeral = (local_base_port_str == "0");
  const int max_attempts = ephemeral ? MAX_PAIR_RETRIES : 1;

  bool pair_acquired = false;
  for (int attempt = 0; attempt < max_attempts; attempt++) {
    control_peer.open(network_address_list_t("::", local_base_port_str));
    if (!control_peer.is_open()) {
      last_error = FMT::format("could not open the control socket ({}): {}",
                               local_base_port_str,
                               strerror(control_peer.get_errno()));
      DEBUG("{} (attempt {}/{})", last_error, attempt + 1, max_attempts);
      continue;
    }
    local_base_port = control_peer.get_address().port();

    midi_peer.open(
        network_address_list_t("::", std::to_string(local_base_port + 1)));
    if (midi_peer.is_open()) {
      pair_acquired = true;
      break;
    }

    last_error =
        ephemeral
            ? FMT::format("midi port {} after control port {} is taken",
                          local_base_port + 1, local_base_port)
            : FMT::format("could not bind midi port {} (configured "
                          "local_udp_port={}); pick a port where both (port, "
                          "port+1) are free",
                          local_base_port + 1, local_base_port_str);
    DEBUG("{} (attempt {}/{})", last_error, attempt + 1, max_attempts);
    control_peer.close();
  }

  if (!pair_acquired) {
    // A bind that fails is one lost try (C8)
    last_try = steady::now();
    tries_in_round++;
    still_no_session();
    attempt_failed(last_error);
    return;
  }

  // A new attempt: a new token and a new SSRC (C2, D8), the same for all the
  // repetitions of this attempt.
  peer.reset();
  peer.initiator_id = ::rtpmidid::rand_u32_nonzero();
  peer.local_ssrc = ::rtpmidid::rand_u32_nonzero();

  control_on_read_connection = control_peer.on_read.connect(
      [this](const packet_t &packet, const network_address_t &) {
        io_bytes_reader data(packet.get_data(), packet.get_size());
        this->peer.data_ready(std::move(data), rtppeer_t::CONTROL_PORT);
      });
  midi_on_read_connection.disconnect();

  control_connected_event_connection =
      peer.status_change_event.connect([this](rtppeer_t::status_e status) {
        if (status == rtppeer_t::CONTROL_CONNECTED) {
          control_connected_event_connection.disconnect();
          auto address = control_peer.get_address();
          DEBUG("Connected control port {} to {}", address.port(),
                control_address.to_string());
          handle_event(Connected);
        } else if (rtppeer_t::is_disconnected(status)) {
          attempt_failed(FMT::format("invitation refused by {}",
                                     control_address.to_string()));
        }
      });

  tries = 0;
  send_invitation(rtppeer_t::CONTROL_PORT);
}

// One try of the current phase: one IN, the next one a tick later (C1). A send
// that fails ends the attempt (C8).
void rtpclient_t::send_invitation(rtppeer_t::port_e port) {
  tries++;
  tries_in_round++;
  last_try = steady::now();
  if (port == rtppeer_t::CONTROL_PORT)
    invitations_while_down++;
  send_errno = 0;
  peer.connect_to(port);
  if (send_errno != 0) {
    still_no_session();
    if (port == rtppeer_t::MIDI_PORT) {
      timer.disable();
      midi_connected_event_connection.disconnect();
      if (!last_error.empty())
        DEBUG("{}", last_error);
      handle_event(ConnectFailed); // to DisconnectControl
    } else {
      attempt_failed(last_error);
    }
    return;
  }
  still_no_session();
  if (port == rtppeer_t::CONTROL_PORT)
    timer = poller.add_timer_event(tick, [this] { on_control_tick(); });
  else
    timer = poller.add_timer_event(tick, [this] { on_midi_tick(); });
}

void rtpclient_t::on_control_tick() {
  if (state != ConnectControl)
    return;
  if (tries < control_tries) {
    send_invitation(rtppeer_t::CONTROL_PORT);
    return;
  }
  attempt_failed(FMT::format("no answer from {} to {} invitations",
                             control_address.to_string(), tries));
}

void rtpclient_t::state_connect_midi() {
  timer.disable();

  // midi_peer was already opened in state_connect_control to guarantee the
  // control+midi port pair is contiguous and both ports are bindable. Sanity
  // guard in case the state machine is somehow re-entered out of order.
  if (!midi_peer.is_open()) {
    ERROR("Internal: midi_peer should be open from state_connect_control");
    handle_event(ConnectFailed);
    return;
  }

  midi_on_read_connection = midi_peer.on_read.connect(
      [this](const packet_t &packet, const network_address_t &) {
        io_bytes_reader data(packet.get_data(), packet.get_size());
        this->peer.data_ready(std::move(data), rtppeer_t::MIDI_PORT);
      });

  midi_connected_event_connection =
      peer.status_change_event.connect([this](rtppeer_t::status_e status) {
        if (status == rtppeer_t::CONNECTED) {
          midi_connected_event_connection.disconnect();
          timer.disable();
          handle_event(Connected);
        } else if (rtppeer_t::is_disconnected(status)) {
          midi_connected_event_connection.disconnect();
          timer.disable();
          last_error = FMT::format("MIDI invitation refused by {}",
                                   midi_address.to_string());
          handle_event(ConnectFailed);
        }
      });

  // The first MIDI IN leaves at once, in the turn of the control OK (C1).
  tries = 0;
  send_invitation(rtppeer_t::MIDI_PORT);
}

void rtpclient_t::on_midi_tick() {
  if (state != ConnectMidi)
    return;
  if (tries < midi_tries) {
    send_invitation(rtppeer_t::MIDI_PORT);
    return;
  }
  midi_connected_event_connection.disconnect();
  last_error = FMT::format("no answer from {} to {} MIDI invitations",
                           midi_address.to_string(), tries);
  handle_event(ConnectFailed);
}

void rtpclient_t::state_disconnect_control() {
  timer.disable();
  peer.send_goodbye(rtppeer_t::CONTROL_PORT);
  control_peer.close();
  // With speculative midi_peer binding in state_connect_control, midi_peer is
  // open by the time we reach this state via the ConnectMidi failure path.
  midi_peer.close();
  handle_event(ConnectFailed);
}

void rtpclient_t::state_all_connected() {
  timer.disable();
  peer.remote_address = control_address.dup();
  peer.local_address = control_peer.get_address().dup();

  has_last_connected = true;
  last_connected = resolve_next_dns_endpoint;
  if (session_lost) {
    INFO("Session restored: {} after {} s", peer_label(),
         seconds_since(down_since));
  } else {
    INFO("Connected to {}", peer_label());
  }
  session_lost = false;
  down_since = steady::time_point{};
  invitations_while_down = 0;
  last_error.clear();

  ck_count = 0;
  handle_event(SendCK);
}

// C4: one clock-sync exchange: CK0 at once and again each tick while no reply
// has come; ten tries (D1), each armed when the one before it is sent, so a
// stalled loop costs one try however long it lasts.
void rtpclient_t::start_ck_exchange() {
  in_ck_exchange = true;
  tries = 0;
  send_ck0_try();
}

void rtpclient_t::send_ck0_try() {
  tries++;
  last_try = steady::now();
  send_errno = 0;
  peer.send_ck0(); // a send that fails is a lost try, nothing more (C8)
  timer = poller.add_timer_event(tick, [this] { on_ck_tick(); });
}

void rtpclient_t::on_ck_tick() {
  if (!in_ck_exchange || (state != SendCkShort && state != SendCkLong))
    return;
  if (tries < ck_tries) {
    send_ck0_try();
    return;
  }
  // D10: MIDI from the server's SSRC lately: the server still has the session,
  // only the replies are lost. The verdict waits, the tries go on.
  if (peer.last_midi_at != steady::time_point{} &&
      steady::now() - peer.last_midi_at < midi_postpones_verdict) {
    DEBUG("Clock sync unanswered after {} tries, but MIDI keeps coming: "
          "verdict postponed",
          tries);
    send_ck0_try();
    return;
  }
  in_ck_exchange = false;
  handle_event(Timeout);
}

// A CK1 to any CK0 of the exchange ends it (C4); outside an exchange it changes
// nothing (C3: the peer does not even report it).
void rtpclient_t::on_ck_reply() {
  if (!in_ck_exchange)
    return;
  if (state != SendCkShort && state != SendCkLong)
    return;
  in_ck_exchange = false;
  timer.disable();
  if (state == SendCkShort) {
    if (ck_count < 6) {
      handle_event(WaitSendCK);
    } else {
      handle_event(LatencyMeasured);
    }
  } else {
    handle_event(WaitSendCK);
  }
}

void rtpclient_t::state_send_ck_short() {
  ck_count++;
  start_ck_exchange();
}

void rtpclient_t::state_wait_send_ck_short() {
  timer =
      poller.add_timer_event(ck_short_period, [this] { handle_event(SendCK); });
}

void rtpclient_t::state_send_ck_long() {
  ck_count++;
  start_ck_exchange();
}

void rtpclient_t::state_wait_send_ck_long() {
  timer =
      poller.add_timer_event(ck_long_period, [this] { handle_event(SendCK); });
}

void rtpclient_t::state_disconnect_because_cktimeout() {
  timer.disable();
  in_ck_exchange = false;
  note_down(FMT::format("clock sync unanswered after {} tries", tries));
  last_error = "clock sync unanswered";
  peer.disconnect();
  // The next attempt goes to the same endpoint first (C6), a tick after the
  // last CK0 (C8, in state_connect_control).
  handle_event(ConnectFailed);
}

// Every endpoint was tried. The next round starts at once when this round made
// a try and the last one is a tick old (the attempt that just ran out of tries:
// its next IN is due now, C1); otherwise a tick later, so that a round that
// could try nothing (a name with no answer yet) never turns in a loop (C8).
void rtpclient_t::state_error() {
  timer.disable();
  peer.disconnect();
  still_no_session();
  if (tries_in_round > 0 && steady::now() - last_try + 5ms >= tick) {
    DEBUG("rtpclient_t: every endpoint tried, next round now");
    handle_event(Connect);
    return;
  }
  DEBUG("rtpclient_t: every endpoint tried, next round in {} ms",
        tick.count());
  timer = poller.add_timer_event(tick, [this] { handle_event(Connect); });
}

void rtpclient_t::state_try_connect_to_all_known_dns() {
  rebuild_pending();
  tries_in_round = 0;
  handle_event(Connect);
}

#include "rtpclient_statemachine.cpp"
