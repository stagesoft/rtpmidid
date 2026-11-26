// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// RTP-MIDI session recovery in the daemon (ClickUp 1amfwm7fgu7, step 4; the
// ALSA port leak 1amfwm7h80b): N3, M1, M2.
//
// They need a sequencer and skip where there is none, as test_midirouter does.
// Discovery is driven by firing the library's discover and remove events by
// hand: the avahi client is closed as soon as it is made, so that nothing on
// the real network reaches the test. Network sessions use loopback only.

#include "../src/aseq.hpp"
#include "../src/control_socket.hpp"
#include "../src/factory.hpp"
#include "../src/hwautoannounce.hpp"
#include "../src/local_alsa_listener.hpp"
#include "../src/local_alsa_peer.hpp"
#include "../src/midirouter.hpp"
#include "../src/network_rtpmidi_multi_listener.hpp"
#include "../src/rtpmidiremotehandler.hpp"
#include "../src/settings.hpp"
#include "../tests/test_case.hpp"
#include "test_utils.hpp"
#include <memory>
#include <rtpmidid/mdns_rtpmidi.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpclient.hpp>
#include <rtpmidid/rtpserver.hpp>
#include <unistd.h>

using namespace std::chrono_literals;

namespace rtpmididns {
// NOLINTNEXTLINE
settings_t settings;
// NOLINTNEXTLINE
std::shared_ptr<::rtpmidid::mdns_rtpmidi_t> mdns;
const char *VERSION = "TEST"; // NOLINT
} // namespace rtpmididns

static void run_for(std::chrono::milliseconds time) {
  const auto deadline = std::chrono::steady_clock::now() + time;
  while (std::chrono::steady_clock::now() < deadline)
    rtpmidid::poller.wait(10ms);
}

static std::shared_ptr<rtpmididns::aseq_t> open_aseq(const char *name) {
  try {
    return std::make_shared<rtpmididns::aseq_t>(name);
  } catch (rtpmididns::alsa_connect_exception &exc) {
    ERROR("ALSA CONNECT EXCEPTION: {}", exc.what());
    INFO("Skipping test as ALSA is not available.");
    return nullptr;
  }
}

// The library's discovery events, with no avahi behind them.
static void discovery_without_avahi() {
  if (!rtpmididns::mdns) {
    rtpmididns::mdns = std::make_shared<rtpmidid::mdns_rtpmidi_t>();
    rtpmididns::mdns->close_avahi();
    rtpmididns::mdns->reconnect_timer.disable();
  }
}

// Make ports until the sequencer refuses one: returns how many were made.
static int exhaust_ports(rtpmididns::aseq_t &aseq, int &last) {
  int made = 0;
  for (int i = 0; i < 1024; i++) {
    last = aseq.create_port(FMT::format("filler {}", i));
    if (last < 0)
      break;
    made++;
  }
  return made;
}

template <typename T>
static int count_peers(rtpmididns::midirouter_t &router) {
  int count = 0;
  router.for_each_peer<T>([&](T *) { count++; });
  return count;
}

static rtpmididns::local_alsa_listener_t *
find_listener(rtpmididns::midirouter_t &router, const std::string &name) {
  rtpmididns::local_alsa_listener_t *found = nullptr;
  router.for_each_peer<rtpmididns::local_alsa_listener_t>(
      [&](rtpmididns::local_alsa_listener_t *peer) {
        if (peer->remote_name == name)
          found = peer;
      });
  return found;
}

static rtpmidid::io_bytes_managed invitation(const char *token,
                                             const char *ssrc) {
  return hex_to_bin(std::string("FF FF 'IN' 0000 0002 ") + token + " " + ssrc +
                    " 'node' 00");
}

// ---------------------------------------------------------------------- M2

// The convention the daemon relies on, pinned: alsa-lib reports a port it
// cannot make as a negative number (the sequencer has a limit per client).
void create_port_reports_a_failure_as_a_negative_number() {
  auto aseq = open_aseq("rtpm-test-ports");
  if (!aseq)
    return;
  int last = 0;
  const int made = exhaust_ports(*aseq, last);
  INFO("ports made before the refusal: {}; the refusal: {}", made, last);
  ASSERT_GT(made, 0);
  ASSERT_LT(last, 0);
}

// A discovered peer whose ALSA port cannot be made gets none: no listener,
// and nothing thrown out of the (avahi) callback.
void a_discovered_peer_without_a_port_gets_no_listener() {
  auto aseq = open_aseq("rtpm-test-discovery");
  if (!aseq)
    return;
  discovery_without_avahi();
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  rtpmididns::rtpmidi_remote_handler_t handler(router, aseq);
  int last = 0;
  exhaust_ports(*aseq, last);

  rtpmididns::mdns->discover_event("svc", "127.0.0.1", "5004", 1);
  INFO("listeners after the discovery: {}; known peers: {}",
       count_peers<rtpmididns::local_alsa_listener_t>(*router),
       handler.peers.size());
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_listener_t>(*router), 0);
  ASSERT_EQUAL(handler.peers.size(), size_t(0));
}

void a_port_that_cannot_be_made_gives_the_control_socket_an_error() {
  auto aseq = open_aseq("rtpm-test-control");
  if (!aseq)
    return;
  rtpmididns::settings.control_filename =
      FMT::format("/tmp/rtpm-test-control-{}.sock", getpid());
  rtpmididns::control_socket_t control;
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  control.router = router;
  control.aseq = aseq;
  int last = 0;
  exhaust_ports(*aseq, last);

  auto reply = control.parse_command(
      R"({"id": 1, "method": "connect", "params": ["peer", "127.0.0.1", 5004]})");
  INFO("control socket reply: {}", reply);
  ASSERT_TRUE(reply.find("error") != std::string::npos);
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_listener_t>(*router), 0);

  auto created = control.parse_command(
      R"({"id": 2, "method": "router.create", "params": {"type": "local_alsa_peer_t", "name": "x"}})");
  INFO("router.create reply: {}", created);
  ASSERT_TRUE(created.find("error") != std::string::npos);
  unlink(rtpmididns::settings.control_filename.c_str());
}

// The start-up ports (Network Export, Announcements, a [connect_to] listener)
// throw, so that setup() fails the start with status 1, as any setup error.
void the_start_up_ports_fail_the_start() {
  auto aseq = open_aseq("rtpm-test-startup");
  if (!aseq)
    return;
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  int last = 0;
  exhaust_ports(*aseq, last);

  bool export_threw = false;
  try {
    rtpmididns::make_local_alsa_multi_listener("Network Export", aseq);
  } catch (const std::exception &) {
    export_threw = true;
  }
  bool announcements_threw = false;
  try {
    rtpmididns::HwAutoAnnounce announce(aseq, router);
  } catch (const std::exception &) {
    announcements_threw = true;
  }
  bool connect_to_threw = false;
  try {
    rtpmididns::make_local_alsa_listener(router, "controller", "127.0.0.1",
                                         "5004", aseq, "0");
  } catch (const std::exception &) {
    connect_to_threw = true;
  }
  INFO("threw: export {}, announcements {}, [connect_to] {}", export_threw,
       announcements_threw, connect_to_threw);
  ASSERT_TRUE(export_threw);
  ASSERT_TRUE(announcements_threw);
  ASSERT_TRUE(connect_to_threw);
  ASSERT_EQUAL(router->add_peer(nullptr), rtpmididns::MIDIPEER_ID_INVALID);
}

// An incoming session whose ALSA port cannot be made is refused, after its
// CONNECTED event has returned; the server is left with no session.
void a_session_without_a_port_is_refused_after_its_event() {
  auto aseq = open_aseq("rtpm-test-session");
  if (!aseq)
    return;
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  auto listener = std::make_shared<rtpmididns::network_rtpmidi_multi_listener_t>(
      "test-server", "0", aseq);
  router->add_peer(listener);
  int last = 0;
  exhaust_ports(*aseq, last);

  test_client_t control(0, listener->server.port());
  test_client_t midi(control.local_port + 1, listener->server.port() + 1);
  control.send(invitation("00 12 34 00", "00 BE EF 00"));
  midi.send(invitation("00 12 34 00", "00 BE EF 00"));
  run_for(300ms);
  INFO("sessions on the server after the refusal: {}; ALSA peers: {}",
       listener->server.peers.size(),
       count_peers<rtpmididns::local_alsa_peer_t>(*router));
  ASSERT_EQUAL(listener->server.peers.size(), size_t(0));
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_peer_t>(*router), 0);
}

// ---------------------------------------------------------------------- M1

// The announce server's listener makes one ALSA port per session, however
// many CONNECTED events the library fires for it; the port goes with the
// session.
void a_second_connected_makes_no_port() {
  auto aseq = open_aseq("rtpm-test-m1");
  if (!aseq)
    return;
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  auto listener = std::make_shared<rtpmididns::network_rtpmidi_multi_listener_t>(
      "test-server", "0", aseq);
  router->add_peer(listener);

  test_client_t control(0, listener->server.port());
  test_client_t midi(control.local_port + 1, listener->server.port() + 1);
  control.send(invitation("00 12 34 00", "00 BE EF 00"));
  midi.send(invitation("00 12 34 00", "00 BE EF 00"));
  run_for(200ms);
  ASSERT_EQUAL(listener->server.peers.size(), size_t(1));
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_peer_t>(*router), 1);

  auto peer = listener->server.peers.front().peer;
  listener->server.status_change_event(peer, rtpmidid::rtppeer_t::CONNECTED);
  midi.send(invitation("00 12 34 00", "00 BE EF 00")); // a repeated IN
  run_for(200ms);
  INFO("ALSA peers after a second CONNECTED and a repeated IN: {}",
       count_peers<rtpmididns::local_alsa_peer_t>(*router));
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_peer_t>(*router), 1);

  control.send(hex_to_bin("FF FF 'BY' 0000 0002 00 12 34 00 00 BE EF 00"));
  run_for(200ms);
  INFO("ALSA peers after the goodbye: {}",
       count_peers<rtpmididns::local_alsa_peer_t>(*router));
  ASSERT_EQUAL(count_peers<rtpmididns::local_alsa_peer_t>(*router), 0);
  ASSERT_EQUAL(listener->sessions.size(), size_t(0));
}

// ---------------------------------------------------------------------- N3

// A service whose address changes keeps one endpoint per interface, with the
// new address; a second interface is a second endpoint; a REMOVE takes only
// its interface's endpoint, and the listener goes with the last one.
void a_moved_service_keeps_one_endpoint_per_interface() {
  auto aseq = open_aseq("rtpm-test-n3");
  if (!aseq)
    return;
  discovery_without_avahi();
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  rtpmididns::rtpmidi_remote_handler_t handler(router, aseq);
  auto &mdns = *rtpmididns::mdns;

  mdns.discover_event("svc", "10.0.0.1", "5004", 2);
  auto *listener = find_listener(*router, "svc");
  ASSERT_TRUE(listener != nullptr);
  mdns.discover_event("svc", "10.0.0.2", "5004", 2);
  INFO("endpoints after the move: {} ({})", listener->endpoints.size(),
       listener->endpoints[0].hostname);
  ASSERT_EQUAL(listener->endpoints.size(), size_t(1));
  ASSERT_EQUAL(listener->endpoints[0].hostname, std::string("10.0.0.2"));

  mdns.discover_event("svc", "10.0.1.2", "5004", 3);
  ASSERT_EQUAL(listener->endpoints.size(), size_t(2));

  mdns.remove_event("svc", "10.0.0.2", "5004", 2);
  ASSERT_TRUE(find_listener(*router, "svc") != nullptr);
  ASSERT_EQUAL(listener->endpoints.size(), size_t(1));
  ASSERT_EQUAL(listener->endpoints[0].interface, 3);

  mdns.remove_event("svc", "10.0.1.2", "5004", 3);
  ASSERT_TRUE(find_listener(*router, "svc") == nullptr);
  ASSERT_EQUAL(handler.peers.size(), size_t(0));
}

// A live client is handed the new endpoint; once it has gone, a move or a
// REMOVE does not touch it.
void a_live_client_is_handed_the_new_endpoint() {
  auto aseq = open_aseq("rtpm-test-n3-client");
  if (!aseq)
    return;
  discovery_without_avahi();
  auto router = std::make_shared<rtpmididns::midirouter_t>();
  rtpmididns::rtpmidi_remote_handler_t handler(router, aseq);
  auto &mdns = *rtpmididns::mdns;
  rtpmidid::rtpserver_t first("first", "0");
  rtpmidid::rtpserver_t second("second", "0");

  mdns.discover_event("svc", "127.0.0.1", std::to_string(first.port()), 2);
  auto *listener = find_listener(*router, "svc");
  ASSERT_TRUE(listener != nullptr);
  listener->connect_to_remote_server("svc-local");
  auto client = listener->rtpclient.lock();
  ASSERT_TRUE(client != nullptr);
  run_for(300ms);
  ASSERT_TRUE(client->peer.is_connected());

  mdns.discover_event("svc", "127.0.0.1", std::to_string(second.port()), 2);
  ASSERT_EQUAL(client->address_port_known.size(), size_t(1));
  INFO("the client's endpoint after the move: {}",
       client->address_port_known.front().port);
  ASSERT_EQUAL(client->address_port_known.front().port,
               std::to_string(second.port()));
  ASSERT_TRUE(client->peer.is_connected()); // the session that is up stays

  client.reset();
  listener->disconnect_from_remote_server();
  run_for(100ms);
  ASSERT_TRUE(listener->rtpclient.expired());
  mdns.discover_event("svc", "127.0.0.1", std::to_string(first.port()), 2);
  mdns.remove_event("svc", "127.0.0.1", std::to_string(first.port()), 2);
  ASSERT_TRUE(find_listener(*router, "svc") == nullptr);
}

int main(int argc, char **argv) {
  test_case_t testcase{
      TEST(create_port_reports_a_failure_as_a_negative_number),
      TEST(a_discovered_peer_without_a_port_gets_no_listener),
      TEST(a_port_that_cannot_be_made_gives_the_control_socket_an_error),
      TEST(the_start_up_ports_fail_the_start),
      TEST(a_session_without_a_port_is_refused_after_its_event),
      TEST(a_second_connected_makes_no_port),
      TEST(a_moved_service_keeps_one_endpoint_per_interface),
      TEST(a_live_client_is_handed_the_new_endpoint),
  };

  testcase.run(argc, argv);
  rtpmididns::mdns = nullptr;
  return testcase.exit_code();
}
