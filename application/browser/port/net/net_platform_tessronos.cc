/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	net_platform_tessronos.cc
 *	ブラウザ: what the network stack asks the system about itself
 *	(design 17.19).
 *
 *	Upstream reads the kernel's table of interfaces, its netlink socket
 *	and the resolver's files. The system here offers one configured
 *	interface (so_getifaddr gives its IPv4 address and mask, the system's
 *	real object its IPv6 addresses in record 7), and
 *	resolves names by a call that says nothing of how it is configured.
 *	Each answer below is the true one for such a machine: the stack works
 *	with it, with less to go on.
 *
 *	The network changing under the browser -- Wi-Fi joined to another
 *	network, another address, gateway or name server from DHCP, IPv6's
 *	routers moving -- is told by the system as a number it counts up
 *	(so_netwait, docs/tessronos 9.3.5). A thread of the browser waits in
 *	that call, as Linux's watcher reads its netlink socket, and has the
 *	network thread look at what changed: the stack's observers are told of
 *	new addresses (its pools and sessions closed, the names it holds
 *	forgotten), and the name servers are read again (design 14.5).
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_change_notifier.h"
#include "net/base/network_interfaces.h"
#include "net/dns/address_sorter.h"
#include "net/dns/dns_config.h"
#include "net/dns/dns_config_service.h"
#include "net/dns/dns_hosts.h"
#include "net/dns/loopback_only.h"
#include "net/dns/system_dns_config_change_notifier.h"

/* The system's interface (include/ts/soapp.h): its address, mask and gateway, in the network's order */
extern "C" int so_getifaddr(unsigned* p_addr, unsigned* p_mask, unsigned* p_gw);
/* Its IPv6 addresses in use and their prefix lengths, as the system's record 7 shows them (br_sysnet.cc) */
extern "C" int br_sys_addr6(unsigned char (*out)[16], int* plen, int max);
extern "C" unsigned br_sys_ifindex(void);
/* The first IPv4 name server, in network order, 0 for none; the IPv6 ones (br_sysnet.cc) */
extern "C" int so_getdns(unsigned* p_dns);
extern "C" int br_sys_dns6(unsigned char (*out)[16], int max);
/* Waits for the system's count of the network's changes to move (include/ts/soapp.h) */
extern "C" int so_netwait(unsigned* p_gen, int tmo);

/* Posts work to the network thread, from any thread (net_fetch_tessronos.cc) */
void br_net_post(base::OnceClosure work);

namespace net {

// This machine reaches the network through a configured interface, so it is
// never confined to loopback. The answer is posted rather than returned in
// place: callers treat this as an asynchronous question, and answering it
// synchronously re-enters them from inside their own call.
void RunHaveOnlyLoopbackAddressesJob(
    base::OnceCallback<void(bool)> finished_cb) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(finished_cb), false));
}

// The machine's one interface, while it has an address (WebRTC gathers its
// candidates on it): an Ethernet, its IPv4 address and the prefix of its
// mask. No address yet, the list is empty.
bool GetNetworkList(NetworkInterfaceList* networks, int policy) {
  unsigned addr = 0, mask = 0, gw = 0;
  unsigned index = br_sys_ifindex();
  if (so_getifaddr(&addr, &mask, &gw) >= 0 && addr != 0) {
    uint8_t b[4];
    memcpy(b, &addr, sizeof(b));
    networks->push_back(NetworkInterface(
        "neta0", "neta0", index, NetworkChangeNotifier::CONNECTION_ETHERNET,
        IPAddress(b[0], b[1], b[2], b[3]),
        static_cast<uint32_t>(__builtin_popcount(mask)),
        IP_ADDRESS_ATTRIBUTE_NONE));
  }
  // and its IPv6 addresses, the link-local one among them
  unsigned char a6[8][16];
  int plen[8];
  int n6 = br_sys_addr6(a6, plen, 8);
  for (int i = 0; i < n6; ++i) {
    networks->push_back(NetworkInterface(
        "neta0", "neta0", index, NetworkChangeNotifier::CONNECTION_ETHERNET,
        IPAddress(base::span<const uint8_t>(a6[i], 16u)),
        static_cast<uint32_t>(plen[i]), IP_ADDRESS_ATTRIBUTE_NONE));
  }
  return true;
}

namespace {

// Ordering addresses by likelihood of connecting needs to know which source
// address the machine would use for each candidate, which needs the interface
// table. Without it the resolver's own order stands, which is the order the
// system resolver returned -- already its best answer.
class KeepOrderSorter : public AddressSorter {
 public:
  void Sort(const std::vector<IPEndPoint>& endpoints,
            const NetworkAnonymizationKey& anonymization_key,
            handles::NetworkHandle target_network,
            CallbackType callback) const override {
    std::move(callback).Run(true, endpoints);
  }
};

}  // namespace

std::unique_ptr<AddressSorter> AddressSorter::CreateAddressSorter() {
  return std::make_unique<KeepOrderSorter>();
}

namespace {

constexpr int kE_TMOUT = -50;		// T-Kernel's E_TMOUT
constexpr int kWatchSliceMs = 60000;	// one so_netwait at most this long

// The name servers the system asks: the IPv4 one, then the IPv6 ones in
// the order they are asked (the settings', then the routers'). Read on the
// DNS service's sequence and on the network thread.
std::vector<IPEndPoint> SystemNameServers() {
  std::vector<IPEndPoint> servers;
  unsigned dns = 0;
  if (so_getdns(&dns) >= 0 && dns != 0) {
    uint8_t b[4];
    memcpy(b, &dns, sizeof(b));
    servers.emplace_back(IPAddress(b[0], b[1], b[2], b[3]), 53);
  }
  unsigned char six[3][16];
  const int n6 = br_sys_dns6(six, 3);
  for (int i = 0; i < n6; ++i)
    servers.emplace_back(IPAddress(base::span<const uint8_t>(six[i], 16u)), 53);
  return servers;
}

// What of the network the stack's connections depend on, as bytes to
// compare: the IPv4 address, mask and gateway, and the IPv6 addresses in
// use with their prefix lengths
std::string AddressShape(bool* online) {
  std::string s;
  unsigned addr = 0, mask = 0, gw = 0;
  if (so_getifaddr(&addr, &mask, &gw) < 0)
    addr = mask = gw = 0;
  s.append(reinterpret_cast<const char*>(&addr), sizeof(addr));
  s.append(reinterpret_cast<const char*>(&mask), sizeof(mask));
  s.append(reinterpret_cast<const char*>(&gw), sizeof(gw));
  bool reach = addr != 0;
  unsigned char a6[8][16];
  int plen[8];
  const int n6 = br_sys_addr6(a6, plen, 8);
  for (int i = 0; i < n6; ++i) {
    s.append(reinterpret_cast<const char*>(a6[i]), 16);
    s.push_back(static_cast<char>(plen[i]));
    // a link-local address (fe80::/10) reaches no farther than the link
    if (!(a6[i][0] == 0xfe && (a6[i][1] & 0xc0) == 0x80))
      reach = true;
  }
  *online = reach;
  return s;
}

std::string ServersShape() {
  std::string s;
  for (const IPEndPoint& e : SystemNameServers())
    s += e.ToString() + " ";
  return s;
}

// The system's resolver settings, read again whenever the notifier below
// says they may have changed (RefreshConfig). There is no hosts file.
class DnsConfigServiceTessronOS : public DnsConfigService {
 public:
  DnsConfigServiceTessronOS()
      : DnsConfigService(base::FilePath::StringViewType(),
                         /*config_change_delay=*/std::nullopt) {}

  void RefreshConfig() override {
    InvalidateConfig();
    ReadConfigNow();
  }

 protected:
  void ReadConfigNow() override {
    DnsConfig config;
    config.nameservers = SystemNameServers();
    std::string said;
    for (const IPEndPoint& e : config.nameservers)
      said += " " + e.ToString();
    printf("browser: net: the system's name servers:%s\n",
           said.empty() ? " none (names through the system)" : said.c_str());
    OnConfigRead(std::move(config));
  }
  void ReadHostsNow() override { OnHostsRead(DnsHosts()); }
  bool StartWatching() override { return true; }
};

// The notifier: the stack's observers told of what the system's count of
// the network's changes moved for. The delays are Linux's, which turn the
// address going and the new one coming into one change of the network.
class NetworkChangeNotifierTessronOS : public NetworkChangeNotifier {
 public:
  NetworkChangeNotifierTessronOS() : NetworkChangeNotifier(Params()) {
    unsigned gen = 0;
    (void)so_netwait(&gen, 0);  // the number as it is, before what it stands for is read
    bool online = false;
    addresses_ = AddressShape(&online);
    servers_ = ServersShape();
    type_.store(online ? CONNECTION_ETHERNET : CONNECTION_NONE);
    gen_ = gen;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, &Watch, this) != 0)
      printf("browser: net: no thread to watch the network's changes\n");
    pthread_attr_destroy(&at);
  }

 protected:
  ConnectionType GetCurrentConnectionType() const override {
    return type_.load();
  }

 private:
  static NetworkChangeCalculatorParams Params() {
    NetworkChangeCalculatorParams params;
    params.ip_address_offline_delay_ = base::Milliseconds(2000);
    params.ip_address_online_delay_ = base::Milliseconds(2000);
    params.connection_type_offline_delay_ = base::Milliseconds(1500);
    params.connection_type_online_delay_ = base::Milliseconds(500);
    return params;
  }

  // The watcher's thread: in so_netwait until the count moves, then the
  // network thread looks. The notifier lives as long as the program.
  static void* Watch(void* arg) {
    auto* self = static_cast<NetworkChangeNotifierTessronOS*>(arg);
    unsigned gen = self->gen_;
    int failures = 0;
    for (;;) {
      const int er = so_netwait(&gen, kWatchSliceMs);
      if (er == kE_TMOUT)
        continue;
      if (er < 0) {
        if (++failures >= 5) {
          printf("browser: net: the network's changes cannot be waited for (%d)\n", er);
          return nullptr;
        }
        sleep(1);
        continue;
      }
      failures = 0;
      br_net_post(base::BindOnce(&NetworkChangeNotifierTessronOS::Changed,
                                 base::Unretained(self)));
    }
  }

  // On the network thread: what moved, told to the stack
  void Changed() {
    bool online = false;
    const std::string addresses = AddressShape(&online);
    const std::string servers = ServersShape();
    const bool moved = addresses != addresses_;
    const bool renamed = servers != servers_;
    addresses_ = addresses;
    servers_ = servers;
    const ConnectionType type = online ? CONNECTION_ETHERNET : CONNECTION_NONE;
    const bool retyped = type != type_.load();
    printf("browser: net: the network changed:%s%s%s\n",
           moved ? " addresses" : "", renamed ? " name servers" : "",
           retyped ? (online ? " online" : " offline") : "");
    if (moved) {
      // the connections, sessions and names of the addresses before are let go
      NotifyObserversOfIPAddressChange();
    }
    if (retyped) {
      type_.store(type);
      NotifyObserversOfConnectionTypeChange();
      NotifyObserversOfMaxBandwidthChange(
          GetMaxBandwidthMbpsForConnectionSubtype(online ? SUBTYPE_UNKNOWN
                                                         : SUBTYPE_NONE),
          type);
    }
    if (moved || renamed) {
      // the resolver's settings read again; it asks the new servers
      SystemDnsConfigChangeNotifier* dns = GetCurrentSystemDnsConfigNotifier();
      if (dns != nullptr)
        dns->RefreshConfig();
    }
  }

  std::string addresses_, servers_;	// on the network thread
  std::atomic<ConnectionType> type_{CONNECTION_NONE};
  unsigned gen_ = 0;
};

}  // namespace

std::unique_ptr<DnsConfigService> DnsConfigService::CreateSystemService() {
  return std::make_unique<DnsConfigServiceTessronOS>();
}

}  // namespace net

// The notifier made on the network thread before the stack is built, so
// that its pools, sessions and resolver find it (net_fetch_tessronos.cc)
void br_net_watch_changes() {
  static net::NetworkChangeNotifier* notifier =
      new net::NetworkChangeNotifierTessronOS();
  (void)notifier;
}
