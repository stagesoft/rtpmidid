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

#include "rtpmidiremotehandler.hpp"
#include "factory.hpp"
#include "local_alsa_listener.hpp"
#include "midirouter.hpp"
#include "rtpmidid/mdns_rtpmidi.hpp"
#include "settings.hpp"

// Do nothing or redefine DEBUG0 to DEBUG
#define DEBUG0(...)

namespace rtpmididns {
extern std::shared_ptr<::rtpmidid::mdns_rtpmidi_t> mdns;

rtpmidi_remote_handler_t::rtpmidi_remote_handler_t(
    std::shared_ptr<midirouter_t> router_, std::shared_ptr<aseq_t> aseq_)
    : router(router_), aseq(aseq_) {

  // N1: the library hands on the address avahi resolved (and the interface,
  // used by N3), never the host name.
  // N1: the library hands on the address avahi resolved, and the interface
  // it was seen on (N3); never the host name. M2: nothing thrown here leaves
  // the avahi callback.
  discover_connection = rtpmididns::mdns->discover_event.connect(
      [this](const std::string &name, const std::string &address,
             const std::string &port, int interface) {
        try {
          discover_peer(name, address, port, interface);
        } catch (const std::exception &exc) {
          ERROR("Discovered peer={} not added: {}", name, exc.what());
        }
      });

  remove_connection = rtpmididns::mdns->remove_event.connect(
      [this](const std::string &name, const std::string &address,
             const std::string &port, int interface) {
        try {
          remove_peer(name, address, port, interface);
        } catch (const std::exception &exc) {
          ERROR("Removing peer={}: {}", name, exc.what());
        }
      });
}

void rtpmidi_remote_handler_t::discover_peer(const std::string &name,
                                             const std::string &address,
                                             const std::string &port,
                                             int interface) {
  std::string fullname = FMT::format("{}:{}/{}", address, port, name);

  if (!check_if_add_peer(name, address, port)) {
    INFO("Not adding peer=\"{}\", as "
         "requested by settings",
         fullname);
    return;
  }
  INFO("Discover peer=\"{}\" on interface {}", fullname, interface);

  for (auto &peer : peers) {
    if (peer.name == name) {
      local_alsa_listener_t *alsawaiter =
          dynamic_cast<local_alsa_listener_t *>(peer.alsawaiter.get());
      // N3: one endpoint per interface, replaced when the service moves.
      if (alsawaiter)
        alsawaiter->add_endpoint(address, port, interface);
      DEBUG("Reuse peer=\"{}\"", fullname);
      return;
    }
  }

  DEBUG("New peer=\"{}\"", fullname);
  std::shared_ptr<midipeer_t> peer;
  try {
    peer = rtpmididns::make_local_alsa_listener(router, name, address, port,
                                                aseq, "0", interface);
  } catch (const std::exception &exc) {
    // M2: no port, no listener: that peer gets none.
    ERROR("Discovered peer=\"{}\" gets no ALSA port: {}", fullname,
          exc.what());
    return;
  }
  if (router->add_peer(peer) == MIDIPEER_ID_INVALID)
    return;
  peers.push_back(known_remote_peer_t{name, peer});
}

// N3: the browser's REMOVE takes that interface's endpoint away; when no
// discovered endpoint is left, the listener goes.
void rtpmidi_remote_handler_t::remove_peer(const std::string &name,
                                           const std::string &address,
                                           const std::string &port,
                                           int interface) {
  DEBUG("Remove peer: name=\"{}\" address={}:{} interface={}", name, address,
        port, interface);
  for (auto I = peers.begin(); I != peers.end(); ++I) {
    if (I->name != name)
      continue;
    local_alsa_listener_t *alsawaiter =
        dynamic_cast<local_alsa_listener_t *>(I->alsawaiter.get());
    if (alsawaiter && alsawaiter->remove_discovered_endpoint(interface) > 0) {
      INFO("Remote peer {}: endpoint on interface {} removed, {} left", name,
           interface, alsawaiter->discovered_endpoints());
      return;
    }
    INFO("Remove remote peer {} / midipeer {}", I->name,
         I->alsawaiter->peer_id);
    router->remove_peer(I->alsawaiter->peer_id);
    peers.erase(I);
    return;
  }
}

bool rtpmidi_remote_handler_t::check_if_add_peer(const std::string &name,
                                                 const std::string &hostname,
                                                 const std::string &port) {
  if (settings.rtpmidi_discover.enabled == false) {
    return false;
  }

  std::string fullname = FMT::format("{}:{} - {}", hostname, port, name);
  DEBUG0("Checking if we should add peer: fullname=\"{}\"", fullname);
  bool match_negative = std::regex_search(
      fullname, settings.rtpmidi_discover.name_negative_regex);
  DEBUG0("Match negative: {}", match_negative);
  if (match_negative) {
    return false;
  }

  bool match_positive = std::regex_search(
      fullname, settings.rtpmidi_discover.name_positive_regex);
  DEBUG0("Match positive: {}", match_positive);
  if (match_positive) {
    return true;
  }
  return false;
}

} // namespace rtpmididns
