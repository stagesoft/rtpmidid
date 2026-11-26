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
 *
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 */

#include <atomic>
#include <cassert>
#include <functional>
#include <rtpmidid/exceptions.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/networkaddress.hpp>

namespace rtpmidid {

network_address_t::network_address_t(int fd) {
  auto addr = sockaddr_storage_to_sockaddr(new sockaddr_storage());
  len = sizeof(sockaddr_storage);
  managed = true;

  int res;
  res = ::getsockname(fd, addr, &len);
  if (res < 0) {
    throw rtpmidid::exception("Error getting info the newly created midi "
                              "socket. Can not create server.");
  }
  this->addr = addr;
}

network_address_t::~network_address_t() {
  if (managed) {
    delete (sockaddr_storage *)addr;
  }
}

int network_address_t::port() const {
  if (!addr) {
    ERROR("This network address do not point to any address.");
    return 0;
  }
  if (addr->sa_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in *>(addr)->sin_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_port);
}

std::string network_address_t::ip() const {
  if (!addr) {
    ERROR("This network address do not point to any address.");
    return "null";
  }
  std::array<char, INET6_ADDRSTRLEN> name{};
  if (addr->sa_family == AF_INET) {
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(addr)->sin_addr,
              name.data(), name.size());
    return name.data();
  }
  inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_addr,
            name.data(), name.size());
  return name.data();
}

std::string network_address_t::hostname() const {
  if (!addr) {
    return "null";
  }
  std::array<char, NI_MAXHOST> name{};
  if (getnameinfo(addr, len, name.data(), NI_MAXHOST, nullptr, 0, 0) != 0) {
    return ip();
  }
  return name.data();
}

std::string network_address_t::to_string() const {
  if (!addr) {
    return "null";
  }

  std::array<char, INET6_ADDRSTRLEN> name{};
  if (addr->sa_family == AF_INET) {
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(addr)->sin_addr,
              name.data(), name.size());
    return FMT::format(
        "{}:{}", name.data(),
        ntohs(reinterpret_cast<const sockaddr_in *>(addr)->sin_port));
  }
  inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_addr,
            name.data(), name.size());
  return FMT::format(
      "{}:{}", name.data(),
      ntohs(reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_port));
}

bool network_address_t::resolve_loop(
    const std::string &address, const std::string &port,
    std::function<bool(const network_address_t &)> cb) {

  return network_address_t::resolve_loop(
      address, port, [&cb](const addrinfo *addr) {
        network_address_t address{addr->ai_addr, addr->ai_addrlen};
        return cb(address);
      });
}

bool network_address_t::resolve_loop(const std::string &address,
                                     const std::string &port,
                                     std::function<bool(const addrinfo *)> cb) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_protocol = 0;
  hints.ai_canonname = nullptr;
  hints.ai_addr = nullptr;
  hints.ai_next = nullptr;

  addrinfo *result;
  if (getaddrinfo(address.c_str(), port.c_str(), &hints, &result) != 0) {
    return false;
  }

  for (auto *addr = result; addr != nullptr; addr = addr->ai_next) {
    network_address_t address{addr->ai_addr, addr->ai_addrlen};
    if (cb(addr)) {
      freeaddrinfo(result);
      return true;
    }
  }
  freeaddrinfo(result);
  return false;
}

// The address part of a socket address, with an IPv4-mapped IPv6 address taken
// as the IPv4 address it maps: a dual-stack socket reports IPv4 peers that way.
static bool address_bytes(const sockaddr *addr, int &family,
                          const uint8_t *&bytes) {
  if (!addr)
    return false;
  if (addr->sa_family == AF_INET) {
    family = AF_INET;
    bytes = reinterpret_cast<const uint8_t *>(
        &reinterpret_cast<const sockaddr_in *>(addr)->sin_addr);
    return true;
  }
  if (addr->sa_family == AF_INET6) {
    const auto *in6 = &reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(in6)) {
      family = AF_INET;
      bytes = reinterpret_cast<const uint8_t *>(in6) + 12;
    } else {
      family = AF_INET6;
      bytes = reinterpret_cast<const uint8_t *>(in6);
    }
    return true;
  }
  return false;
}

bool network_address_t::same_host(const network_address_t &other) const {
  int family_a = 0;
  int family_b = 0;
  const uint8_t *bytes_a = nullptr;
  const uint8_t *bytes_b = nullptr;
  if (!address_bytes(addr, family_a, bytes_a) ||
      !address_bytes(other.addr, family_b, bytes_b) || family_a != family_b)
    return false;
  return memcmp(bytes_a, bytes_b, family_a == AF_INET ? 4 : 16) == 0;
}

bool network_address_t::same_endpoint(const network_address_t &other) const {
  return same_host(other) && port() == other.port();
}

void network_address_t::set_port(int port) {
  if (!addr) {
    ERROR("This network address do not point to any address.");
    return;
  }
  assert(managed); // Only managed addresses can be modified.

  sockaddr *addr = const_cast<sockaddr *>(this->addr);

  if (addr->sa_family == AF_INET) {
    reinterpret_cast<sockaddr_in *>(addr)->sin_port = htons(port);
  } else {
    reinterpret_cast<sockaddr_in6 *>(addr)->sin6_port = htons(port);
  }
}

network_address_list_t::network_address_list_t() : info(nullptr) {}

static std::atomic<unsigned> name_lookup_count{0};

unsigned network_address_list_t::name_lookups() { return name_lookup_count; }

bool network_address_list_t::is_numeric(const std::string &name) {
  if (name.empty())
    return false;
  std::array<uint8_t, sizeof(in6_addr)> buffer{};
  return inet_pton(AF_INET, name.c_str(), buffer.data()) == 1 ||
         inet_pton(AF_INET6, name.c_str(), buffer.data()) == 1;
}

// A numeric address (or none, for a wildcard bind) is converted without any
// lookup. Anything else is a name, looked up with the blocking getaddrinfo and
// counted, so that a test can tell that no lookup was made. The caller reports
// a failure (error()), as it decides how often to say so.
network_address_list_t::network_address_list_t(const std::string &name,
                                               const std::string &port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_protocol = 0;
  hints.ai_canonname = nullptr;
  hints.ai_addr = nullptr;
  hints.ai_next = nullptr;

  const bool numeric = is_numeric(name);
  if (numeric) {
    hints.ai_flags |= AI_NUMERICHOST;
  } else {
    name_lookup_count++;
  }
  error = getaddrinfo(name.c_str(), port.c_str(), &hints, &info);
  if (error != 0) {
    DEBUG("Error getting address info for {}:{}: {}", name, port,
          gai_strerror(error));
    if (info) {
      freeaddrinfo(info);
    }
    info = nullptr;
  }
}

std::string network_address_list_t::error_string() const {
  return error == 0 ? std::string("no error") : std::string(gai_strerror(error));
}

network_address_list_t::~network_address_list_t() {
  if (info) {
    freeaddrinfo(info);
  }
}

network_address_list_t &
network_address_list_t::operator=(network_address_list_t &&other) {
  if (info) {
    freeaddrinfo(info);
  }
  info = other.info;
  error = other.error;
  other.info = nullptr;
  other.error = 0;
  return *this;
}

network_address_t network_address_list_t::get_first() const {
  if (info == nullptr) {
    return network_address_t{};
  }
  return network_address_t{info->ai_addr, info->ai_addrlen};
}

std::string network_address_list_t::to_string() const {
  if (info == nullptr) {
    return "null";
  }
  std::string result = "{ ";
  for (auto address : *this) {
    result += address.to_string() + ", ";
  }
  result += "}";
  return result;
}

} // namespace rtpmidid
