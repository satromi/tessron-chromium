/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	eme_tessronos.cc
 *	ブラウザ: Encrypted Media Extensions (design 14.6.3): what a page's
 *	navigator.requestMediaKeySystemAccess and MediaKeys stand on.
 *
 *	Blink's EME modules ask the frame for its encrypted media client;
 *	this is that client (blink::WebEncryptedMediaClientImpl) with the key
 *	systems the program has, the factory of their CDMs and the answers
 *	of the permissions they may ask for.
 *
 *	Clear Key (org.w3.clearkey) is always there; its CDM is the media
 *	library's AesDecryptor (media::DefaultCdmFactory).
 *
 *	Widevine (com.widevine.alpha) is there only when the user has
 *	installed its CDM (br_widevine.cc): the library CDM is then loaded
 *	through the media library's own entry for such CDMs. Its manifest
 *	is checked (components/cdm's IsCdmManifestCompatibleWithChrome) and
 *	read for the codecs and encryption schemes it takes; the library,
 *	read from the user's object, is loaded into the process (ts_so.c)
 *	under the name libwidevinecdm.so, which dlopen then finds, and
 *	media::CdmModule initialises it; each MediaKeys is a media::CdmAdapter
 *	made by media::CdmAdapterFactory. The key system is described by
 *	components/cdm's WidevineKeySystemInfo with software security only:
 *	audio up to SW_SECURE_CRYPTO, video up to SW_SECURE_DECODE, no
 *	hardware-secure codecs, no persistent licenses, no distinctive
 *	identifier. What the CDM keeps (cdm::FileIO) goes to record 11 of
 *	the browser's storage object ("cdmfiles"), apart for each user and
 *	each origin of the page shown. Without the CDM, asking for
 *	Widevine is refused with NotSupportedError.
 *
 *	The key systems can change while the browser runs (the CDM installed
 *	or taken away): the list is given to the media library's
 *	KeySystemsImpl through the callback it registers, as a media client
 *	does, and given again on a change. The media client set for this
 *	(BrMediaClient) answers everything else as the library does without
 *	one. A CDM loaded stays in the process until it ends; taken away, it
 *	is no longer offered.
 */

#include <dlfcn.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "components/cdm/common/cdm_manifest.h"
#include "components/cdm/renderer/widevine_key_system_info.h"
#include "media/base/cdm_capability.h"
#include "media/base/cdm_config.h"
#include "media/base/eme_constants.h"
#include "media/base/key_systems_impl.h"
#include "media/base/key_systems_support_registration.h"
#include "media/base/media_client.h"
#include "media/base/media_permission.h"
#include "media/base/supported_types.h"
#include "media/cdm/api/content_decryption_module.h"
#include "media/cdm/cdm_adapter_factory.h"
#include "media/cdm/cdm_auxiliary_helper.h"
#include "media/cdm/cdm_helpers.h"
#include "media/cdm/cdm_module.h"
#include "media/cdm/default_cdm_factory.h"
#include "media/cdm/simple_cdm_allocator.h"
#include "third_party/blink/public/platform/media/key_system_config_selector.h"
#include "third_party/blink/public/platform/media/web_encrypted_media_client_impl.h"
#include "third_party/blink/public/platform/web_content_settings_client.h"
#include "third_party/blink/public/platform/web_encrypted_media_client.h"
#include "third_party/widevine/cdm/widevine_cdm_common.h"
#include "url/gurl.h"
#include "url/origin.h"

#include "../compat/ts_so.h"
#include "../net/br_store.h"
#include "br_widevine.h"

namespace {

/*
 * The permissions a key system may ask for: encrypted media is allowed,
 * and an identifier of the machine (which neither key system asks for
 * here) is not given
 */
class BrMediaPermission final : public media::MediaPermission {
public:
	void HasPermission( Type, PermissionStatusCB cb ) override { std::move(cb).Run(false); }
	void RequestPermission( Type, PermissionStatusCB cb ) override { std::move(cb).Run(false); }
	bool IsEncryptedMediaEnabled() override { return true; }
};

/* The page is the top of its window, and may keep what a CDM keeps */
class BrFrameDelegate final : public blink::KeySystemConfigSelector::WebLocalFrameDelegate {
public:
	bool IsCrossOriginToOutermostMainFrame() override { return false; }
	bool AllowStorageAccessSync( blink::WebContentSettingsClient::StorageType ) override { return true; }
};

/*
 * The media client: present so that the key systems are asked for
 * (KeySystemsImpl asks only when there is one); every other answer is
 * the media library's own when it has no client
 */
class BrMediaClient final : public media::MediaClient {
public:
	bool IsDecoderSupportedAudioType( const media::AudioType &t ) override
	{
		return media::IsDefaultDecoderSupportedAudioType(t);
	}
	bool IsDecoderSupportedVideoType( const media::VideoType &t ) override
	{
		return media::IsDefaultDecoderSupportedVideoType(t);
	}
	bool IsEncoderSupportedVideoType( const media::VideoType &t ) override
	{
		return media::IsDefaultEncoderSupportedVideoType(t);
	}
	bool IsSupportedBitstreamAudioCodec( media::AudioCodec ) override { return false; }
	bool ShouldSuppressAudioTracks() override { return false; }
	std::optional<media::AudioRendererAlgorithmParameters>
	GetAudioRendererAlgorithmParameters( media::AudioParameters ) override { return std::nullopt; }
	media::ExternalMemoryAllocator *GetMediaAllocator() override { return nullptr; }
};

/* ---------------------------------------------------------------- the Widevine CDM */

const char	kCdmName[] = "libwidevinecdm.so";	/* what it is loaded under, and dlopen finds */

int			g_wv_state;		/* 0 not loaded, 1 loaded, -1 failed (for this run) */
std::string		g_wv_version;		/* the manifest's version of what is loaded */
media::CdmCapability	g_wv_cap;
bool			g_wv_offered;		/* whether Widevine is in the list given now */

/* The manifest as a dictionary: nullopt with 'why' when it is not one */
std::optional<base::DictValue> manifest_of( const std::string &json, std::string *why )
{
	std::optional<base::DictValue> m = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
	if ( !m ) *why = "CDMの説明(manifest.json)が読めません";
	return m;
}

/* The user's CDM loaded and initialised, once a run: false when it cannot be */
bool wv_load()
{
	if ( g_wv_state != 0 ) return g_wv_state > 0;
	g_wv_state = -1;

	std::string manifest, library, err;
	if ( !br_wv_read(&manifest, &library, &err) ) {
		printf("browser: widevine: the CDM is not read: %s\n", err.c_str());
		return false;
	}
	std::optional<base::DictValue> m = manifest_of(manifest, &err);
	if ( !m || !IsCdmManifestCompatibleWithChrome(*m) || !ParseCdmManifest(*m, &g_wv_cap) ) {
		printf("browser: widevine: the CDM's manifest does not fit this browser\n");
		return false;
	}
	const std::string *v = m->FindString("version");
	g_wv_version = ( v != nullptr ) ? *v : "";

	char why[512] = "";
	void *so = ts_so_load(kCdmName, library.data(), library.size(), why, sizeof(why));
	library.clear();
	library.shrink_to_fit();
	if ( so == nullptr ) {
		printf("browser: widevine: the CDM is not loaded: %s\n", why);
		return false;
	}
	media::CdmModule *module = media::CdmModule::GetInstance();
	if ( !module->Initialize(base::FilePath(kCdmName)) ) {
		printf("browser: widevine: the CDM's entry points are not there\n");
		(void)ts_so_close(so);
		return false;
	}
	module->InitializeCdmModule();
	{
		void *h = dlopen(kCdmName, RTLD_LAZY);
		auto version = reinterpret_cast<const char *(*)()>(h != nullptr ? dlsym(h, "GetCdmVersion") : nullptr);
		printf("browser: widevine: CDM %s loaded (manifest %s)\n", version != nullptr ? version() : "?",
		       g_wv_version.c_str());
		if ( h != nullptr ) dlclose(h);
	}
	(void)ts_so_close(so);	/* CdmModule holds it open */
	g_wv_state = 1;
	return true;
}

/* The codecs the manifest names, as the EME layer counts them */
media::SupportedCodecs codecs_of( const media::CdmCapability &cap )
{
	media::SupportedCodecs c = media::EME_CODEC_NONE;

	for ( media::AudioCodec a : cap.audio_codecs ) {
		switch ( a ) {
		case media::AudioCodec::kOpus:		c |= media::EME_CODEC_OPUS; break;
		case media::AudioCodec::kVorbis:	c |= media::EME_CODEC_VORBIS; break;
		case media::AudioCodec::kFLAC:		c |= media::EME_CODEC_FLAC; break;
		case media::AudioCodec::kAAC:		c |= media::EME_CODEC_AAC; break;
		default:				break;
		}
	}
	for ( const auto &[codec, info] : cap.video_codecs ) {
		if ( !info.supports_clear_lead ) continue;
		switch ( codec ) {
		case media::VideoCodec::kVP8:
			c |= media::EME_CODEC_VP8;
			break;
		case media::VideoCodec::kVP9:
			if ( info.supported_profiles.empty() ) {
				c |= media::EME_CODEC_VP9_PROFILE0 | media::EME_CODEC_VP9_PROFILE2;
			} else {
				if ( info.supported_profiles.contains(media::VP9PROFILE_PROFILE0) ) c |= media::EME_CODEC_VP9_PROFILE0;
				if ( info.supported_profiles.contains(media::VP9PROFILE_PROFILE2) ) c |= media::EME_CODEC_VP9_PROFILE2;
			}
			break;
		case media::VideoCodec::kAV1:
			c |= media::EME_CODEC_AV1;
			break;
		case media::VideoCodec::kH264:
			c |= media::EME_CODEC_AVC1;
			break;
		default:
			break;
		}
	}
	return c;
}

/* The key systems beside Clear Key: Widevine when the user's CDM is loaded */
media::KeySystemInfos offered()
{
	media::KeySystemInfos v;

	g_wv_offered = false;
	if ( br_wv_installed() && wv_load() ) {
		using Robustness = cdm::WidevineKeySystemInfo::Robustness;
		/* temporary sessions only: persistent licenses need what verifies the host */
		base::flat_set<media::CdmSessionType> sessions = { media::CdmSessionType::kTemporary };
		v.push_back(std::make_unique<cdm::WidevineKeySystemInfo>(
			codecs_of(g_wv_cap), g_wv_cap.encryption_schemes, sessions,
			media::EME_CODEC_NONE, base::flat_set<media::EncryptionScheme>(),
			base::flat_set<media::CdmSessionType>(),
			Robustness::SW_SECURE_CRYPTO, Robustness::SW_SECURE_DECODE,
			media::EmeFeatureSupport::REQUESTABLE, media::EmeFeatureSupport::NOT_SUPPORTED));
		g_wv_offered = true;
	}
	printf("browser: eme: key systems: org.w3.clearkey%s\n", g_wv_offered ? ", com.widevine.alpha" : "");
	return v;
}

/* What KeySystemsImpl registered, told the key systems again on a change */
media::GetSupportedKeySystemsCB	g_ks_cb;

class BrKeySystemsRegistration final : public media::KeySystemSupportRegistration {
public:
	~BrKeySystemsRegistration() override { g_ks_cb.Reset(); }
};

/* ---------------------------------------------------------------- what the CDM keeps */

/*
 * The CDM's files, of every user and origin, as record 11 of the
 * browser's storage object holds them: the key "<user>\n<origin>\n<name>"
 * and the bytes, each as a 4-byte little-endian length and what follows
 */
std::map<std::string, std::string>	g_files;
bool					g_files_read;
std::set<std::string>			g_files_open;

void put32( std::string *s, uint32_t v )
{
	for ( int i = 0; i < 4; i++ ) s->push_back((char)( ( v >> ( 8 * i ) ) & 0xFF ));
}

void files_read()
{
	unsigned char	*data = nullptr;
	int		len = 0;

	if ( g_files_read ) return;
	g_files_read = true;
	if ( br_store_load("cdmfiles", &data, &len) < 0 ) return;
	size_t p = 0, n = (size_t)len;
	auto get32 = [&]( uint32_t *v ) {
		if ( n - p < 4 ) return false;
		*v = (uint32_t)data[p] | ( (uint32_t)data[p + 1] << 8 ) | ( (uint32_t)data[p + 2] << 16 )
		     | ( (uint32_t)data[p + 3] << 24 );
		p += 4;
		return true;
	};
	uint32_t kl, vl;
	while ( get32(&kl) && kl <= n - p ) {
		std::string k((const char *)data + p, kl);
		p += kl;
		if ( !get32(&vl) || vl > n - p ) break;
		g_files[k].assign((const char *)data + p, vl);
		p += vl;
	}
	free(data);
}

bool files_write()
{
	std::string out;
	for ( const auto &[k, v] : g_files ) {
		put32(&out, (uint32_t)k.size());
		out += k;
		put32(&out, (uint32_t)v.size());
		out += v;
	}
	return br_store_save("cdmfiles", out.data(), (int)out.size()) == 0;
}

/* A name the CDM may give a file: letters, digits, '.', '_' and '-', not starting with '_' */
bool file_name_ok( const std::string &n )
{
	if ( n.empty() || n.size() > 256 || n[0] == '_' ) return false;
	for ( char c : n ) {
		if ( !isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-' ) return false;
	}
	return true;
}

/* A file of the CDM's, its answers given later, on the thread it is used on */
class BrFileIO final : public cdm::FileIO {
public:
	BrFileIO( cdm::FileIOClient *client, std::string prefix )
		: client_(client), prefix_(std::move(prefix)) {}

	void Open( const char *name, uint32_t size ) override
	{
		using S = cdm::FileIOClient::Status;
		std::string n(name, size);
		S s = S::kSuccess;
		if ( !key_.empty() || prefix_.empty() || !file_name_ok(n) ) {
			s = S::kError;
		} else if ( g_files_open.count(prefix_ + n) > 0 ) {
			s = S::kInUse;
		} else {
			key_ = prefix_ + n;
			g_files_open.insert(key_);
		}
		Later(base::BindOnce(&BrFileIO::Opened, weak_.GetWeakPtr(), s));
	}

	void Read() override
	{
		using S = cdm::FileIOClient::Status;
		S s = key_.empty() ? S::kError : busy_ ? S::kInUse : S::kSuccess;
		if ( s == S::kSuccess ) busy_ = true;
		Later(base::BindOnce(&BrFileIO::ReadDone, weak_.GetWeakPtr(), s));
	}

	void Write( const uint8_t *data, uint32_t size ) override
	{
		bool ok = !key_.empty() && !busy_ && size <= media::kMaxFileSizeBytes;
		if ( ok ) {
			files_read();
			if ( size == 0 ) {
				g_files.erase(key_);
			} else {
				g_files[key_].assign((const char *)data, size);
			}
			ok = files_write();
			busy_ = true;
		}
		Later(base::BindOnce(&BrFileIO::Written, weak_.GetWeakPtr(), ok));
	}

	void Close() override
	{
		if ( !key_.empty() ) g_files_open.erase(key_);
		delete this;
	}

private:
	void Later( base::OnceClosure fn )
	{
		base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, std::move(fn));
	}

	void Opened( cdm::FileIOClient::Status s ) { client_->OnOpenComplete(s); }

	void ReadDone( cdm::FileIOClient::Status s )
	{
		if ( s != cdm::FileIOClient::Status::kSuccess ) {
			client_->OnReadComplete(s, nullptr, 0);
			return;
		}
		busy_ = false;
		files_read();
		auto it = g_files.find(key_);
		const std::string empty;
		const std::string &d = ( it != g_files.end() ) ? it->second : empty;
		client_->OnReadComplete(s, (const uint8_t *)d.data(), (uint32_t)d.size());
	}

	void Written( bool ok )
	{
		busy_ = false;
		client_->OnWriteComplete(ok ? cdm::FileIOClient::Status::kSuccess : cdm::FileIOClient::Status::kError);
	}

	~BrFileIO() override = default;

	cdm::FileIOClient	*client_;
	std::string		prefix_;	/* "<user>\n<origin>\n", "" when there is no origin */
	std::string		key_;
	bool			busy_ = false;
	base::WeakPtrFactory<BrFileIO> weak_{this};
};

/*
 * What a CdmAdapter asks of its host beside the CDM: the buffers and
 * frames (the media library's plain allocator), the files, and the
 * origin of the page shown
 */
class BrCdmHelper final : public media::CdmAuxiliaryHelper {
public:
	BrCdmHelper() : origin_(br_page_origin()) {}

	cdm::FileIO *CreateCdmFileIO( cdm::FileIOClient *client ) override
	{
		std::string user = br_wv_user();
		std::string prefix = ( user.empty() || origin_.empty() ) ? std::string() : user + "\n" + origin_ + "\n";
		return new BrFileIO(client, prefix);
	}
	url::Origin GetCdmOrigin() override { return url::Origin::Create(GURL(origin_)); }
	cdm::Buffer *CreateCdmBuffer( size_t capacity ) override { return alloc_.CreateCdmBuffer(capacity); }
	std::unique_ptr<media::VideoFrameImpl> CreateCdmVideoFrame() override { return alloc_.CreateCdmVideoFrame(); }

private:
	std::string			origin_;
	media::SimpleCdmAllocator	alloc_;
};

/* The CDMs: Widevine's through the library CDM's adapter, Clear Key's the library's own */
class BrCdmFactory final : public media::CdmFactory {
public:
	void Create( const media::CdmConfig &config, const media::SessionMessageCB &message,
		     const media::SessionClosedCB &closed, const media::SessionKeysChangeCB &keys,
		     const media::SessionExpirationUpdateCB &expiry, media::CdmCreatedCB done ) override
	{
		if ( config.key_system != kWidevineKeySystem ) {
			clear_key_.Create(config, message, closed, keys, expiry, std::move(done));
			return;
		}
		if ( !g_wv_offered ) {
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(std::move(done), nullptr,
							  media::CreateCdmStatus::kUnsupportedKeySystem));
			return;
		}
		widevine_.Create(config, message, closed, keys, expiry, std::move(done));
	}

private:
	media::DefaultCdmFactory	clear_key_;
	media::CdmAdapterFactory	widevine_{base::BindRepeating(
		[]() -> std::unique_ptr<media::CdmAuxiliaryHelper> { return std::make_unique<BrCdmHelper>(); })};
};

}  // namespace

bool br_eme_manifest_ok( const std::string &json, std::string *version, std::string *why )
{
	std::optional<base::DictValue> m = manifest_of(json, why);
	if ( !m ) return false;
	media::CdmCapability cap;
	if ( !IsCdmManifestCompatibleWithChrome(*m) || !ParseCdmManifest(*m, &cap) ) {
		*why = "このCDMは、このブラウザが使うCDMのインタフェースのバージョンに合いません";
		return false;
	}
	const std::string *v = m->FindString("version");
	*version = ( v != nullptr ) ? *v : "";
	return true;
}

void br_eme_widevine_changed()
{
	if ( g_ks_cb ) g_ks_cb.Run(offered());
}

/* The frame's encrypted media client (encrypted_media.patch) */
blink::WebEncryptedMediaClient *br_encrypted_media_client()
{
	static blink::WebEncryptedMediaClientImpl *client;
	if ( client == nullptr ) {
		media::SetMediaClient(new BrMediaClient());
		auto *key_systems = new media::KeySystemsImpl(base::BindOnce(
			[]( media::GetSupportedKeySystemsCB cb ) -> std::unique_ptr<media::KeySystemSupportRegistration> {
				g_ks_cb = cb;
				cb.Run(offered());
				return std::make_unique<BrKeySystemsRegistration>();
			}));
		client = new blink::WebEncryptedMediaClientImpl(key_systems, new BrCdmFactory(),
								new BrMediaPermission(),
								std::make_unique<BrFrameDelegate>());
	}
	return client;
}
