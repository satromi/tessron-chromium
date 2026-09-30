/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	root_store_tessronos.cc
 *	ブラウザ: the roots certificates are checked against, and the
 *	verifier that uses them (design 17.19).
 *
 *	The roots are the certificates of Chromium's root store built into
 *	the program as PEM blocks (br_roots, tools/browser/build.sh). They
 *	are loaded into a trust store and the ordinary path-building
 *	verifier is built on top, so a certificate is checked here the same
 *	way it is anywhere else.
 *
 *	Not carried over: the store's own limits on a root (an expiry beyond
 *	the certificate's, a name it is kept to), certificate transparency
 *	and revocation lists. Without them a few chains a full check would
 *	refuse are taken, never the other way round: the roots, and every
 *	signature and date along a chain, are checked in full.
 */

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"
#include "net/log/net_log_with_source.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/crl_set.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/do_nothing_ct_verifier.h"
#include "net/cert/internal/platform_trust_store.h"
#include "net/cert/internal/system_trust_store.h"
#include "third_party/boringssl/src/include/openssl/pool.h"
#include "third_party/boringssl/src/pki/cert_errors.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "third_party/boringssl/src/pki/trust_store.h"
#include "third_party/boringssl/src/pki/trust_store_in_memory.h"

#include <stdio.h>

#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/x509.h"

extern "C" const char br_roots[];
extern "C" const char br_roots_end[];

namespace net {

namespace {

class TessronosSystemTrustStore : public SystemTrustStore {
 public:
  TessronosSystemTrustStore() {
    size_t loaded = 0, seen = 0;
    BIO* m = BIO_new_mem_buf(br_roots, (ossl_ssize_t)(br_roots_end - br_roots));
    X509* x;
    while (m != nullptr &&
           (x = PEM_read_bio_X509(m, nullptr, nullptr, nullptr)) != nullptr) {
      seen++;
      uint8_t* der = nullptr;
      int len = i2d_X509(x, &der);
      X509_free(x);
      if (len <= 0)
        continue;
      bssl::UniquePtr<CRYPTO_BUFFER> buf(
          CRYPTO_BUFFER_new(der, (size_t)len, nullptr));
      OPENSSL_free(der);
      if (!buf)
        continue;
      bssl::CertErrors errors;
      std::shared_ptr<const bssl::ParsedCertificate> cert =
          bssl::ParsedCertificate::Create(std::move(buf), {}, &errors);
      if (cert == nullptr)
        continue;
      store_.AddTrustAnchor(cert);
      loaded++;
    }
    // the end of the blocks is reported as an error
    ERR_clear_error();
    if (m != nullptr)
      BIO_free(m);
    printf("browser: roots: %u of %u loaded\n", (unsigned)loaded,
           (unsigned)seen);
  }

  bssl::TrustStore* GetTrustStore() override { return &store_; }

  // Every root here came from the store; none was added by whoever is using
  // the machine, which is what the distinction is for.
  bool IsKnownRoot(const bssl::ParsedCertificate* cert) const override {
    return store_.Contains(cert);
  }

  // The system keeps no trust settings of its own, so there is nothing
  // below the compiled list to consult and nothing locally trusted.
  net::PlatformTrustStore* GetPlatformTrustStore() override { return nullptr; }

  bool IsLocallyTrustedRoot(
      const bssl::ParsedCertificate* trust_anchor) override {
    return false;
  }

  int64_t chrome_root_store_version() const override {
    return 0;
  }

  // The store's extra per-root limits are not carried over, so no root
  // carries one.
  base::span<const ChromeRootCertConstraints> GetChromeRootConstraints(
      const bssl::CertPathBuilderResultPath* path) const override {
    return {};
  }

  // The compiled list holds certificates only: no Merkle tree certificate
  // anchors, no metadata for them and no cosigner keys. Every question about
  // them answers "none", so a chain can only end at one of the roots above.
  bool IsKnownMtcAnchor(const bssl::MTCAnchor* anchor) const override {
    return false;
  }

  std::optional<base::Time> mtc_metadata_update_time() const override {
    return std::nullopt;
  }

  const TrustStoreChrome::MtcAnchorExtraData* GetMTCAnchorData(
      base::span<const uint8_t> log_id) const override {
    return nullptr;
  }

  std::optional<bssl::VerifyCertificateChainDelegate::MTCCosigner>
  GetMtcMirrorKey(base::span<const uint8_t> cosigner_id) const override {
    return std::nullopt;
  }

  bool IsMtcCosignerPolicySatisfied(
      const bssl::ParsedCertificate& target_cert,
      base::Time current_time,
      const bssl::MTCAnchor* mtc_anchor,
      base::span<const std::vector<uint8_t>> valid_additional_cosigners)
      const override {
    return false;
  }

  // The roots carry no store identifiers, only their certificates.
  std::optional<int32_t> GetCrsRootIdForCert(
      const bssl::CertPathBuilderResultPath* path) const override {
    return std::nullopt;
  }

  // The separate list for qualified signatures in the European scheme is a
  // different trust decision, and nothing here makes it.
  bssl::TrustStore* eutl_trust_store() override { return &empty_; }

 private:
  bssl::TrustStoreInMemory store_;
  bssl::TrustStoreInMemory empty_;
};

// The verifier can be asked to rebuild its checker when the trust data
// changes underneath it. Here the roots are compiled in and never change, so
// each request for a new one produces the same thing.
class TessronosVerifyProcFactory : public CertVerifyProcFactory {
 public:
  scoped_refptr<CertVerifyProc> CreateCertVerifyProc(
      scoped_refptr<CertNetFetcher> cert_net_fetcher,
      const CertVerifyProc::ImplParams& impl_params,
      const CertVerifyProc::InstanceParams& instance_params) override {
    return CreateCertVerifyProcBuiltin(
        std::move(cert_net_fetcher), impl_params.crl_set,
        std::make_unique<DoNothingCTVerifier>(),
        base::MakeRefCounted<DefaultCTPolicyEnforcer>(),
        std::make_unique<TessronosSystemTrustStore>(), instance_params,
        std::nullopt);
  }

 private:
  ~TessronosVerifyProcFactory() override = default;
};

// Checks a certificate where it is asked, on the network thread, rather than
// handing the work to a worker thread of the pool.
//
// The check itself is the same one the stock verifier performs; only where
// it runs differs. The answer still arrives through a task of its own,
// because the caller starts a verification and then keeps working on
// itself, and answering from inside that call destroys it mid-flight.
// A verification whose result is delivered from a task of its own. The caller
// holds it as the Request and drops it to cancel -- a TLS socket torn down
// while its handshake waits for the result does exactly that -- so the task
// only reaches the callback while the request still exists.
class InlineVerifyRequest final : public CertVerifier::Request {
 public:
  InlineVerifyRequest(CompletionOnceCallback callback, int result)
      : callback_(std::move(callback)), result_(result) {}

  base::WeakPtr<InlineVerifyRequest> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // The callback normally destroys this request, so nothing here is touched
  // after it runs.
  void Deliver() {
    CompletionOnceCallback callback = std::move(callback_);
    std::move(callback).Run(result_);
  }

 private:
  CompletionOnceCallback callback_;
  const int result_;
  base::WeakPtrFactory<InlineVerifyRequest> weak_factory_{this};
};

class InlineCertVerifier : public CertVerifier {
 public:
  explicit InlineCertVerifier(scoped_refptr<CertVerifyProc> proc)
      : proc_(std::move(proc)) {}

  // The verification itself is done in place. The result still goes back
  // from a task, not from within this call, because callers are not ready to
  // be re-entered here; and it goes through a request the caller can cancel,
  // because the posted result used to reach a socket that had already been
  // destroyed (its handshake state read back as freed memory).
  int Verify(const RequestParams& params,
             CertVerifyResult* verify_result,
             CompletionOnceCallback callback,
             std::unique_ptr<Request>* out_req,
             const NetLogWithSource& net_log) override {
    const int rv = proc_->Verify(
        params.certificate().get(), params.hostname(), params.ocsp_response(),
        params.sct_list(), params.flags(), verify_result, net_log);
    auto request =
        std::make_unique<InlineVerifyRequest>(std::move(callback), rv);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&InlineVerifyRequest::Deliver, request->GetWeakPtr()));
    *out_req = std::move(request);
    return ERR_IO_PENDING;
  }

  // The binding that ties a second certificate to the one on the connection
  // is a check this port does not make; nothing here presents one.
  void Verify2QwacBinding(
      const std::string& binding,
      const std::string& hostname,
      const scoped_refptr<X509Certificate>& tls_cert,
      base::OnceCallback<void(const scoped_refptr<X509Certificate>&)> callback,
      const NetLogWithSource& net_log) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), nullptr));
  }

  void SetConfig(const Config& config) override {}
  void AddObserver(Observer* observer) override {}
  void RemoveObserver(Observer* observer) override {}

 private:
  scoped_refptr<CertVerifyProc> proc_;
};

}  // namespace

// One verifier, built on the first request and kept: reading the roots is
// worth doing once.
std::unique_ptr<CertVerifier> BrCreateCertVerifier() {
  auto factory = base::MakeRefCounted<TessronosVerifyProcFactory>();
  scoped_refptr<CertVerifyProc> proc = factory->CreateCertVerifyProc(
      /*cert_net_fetcher=*/nullptr, CertVerifyProc::ImplParams(),
      CertVerifyProc::InstanceParams());
  if (proc == nullptr)
    return nullptr;
  return std::make_unique<InlineCertVerifier>(std::move(proc));
}

}  // namespace net
