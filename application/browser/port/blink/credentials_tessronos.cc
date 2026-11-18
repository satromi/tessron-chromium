/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	credentials_tessronos.cc
 *	ブラウザ: what navigator.credentials asks of a browser's process
 *	(design 14.4.7).
 *
 *	  - WebAuthn (passkeys): an authenticator of the browser's own, in
 *	    software, a platform authenticator. A credential is a P-256 key
 *	    made for the relying party; its attestation is "none". Making one
 *	    and signing in with one each ask the user on a panel first. The
 *	    keys are kept in the browser's storage object (record 9, with the
 *	    passwords, encrypted with a key of the user's own: br_store.cc),
 *	    and are there the next time the browser runs.
 *	  - FedCM (IdentityCredential): the identity provider's config is
 *	    fetched, then its accounts (with the user's cookies at the
 *	    provider), one is chosen on a panel, and the token the provider
 *	    gives for it is handed to the page.
 *	  - Passwords: PasswordCredential stored (navigator.credentials.store)
 *	    and a login form sent (br_password_offer) are kept for the origin
 *	    once the user agrees on a panel; navigator.credentials.get and a
 *	    field of a login form taking the focus (br_password_find) give
 *	    them back, asked on a panel first. There is no message service for
 *	    WebOTP: the page is told so.
 *
 *	What is kept is not here: the passwords of an origin, the passkeys
 *	of a relying party, a passkey made and a challenge signed are
 *	operations of br_cred_core.cc, done in this process when it opens the
 *	stores' objects, else by the network process for the renderer's site
 *	alone. A passkey's private key never comes here.
 *
 *	The services are on the services' thread; what waits for the user
 *	or the network is done on a thread of its own and answered back on
 *	the services' thread.
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/escape.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/credentialmanagement/credential_manager.mojom-blink.h"
#include "../net/br_data.h"
#include "third_party/blink/public/mojom/sms/webotp_service.mojom-blink.h"
#include "third_party/blink/public/mojom/webauthn/authenticator.mojom-blink.h"
#include "third_party/blink/public/mojom/webid/federated_request.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/gurl.h"

#include "../../br.h"
#include "../net/br_cred_core.h"
#include "../net/br_net.h"
#include "../net/br_store_core.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;

blink::Vector<uint8_t> vec_of( const std::string &s )
{
	blink::Vector<uint8_t> v;
	v.append_range(s);
	return v;
}

std::string str_of( const blink::Vector<uint8_t> &v )
{
	return std::string(v.begin(), v.end());
}

std::string b64url( const std::string &s )
{
	std::string out;
	base::Base64UrlEncode(s, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
	return out;
}

/* The host of an origin ("https://host:port") */
std::string host_of( const std::string &origin )
{
	return std::string(GURL(origin).host());
}

/* clientDataJSON: what the authenticator signs over, as a browser writes it */
std::string client_data( const char *type, const blink::Vector<uint8_t> &challenge, const std::string &origin )
{
	std::string ch(challenge.begin(), challenge.end());
	return std::string("{\"type\":\"") + type + "\",\"challenge\":\"" + b64url(ch) + "\",\"origin\":\""
	       + origin + "\",\"crossOrigin\":false}";
}

/*
 * The passwords a document of 'origin' may be given. A document of
 * another site than the window's page (an iframe; there is one renderer
 * for the page and its iframes) is given none. In a renderer the network
 * process says so itself from the site the browser gave the renderer; with
 * one process it is seen here from the page's origin.
 */
std::vector<BrSavedPassword> PasswordsOf( const std::string &origin )
{
	if ( origin.empty() ) return {};
	if ( !br_store_remote() && br_cred_site_of(origin) != br_cred_site_of(br_page_origin()) ) {
		printf("browser: credentials: %s is not of the page's site: no password given\n", origin.c_str());
		return {};
	}
	return br_cred_passwords(origin);
}

/* The origins that called preventSilentAccess() (the services' thread's and the engine's) */
std::mutex			g_silent_lock;
std::vector<std::string>	g_silent_off;

/*
 * Which of the passwords kept for an origin is to be used, asked on a
 * panel (a list for more than one, the one kept last chosen at first):
 * its index, -1 when none is (any thread but the services')
 */
int PickPassword( const std::string &origin, const std::vector<BrSavedPassword> &have )
{
	if ( have.size() == 1 ) return br_ask_permission(origin, "保存したパスワード「" + have[0].id + "」の入力") ? 0 : -1;
	std::vector<std::string> names;
	for ( const BrSavedPassword &p : have ) names.push_back(p.id);
	return br_choose(names, (int)have.size() - 1);
}

/* A password kept for an origin once the user agreed (any thread but the services') */
void KeepAsked( const std::string &origin, const BrSavedPassword &p )
{
	if ( !br_ask_permission(origin, "「" + ( p.id.empty() ? std::string("(名前なし)") : p.id ) + "」のパスワードの保存") ) return;
	if ( !br_cred_password_keep(origin, p) ) printf("browser: credentials: the password for %s not kept\n", origin.c_str());
}

/* ---------------------------------------------------------------- the authenticator */

class Authenticator final : public bm::Authenticator {
public:
	void Bind( mojo::PendingReceiver<bm::Authenticator> r ) { receivers_.Add(this, std::move(r)); }

	void MakeCredential( bm::PublicKeyCredentialCreationOptionsPtr options, MakeCredentialCallback cb ) override
	{
		std::string origin = br_page_origin();
		std::string rp = options->relying_party->id.Utf8();
		if ( rp.empty() ) rp = host_of(origin);
		if ( !br_cred_rp_fits(rp, origin) ) {
			std::move(cb).Run(bm::AuthenticatorStatus::BAD_RELYING_PARTY_ID, nullptr, nullptr);
			return;
		}
		bool es256 = options->public_key_parameters.empty();
		for ( const auto &p : options->public_key_parameters ) {
			if ( p->algorithm_identifier == -7 ) es256 = true;
		}
		if ( !es256 ) {
			std::move(cb).Run(bm::AuthenticatorStatus::ALGORITHM_UNSUPPORTED, nullptr, nullptr);
			return;
		}
		std::vector<std::string> exclude;
		for ( const auto &x : options->exclude_credentials ) exclude.push_back(str_of(x->id));
		std::string user_id = str_of(options->user->id);
		std::string user_name = options->user->name.Utf8();
		std::string cdata = client_data("webauthn.create", options->challenge, origin);
		/* the passkeys kept asked for, and the user on a panel, off the services' thread */
		std::thread([origin, rp, user_id, user_name, exclude, cdata, cb = std::move(cb)]() mutable {
			bool excluded = false;
			for ( const BrPasskeyFound &k : br_cred_passkeys(origin, rp) ) {
				if ( std::find(exclude.begin(), exclude.end(), k.id) != exclude.end() ) excluded = true;
			}
			int rc = BR_CRED_EXCLUDED;
			BrPasskeyMade made;
			if ( !excluded ) {
				rc = br_ask_permission(origin, "「" + user_name + "」のパスキーの作成")
				     ? br_cred_passkey_make(origin, rp, user_id, user_name, exclude, &made) : BR_CRED_REFUSED;
			}
			br_on_service_thread(base::BindOnce(&Authenticator::Made, rc, std::move(made), cdata, std::move(cb)));
		}).detach();
	}

	void GetCredential( bm::GetCredentialOptionsPtr options, GetCredentialCallback cb ) override
	{
		if ( !options->public_key ) {
			/* a password asked for: none is kept */
			auto info = bm::CredentialInfo::New();
			info->type = bm::CredentialType::EMPTY;
			std::move(cb).Run(bm::GetCredentialResponse::NewPasswordResponse(std::move(info)));
			return;
		}
		std::string origin = br_page_origin();
		const bm::PublicKeyCredentialRequestOptions &o = *options->public_key;
		std::string rp = o.relying_party_id.Utf8();
		if ( rp.empty() ) rp = host_of(origin);
		if ( !br_cred_rp_fits(rp, origin) ) {
			Fail(std::move(cb), bm::AuthenticatorStatus::BAD_RELYING_PARTY_ID);
			return;
		}
		std::vector<std::string> allow;
		for ( const auto &a : o.allow_credentials ) allow.push_back(str_of(a->id));
		std::string cdata = client_data("webauthn.get", o.challenge, origin);
		/* which passkey, or whether this one, asked on a panel, and the challenge signed */
		std::thread([origin, rp, allow, cdata, cb = std::move(cb)]() mutable {
			std::vector<BrPasskeyFound> found;
			for ( BrPasskeyFound &k : br_cred_passkeys(origin, rp) ) {
				if ( allow.empty() || std::find(allow.begin(), allow.end(), k.id) != allow.end() ) {
					found.push_back(std::move(k));
				}
			}
			int rc = BR_CRED_NONE, k = -1;
			BrPasskeySigned s;
			if ( found.empty() ) {
				printf("browser: webauthn: no passkey for %s\n", rp.c_str());
			} else {
				std::vector<std::string> names;
				for ( const BrPasskeyFound &f : found ) names.push_back(f.user_name + "  (" + rp + ")");
				if ( names.size() > 1 ) {
					k = br_choose(names, 0);
				} else if ( br_ask_permission(origin, "パスキー「" + names[0] + "」でのログイン") ) {
					k = 0;
				}
				rc = BR_CRED_REFUSED;
				if ( k >= 0 && k < (int)found.size() ) rc = br_cred_passkey_sign(origin, rp, found[(size_t)k].id, cdata, &s);
			}
			std::string id = ( rc == BR_CRED_OK ) ? found[(size_t)k].id : std::string();
			br_on_service_thread(base::BindOnce(&Authenticator::Signed, rc, id, std::move(s), cdata, std::move(cb)));
		}).detach();
	}

	void IsUserVerifyingPlatformAuthenticatorAvailable( IsUserVerifyingPlatformAuthenticatorAvailableCallback cb ) override
	{
		std::move(cb).Run(true);
	}
	void IsConditionalMediationAvailable( IsConditionalMediationAvailableCallback cb ) override
	{
		std::move(cb).Run(false);
	}
	void Report( bm::PublicKeyCredentialReportOptionsPtr, ReportCallback cb ) override
	{
		std::move(cb).Run(bm::AuthenticatorStatus::SUCCESS, nullptr);
	}
	/*
	 * What this client can do, named as a browser names it to
	 * PublicKeyCredential.getClientCapabilities(): the passkeys kept here
	 * are a platform authenticator that verifies the user (on a panel);
	 * passkeys are not offered in a field's autofill (conditional
	 * mediation) nor through a phone (hybrid), nor got without asking
	 * (immediate); related origins and the signals (which are taken) are
	 * as in Chrome. Blink adds the extensions.
	 */
	void GetClientCapabilities( GetClientCapabilitiesCallback cb ) override
	{
		static const struct { const char *name; bool supported; } kCaps[] = {
			{ "conditionalCreate", false }, { "conditionalGet", false }, { "hybridTransport", false },
			{ "immediateGet", false }, { "passkeyPlatformAuthenticator", true }, { "relatedOrigins", true },
			{ "signalAllAcceptedCredentials", true }, { "signalCurrentUserDetails", true },
			{ "signalUnknownCredential", true }, { "userVerifyingPlatformAuthenticator", true },
		};
		blink::Vector<bm::WebAuthnClientCapabilityPtr> caps;
		for ( const auto &c : kCaps ) {
			caps.push_back(bm::WebAuthnClientCapability::New(blink::String::FromUtf8(c.name), c.supported));
		}
		std::move(cb).Run(std::move(caps));
	}
	void Cancel() override {}

private:
	static void Fail( GetCredentialCallback cb, bm::AuthenticatorStatus status )
	{
		std::move(cb).Run(bm::GetCredentialResponse::NewGetAssertionResponse(
			bm::GetAssertionResponse::New(status, nullptr, nullptr)));
	}

	/* A passkey made (or why not), on the services' thread */
	static void Made( int rc, BrPasskeyMade made, std::string cdata, MakeCredentialCallback cb )
	{
		if ( rc != BR_CRED_OK ) {
			bm::AuthenticatorStatus st = rc == BR_CRED_EXCLUDED ? bm::AuthenticatorStatus::CREDENTIAL_EXCLUDED
						     : rc == BR_CRED_BAD_RP ? bm::AuthenticatorStatus::BAD_RELYING_PARTY_ID
						     : rc == BR_CRED_FAILED ? bm::AuthenticatorStatus::UNKNOWN_ERROR
						     : bm::AuthenticatorStatus::NOT_ALLOWED_ERROR;
			std::move(cb).Run(st, nullptr, nullptr);
			return;
		}
		auto r = bm::MakeCredentialAuthenticatorResponse::New();
		r->info = bm::CommonCredentialInfo::New(blink::String::FromUtf8(b64url(made.id)), vec_of(made.id),
							vec_of(cdata), vec_of(made.auth_data));
		r->authenticator_attachment = bm::AuthenticatorAttachment::PLATFORM;
		r->attestation_object = vec_of(made.attestation);
		r->transports.push_back(bm::AuthenticatorTransport::INTERNAL);
		r->public_key_der = vec_of(made.spki);
		r->public_key_algo = -7;
		std::move(cb).Run(bm::AuthenticatorStatus::SUCCESS, std::move(r), nullptr);
	}

	/* An assertion signed with the passkey chosen (or why not), on the services' thread */
	static void Signed( int rc, std::string id, BrPasskeySigned s, std::string cdata, GetCredentialCallback cb )
	{
		if ( rc != BR_CRED_OK ) {
			Fail(std::move(cb), rc == BR_CRED_BAD_RP ? bm::AuthenticatorStatus::BAD_RELYING_PARTY_ID
					    : rc == BR_CRED_FAILED ? bm::AuthenticatorStatus::UNKNOWN_ERROR
					    : bm::AuthenticatorStatus::NOT_ALLOWED_ERROR);
			return;
		}
		auto a = bm::GetAssertionAuthenticatorResponse::New();
		a->info = bm::CommonCredentialInfo::New(blink::String::FromUtf8(b64url(id)), vec_of(id), vec_of(cdata),
							vec_of(s.auth_data));
		a->authenticator_attachment = bm::AuthenticatorAttachment::PLATFORM;
		a->signature = vec_of(s.signature);
		a->user_handle = vec_of(s.user_handle);
		a->extensions = bm::AuthenticationExtensionsClientOutputs::New();
		std::move(cb).Run(bm::GetCredentialResponse::NewGetAssertionResponse(
			bm::GetAssertionResponse::New(bm::AuthenticatorStatus::SUCCESS, std::move(a), nullptr)));
	}

	mojo::ReceiverSet<bm::Authenticator>	receivers_;
};

/* ---------------------------------------------------------------- passwords and one-time codes */

class Passwords final : public bm::CredentialManager {
public:
	void Bind( mojo::PendingReceiver<bm::CredentialManager> r ) { receivers_.Add(this, std::move(r)); }

	/* navigator.credentials.store(): a password kept when the user agrees */
	void Store( bm::CredentialInfoPtr info, StoreCallback cb ) override
	{
		std::move(cb).Run();
		if ( !info || info->type != bm::CredentialType::PASSWORD || info->id.IsNull() || info->password.IsNull() ) return;
		std::string origin = br_page_origin();
		BrSavedPassword p{ info->id.Utf8(), info->name.IsNull() ? std::string() : info->name.Utf8(), info->password.Utf8() };
		if ( origin.empty() || p.id.empty() ) return;
		std::thread(&KeepAsked, origin, p).detach();
	}

	void PreventSilentAccess( PreventSilentAccessCallback cb ) override
	{
		{
			std::lock_guard<std::mutex> hold(g_silent_lock);
			g_silent_off.push_back(br_page_origin());
		}
		std::move(cb).Run();
	}

	/*
	 * navigator.credentials.get({password: true}): a password kept for the
	 * origin, given without asking only when the page asks silently and
	 * there is one (and the page has not prevented that), else after the
	 * user chose it on a panel. The passwords are asked for off the
	 * services' thread: in a renderer it is a call to the network process.
	 */
	void Get( bm::CredentialMediationRequirement mediation, bool passwords, const blink::Vector<blink::KURL> &,
		  GetCallback cb ) override
	{
		std::string origin = br_page_origin();
		bool silent_off;
		{
			std::lock_guard<std::mutex> hold(g_silent_lock);
			silent_off = std::find(g_silent_off.begin(), g_silent_off.end(), origin) != g_silent_off.end();
		}
		std::thread([origin, mediation, passwords, silent_off, cb = std::move(cb)]() mutable {
			std::vector<BrSavedPassword> have = passwords ? PasswordsOf(origin) : std::vector<BrSavedPassword>();
			int k = -1;
			if ( have.empty() || ( mediation == bm::CredentialMediationRequirement::kSilent
					       && ( have.size() != 1 || silent_off ) ) ) {
				k = -1;
			} else if ( mediation == bm::CredentialMediationRequirement::kSilent ) {
				k = 0;
			} else {
				k = PickPassword(origin, have);
			}
			br_on_service_thread(base::BindOnce([]( std::vector<BrSavedPassword> have, int k, GetCallback cb ) {
				std::move(cb).Run(bm::CredentialManagerError::SUCCESS,
						  k >= 0 && k < (int)have.size() ? Info(have[(size_t)k]) : Empty());
			}, std::move(have), k, std::move(cb)));
		}).detach();
	}

	static bm::CredentialInfoPtr Empty()
	{
		auto info = bm::CredentialInfo::New();
		info->type = bm::CredentialType::EMPTY;
		return info;
	}

	static bm::CredentialInfoPtr Info( const BrSavedPassword &p )
	{
		auto info = bm::CredentialInfo::New();
		info->type = bm::CredentialType::PASSWORD;
		info->id = blink::String::FromUtf8(p.id);
		info->name = blink::String::FromUtf8(p.name);
		info->password = blink::String::FromUtf8(p.password);
		return info;
	}

private:
	mojo::ReceiverSet<bm::CredentialManager>	receivers_;
};

class OneTimeCodes final : public bm::WebOTPService {
public:
	void Bind( mojo::PendingReceiver<bm::WebOTPService> r ) { receivers_.Add(this, std::move(r)); }
	void Receive( ReceiveCallback cb ) override { std::move(cb).Run(bm::SmsStatus::kBackendNotAvailable, blink::String()); }
	void Abort() override {}

private:
	mojo::ReceiverSet<bm::WebOTPService>	receivers_;
};

/* ---------------------------------------------------------------- FedCM */

/* A fetch for FedCM, waited for: the body when it came (2xx) */
bool fed_fetch( const std::string &url, const std::string &method, const std::string &body,
		const std::string &headers, std::string *out )
{
	char *buf = nullptr, *resp = nullptr, ctype[128] = "", final_url[2048] = "";
	unsigned len = 0;
	int status = 0;
	int ok = br_net_fetch_sync(url.c_str(), method.c_str(), body.data(), (unsigned)body.size(), headers.c_str(), &buf,
				   &len, ctype, sizeof(ctype), &status, &resp, final_url, sizeof(final_url), 30000);
	if ( ok == 1 && buf != nullptr ) out->assign(buf, len);
	free(buf);
	free(resp);
	printf("browser: fedcm: %s %s: %d\n", method.c_str(), url.c_str(), status);
	return ok == 1 && status >= 200 && status < 300;
}

std::string resolve( const std::string &base, const std::string *rel )
{
	return rel != nullptr ? GURL(base).Resolve(*rel).spec() : std::string();
}

class Federated final : public bm::FederatedRequestService {
public:
	void Bind( mojo::PendingReceiver<bm::FederatedRequestService> r ) { receivers_.Add(this, std::move(r)); }

	void StartTokenRequest( blink::Vector<bm::IdentityProviderGetParametersPtr> params, bm::CredentialMediationRequirement,
				mojo::PendingReceiver<bm::FederatedRequest>, StartTokenRequestCallback cb ) override
	{
		if ( params.empty() || params[0]->providers.empty() ) {
			Failed(std::move(cb), "no identity provider");
			return;
		}
		const bm::IdentityProviderRequestOptions &p = *params[0]->providers[0];
		std::string config = p.config->config_url.GetString().Utf8();
		std::string client = p.config->client_id.Utf8();
		std::string nonce = p.nonce.Utf8();
		std::string origin = br_page_origin();
		std::thread([config, client, nonce, origin, cb = std::move(cb)]() mutable {
			std::string token, why;
			Ask(config, client, nonce, origin, &token, &why);
			br_on_service_thread(base::BindOnce(&Federated::Answer, config, token, why, std::move(cb)));
		}).detach();
	}

	void RegisterIdP( const blink::KURL &, RegisterIdPCallback cb ) override
	{
		std::move(cb).Run(bm::RegisterIdpStatus::kErrorFeatureDisabled);
	}
	void UnregisterIdP( const blink::KURL &, UnregisterIdPCallback cb ) override { std::move(cb).Run(false); }
	void CloseModalDialogView() override {}
	void PreventSilentAccess( PreventSilentAccessCallback cb ) override { std::move(cb).Run(); }
	void RequestUserInfo( bm::IdentityProviderConfigPtr, RequestUserInfoCallback cb ) override
	{
		std::move(cb).Run(bm::RequestUserInfoResult::NewStatus(bm::RequestUserInfoStatus::kError));
	}
	void Disconnect( bm::IdentityCredentialDisconnectOptionsPtr, DisconnectCallback cb ) override
	{
		std::move(cb).Run(bm::DisconnectStatus::kError);
	}
	void ResolveTokenRequest( const blink::String &, bm::ResolveTokenParamsPtr, ResolveTokenRequestCallback cb ) override
	{
		std::move(cb).Run(false);
	}
	void SetIdpSigninStatus( const scoped_refptr<const blink::SecurityOrigin> &, bm::IdpSigninStatus,
				 bm::LoginStatusOptionsPtr, SetIdpSigninStatusCallback cb ) override
	{
		std::move(cb).Run();
	}

private:
	/*
	 * The provider asked, off the services' thread: its config, its
	 * accounts, the account the user chooses, and the token for it
	 */
	static void Ask( const std::string &config, const std::string &client, const std::string &nonce,
			 const std::string &origin, std::string *token, std::string *why )
	{
		const std::string hdrs = "Accept: application/json\r\nSec-Fetch-Dest: webidentity\r\n";
		std::string body;
		if ( !fed_fetch(config, "GET", std::string(), hdrs, &body) ) {
			*why = "the provider's config was not fetched";
			return;
		}
		std::optional<base::DictValue> c = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
		if ( !c ) {
			*why = "the provider's config is not JSON";
			return;
		}
		std::string accounts_url = resolve(config, c->FindString("accounts_endpoint"));
		std::string assertion_url = resolve(config, c->FindString("id_assertion_endpoint"));
		if ( accounts_url.empty() || assertion_url.empty() ) {
			*why = "the provider's config names no endpoints";
			return;
		}
		if ( !fed_fetch(accounts_url, "GET", std::string(), hdrs, &body) ) {
			*why = "the accounts were not fetched";
			return;
		}
		std::optional<base::DictValue> a = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
		const base::ListValue *list = a ? a->FindList("accounts") : nullptr;
		std::vector<std::string> ids, names;
		for ( size_t i = 0; list != nullptr && i < list->size(); i++ ) {
			const base::DictValue *acc = (*list)[i].GetIfDict();
			const std::string *id = acc ? acc->FindString("id") : nullptr;
			if ( id == nullptr ) continue;
			const std::string *name = acc->FindString("name");
			const std::string *mail = acc->FindString("email");
			ids.push_back(*id);
			names.push_back(( name ? *name : *id ) + ( mail ? "  <" + *mail + ">" : std::string() ));
		}
		if ( ids.empty() ) {
			*why = "the provider has no account signed in";
			return;
		}
		int k = br_choose(names, 0);
		if ( k < 0 || k >= (int)ids.size() ) {
			*why = "the user chose no account";
			return;
		}
		std::string form = "account_id=" + base::EscapeQueryParamValue(ids[(size_t)k], true)
				   + "&client_id=" + base::EscapeQueryParamValue(client, true)
				   + "&nonce=" + base::EscapeQueryParamValue(nonce, true)
				   + "&disclosure_text_shown=true&is_auto_selected=false";
		std::string post = hdrs + "Content-Type: application/x-www-form-urlencoded\r\nOrigin: " + origin + "\r\n";
		if ( !fed_fetch(assertion_url, "POST", form, post, &body) ) {
			*why = "the token was not given";
			return;
		}
		std::optional<base::DictValue> t = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
		const std::string *tok = t ? t->FindString("token") : nullptr;
		if ( tok == nullptr ) {
			*why = "the provider's answer has no token";
			return;
		}
		*token = *tok;
	}

	static void Answer( std::string config, std::string token, std::string why, StartTokenRequestCallback cb )
	{
		if ( token.empty() ) {
			Failed(std::move(cb), why);
			return;
		}
		printf("browser: fedcm: a token from %s\n", config.c_str());
		std::move(cb).Run(bm::TokenRequestSuccess::New(blink::KURL(blink::String::FromUtf8(config)),
							       base::Value(token), false));
	}

	static void Failed( StartTokenRequestCallback cb, const std::string &why )
	{
		printf("browser: fedcm: %s\n", why.c_str());
		std::move(cb).Run(base::unexpected(bm::TokenRequestFailure::New(bm::RequestTokenStatus::kError, nullptr)));
	}

	mojo::ReceiverSet<bm::FederatedRequestService>	receivers_;
};

Authenticator	*g_authenticator;
Passwords	*g_passwords;
OneTimeCodes	*g_codes;
Federated	*g_federated;

template <typename Interface, typename Service>
void Binder( Service **svc )
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( Service **svc, mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( Service **svc, mojo::PendingReceiver<Interface> r ) {
				if ( *svc == nullptr ) *svc = new Service();
				( *svc )->Bind(std::move(r));
			}, svc, mojo::PendingReceiver<Interface>(std::move(h))));
		}, svc));
}

}  // namespace

/*
 * A login form the page sends (its user name and password, from the
 * engine's thread): kept for the origin when the user agrees on a panel,
 * unless the same is kept already
 */
void br_password_offer( const std::string &origin, const std::string &user, const std::string &password )
{
	if ( origin.empty() || password.empty() ) return;
	BrSavedPassword p{ user, std::string(), password };
	std::thread([origin, p]() {
		for ( const BrSavedPassword &q : PasswordsOf(origin) ) {
			if ( q.id == p.id && q.password == p.password ) return;
		}
		KeepAsked(origin, p);
	}).detach();
}

/*
 * A field of a login form taking the focus (from the engine's thread):
 * the user name and password to put in it, chosen on a panel; false when
 * none is kept or none was chosen
 */
bool br_password_find( const std::string &origin, std::string *user, std::string *password )
{
	std::vector<BrSavedPassword> have = PasswordsOf(origin);
	if ( have.empty() ) return false;
	int k = PickPassword(origin, have);
	if ( k < 0 || k >= (int)have.size() ) return false;
	*user = have[(size_t)k].id;
	*password = have[(size_t)k].password;
	return true;
}

/* ---------------------------------------------------------------- the settings window (br_data.h) */

namespace {

/* 'work' done on the services' thread, waited for */
void on_service_and_wait( base::OnceClosure work )
{
	struct Wait {
		std::mutex		lock;
		std::condition_variable	cv;
		bool			done = false;
	};
	auto w = std::make_shared<Wait>();
	br_on_service_thread(base::BindOnce([]( base::OnceClosure work, std::shared_ptr<Wait> w ) {
		std::move(work).Run();
		std::lock_guard<std::mutex> hold(w->lock);
		w->done = true;
		w->cv.notify_all();
	}, std::move(work), w));
	std::unique_lock<std::mutex> hold(w->lock);
	w->cv.wait(hold, [&]() { return w->done; });
}

}  // namespace

void br_on_page_thread( base::OnceClosure work );			/* indexeddb_tessronos.cc */
void br_storage_clear( const BrClear &c );
void br_storage_sites( std::set<std::string> *out );
void br_idb_clear( const BrClear &c );
void br_idb_sites( std::set<std::string> *out );
void br_sw_clear( const BrClear &c );
void br_sw_sites( std::set<std::string> *out );
void br_cache_storage_clear( const BrClear &c );
void br_cache_storage_sites( std::set<std::string> *out );
void br_opfs_clear( const BrClear &c );
void br_opfs_sites( std::set<std::string> *out );
void br_perms_clear( const BrClear &c );

namespace {

/* 'work' done on the page's thread, waited for; given up after 20 s when a page's question holds that thread */
bool on_page_and_wait( base::OnceClosure work )
{
	struct Wait {
		std::mutex		lock;
		std::condition_variable	cv;
		bool			done = false;
	};
	auto w = std::make_shared<Wait>();
	br_on_page_thread(base::BindOnce([]( base::OnceClosure work, std::shared_ptr<Wait> w ) {
		std::move(work).Run();
		std::lock_guard<std::mutex> hold(w->lock);
		w->done = true;
		w->cv.notify_all();
	}, std::move(work), w));
	std::unique_lock<std::mutex> hold(w->lock);
	return w->cv.wait_for(hold, std::chrono::seconds(20), [&]() { return w->done; });
}

}  // namespace

void br_page_data_clear( const BrClear &c )
{
	(void)on_page_and_wait(base::BindOnce([]( BrClear c ) {
		if ( ( c.kinds & BR_CLEAR_LOCAL ) != 0 ) br_storage_clear(c);
		if ( ( c.kinds & BR_CLEAR_IDB ) != 0 ) br_idb_clear(c);
		if ( ( c.kinds & BR_CLEAR_SW ) != 0 ) br_sw_clear(c);
	}, c));
}

void br_page_data_sites( std::set<std::string> *out )
{
	auto got = std::make_shared<std::set<std::string>>();
	if ( on_page_and_wait(base::BindOnce([]( std::shared_ptr<std::set<std::string>> got ) {
		br_storage_sites(got.get());
		br_idb_sites(got.get());
		br_sw_sites(got.get());
	}, got)) ) out->insert(got->begin(), got->end());
}

void br_service_data_clear( const BrClear &c )
{
	on_service_and_wait(base::BindOnce([]( BrClear c ) {
		if ( ( c.kinds & BR_CLEAR_CACHES ) != 0 ) br_cache_storage_clear(c);
		if ( ( c.kinds & BR_CLEAR_OPFS ) != 0 ) br_opfs_clear(c);
		if ( ( c.kinds & BR_CLEAR_PERMS ) != 0 ) br_perms_clear(c);
	}, c));
}

void br_service_data_sites( std::set<std::string> *out )
{
	on_service_and_wait(base::BindOnce([]( std::set<std::string> *out ) {
		br_cache_storage_sites(out);
		br_opfs_sites(out);
	}, base::Unretained(out)));
}


/* The binders of the frames' broker, once when the engine is up */
void br_credentials_init()
{
	br_data_init();
	Binder<bm::Authenticator, Authenticator>(&g_authenticator);
	Binder<bm::CredentialManager, Passwords>(&g_passwords);
	Binder<bm::WebOTPService, OneTimeCodes>(&g_codes);
	Binder<bm::FederatedRequestService, Federated>(&g_federated);
}
