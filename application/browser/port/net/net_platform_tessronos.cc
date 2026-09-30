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
 *	interface, resolves names by a call that says nothing of how it is
 *	configured, and cannot be asked what interfaces it has. Each answer
 *	below is the true one for such a machine: the stack works with it,
 *	with less to go on.
 */

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_interfaces.h"
#include "net/dns/address_sorter.h"
#include "net/dns/dns_config_service.h"
#include "net/dns/loopback_only.h"

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

// The interface table cannot be read here. Reporting failure leaves the list
// empty, which callers already handle: they use it to choose a source address
// or to name an interface, and otherwise let the stack choose for them.
bool GetNetworkList(NetworkInterfaceList* networks, int policy) {
  return false;
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

// The resolver configuration -- name servers, search domains -- is held
// inside the system's stack and is not readable. With no configuration to watch,
// there is no service to run, and the resolver falls back to asking the
// system to resolve each name, which is what it does here anyway.
std::unique_ptr<DnsConfigService> DnsConfigService::CreateSystemService() {
  return nullptr;
}

}  // namespace net
