// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// RTP-MIDI session recovery (ClickUp 1amfwm7fgu7, T7-16; 1amfwm7h80b).
//
// Every test states what a cluster needs, not how the library gets there, so
// that it stays valid whatever the fix looks like. Tests named control_* are
// expected to pass on the unfixed library: they show that the harness can
// connect, break and judge a session. The others describe behaviour the
// unfixed library does not have.
//
// Everything runs on loopback (127.0.0.1, and 127.0.0.2 as "another host").
// Nothing here drops packets on a real network: where a test needs a loss it
// puts a relay of its own between client and server, or a server written by
// hand that ignores chosen datagrams.

#include "../tests/test_case.hpp"
#include "../tests/test_utils.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <rtpmidid/iobytes.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/networkaddress.hpp>
#include <rtpmidid/packet.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpclient.hpp>
#include <rtpmidid/rtpmidipacket.hpp>
#include <rtpmidid/rtpserver.hpp>
#include <rtpmidid/udppeer.hpp>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using rtpmidid::rtpclient_t;
using rtpmidid::rtppeer_t;

// ---------------------------------------------------------------- helpers

// Run the event loop until `done` holds or `budget` has passed on the
// monotonic clock. Returns the final value of `done`.
static bool run_until(const std::function<bool()> &done,
                      std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    rtpmidid::poller.wait(10ms);
  }
  return true;
}

static void run_for(std::chrono::milliseconds time) {
  run_until([] { return false; }, time);
}

static bool is_up(const rtpclient_t &client) {
  return client.peer.status == rtppeer_t::status_e::CONNECTED;
}

// Watch the client for `time`: true when it was up all along.
static bool stays_up(const rtpclient_t &client,
                     std::chrono::milliseconds time) {
  bool went_down = false;
  run_until(
      [&] {
        if (!is_up(client))
          went_down = true;
        return false;
      },
      time);
  return !went_down;
}

static uint64_t read_u64(const uint8_t *bytes) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++)
    value = (value << 8) | bytes[i];
  return value;
}

// CK is kept slow so that the only CK timeout in a test is the one the test
// asks for. (The 500 ms connect and reconnect timeouts written here for the
// unfixed client are gone: the fixed one tries once a tick, 1 s.)
static void fast_timeouts(rtpclient_t &client) {
  client.ck_short_period = 5000ms;
  client.ck_long_period = 5000ms;
}

static void connect_client_to(rtpclient_t &client, int port) {
  client.add_server_address("127.0.0.1", std::to_string(port));
  ASSERT_TRUE(run_until([&] { return is_up(client); }, 3000ms));
  // The first CK exchange has been answered and the client waits for the next.
  ASSERT_TRUE(run_until(
      [&] { return client.state == rtpclient_t::state_e::WaitSendCkShort; },
      3000ms));
}

static void connect_client(rtpclient_t &client, rtpmidid::rtpserver_t &server) {
  fast_timeouts(client);
  connect_client_to(client, server.port());
}

// What a client puts on the wire, seen from the client itself.
struct send_tap_t {
  int control_invitations = 0;
  int ck0_sent = 0;
  uint64_t last_ck0 = 0; // the timestamp of the last CK0 sent
  rtppeer_t::send_event_t::connection_t connection;

  explicit send_tap_t(rtpclient_t &client) {
    connection = client.peer.send_event.connect(
        [this](const rtpmidid::io_bytes_reader &data, rtppeer_t::port_e port) {
          const uint8_t *bytes = data.start;
          const size_t size = data.size();
          if (size < 4 || bytes[0] != 0xFF || bytes[1] != 0xFF)
            return;
          if (bytes[2] == 'I' && bytes[3] == 'N' &&
              port == rtppeer_t::CONTROL_PORT)
            control_invitations++;
          if (bytes[2] == 'C' && bytes[3] == 'K' && size >= 20 &&
              bytes[8] == 0) {
            ck0_sent++;
            last_ck0 = read_u64(bytes + 12);
          }
        });
  }
};

// One clock-sync request that is not answered in time: what a stalled event
// loop produces on a healthy network. The client is made to send a CK and its
// timeout fires before the reply is read. No packet is dropped.
//
// The verdict must end the session, and a session that ends invites again: a
// new invitation has to leave the client. Without this check a test of two
// clients could pass because no session ever dropped.
static void one_ck_timeout(rtpclient_t &client) {
  ASSERT_TRUE(run_until(
      [&] { return client.state == rtpclient_t::state_e::WaitSendCkShort; },
      3000ms));
  client.handle_event(rtpclient_t::SendCK);
  ASSERT_TRUE(client.state == rtpclient_t::state_e::SendCkShort);
  send_tap_t sent(client);
  client.handle_event(rtpclient_t::Timeout);
  const bool invited =
      run_until([&] { return sent.control_invitations >= 1; }, 3000ms);
  INFO("one CK timeout: a new invitation was sent: {}", invited);
  ASSERT_TRUE(invited);
}

static void log_server_peers(const char *when, rtpmidid::rtpserver_t &server) {
  INFO("[{}] server holds {} peer(s)", when, server.peers.size());
  for (auto &entry : server.peers) {
    INFO("[{}]   peer id={} name={} initiator_id={:X} remote_ssrc={:X} "
         "status={}",
         when, entry.id, entry.peer->remote_name, entry.peer->initiator_id,
         entry.peer->remote_ssrc, int(entry.peer->status));
  }
}

// What a cluster node needs: MIDI sent by the server reaches it.
static void send_probe_note(rtpmidid::rtpserver_t &server) {
  auto note = hex_to_bin("90 3C 7F");
  server.send_midi_to_all_peers(note);
}

// Every status the server reports, in order, by the peer it reports it for.
// In the daemon a CONNECTED makes an ALSA port and a disconnect removes it.
struct server_events_t {
  struct event_t {
    const rtppeer_t *peer;
    rtppeer_t::status_e status;
  };
  std::vector<event_t> events;
  rtpmidid::rtpserver_t::status_change_event_t::connection_t connection;

  explicit server_events_t(rtpmidid::rtpserver_t &server) {
    connection = server.status_change_event.connect(
        [this](std::shared_ptr<rtppeer_t> peer, rtppeer_t::status_e status) {
          events.push_back({peer.get(), status});
        });
  }

  // The peers that reported CONNECTED, in the order they first did.
  std::vector<const rtppeer_t *> connected_peers() const {
    std::vector<const rtppeer_t *> peers;
    for (auto &event : events) {
      if (event.status != rtppeer_t::CONNECTED)
        continue;
      bool known = false;
      for (auto *peer : peers)
        known = known || peer == event.peer;
      if (!known)
        peers.push_back(event.peer);
    }
    return peers;
  }
  const rtppeer_t *first_connected() const {
    auto peers = connected_peers();
    return peers.empty() ? nullptr : peers[0];
  }
  int connected() const {
    int count = 0;
    for (auto &event : events)
      count += event.status == rtppeer_t::CONNECTED;
    return count;
  }
  int disconnects() const {
    int count = 0;
    for (auto &event : events)
      count += rtppeer_t::is_disconnected(event.status);
    return count;
  }
  int events_of(const rtppeer_t *peer) const {
    int count = 0;
    for (auto &event : events)
      count += event.peer == peer;
    return count;
  }
  int disconnects_of(const rtppeer_t *peer) const {
    int count = 0;
    for (auto &event : events)
      count += event.peer == peer && rtppeer_t::is_disconnected(event.status);
    return count;
  }
  int disconnects_except(const rtppeer_t *peer) const {
    return disconnects() - disconnects_of(peer);
  }
};

// ------------------------------------------------- hand-made hosts (wire)
//
// Plain UDP sockets the poller does not see: a test decides exactly what such
// a host sends, and reads what it got when it wants to.

static int bind_udp(const char *ip, int port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(port));
  ::inet_pton(AF_INET, ip, &address.sin_addr);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

static int local_port(int fd) {
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
  return ntohs(address.sin_port);
}

// Two sockets on `ip`, the second on the port after the first: an RTP-MIDI
// session's control and MIDI ports.
static bool bind_pair(const char *ip, int &control_fd, int &midi_fd,
                      int &base_port) {
  for (int attempt = 0; attempt < 50; attempt++) {
    const int control = bind_udp(ip, 0);
    if (control < 0)
      return false;
    const int port = local_port(control);
    const int midi = port < 65535 ? bind_udp(ip, port + 1) : -1;
    if (midi >= 0) {
      control_fd = control;
      midi_fd = midi;
      base_port = port;
      return true;
    }
    ::close(control);
  }
  return false;
}

// A local port whose next port is free too, for a client given a fixed local
// port (the daemon's `local_udp_port`).
static int free_port_pair() {
  auto bind_any6 = [](int port) {
    const int fd = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return -1;
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(uint16_t(port));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
        0) {
      ::close(fd);
      return -1;
    }
    return fd;
  };
  for (int attempt = 0; attempt < 50; attempt++) {
    const int control = bind_any6(0);
    if (control < 0)
      continue;
    sockaddr_in6 address{};
    socklen_t length = sizeof(address);
    ::getsockname(control, reinterpret_cast<sockaddr *>(&address), &length);
    const int port = ntohs(address.sin6_port);
    const int midi = port < 65535 ? bind_any6(port + 1) : -1;
    ::close(control);
    if (midi >= 0) {
      ::close(midi);
      return port;
    }
  }
  throw test_exception(__FILE__, __LINE__, "no free pair of local ports");
}

static void send_udp(int fd, const char *ip, int port, const uint8_t *data,
                     size_t size) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(port));
  ::inet_pton(AF_INET, ip, &address.sin_addr);
  ::sendto(fd, data, size, 0, reinterpret_cast<sockaddr *>(&address),
           sizeof(address));
}

static void put32(std::vector<uint8_t> &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(uint8_t(value >> shift));
}

static void put64(std::vector<uint8_t> &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(uint8_t(value >> shift));
}

// IN (with a name) or BY (without): FF FF, command, protocol 2, initiator
// token, sender SSRC.
static std::vector<uint8_t> session_command(const char *command,
                                            uint32_t token, uint32_t ssrc,
                                            const std::string &name) {
  std::vector<uint8_t> out{0xFF, 0xFF, uint8_t(command[0]),
                           uint8_t(command[1])};
  put32(out, 2);
  put32(out, token);
  put32(out, ssrc);
  if (!name.empty()) {
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
  }
  return out;
}

static std::vector<uint8_t> ck0_packet(uint32_t ssrc, uint64_t timestamp) {
  std::vector<uint8_t> out{0xFF, 0xFF, 'C', 'K'};
  put32(out, ssrc);
  out.insert(out.end(), {0, 0, 0, 0}); // count 0, padding
  put64(out, timestamp);
  put64(out, 0);
  put64(out, 0);
  return out;
}

// What one socket of a hand-made host has received.
struct inbox_t {
  int ok = 0;
  int ck_replies = 0; // CK with count 1
  int midi = 0;       // RTP MIDI packets
  int goodbyes = 0;

  void take(const uint8_t *bytes, ssize_t size) {
    if (size >= 4 && bytes[0] == 0xFF && bytes[1] == 0xFF) {
      if (bytes[2] == 'O' && bytes[3] == 'K')
        ok++;
      else if (bytes[2] == 'C' && bytes[3] == 'K' && size >= 9 &&
               bytes[8] == 1)
        ck_replies++;
      else if (bytes[2] == 'B' && bytes[3] == 'Y')
        goodbyes++;
    } else if (size >= 12 && (bytes[0] & 0xC0) == 0x80 &&
               (bytes[1] & 0x7F) == 0x61) {
      midi++;
    }
  }
};

static void drain(int fd, inbox_t &inbox) {
  std::array<uint8_t, 1500> buffer{};
  for (;;) {
    auto size = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (size <= 0)
      return;
    inbox.take(buffer.data(), size);
  }
}

// An RTP-MIDI session written by hand, from `ip`, to a server on 127.0.0.1.
struct wire_session_t {
  int control_fd = -1;
  int midi_fd = -1;
  int base_port = 0;
  int server_port;
  uint32_t token;
  uint32_t ssrc;
  std::string name;
  inbox_t control_in;
  inbox_t midi_in;

  wire_session_t(const char *ip, int server_port_, uint32_t token_,
                 uint32_t ssrc_, std::string name_ = "node")
      : server_port(server_port_), token(token_), ssrc(ssrc_),
        name(std::move(name_)) {
    if (!bind_pair(ip, control_fd, midi_fd, base_port))
      FAIL("could not bind a pair of ports for a hand-made session");
  }
  wire_session_t(const wire_session_t &) = delete;
  wire_session_t &operator=(const wire_session_t &) = delete;
  ~wire_session_t() {
    ::close(control_fd);
    ::close(midi_fd);
  }

  void send(rtppeer_t::port_e port, const std::vector<uint8_t> &data) {
    const bool control = port == rtppeer_t::CONTROL_PORT;
    send_udp(control ? control_fd : midi_fd, "127.0.0.1",
             control ? server_port : server_port + 1, data.data(),
             data.size());
  }
  void invite(rtppeer_t::port_e port) {
    send(port, session_command("IN", token, ssrc, name));
  }
  void goodbye(rtppeer_t::port_e port) {
    send(port, session_command("BY", token, ssrc, ""));
  }
  void ck0() { send(rtppeer_t::MIDI_PORT, ck0_packet(ssrc, 1)); }
  void pump() {
    drain(control_fd, control_in);
    drain(midi_fd, midi_in);
  }
  // Both invitations, the server's answers read.
  void connect() {
    invite(rtppeer_t::CONTROL_PORT);
    run_for(50ms);
    invite(rtppeer_t::MIDI_PORT);
    run_for(50ms);
    pump();
  }
};

// A relay between clients and a server on loopback, run by the event loop,
// so that a test can lose chosen datagrams as a network would. Each client
// session (control port and control port + 1) gets its own pair of ports
// towards the server, as a real host's sessions do.
struct lossy_link_t {
  struct upstream_t {
    int control_fd = -1;
    int midi_fd = -1;
    int base_port = 0;
    int client_base = 0;
    rtpmidid::poller_t::listener_t on_control;
    rtpmidid::poller_t::listener_t on_midi;
    ~upstream_t() {
      on_control.stop();
      on_midi.stop();
      ::close(control_fd);
      ::close(midi_fd);
    }
  };

  int server_port;
  int control_fd = -1;
  int midi_fd = -1;
  int port = 0;
  bool drop_goodbyes_to_server = false;
  int goodbyes_dropped = 0;
  rtpmidid::poller_t::listener_t on_control;
  rtpmidid::poller_t::listener_t on_midi;
  std::map<int, std::unique_ptr<upstream_t>> upstreams;

  explicit lossy_link_t(int server_port_) : server_port(server_port_) {
    if (!bind_pair("127.0.0.1", control_fd, midi_fd, port))
      FAIL("could not bind the relay's ports");
    on_control = rtpmidid::poller.add_fd_in(
        control_fd, [this](int) { from_client(false); });
    on_midi =
        rtpmidid::poller.add_fd_in(midi_fd, [this](int) { from_client(true); });
  }
  lossy_link_t(const lossy_link_t &) = delete;
  lossy_link_t &operator=(const lossy_link_t &) = delete;
  ~lossy_link_t() {
    upstreams.clear();
    on_control.stop();
    on_midi.stop();
    ::close(control_fd);
    ::close(midi_fd);
  }

  upstream_t &upstream_for(int client_base) {
    auto found = upstreams.find(client_base);
    if (found != upstreams.end())
      return *found->second;
    auto upstream = std::make_unique<upstream_t>();
    upstream->client_base = client_base;
    if (!bind_pair("127.0.0.1", upstream->control_fd, upstream->midi_fd,
                   upstream->base_port))
      FAIL("could not bind a pair of relay ports towards the server");
    upstream_t *raw = upstream.get();
    upstream->on_control = rtpmidid::poller.add_fd_in(
        raw->control_fd, [this, raw](int) { from_server(*raw, false); });
    upstream->on_midi = rtpmidid::poller.add_fd_in(
        raw->midi_fd, [this, raw](int) { from_server(*raw, true); });
    upstreams[client_base] = std::move(upstream);
    return *raw;
  }

  void from_client(bool midi) {
    std::array<uint8_t, 1500> buffer{};
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    auto size = ::recvfrom(midi ? midi_fd : control_fd, buffer.data(),
                           buffer.size(), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr *>(&from), &length);
    if (size <= 0)
      return;
    if (drop_goodbyes_to_server && size >= 4 && buffer[0] == 0xFF &&
        buffer[1] == 0xFF && buffer[2] == 'B' && buffer[3] == 'Y') {
      goodbyes_dropped++;
      return;
    }
    const int client_base = ntohs(from.sin_port) - (midi ? 1 : 0);
    auto &upstream = upstream_for(client_base);
    send_udp(midi ? upstream.midi_fd : upstream.control_fd, "127.0.0.1",
             server_port + (midi ? 1 : 0), buffer.data(), size_t(size));
  }

  void from_server(upstream_t &upstream, bool midi) {
    std::array<uint8_t, 1500> buffer{};
    auto size = ::recv(midi ? upstream.midi_fd : upstream.control_fd,
                       buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (size <= 0)
      return;
    send_udp(midi ? midi_fd : control_fd, "127.0.0.1",
             upstream.client_base + (midi ? 1 : 0), buffer.data(),
             size_t(size));
  }
};

// ---------------------------------------------------------------- controls

void control_one_client_recovers_after_a_ck_timeout() {
  rtpmidid::rtpserver_t server("server", "0");
  rtpclient_t node_a("node-a");
  int got_a = 0;
  auto midi_a = node_a.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got_a++; });

  connect_client(node_a, server);
  send_probe_note(server);
  ASSERT_TRUE(run_until([&] { return got_a == 1; }, 1000ms));

  one_ck_timeout(node_a);
  ASSERT_TRUE(run_until([&] { return is_up(node_a); }, 8000ms));
  run_for(300ms);
  log_server_peers("after node-a timeout", server);

  got_a = 0;
  send_probe_note(server);
  ASSERT_TRUE(run_until([&] { return got_a >= 1; }, 1000ms));
}

void control_two_clients_both_receive() {
  rtpmidid::rtpserver_t server("server", "0");
  rtpclient_t node_a("node-a");
  rtpclient_t node_b("node-b");
  int got_a = 0;
  int got_b = 0;
  auto midi_a = node_a.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got_a++; });
  auto midi_b = node_b.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got_b++; });

  connect_client(node_a, server);
  connect_client(node_b, server);
  log_server_peers("both connected", server);

  send_probe_note(server);
  ASSERT_TRUE(run_until([&] { return got_a == 1 && got_b == 1; }, 1000ms));
}

// ------------------------------------------------- identity and reconnects

// Two nodes, one CK timeout each, one after the other, no packet lost. Both
// must keep receiving.
void two_clients_that_each_had_one_ck_timeout_both_receive() {
  rtpmidid::rtpserver_t server("server", "0");
  rtpclient_t node_a("node-a");
  rtpclient_t node_b("node-b");
  int got_a = 0;
  int got_b = 0;
  auto midi_a = node_a.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got_a++; });
  auto midi_b = node_b.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got_b++; });

  connect_client(node_a, server);
  connect_client(node_b, server);

  one_ck_timeout(node_a);
  ASSERT_TRUE(run_until([&] { return is_up(node_a); }, 8000ms));
  run_for(300ms);
  log_server_peers("after node-a timeout", server);
  got_a = got_b = 0;
  send_probe_note(server);
  ASSERT_TRUE(run_until([&] { return got_a >= 1 && got_b >= 1; }, 1000ms));

  one_ck_timeout(node_b);
  const bool b_back = run_until([&] { return is_up(node_b); }, 8000ms);
  run_for(300ms);
  log_server_peers("after node-b timeout", server);
  INFO("node-a up={} node-b up={}", is_up(node_a), is_up(node_b));
  ASSERT_TRUE(b_back);

  got_a = got_b = 0;
  send_probe_note(server);
  const bool both = run_until([&] { return got_a >= 1 && got_b >= 1; }, 1000ms);
  INFO("probe note after both timeouts: node-a got {}, node-b got {}", got_a,
       got_b);
  ASSERT_TRUE(both);
}

// One client alone (P1): its session after a first reset ends with its
// goodbyes lost, and it invites again at once, while the server still holds
// the dead session. It must come back and receive.
void one_client_that_has_reset_once_comes_back_after_lost_goodbyes() {
  rtpmidid::rtpserver_t server("server", "0");
  lossy_link_t link(server.port());
  rtpclient_t node("node");
  int got = 0;
  auto midi = node.peer.midi_event.connect(
      [&](const rtpmidid::io_bytes_reader &) { got++; });

  fast_timeouts(node);
  connect_client_to(node, link.port);

  // A first reset, with nothing lost: the node comes back and receives.
  one_ck_timeout(node);
  ASSERT_TRUE(run_until([&] { return is_up(node); }, 8000ms));
  run_for(300ms);
  got = 0;
  send_probe_note(server);
  ASSERT_TRUE(run_until([&] { return got >= 1; }, 1000ms));

  // A second one whose goodbyes the network loses.
  link.drop_goodbyes_to_server = true;
  one_ck_timeout(node);
  run_for(300ms);
  link.drop_goodbyes_to_server = false;
  INFO("goodbyes lost on the way to the server: {}", link.goodbyes_dropped);
  ASSERT_GTE(link.goodbyes_dropped, 1);

  const bool back = run_until([&] { return is_up(node); }, 5000ms);
  log_server_peers("after the second reset", server);
  INFO("node up again within 5 s: {}", back);
  ASSERT_TRUE(back);
  run_for(300ms);
  got = 0;
  send_probe_note(server);
  const bool receives = run_until([&] { return got >= 1; }, 1000ms);
  INFO("probe note after the second reset: {}", got);
  ASSERT_TRUE(receives);
}

// ------------------------------------------- a server that drops packets

// A server written by hand, so that a test can lose chosen datagrams.
struct fake_server_t {
  rtpmidid::udppeer_t control;
  rtpmidid::udppeer_t midi;
  std::string name = "fake server";
  int invitations = 0; // both ports
  int control_invitations = 0;
  int midi_invitations = 0;
  int ck_requests = 0;
  int goodbyes = 0;
  int ignore_invitations = 0; // how many INs to lose, from the first
  int ignore_ck_requests = 0; // how many CK0 to lose, from the first
  int extra_oks = 0;          // how many times to repeat each OK
  std::vector<std::chrono::steady_clock::time_point> invitation_times;
  struct invitation_t {
    bool control;
    uint32_t token;
    uint32_t ssrc;
    std::chrono::steady_clock::time_point when;
  };
  std::vector<invitation_t> seen; // every IN, lost or answered
  rtpmidid::network_address_t midi_from; // the client's MIDI port, last seen
  rtpmidid::udppeer_t::on_read_t::connection_t on_control;
  rtpmidid::udppeer_t::on_read_t::connection_t on_midi;

  fake_server_t(const std::string &port, const std::string &midi_port)
      : control("127.0.0.1", port), midi("127.0.0.1", midi_port) {
    listen();
  }
  // On a free port.
  fake_server_t()
      : control("127.0.0.1", "0"),
        midi("127.0.0.1", uint16_t(control.get_address().port() + 1)) {
    if (!midi.is_open())
      FAIL("the fake server's MIDI port is taken");
    listen();
  }

  int port() { return control.get_address().port(); }

  void listen() {
    on_control = control.on_read.connect(
        [this](const rtpmidid::packet_t &data,
               const rtpmidid::network_address_t &from) {
          handle(control, data, from);
        });
    on_midi = midi.on_read.connect(
        [this](const rtpmidid::packet_t &data,
               const rtpmidid::network_address_t &from) {
          handle(midi, data, from);
        });
  }

  std::vector<uint32_t> control_tokens() const {
    std::vector<uint32_t> tokens;
    for (auto &invitation : seen)
      if (invitation.control)
        tokens.push_back(invitation.token);
    return tokens;
  }

  // A clock-sync reply to the CK0 sent at `ck0`, to the client's MIDI port.
  void send_ck_reply(uint64_t ck0) {
    std::array<uint8_t, 1500> memory{};
    rtpmidid::packet_command_ck_t reply(memory.data(), memory.size());
    reply.initialize()
        .set_count(1)
        .set_sender_ssrc(0xBEEF)
        .set_ck0(ck0)
        .set_ck1(100);
    midi.sendto(reply.as_send_packet(), midi_from);
  }

  void handle(rtpmidid::udppeer_t &socket, const rtpmidid::packet_t &data,
              const rtpmidid::network_address_t &from) {
    const bool on_control_port = &socket == &control;
    if (!on_control_port)
      midi_from = from.dup();
    if (data.get_packet_type() != rtpmidid::packet_type_e::COMMAND)
      return;
    rtpmidid::packet_command_t command(data);
    rtpmidid::network_address_t reply_to = from.dup();

    if (command.get_command() == rtpmidid::command_e::BY) {
      goodbyes++;
      return;
    }
    if (command.get_command() == rtpmidid::command_e::IN) {
      const auto now = std::chrono::steady_clock::now();
      rtpmidid::packet_command_in_ok_t request(data);
      invitations++;
      if (on_control_port)
        control_invitations++;
      else
        midi_invitations++;
      invitation_times.push_back(now);
      seen.push_back({on_control_port, request.get_initiator_token(),
                      request.get_sender_ssrc(), now});
      if (invitations <= ignore_invitations)
        return;
      rtpmidid::packet_managed_t<100> memory;
      rtpmidid::packet_command_in_ok_t reply(memory);
      reply.initialize(rtpmidid::command_e::OK)
          .set_sender_ssrc(0xBEEF)
          .set_initiator_token(request.get_initiator_token())
          .set_name(name);
      for (int i = 0; i <= extra_oks; i++)
        socket.sendto(reply.as_send_packet(), reply_to);
      return;
    }
    if (command.get_command() == rtpmidid::command_e::CK) {
      rtpmidid::packet_command_ck_t request(data);
      if (request.get_count() != 0)
        return;
      ck_requests++;
      if (ck_requests <= ignore_ck_requests)
        return;
      std::array<uint8_t, 1500> memory;
      rtpmidid::packet_command_ck_t reply(memory.data(), memory.size());
      reply.initialize()
          .set_count(1)
          .set_sender_ssrc(0xBEEF)
          .set_ck0(request.get_ck0())
          .set_ck1(100);
      socket.sendto(reply.as_send_packet(), reply_to);
    }
  }
};

static void connect_to_fake(rtpclient_t &client, fake_server_t &server) {
  connect_client_to(client, server.port());
}

// One lost clock-sync datagram must not end the session: the request has to be
// repeated while the client's own timeout has not run out yet.
void one_lost_ck_does_not_end_the_session() {
  fake_server_t server("15421", "15422");
  server.ignore_ck_requests = 1;

  rtpclient_t node("node");
  node.add_server_address("127.0.0.1", "15421");

  ASSERT_TRUE(run_until([&] { return server.ck_requests >= 1; }, 3000ms));
  const bool repeated =
      run_until([&] { return server.ck_requests >= 2; }, 3500ms);
  INFO("CK0 seen={} goodbyes={} client up={}", server.ck_requests,
       server.goodbyes, is_up(node));
  run_for(1500ms); // past the 4 s budget
  INFO("after the budget: CK0 seen={} goodbyes={} client up={}",
       server.ck_requests, server.goodbyes, is_up(node));

  ASSERT_TRUE(repeated);
  ASSERT_EQUAL(server.goodbyes, 0);
  ASSERT_TRUE(is_up(node));
}

// A lost invitation (the network is still down) must be repeated within about
// a second, not after the connect timeout plus the 30 s retry. The client runs
// with its built-in timeouts: they are what is being judged.
void a_lost_invitation_is_repeated_quickly() {
  fake_server_t server("15431", "15432");
  server.ignore_invitations = 1;

  rtpclient_t node("node");
  node.add_server_address("127.0.0.1", "15431");

  ASSERT_TRUE(run_until([&] { return server.invitations >= 1; }, 2000ms));
  const bool repeated =
      run_until([&] { return server.invitations >= 2; }, 2500ms);
  INFO("invitations seen in 2.5 s after the lost one: {}",
       server.invitations - 1);
  ASSERT_TRUE(repeated);
}

// While the network stays down the client keeps inviting, about once a
// second, neither slower nor in a burst. Built-in timers: nothing shortened.
void invitations_keep_coming_once_a_second_with_the_built_in_timers() {
  fake_server_t server;
  server.ignore_invitations = INT_MAX;

  rtpclient_t node("node");
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  ASSERT_TRUE(
      run_until([&] { return server.control_invitations >= 1; }, 2000ms));
  const auto first = server.seen[0].when;
  run_for(35000ms);

  std::vector<std::chrono::steady_clock::time_point> times;
  for (auto &invitation : server.seen)
    if (invitation.control && invitation.when - first <= 35000ms)
      times.push_back(invitation.when);
  long max_gap_ms = 0;
  long min_gap_ms = 35000;
  for (size_t i = 1; i < times.size(); i++) {
    const long gap = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                              times[i] - times[i - 1])
                              .count());
    max_gap_ms = std::max(max_gap_ms, gap);
    min_gap_ms = std::min(min_gap_ms, gap);
  }
  INFO("control invitations in 35 s: {}; gaps between them: {} to {} ms",
       times.size(), min_gap_ms, max_gap_ms);
  ASSERT_GTE(times.size(), size_t(30));
  ASSERT_LTE(max_gap_ms, 1500L);
  ASSERT_GTE(min_gap_ms, 900L);
}

// A server that answers every invitation twice (a repeated OK, as after a
// repeated IN): the client connects once and stays connected, in both phases.
void a_repeated_ok_is_ignored_in_both_phases() {
  fake_server_t server;
  server.extra_oks = 1;

  rtpclient_t node("node");
  fast_timeouts(node);
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  const bool up = run_until([&] { return is_up(node); }, 2000ms);
  const bool stayed = up && stays_up(node, 1500ms);
  INFO("up={} stayed up={} control INs={} MIDI INs={} goodbyes={}", up, stayed,
       server.control_invitations, server.midi_invitations, server.goodbyes);
  ASSERT_TRUE(up);
  ASSERT_TRUE(stayed);
  ASSERT_EQUAL(server.goodbyes, 0);
  ASSERT_EQUAL(server.control_invitations, 1);
  ASSERT_EQUAL(server.midi_invitations, 1);
}

// The client's loop stalls with a clock-sync reply already waiting in its
// socket and its own deadline passed meanwhile (P3). The reply is there: the
// session must stay.
void a_ck_reply_queued_behind_an_expired_timer_keeps_the_session() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  connect_to_fake(node, server);
  server.ignore_ck_requests = INT_MAX; // only the reply below answers

  send_tap_t sent(node);
  node.handle_event(rtpclient_t::SendCK);
  ASSERT_GTE(sent.ck0_sent, 1);
  server.send_ck_reply(sent.last_ck0); // queued at the client now
  const int invitations = server.control_invitations;

  // The loop does not turn for longer than any one wait for a reply.
  std::this_thread::sleep_for(2000ms);

  const bool stayed = stays_up(node, 1500ms);
  INFO("after the stall: stayed up={} goodbyes={} new control INs={}", stayed,
       server.goodbyes, server.control_invitations - invitations);
  ASSERT_EQUAL(server.goodbyes, 0);
  ASSERT_EQUAL(server.control_invitations, invitations);
  ASSERT_TRUE(stayed);
}

// A clock-sync reply that arrives after its session has ended lands while the
// next handshake waits for its OK (a configured local port makes every attempt
// use the same ports). It must change nothing: the handshake completes as soon
// as the server answers, with no second attempt.
void a_late_ck_reply_during_a_handshake_changes_nothing() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  node.local_base_port_str = std::to_string(free_port_pair());
  connect_to_fake(node, server);

  send_tap_t sent(node);
  one_ck_timeout(node); // the new invitation is on its way
  ASSERT_GTE(sent.ck0_sent, 1);
  server.send_ck_reply(sent.last_ck0); // the late reply

  const bool up = run_until([&] { return is_up(node); }, 1500ms);
  INFO("up again within 1.5 s: {}; control INs seen by the server: {}", up,
       server.control_invitations);
  ASSERT_TRUE(up);
  ASSERT_EQUAL(server.control_invitations, 2);
}

// A clock sync that is never answered ends the session, and the invitation
// that follows carries a token of its own: not 0, not the old one (P1).
void an_exhausted_clock_sync_ends_the_session_and_the_next_invitation_has_a_new_token() {
  fake_server_t server;
  server.ignore_ck_requests = INT_MAX;

  rtpclient_t node("node");
  fast_timeouts(node);
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  ASSERT_TRUE(run_until([&] { return is_up(node); }, 2000ms));
  ASSERT_EQUAL(server.control_invitations, 1);

  const bool invited_again =
      run_until([&] { return server.control_invitations >= 2; }, 20000ms);
  ASSERT_TRUE(invited_again);
  const auto tokens = server.control_tokens();
  INFO("token of the first session {:08X}, of the next invitation {:08X}",
       tokens[0], tokens[1]);
  ASSERT_NOT_EQUAL(tokens[1], 0u);
  ASSERT_NOT_EQUAL(tokens[1], tokens[0]);
}

// The SSRC a client sends uses all 32 bits: sixteen clients, and at least one
// SSRC above 0xFFFF (with 32 random bits, all sixteen below it has a chance of
// 2^-256). None is 0.
void the_client_ssrc_is_not_limited_to_16_bits() {
  fake_server_t server;
  server.ignore_invitations = INT_MAX;

  std::vector<std::unique_ptr<rtpclient_t>> nodes;
  for (int i = 0; i < 16; i++) {
    nodes.push_back(std::make_unique<rtpclient_t>("node-" + std::to_string(i)));
    fast_timeouts(*nodes.back());
    nodes.back()->add_server_address("127.0.0.1",
                                     std::to_string(server.port()));
  }
  ASSERT_TRUE(
      run_until([&] { return server.control_invitations >= 16; }, 3000ms));

  int wide = 0;
  int zero = 0;
  for (auto &invitation : server.seen) {
    if (!invitation.control)
      continue;
    wide += invitation.ssrc > 0xFFFF;
    zero += invitation.ssrc == 0;
  }
  INFO("of {} invitations, {} carry an SSRC above 0xFFFF, {} an SSRC of 0",
       server.control_invitations, wide, zero);
  ASSERT_GT(wide, 0);
  ASSERT_EQUAL(zero, 0);
}

// Of two endpoints given for a server, the one in use stops answering: the
// client comes back through the other. Two servers written by hand stand for
// the two endpoints, so that only the client's walk is judged.
void control_the_client_comes_back_through_another_endpoint() {
  fake_server_t first;
  first.name = "server-a";
  fake_server_t second;
  second.name = "server-b";

  rtpclient_t node("node");
  fast_timeouts(node);
  node.ck_short_period = 1000ms;
  node.ck_long_period = 1000ms;
  std::vector<rtpclient_t::endpoint_t> endpoints{
      {"127.0.0.1", std::to_string(first.port())},
      {"127.0.0.1", std::to_string(second.port())}};
  node.add_server_addresses(endpoints);
  ASSERT_TRUE(run_until([&] { return is_up(node); }, 3000ms));

  fake_server_t &in_use = node.peer.remote_name == first.name ? first : second;
  fake_server_t &other = &in_use == &first ? second : first;
  INFO("connected through {}; it stops answering", in_use.name);
  in_use.ignore_invitations = INT_MAX;
  in_use.ignore_ck_requests = INT_MAX;

  const bool back = run_until(
      [&] { return is_up(node) && node.peer.remote_name == other.name; },
      40000ms);
  INFO("back through {}: {}", other.name, back);
  ASSERT_TRUE(back);
}

// ------------------------------------------------- the server's peer table

static rtpmidid::io_bytes_managed invitation(const char *initiator_hex,
                                             const char *ssrc_hex,
                                             const char *name) {
  return hex_to_bin(std::string("FF FF 'IN' 0000 0002 ") + initiator_hex + " " +
                    ssrc_hex + " '" + name + "' 00");
}

// A retransmitted invitation must not look like a new connection. In the
// daemon every CONNECTED event creates an ALSA port (1amfwm7h80b).
void a_repeated_invitation_does_not_connect_again() {
  rtpmidid::rtpserver_t server("server", "0");
  int connected_events = 0;
  auto on_status = server.status_change_event.connect(
      [&](std::shared_ptr<rtppeer_t>, rtppeer_t::status_e status) {
        if (status == rtppeer_t::status_e::CONNECTED)
          connected_events++;
      });
  test_client_t control(0, server.port());
  test_client_t midi(control.local_port + 1, server.port() + 1);

  control.send(invitation("00 12 34 00", "00 BE EF 00", "node"));
  midi.send(invitation("00 12 34 00", "00 BE EF 00", "node"));
  run_for(100ms);
  ASSERT_EQUAL(connected_events, 1);

  midi.send(invitation("00 12 34 00", "00 BE EF 00", "node"));
  run_for(100ms);
  INFO("CONNECTED events after one repeated invitation: {}", connected_events);
  ASSERT_EQUAL(server.peers.size(), 1);
  ASSERT_EQUAL(connected_events, 1);
}

// The same on the control port: an invitation repeated before the MIDI one
// (its OK was lost) and again once the session is up. Each is answered, and
// the session connects once.
void a_repeated_control_invitation_does_not_connect_again() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t node("127.0.0.1", server.port(), 0x00123400, 0x00BEEF00);

  node.invite(rtppeer_t::CONTROL_PORT);
  run_for(50ms);
  node.invite(rtppeer_t::CONTROL_PORT);
  run_for(50ms);
  node.invite(rtppeer_t::MIDI_PORT);
  run_for(50ms);
  ASSERT_EQUAL(events.connected(), 1);
  node.invite(rtppeer_t::CONTROL_PORT);
  run_for(50ms);
  node.pump();

  INFO("CONNECTED events {}, disconnects {}, OKs on the control port {}",
       events.connected(), events.disconnects(), node.control_in.ok);
  ASSERT_EQUAL(events.connected(), 1);
  ASSERT_EQUAL(events.disconnects(), 0);
  ASSERT_EQUAL(node.control_in.ok, 3);
  ASSERT_EQUAL(server.peers.size(), 1u);
}

// Both goodbyes of an old session are lost and the node comes back on new
// ports with the same SSRC. Its clock-sync request must be answered on the new
// ports, not on the old ones.
void a_new_session_gets_its_ck_reply_after_lost_goodbyes() {
  rtpmidid::rtpserver_t server("server", "0");

  test_client_t old_control(0, server.port());
  test_client_t old_midi(old_control.local_port + 1, server.port() + 1);
  old_control.send(invitation("00 12 34 00", "00 BE EF 00", "node"));
  old_midi.send(invitation("00 12 34 00", "00 BE EF 00", "node"));
  run_for(100ms);

  test_client_t new_control(0, server.port());
  test_client_t new_midi(new_control.local_port + 1, server.port() + 1);
  new_control.send(invitation("00 56 78 00", "00 BE EF 00", "node"));
  new_midi.send(invitation("00 56 78 00", "00 BE EF 00", "node"));
  run_for(100ms);
  log_server_peers("old session never said goodbye", server);

  auto drain_for_ck = [](test_client_t &socket) {
    bool seen = false;
    uint8_t buffer[1500];
    for (;;) {
      auto length =
          ::recv(socket.sockfd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (length <= 0)
        return seen;
      if (length >= 4 && buffer[2] == 'C' && buffer[3] == 'K')
        seen = true;
    }
  };
  drain_for_ck(old_midi);
  drain_for_ck(new_midi);

  new_midi.send(hex_to_bin("FF FF 'CK'"
                           "00 BE EF 00" // sender SSRC
                           "00 000000"   // count 0, padding
                           "0000 0000 0000 0001"
                           "0000 0000 0000 0000"
                           "0000 0000 0000 0000"));
  run_for(200ms);
  const bool on_new = drain_for_ck(new_midi);
  const bool on_old = drain_for_ck(old_midi);
  INFO("CK reply on the new ports: {}, on the old ports: {}", on_new, on_old);
  ASSERT_TRUE(on_new);
  ASSERT_FALSE(on_old);
}

// Another host sends the very invitation of a node's session (same token,
// same SSRC), on both ports. The node's session sees nothing of it, and the
// other host gets a session of its own: two hosts, two sessions.
void another_address_does_not_take_a_session() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t node("127.0.0.1", server.port(), 0x00123400, 0x00BEEF00);
  node.connect();
  ASSERT_EQUAL(node.control_in.ok, 1);
  ASSERT_EQUAL(node.midi_in.ok, 1);
  const rtppeer_t *session = events.first_connected();
  ASSERT_TRUE(session != nullptr);
  const int session_events = events.events_of(session);

  wire_session_t other("127.0.0.2", server.port(), node.token, node.ssrc);
  other.connect();
  node.pump();
  INFO("events for the node's session after the other host's invitation: {} "
       "(before: {}); OKs at the other host: control {}, MIDI {}",
       events.events_of(session), session_events, other.control_in.ok,
       other.midi_in.ok);
  ASSERT_EQUAL(events.events_of(session), session_events);
  ASSERT_EQUAL(other.control_in.ok, 1);
  ASSERT_EQUAL(other.midi_in.ok, 1);

  send_probe_note(server);
  run_for(100ms);
  node.pump();
  other.pump();
  INFO("MIDI received: node {}, other host {}", node.midi_in.midi,
       other.midi_in.midi);
  ASSERT_GTE(node.midi_in.midi, 1);
  ASSERT_GTE(other.midi_in.midi, 1);

  node.ck0();
  run_for(100ms);
  node.pump();
  ASSERT_GTE(node.midi_in.ck_replies, 1);
}

// Another host opens a session with the SSRC of a node's session and then
// says goodbye on both ports (AC10). The node's session goes on.
void a_goodbye_from_another_address_does_not_end_a_session() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t node("127.0.0.1", server.port(), 0x00123400, 0x00BEEF00);
  node.connect();
  const rtppeer_t *session = events.first_connected();
  ASSERT_TRUE(session != nullptr);

  wire_session_t other("127.0.0.2", server.port(), 0x00567800, node.ssrc);
  other.connect();
  other.goodbye(rtppeer_t::MIDI_PORT);
  other.goodbye(rtppeer_t::CONTROL_PORT);
  run_for(100ms);
  log_server_peers("after the other host's goodbye", server);
  INFO("disconnects reported for the node's session: {}",
       events.disconnects_of(session));
  ASSERT_EQUAL(events.disconnects_of(session), 0);

  node.pump();
  const int midi_before = node.midi_in.midi;
  send_probe_note(server);
  run_for(100ms);
  node.pump();
  INFO("MIDI at the node after the goodbye: {}", node.midi_in.midi - midi_before);
  ASSERT_GTE(node.midi_in.midi - midi_before, 1);

  node.ck0();
  run_for(100ms);
  node.pump();
  ASSERT_GTE(node.midi_in.ck_replies, 1);
}

// Two sessions from one address (one host, two sessions: the announce server
// and an export, say), same name, their own SSRCs and ports. Both live, each
// gets its own clock-sync replies.
void control_two_sessions_from_one_address_coexist() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t first("127.0.0.1", server.port(), 0x00123400, 0x00BEEF00);
  wire_session_t second("127.0.0.1", server.port(), 0x00567800, 0x00DEEB00);
  first.connect();
  second.connect();
  ASSERT_EQUAL(events.connected(), 2);
  ASSERT_EQUAL(events.disconnects(), 0);
  ASSERT_EQUAL(first.midi_in.ok, 1);
  ASSERT_EQUAL(second.midi_in.ok, 1);

  send_probe_note(server);
  run_for(100ms);
  first.pump();
  second.pump();
  ASSERT_GTE(first.midi_in.midi, 1);
  ASSERT_GTE(second.midi_in.midi, 1);

  first.ck0();
  run_for(100ms);
  first.pump();
  second.pump();
  ASSERT_EQUAL(first.midi_in.ck_replies, 1);
  ASSERT_EQUAL(second.midi_in.ck_replies, 0);
  second.ck0();
  run_for(100ms);
  first.pump();
  second.pump();
  ASSERT_EQUAL(first.midi_in.ck_replies, 1);
  ASSERT_EQUAL(second.midi_in.ck_replies, 1);
  ASSERT_EQUAL(events.disconnects(), 0);
}

// A node comes back on new ports with the SSRC of its old session (an old
// client after lost goodbyes). Its new control invitation replaces the old
// session, which reports its disconnect: in the daemon that removes the old
// session's ALSA port. MIDI then goes to the new ports only.
void a_replaced_session_fires_its_disconnect_status() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t old_session("127.0.0.1", server.port(), 0x00123400,
                             0x00BEEF00);
  old_session.connect();
  const rtppeer_t *old_peer = events.first_connected();
  ASSERT_TRUE(old_peer != nullptr);

  wire_session_t new_session("127.0.0.1", server.port(), 0x00567800,
                             0x00BEEF00);
  new_session.invite(rtppeer_t::CONTROL_PORT);
  run_for(200ms);
  log_server_peers("after the new control invitation", server);
  INFO("disconnects reported for the old session: {}",
       events.disconnects_of(old_peer));
  ASSERT_GTE(events.disconnects_of(old_peer), 1);

  new_session.invite(rtppeer_t::MIDI_PORT);
  run_for(100ms);
  new_session.pump();
  old_session.pump();
  const int old_midi = old_session.midi_in.midi;
  send_probe_note(server);
  run_for(100ms);
  new_session.pump();
  old_session.pump();
  INFO("MIDI after the replacement: new ports {}, old ports {}",
       new_session.midi_in.midi, old_session.midi_in.midi - old_midi);
  ASSERT_GTE(new_session.midi_in.midi, 1);
  ASSERT_EQUAL(old_session.midi_in.midi, old_midi);
}

// An old session's goodbyes, delayed, arrive after the node's new control
// invitation (same SSRC, new ports). They end nothing of the new session.
void control_an_old_goodbye_after_the_new_invitation_does_not_touch_the_new_session() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t old_session("127.0.0.1", server.port(), 0x00123400,
                             0x00BEEF00);
  old_session.connect();
  const rtppeer_t *old_peer = events.first_connected();
  ASSERT_TRUE(old_peer != nullptr);

  wire_session_t new_session("127.0.0.1", server.port(), 0x00567800,
                             0x00BEEF00);
  new_session.invite(rtppeer_t::CONTROL_PORT);
  run_for(50ms);
  old_session.goodbye(rtppeer_t::MIDI_PORT);
  old_session.goodbye(rtppeer_t::CONTROL_PORT);
  run_for(50ms);
  new_session.invite(rtppeer_t::MIDI_PORT);
  run_for(50ms);
  new_session.pump();
  old_session.pump();
  log_server_peers("after the late goodbyes", server);
  INFO("new session: OKs control {} MIDI {}; disconnects of other sessions "
       "than the old one: {}",
       new_session.control_in.ok, new_session.midi_in.ok,
       events.disconnects_except(old_peer));
  ASSERT_EQUAL(new_session.control_in.ok, 1);
  ASSERT_EQUAL(new_session.midi_in.ok, 1);
  ASSERT_EQUAL(events.disconnects_except(old_peer), 0);

  const int old_midi = old_session.midi_in.midi;
  send_probe_note(server);
  run_for(100ms);
  new_session.pump();
  old_session.pump();
  ASSERT_GTE(new_session.midi_in.midi, 1);
  ASSERT_EQUAL(old_session.midi_in.midi, old_midi);

  new_session.ck0();
  run_for(100ms);
  new_session.pump();
  ASSERT_GTE(new_session.midi_in.ck_replies, 1);
}

// An invitation that arrives on the MIDI port only, and nothing else: the
// session never completes, and it is gone after 5 s (the server's own timer,
// built in).
void a_midi_only_session_is_gone_after_5_s() {
  rtpmidid::rtpserver_t server("server", "0");
  wire_session_t node("127.0.0.1", server.port(), 0x00123400, 0x00BEEF00);
  node.invite(rtppeer_t::MIDI_PORT);
  run_for(100ms);
  log_server_peers("MIDI invitation only", server);

  const bool gone = run_until([&] { return server.peers.empty(); }, 7000ms);
  log_server_peers("7 s later", server);
  ASSERT_TRUE(gone);
}

// Sessions that go silent expire after the server's 60 s (built in), whatever
// other sessions are opened or closed meanwhile (P7). Here a session is
// opened after each, and the first one says goodbye.
void a_session_still_expires_when_other_sessions_come_and_go() {
  rtpmidid::rtpserver_t server("server", "0");
  server_events_t events(server);
  wire_session_t first("127.0.0.1", server.port(), 0x00111100, 0x00AAAA00);
  wire_session_t silent_a("127.0.0.1", server.port(), 0x00222200, 0x00BBBB00);
  wire_session_t silent_b("127.0.0.1", server.port(), 0x00333300, 0x00CCCC00);
  first.connect();
  silent_a.connect();
  silent_b.connect();
  const auto peers = events.connected_peers();
  ASSERT_EQUAL(peers.size(), size_t(3));
  const rtppeer_t *peer_a = peers[1];
  const rtppeer_t *peer_b = peers[2];

  first.goodbye(rtppeer_t::MIDI_PORT);
  first.goodbye(rtppeer_t::CONTROL_PORT);
  run_for(100ms);
  ASSERT_GTE(events.disconnects_of(peers[0]), 1);

  // From here on silent_a and silent_b send nothing, not even a clock sync.
  const bool expired = run_until(
      [&] {
        return events.disconnects_of(peer_a) >= 1 &&
               events.disconnects_of(peer_b) >= 1;
      },
      75000ms);
  log_server_peers("75 s of silence", server);
  INFO("expired: the second session {}, the third {}",
       events.disconnects_of(peer_a) >= 1, events.disconnects_of(peer_b) >= 1);
  ASSERT_TRUE(expired);
}

// ------------------------------------------- long run, built-in timeouts
//
// Opt-in: set RTPM_SOAK_SECONDS to the time to watch after the last timeout.
// Nothing is shortened: 10 s connect timeout, 30 s reconnect, CK every 2 s and
// then every 25 s, server peers removed after 60 s without CK. One probe note
// a second tells whether each client is receiving. Lines start with SOAK.

static int soak_seconds() {
  const char *value = std::getenv("RTPM_SOAK_SECONDS");
  return value ? std::atoi(value) : 0;
}

struct soak_node_t {
  rtpclient_t client;
  const char *label;
  int received = 0;
  int received_in_window = 0;
  int last_received_s = -1;
  bool was_up = false;
  rtppeer_t::midi_event_t::connection_t on_midi;
  explicit soak_node_t(const char *name) : client(name), label(name) {
    on_midi = client.peer.midi_event.connect(
        [this](const rtpmidid::io_bytes_reader &) { received++; });
  }
};

struct soak_t {
  rtpmidid::rtpserver_t server{"server", "0"};
  std::vector<soak_node_t *> nodes;
  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  int ticks = 0;

  int now_s() const {
    return int(std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
  }

  void connect(soak_node_t &node) {
    node.client.add_server_address("127.0.0.1", std::to_string(server.port()));
    ASSERT_TRUE(run_until([&] { return is_up(node.client); }, 15000ms));
    node.was_up = true;
    nodes.push_back(&node);
    INFO("SOAK t={}s {} connected", now_s(), node.label);
  }

  // Send one probe a second for `seconds`, reporting every change.
  void watch(int seconds) {
    const int end = now_s() + seconds;
    while (now_s() < end) {
      std::vector<int> before;
      for (auto *node : nodes)
        before.push_back(node->received);
      send_probe_note(server);
      run_for(1000ms);
      ticks++;
      for (size_t i = 0; i < nodes.size(); i++) {
        auto *node = nodes[i];
        if (node->received > before[i]) {
          node->received_in_window++;
          node->last_received_s = now_s();
        }
        if (is_up(node->client) != node->was_up) {
          node->was_up = is_up(node->client);
          INFO("SOAK t={}s {} is now {}", now_s(), node->label,
               node->was_up ? "UP" : "DOWN");
        }
      }
      if (ticks % 30 == 0) {
        for (auto *node : nodes) {
          INFO("SOAK t={}s {} up={} probes received in the last 30: {} (last "
               "at t={}s)",
               now_s(), node->label, is_up(node->client),
               node->received_in_window, node->last_received_s);
          node->received_in_window = 0;
        }
        INFO("SOAK t={}s server peers: {}", now_s(), server.peers.size());
      }
    }
  }

  void inject_ck_timeout(soak_node_t &node) {
    auto waiting = [&] {
      return node.client.state == rtpclient_t::state_e::WaitSendCkShort ||
             node.client.state == rtpclient_t::state_e::WaitSendCkLong;
    };
    ASSERT_TRUE(run_until(waiting, 30000ms));
    INFO("SOAK t={}s injecting one CK timeout on {}", now_s(), node.label);
    node.client.handle_event(rtpclient_t::SendCK);
    node.client.handle_event(rtpclient_t::Timeout);
  }

  void verdict() {
    log_server_peers("SOAK end", server);
    bool all = true;
    for (auto *node : nodes) {
      const bool receiving =
          node->last_received_s >= 0 && now_s() - node->last_received_s <= 5;
      INFO("SOAK verdict t={}s {}: up={} receiving={} last probe at t={}s",
           now_s(), node->label, is_up(node->client), receiving,
           node->last_received_s);
      all = all && receiving && is_up(node->client);
    }
    ASSERT_TRUE(all);
  }
};

void soak_control_two_clients_without_timeouts() {
  soak_t soak;
  soak_node_t a("node-a");
  soak_node_t b("node-b");
  soak.connect(a);
  soak.connect(b);
  soak.watch(90 + soak_seconds());
  soak.verdict();
}

void soak_control_one_client_one_timeout() {
  soak_t soak;
  soak_node_t a("node-a");
  soak.connect(a);
  soak.watch(20);
  soak.inject_ck_timeout(a);
  soak.watch(70 + soak_seconds());
  soak.verdict();
}

void soak_two_clients_one_timeout_each() {
  soak_t soak;
  soak_node_t a("node-a");
  soak_node_t b("node-b");
  soak.connect(a);
  soak.connect(b);
  soak.watch(20);
  soak.inject_ck_timeout(a);
  soak.watch(90); // node-a is long stable when node-b has its own
  soak.inject_ck_timeout(b);
  soak.watch(soak_seconds());
  soak.verdict();
}

// ------------------------------------------------------- fix-only tests
//
// AC2 (b): they need what the fix adds (tick and tries, the lookup hook and
// count, replace_server_addresses, rtpserver_t::is_open) and claim nothing
// about the unfixed library.

// A repeating timer of the test's own: how often the loop turned while the
// client was failing. A loop stalled by the client would starve it.
struct loop_ticker_t {
  int ticks = 0;
  rtpmidid::poller_t::timer_t timer;
  void arm() {
    timer = rtpmidid::poller.add_timer_event(100ms, [this] { on_tick(); });
  }
  void on_tick() {
    ticks++;
    arm();
  }
  loop_ticker_t() { arm(); }
};

// Tries made by a client: every IN and CK0 it puts on the wire, sent or not.
struct try_tap_t {
  int invitations = 0;
  int ck0 = 0;
  std::vector<std::chrono::steady_clock::time_point> times;
  rtppeer_t::send_event_t::connection_t connection;
  explicit try_tap_t(rtpclient_t &client) {
    connection = client.peer.send_event.connect(
        [this](const rtpmidid::io_bytes_reader &data, rtppeer_t::port_e) {
          const uint8_t *bytes = data.start;
          if (data.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xFF)
            return;
          if (bytes[2] == 'I' && bytes[3] == 'N')
            invitations++;
          else if (bytes[2] == 'C' && bytes[3] == 'K' && bytes[8] == 0)
            ck0++;
          else
            return;
          times.push_back(std::chrono::steady_clock::now());
        });
  }
  long min_gap_ms() const {
    long gap = LONG_MAX;
    for (size_t i = 1; i < times.size(); i++)
      gap = std::min(gap, long(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(times[i] -
                                                              times[i - 1])
                                   .count()));
    return gap;
  }
};

// The once-a-minute line, with its minute shortened to `period`: how many
// times it was written while watching for `time`.
static int still_no_session_lines(rtpclient_t &client,
                                  std::chrono::milliseconds time) {
  int lines = 0;
  auto last = client.last_still_no_session;
  run_until(
      [&] {
        if (client.last_still_no_session != last) {
          last = client.last_still_no_session;
          lines++;
        }
        return false;
      },
      time);
  return lines;
}

// A failure that does not wait must still cost one try a second at most, keep
// the loop turning, and write one line per (shortened) minute.
static void judge_failing_client(rtpclient_t &client, const char *what) {
  client.still_no_session_period = 2000ms;
  try_tap_t tries(client);
  loop_ticker_t ticker;
  const int lines = still_no_session_lines(client, 6500ms);
  INFO("{}: {} invitations in 6.5 s, closest two {} ms apart; the loop ticked "
       "{} times of 65; 'Still no session' lines: {} (period 2 s); last error: "
       "{}",
       what, tries.invitations, tries.min_gap_ms(), ticker.ticks, lines,
       client.last_error);
  ASSERT_LTE(tries.invitations, 8);
  if (tries.invitations > 1)
    ASSERT_GTE(tries.min_gap_ms(), 900L);
  ASSERT_GTE(ticker.ticks, 55);
  ASSERT_LTE(lines, 4);
  ASSERT_FALSE(client.last_error.empty());
}

void a_name_with_no_answer_costs_one_try_a_second_at_most() {
  rtpclient_t node("node");
  int lookups = 0;
  node.lookup_name = [&](const std::string &name, const std::string &,
                         std::string &error) {
    lookups++;
    error = "lookup of " + name + " failed: no answer";
    return std::vector<std::string>{};
  };
  node.add_server_address("controller.invalid", "5004");
  judge_failing_client(node, "a name with no answer");
  // and the name is looked up once in those 6.5 s (no sooner than 40 s, C6)
  ASSERT_EQUAL(lookups, 1);
}

// Sending to the broadcast address without SO_BROADCAST: the kernel refuses
// every sendto (EACCES), the way an output firewall rule does.
void a_destination_the_kernel_refuses_costs_one_try_a_second_at_most() {
  rtpclient_t node("node");
  node.add_server_address("255.255.255.255", "5004");
  judge_failing_client(node, "a destination refused at sendto");
}

void a_local_port_that_cannot_be_bound_costs_one_try_a_second_at_most() {
  const int port = free_port_pair();
  rtpmidid::udppeer_t holder("::", std::to_string(port)); // takes it
  ASSERT_TRUE(holder.is_open());
  fake_server_t server;
  rtpclient_t node("node");
  node.local_base_port_str = std::to_string(port);
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  judge_failing_client(node, "a local port that cannot be bound");
  ASSERT_EQUAL(server.invitations, 0);
}

// Of two endpoints, the first refuses every send: it is left after one second
// and the second one is reached.
void an_endpoint_whose_send_fails_is_left_after_one_second() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  node.add_server_addresses({{"255.255.255.255", "5004"},
                             {"127.0.0.1", std::to_string(server.port())}});
  const auto start = std::chrono::steady_clock::now();
  const bool up = run_until([&] { return is_up(node); }, 4000ms);
  const long took = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count());
  INFO("up through the second endpoint: {} after {} ms", up, took);
  ASSERT_TRUE(up);
  ASSERT_LTE(took, 2500L);
}

void the_first_midi_invitation_leaves_with_the_control_ok() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  ASSERT_TRUE(run_until([&] { return is_up(node); }, 2000ms));
  ASSERT_GTE(server.seen.size(), size_t(2));
  const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                       server.seen[1].when - server.seen[0].when)
                       .count();
  INFO("control IN to MIDI IN: {} ms", gap);
  ASSERT_FALSE(server.seen[1].control);
  ASSERT_LT(gap, 300);
}

// C6: a client given addresses makes no name lookup, through a connection, a
// clock-sync verdict and a reconnection.
void a_client_given_addresses_looks_no_name_up() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  int hook_lookups = 0;
  node.lookup_name = [&](const std::string &, const std::string &,
                         std::string &) {
    hook_lookups++;
    return std::vector<std::string>{};
  };
  const unsigned before = rtpmidid::network_address_list_t::name_lookups();
  connect_to_fake(node, server);
  one_ck_timeout(node);
  ASSERT_TRUE(run_until([&] { return is_up(node); }, 5000ms));
  const unsigned after = rtpmidid::network_address_list_t::name_lookups();
  INFO("name lookups: {} by getaddrinfo, {} through the hook", after - before,
       hook_lookups);
  ASSERT_EQUAL(after - before, 0u);
  ASSERT_EQUAL(hook_lookups, 0);
}

// C6: a name is looked up when the client starts, and again only after every
// address of its answer failed and 40 s have passed. With the built-in 40 s:
// one lookup in 15 s of failing attempts.
void a_name_is_looked_up_no_sooner_than_40_s_after_the_last() {
  fake_server_t server;
  server.ignore_invitations = INT_MAX;
  rtpclient_t node("node");
  int lookups = 0;
  node.lookup_name = [&](const std::string &, const std::string &,
                         std::string &) {
    lookups++;
    return std::vector<std::string>{"127.0.0.1"};
  };
  node.add_server_address("controller.test", std::to_string(server.port()));
  run_for(15000ms);
  INFO("lookups in 15 s: {}; invitations the server saw: {}", lookups,
       server.control_invitations);
  ASSERT_EQUAL(lookups, 1);
  ASSERT_GTE(server.control_invitations, 12);
}

// C6: a lookup that fails keeps the last good answer: the client goes on
// inviting the address it had. The interval is shortened to 2 s here.
void a_failed_lookup_keeps_the_last_good_answer() {
  fake_server_t server;
  server.ignore_invitations = INT_MAX;
  rtpclient_t node("node");
  node.name_lookup_interval = 2000ms;
  node.control_tries = 2; // an attempt fails after 2 s, so lookups come due
  int lookups = 0;
  node.lookup_name = [&](const std::string &name, const std::string &,
                         std::string &error) {
    lookups++;
    if (lookups == 1)
      return std::vector<std::string>{"127.0.0.1"};
    error = "lookup of " + name + " failed: no answer";
    return std::vector<std::string>{};
  };
  node.add_server_address("controller.test", std::to_string(server.port()));
  ASSERT_TRUE(run_until([&] { return lookups >= 2; }, 8000ms));
  const int invitations = server.control_invitations;
  run_for(3000ms);
  INFO("lookups {}; invitations after the failed lookup: {}", lookups,
       server.control_invitations - invitations);
  ASSERT_GTE(server.control_invitations - invitations, 2);
}

// A live client whose endpoint is replaced invites the new one at its next
// attempt, and never the old one again.
void a_replaced_endpoint_is_invited_at_the_next_attempt() {
  fake_server_t old_server;
  old_server.name = "old";
  fake_server_t new_server;
  new_server.name = "new";
  rtpclient_t node("node");
  fast_timeouts(node);
  node.ck_short_period = 1000ms;
  node.ck_long_period = 1000ms;
  connect_to_fake(node, old_server);

  node.replace_server_addresses({{"127.0.0.1", std::to_string(new_server.port())}});
  old_server.ignore_ck_requests = INT_MAX;
  old_server.ignore_invitations = INT_MAX;
  const int old_invitations = old_server.control_invitations;
  const bool moved = run_until(
      [&] { return is_up(node) && node.peer.remote_name == "new"; }, 20000ms);
  INFO("up on the new endpoint: {}; invitations to the old one after the "
       "replacement: {}",
       moved, old_server.control_invitations - old_invitations);
  ASSERT_TRUE(moved);
  ASSERT_EQUAL(old_server.control_invitations, old_invitations);
}

// D10: the clock sync goes unanswered while the server's MIDI still arrives
// (a loss towards the server only): the session stays. When the MIDI stops
// too, the verdict comes.
void midi_from_the_server_postpones_the_clock_sync_verdict() {
  fake_server_t server;
  rtpclient_t node("node");
  fast_timeouts(node);
  node.ck_short_period = 500ms;
  connect_to_fake(node, server);
  server.ignore_ck_requests = INT_MAX;

  bool send = true;
  uint16_t seq = 0;
  auto send_note = [&] {
    std::vector<uint8_t> packet{0x80, 0x61, uint8_t(seq >> 8), uint8_t(seq)};
    seq++;
    put32(packet, 0);      // timestamp
    put32(packet, 0xBEEF); // the server's SSRC (fake_server_t answers with it)
    packet.insert(packet.end(), {0x03, 0x90, 0x3C, 0x7F});
    server.midi.sendto(rtpmidid::packet_t(packet.data(), packet.size()),
                       server.midi_from);
  };
  rtpmidid::poller_t::timer_t note_timer;
  std::function<void()> arm = [&] {
    note_timer = rtpmidid::poller.add_timer_event(200ms, [&] {
      if (send)
        send_note();
      arm();
    });
  };
  arm();

  // A verdict and a reconnection to this server take milliseconds: counted
  // from the status events, not by watching is_up().
  int downs = 0;
  auto on_status = node.peer.status_change_event.connect(
      [&](rtppeer_t::status_e status) {
        if (rtppeer_t::is_disconnected(status))
          downs++;
      });
  run_for(15000ms);
  INFO("clock sync unanswered for 15 s with MIDI arriving: sessions ended {}, "
       "CK0 seen {}",
       downs, server.ck_requests);
  ASSERT_EQUAL(downs, 0);
  ASSERT_TRUE(is_up(node));
  ASSERT_GTE(server.ck_requests, 12);

  send = false;
  const bool ended = run_until([&] { return downs > 0; }, 14000ms);
  INFO("MIDI stopped: the session ended within 14 s: {}", ended);
  note_timer.disable();
  ASSERT_TRUE(ended);
}

// D8: every attempt carries a new SSRC and a new token; the repetitions of one
// attempt carry the same.
void each_attempt_has_a_new_ssrc() {
  fake_server_t server;
  server.ignore_invitations = INT_MAX;
  rtpclient_t node("node");
  node.control_tries = 2; // an attempt every 2 s
  node.add_server_address("127.0.0.1", std::to_string(server.port()));
  ASSERT_TRUE(
      run_until([&] { return server.control_invitations >= 6; }, 9000ms));
  std::vector<std::pair<uint32_t, uint32_t>> attempts; // token, ssrc
  for (auto &invitation : server.seen) {
    if (!invitation.control)
      continue;
    if (!attempts.empty() && attempts.back().first == invitation.token) {
      ASSERT_EQUAL(attempts.back().second, invitation.ssrc);
      continue;
    }
    attempts.push_back({invitation.token, invitation.ssrc});
  }
  INFO("{} attempts in {} invitations", attempts.size(),
       server.control_invitations);
  ASSERT_GTE(attempts.size(), size_t(3));
  for (size_t i = 1; i < attempts.size(); i++) {
    ASSERT_NOT_EQUAL(attempts[i].second, attempts[i - 1].second);
    ASSERT_NOT_EQUAL(attempts[i].first, attempts[i - 1].first);
  }
}

// V6: a server checks both binds. A fixed port whose port + 1 is taken, or
// that is itself taken, gives a server that is not open and says so (and the
// daemon then announces nothing, step 4). A dynamic port gives an adjacent
// pair. (Forcing the dynamic retry would need the kernel's next choice.)
void a_server_that_cannot_bind_says_so() {
  const int port = free_port_pair();
  {
    rtpmidid::udppeer_t holder("::", std::to_string(port + 1));
    ASSERT_TRUE(holder.is_open());
    rtpmidid::rtpserver_t server("server", std::to_string(port));
    INFO("fixed port {} with {} taken: open={} port()={}", port, port + 1,
         server.is_open(), server.port());
    ASSERT_FALSE(server.is_open());
    ASSERT_EQUAL(server.port(), 0);
  }
  {
    rtpmidid::udppeer_t holder("::", std::to_string(port));
    ASSERT_TRUE(holder.is_open());
    rtpmidid::rtpserver_t server("server", std::to_string(port));
    ASSERT_FALSE(server.is_open());
  }
  rtpmidid::rtpserver_t server("server", "0");
  ASSERT_TRUE(server.is_open());
  ASSERT_EQUAL(server.midi.get_address().port(), server.port() + 1);
}

int main(int argc, char **argv) {
  if (soak_seconds() > 0) {
    test_case_t soaks{
        TEST(soak_control_two_clients_without_timeouts),
        TEST(soak_control_one_client_one_timeout),
        TEST(soak_two_clients_one_timeout_each),
    };
    soaks.run(argc, argv);
    return soaks.exit_code();
  }

  test_case_t testcase{
      // controls
      TEST(control_one_client_recovers_after_a_ck_timeout),
      TEST(control_two_clients_both_receive),
      TEST(control_two_sessions_from_one_address_coexist),
      TEST(
          control_an_old_goodbye_after_the_new_invitation_does_not_touch_the_new_session),
      TEST(control_the_client_comes_back_through_another_endpoint),
      // the client
      TEST(two_clients_that_each_had_one_ck_timeout_both_receive),
      TEST(one_client_that_has_reset_once_comes_back_after_lost_goodbyes),
      TEST(one_lost_ck_does_not_end_the_session),
      TEST(a_lost_invitation_is_repeated_quickly),
      TEST(invitations_keep_coming_once_a_second_with_the_built_in_timers),
      TEST(a_repeated_ok_is_ignored_in_both_phases),
      TEST(a_ck_reply_queued_behind_an_expired_timer_keeps_the_session),
      TEST(a_late_ck_reply_during_a_handshake_changes_nothing),
      TEST(
          an_exhausted_clock_sync_ends_the_session_and_the_next_invitation_has_a_new_token),
      TEST(the_client_ssrc_is_not_limited_to_16_bits),
      // the server
      TEST(a_repeated_invitation_does_not_connect_again),
      TEST(a_repeated_control_invitation_does_not_connect_again),
      TEST(a_new_session_gets_its_ck_reply_after_lost_goodbyes),
      TEST(another_address_does_not_take_a_session),
      TEST(a_goodbye_from_another_address_does_not_end_a_session),
      TEST(a_replaced_session_fires_its_disconnect_status),
      TEST(a_midi_only_session_is_gone_after_5_s),
      TEST(a_session_still_expires_when_other_sessions_come_and_go),
      // fix-only (AC2 b)
      TEST(a_name_with_no_answer_costs_one_try_a_second_at_most),
      TEST(a_destination_the_kernel_refuses_costs_one_try_a_second_at_most),
      TEST(a_local_port_that_cannot_be_bound_costs_one_try_a_second_at_most),
      TEST(an_endpoint_whose_send_fails_is_left_after_one_second),
      TEST(the_first_midi_invitation_leaves_with_the_control_ok),
      TEST(a_client_given_addresses_looks_no_name_up),
      TEST(a_name_is_looked_up_no_sooner_than_40_s_after_the_last),
      TEST(a_failed_lookup_keeps_the_last_good_answer),
      TEST(a_replaced_endpoint_is_invited_at_the_next_attempt),
      TEST(midi_from_the_server_postpones_the_clock_sync_verdict),
      TEST(each_attempt_has_a_new_ssrc),
      TEST(a_server_that_cannot_bind_says_so),
  };

  testcase.run(argc, argv);

  return testcase.exit_code();
}
