/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	host_resolver_tessronos.cc
 *	ブラウザ: the lookup of names for the network stack (design 17.19).
 *
 *	Each lookup is made on a thread of its own (br_stream_resolve_start)
 *	and the task looks for its answer every hundredth of a second, so a
 *	name server that is slow to answer does not hold the network thread.
 *	IPv4 only.
 */

#include "net/dns/host_resolver_system_task.h"

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "net/base/address_list.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/sys_addrinfo.h"

#include <unistd.h>

#include "br_stream.h"

#include <stdio.h>

namespace net {

// Nothing has to be set up in advance: the lookup runs where it is asked
// for, so there is no worker to make ready.
void EnsureSystemHostResolverCallReady() {}

int SystemHostResolverCall(const std::string& host,
                           AddressFamily address_family,
                           HostResolverFlags host_resolver_flags,
                           AddressList* addrlist,
                           int* os_error,
                           handles::NetworkHandle network) {
  if (os_error)
    *os_error = 0;
  if (addrlist == nullptr)
    return ERR_FAILED;
  if (address_family == ADDRESS_FAMILY_IPV6)
    return ERR_NAME_NOT_RESOLVED;

  unsigned char ip4[4] = {0, 0, 0, 0};
  const int slot = br_stream_resolve_start(host.c_str(), ip4);
  int st = slot == -2 ? 1 : slot < 0 ? -1 : 0;
  while (st == 0) {
    usleep(10000);
    st = br_stream_resolve_poll(slot, ip4);
  }
  if (st < 0) {
    printf("browser: dns: %s not resolved\n", host.c_str());
    return ERR_NAME_NOT_RESOLVED;
  }

  // The port belongs to the caller, which fills it in once it knows which
  // service it is reaching; a lookup answers with the address alone.
  *addrlist = AddressList(
      IPEndPoint(IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]), 0));
  return OK;
}

// The parameter objects carry nothing this port acts on -- retry counts and
// delays belong to an attempt that can time out, and a lookup here either
// answers or does not.
HostResolverSystemTask::Params::Params(
    scoped_refptr<HostResolverProc> resolver_proc,
    size_t max_retry_attempts)
    : resolver_proc(std::move(resolver_proc)),
      max_retry_attempts(max_retry_attempts) {}

HostResolverSystemTask::Params::Params(const Params& other) = default;

HostResolverSystemTask::Params::~Params() = default;

HostResolverSystemTask::CacheParams::CacheParams(
    HostResolverCache& cache,
    NetworkAnonymizationKey network_anonymization_key,
    handles::NetworkHandle network)
    : cache(cache),
      network_anonymization_key(std::move(network_anonymization_key)),
      target_network(network) {}

HostResolverSystemTask::CacheParams::CacheParams(const CacheParams&) = default;

HostResolverSystemTask::CacheParams::CacheParams(CacheParams&&) = default;

HostResolverSystemTask::CacheParams::~CacheParams() = default;

// static
std::unique_ptr<HostResolverSystemTask> HostResolverSystemTask::Create(
    std::string hostname,
    AddressFamily address_family,
    HostResolverFlags flags,
    RequestPriority priority,
    const Params& params,
    const NetLogWithSource& job_net_log,
    handles::NetworkHandle network,
    std::optional<CacheParams> cache_params) {
  return std::make_unique<HostResolverSystemTask>(
      std::move(hostname), address_family, flags, params, job_net_log, network,
      std::move(cache_params), priority);
}

// static
std::unique_ptr<HostResolverSystemTask>
HostResolverSystemTask::CreateForOwnHostname(
    AddressFamily address_family,
    HostResolverFlags flags,
    const Params& params,
    const NetLogWithSource& job_net_log,
    handles::NetworkHandle network,
    RequestPriority priority) {
  return std::make_unique<HostResolverSystemTask>(
      std::nullopt, address_family, flags, params, job_net_log, network,
      std::nullopt, priority);
}

HostResolverSystemTask::HostResolverSystemTask(
    std::optional<std::string> hostname,
    AddressFamily address_family,
    HostResolverFlags flags,
    const Params& params,
    const NetLogWithSource& job_net_log,
    handles::NetworkHandle network,
    std::optional<CacheParams> cache_params,
    RequestPriority priority)
    : hostname_(std::move(hostname)),
      address_family_(address_family),
      flags_(flags),
      params_(params),
      net_log_(job_net_log),
      network_(network),
      cache_params_(std::move(cache_params)),
      priority_(priority) {}

HostResolverSystemTask::~HostResolverSystemTask() = default;

namespace {

// The answer, posted: see HostResolverSystemTask::Start. Through a weak
// pointer, so that a task destroyed while the answer is on its way stays
// cancelled, which is what destroying it means.
void DeliverLookup(base::WeakPtr<HostResolverSystemTask> self,
                   SystemDnsResultsCallback cb,
                   AddressList addresses,
                   int rv) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<HostResolverSystemTask> self,
             SystemDnsResultsCallback cb, AddressList addresses, int rv) {
            if (!self)
              return;
            std::move(cb).Run(addresses, 0, rv);
          },
          std::move(self), std::move(cb), std::move(addresses), rv));
}

// Look for a lookup's answer; again in a hundredth of a second while there
// is none, for at most twenty seconds.
void PollLookup(base::WeakPtr<HostResolverSystemTask> self,
                SystemDnsResultsCallback cb,
                int slot,
                base::TimeTicks started,
                std::string host) {
  if (!self) {
    br_stream_resolve_cancel(slot);
    return;
  }
  unsigned char ip4[4] = {0, 0, 0, 0};
  const int st = br_stream_resolve_poll(slot, ip4);
  if (st == 1) {
    std::move(cb).Run(
        AddressList(IPEndPoint(IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]), 0)),
        0, OK);
    return;
  }
  if (st < 0 || base::TimeTicks::Now() - started > base::Seconds(20)) {
    if (st == 0)
      br_stream_resolve_cancel(slot);
    printf("browser: dns: %s not resolved\n", host.c_str());
    std::move(cb).Run(AddressList(), 0, ERR_NAME_NOT_RESOLVED);
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PollLookup, std::move(self), std::move(cb), slot,
                     started, std::move(host)),
      base::Milliseconds(10));
}

}  // namespace

void HostResolverSystemTask::Start(SystemDnsResultsCallback results_cb) {
  // The lookup runs on the resolver's own task (br_stream_resolve_start) and
  // this side looks for the answer every hundredth of a second, so a name
  // server that is slow to answer does not stop the page while it waits.
  //
  // The answer is never delivered from inside this call: callers start a
  // task and then keep working on themselves, and one of them reads its own
  // fields right after starting the lookup -- completing the request here
  // destroyed it first.
  //
  // A task that resolves the machine's own name has no use here and reports
  // that it found nothing.
  if (results_cb.is_null())
    return;
  base::WeakPtr<HostResolverSystemTask> self = weak_ptr_factory_.GetWeakPtr();
  if (!hostname_.has_value() || address_family_ == ADDRESS_FAMILY_IPV6) {
    DeliverLookup(std::move(self), std::move(results_cb), AddressList(),
                  ERR_NAME_NOT_RESOLVED);
    return;
  }
  unsigned char ip4[4] = {0, 0, 0, 0};
  const int slot = br_stream_resolve_start(hostname_->c_str(), ip4);
  if (slot == -2) {
    DeliverLookup(std::move(self), std::move(results_cb),
                  AddressList(IPEndPoint(
                      IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]), 0)),
                  OK);
    return;
  }
  if (slot < 0) {
    printf("browser: dns: %s not resolved\n", hostname_->c_str());
    DeliverLookup(std::move(self), std::move(results_cb), AddressList(),
                  ERR_NAME_NOT_RESOLVED);
    return;
  }
  PollLookup(std::move(self), std::move(results_cb), slot,
             base::TimeTicks::Now(), *hostname_);
}

}  // namespace net
