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
// Everything runs on loopback. Nothing here drops packets on a real network.

#include "../tests/test_case.hpp"
#include "../tests/test_utils.hpp"
#include <chrono>
#include <functional>
#include <rtpmidid/iobytes.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/packet.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpclient.hpp>
#include <rtpmidid/rtpmidipacket.hpp>
#include <rtpmidid/rtpserver.hpp>
#include <rtpmidid/udppeer.hpp>
#include <string>
#include <sys/socket.h>
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

// Timeouts short enough for a unit test. CK is kept slow so that the only CK
// timeout in a test is the one the test asks for.
static void fast_timeouts(rtpclient_t &client) {
  client.connect_timeout = 500ms;
  client.reconnect_timeout = 500ms;
  client.ck_short_period = 5000ms;
  client.ck_long_period = 5000ms;
}

static void connect_client(rtpclient_t &client, rtpmidid::rtpserver_t &server) {
  fast_timeouts(client);
  client.add_server_address("127.0.0.1", std::to_string(server.port()));
  ASSERT_TRUE(run_until([&] { return is_up(client); }, 3000ms));
  // The first CK exchange has been answered and the client waits for the next.
  ASSERT_TRUE(run_until(
      [&] { return client.state == rtpclient_t::state_e::WaitSendCkShort; },
      3000ms));
}

// One clock-sync request that is not answered in time: what a stalled event
// loop produces on a healthy network. The client is made to send a CK and its
// timeout fires before the reply is read. No packet is dropped.
static void one_ck_timeout(rtpclient_t &client) {
  ASSERT_TRUE(run_until(
      [&] { return client.state == rtpclient_t::state_e::WaitSendCkShort; },
      3000ms));
  client.handle_event(rtpclient_t::SendCK);
  ASSERT_TRUE(client.state == rtpclient_t::state_e::SendCkShort);
  client.handle_event(rtpclient_t::Timeout);
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

// ------------------------------------------- a server that drops packets

// A server written by hand, so that a test can lose chosen datagrams.
struct fake_server_t {
  rtpmidid::udppeer_t control;
  rtpmidid::udppeer_t midi;
  int invitations = 0;
  int ck_requests = 0;
  int goodbyes = 0;
  int ignore_invitations = 0; // how many INs to lose, from the first
  int ignore_ck_requests = 0; // how many CK0 to lose, from the first
  std::vector<std::chrono::steady_clock::time_point> invitation_times;
  rtpmidid::udppeer_t::on_read_t::connection_t on_control;
  rtpmidid::udppeer_t::on_read_t::connection_t on_midi;

  fake_server_t(const std::string &port, const std::string &midi_port)
      : control("127.0.0.1", port), midi("127.0.0.1", midi_port) {
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

  void handle(rtpmidid::udppeer_t &socket, const rtpmidid::packet_t &data,
              const rtpmidid::network_address_t &from) {
    if (data.get_packet_type() != rtpmidid::packet_type_e::COMMAND)
      return;
    rtpmidid::packet_command_t command(data);
    rtpmidid::network_address_t reply_to = from.dup();

    if (command.get_command() == rtpmidid::command_e::BY) {
      goodbyes++;
      return;
    }
    if (command.get_command() == rtpmidid::command_e::IN) {
      invitations++;
      invitation_times.push_back(std::chrono::steady_clock::now());
      if (invitations <= ignore_invitations)
        return;
      rtpmidid::packet_command_in_ok_t request(data);
      rtpmidid::packet_managed_t<100> memory;
      rtpmidid::packet_command_in_ok_t reply(memory);
      reply.initialize(rtpmidid::command_e::OK)
          .set_sender_ssrc(0xBEEF)
          .set_initiator_token(request.get_initiator_token())
          .set_name("fake server");
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

// One lost clock-sync datagram must not end the session: the request has to be
// repeated while the client's own timeout has not run out yet.
void one_lost_ck_does_not_end_the_session() {
  fake_server_t server("15421", "15422");
  server.ignore_ck_requests = 1;

  rtpclient_t node("node");
  node.connect_timeout = 4000ms; // the budget for one clock-sync exchange
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

int main(int argc, char **argv) {
  test_case_t testcase{
      TEST(control_one_client_recovers_after_a_ck_timeout),
      TEST(control_two_clients_both_receive),
      TEST(two_clients_that_each_had_one_ck_timeout_both_receive),
      TEST(one_lost_ck_does_not_end_the_session),
      TEST(a_lost_invitation_is_repeated_quickly),
      TEST(a_repeated_invitation_does_not_connect_again),
      TEST(a_new_session_gets_its_ck_reply_after_lost_goodbyes),
  };

  testcase.run(argc, argv);

  return testcase.exit_code();
}
