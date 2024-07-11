/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019-2024 David Moreno Montero <dmoreno@coralbits.com>
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
 */

#pragma once

#include "rtpmidid/iobytes.hpp"
#include "rtpmidid/poller.hpp"
#include "rtpmidid/signal.hpp"
#include <string>

namespace rtpmidid {
class udppeer_t {

public:
  // Read some data from address and port
  signal_t<io_bytes_reader &, const std::string &, int> on_read;

  udppeer_t() { open("::", "0"); };
  udppeer_t(const std::string &address, const std::string &port) {
    open(address, port);
  }
  ~udppeer_t() { close(); }

  int open(const std::string &address, const std::string &port);
  void send(io_bytes &reader, const std::string &address,
            const std::string &port);
  void close();

private:
  std::string address;
  int port = 0;
  int fd = -1;
  poller_t::listener_t listener;

  void data_ready();
};

} // namespace rtpmidid
