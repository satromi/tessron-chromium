/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	web_transport_tessronos.cc
 *	ブラウザ: WebTransport (new WebTransport(url)) over the network
 *	stack's HTTP/3 (design 14.5).
 *
 *	A page's WebTransport asks the network service through
 *	WebTransportConnector; here a connection is the network stack's
 *	WebTransport client (net::CreateWebTransportClient, over QUIC) on the
 *	network's thread, and the page's end of it -- the session, its
 *	streams and datagrams -- is joined to it there:
 *
 *	  - a stream's bytes go between the page's data pipes and the QUIC
 *	    stream as either side can take them; its end (FIN), a reset and
 *	    a request to stop sending are passed on each way;
 *	  - datagrams are sent as the session can and told of once
 *	    processed;
 *	  - the server's certificate may be one the page names by its
 *	    SHA-256 (serverCertificateHashes), as the stack checks;
 *	  - the response's headers reach the page without its cookies.
 *
 *	Everything here is on the network's thread.
 */

#include <stdio.h>

#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_handle.h"
#include "net/http/http_response_headers.h"
#include "net/quic/web_transport_client.h"
#include "net/quic/web_transport_error.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_types.h"
#include "net/url_request/url_request_context.h"
#include "services/network/public/mojom/web_transport.mojom-blink.h"
#include "third_party/blink/public/mojom/webtransport/web_transport_connector.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/gurl.h"
#include "url/origin.h"

#include "../../br.h"

/* The network's thread and its stack (net_fetch_tessronos.cc) */
void br_net_post( base::OnceClosure work );
net::URLRequestContext *br_net_context();

namespace {

namespace nm = network::mojom::blink;
namespace bm = blink::mojom::blink;

base::TimeDelta to_delta( absl::Duration d )
{
	return base::Microseconds(absl::ToInt64Microseconds(d));
}

nm::WebTransportStatsPtr stats_of( const webtransport::SessionStats &s )
{
	return nm::WebTransportStats::New(base::Time::Now(), to_delta(s.min_rtt), to_delta(s.smoothed_rtt),
					  to_delta(s.rtt_variation), s.estimated_send_rate_bps,
					  s.datagram_stats.expired_outgoing, s.datagram_stats.lost_outgoing);
}

webtransport::StreamPriority priority_of( const nm::WebTransportStreamPriority &p )
{
	return {p.send_group_id.value_or(0), p.send_order};
}

class Transport;

/* ---------------------------------------------------------------- a stream */

class Stream final {
public:
	/* told by the QUIC stream; the stream may outlive this */
	class Visitor final : public quic::WebTransportStreamVisitor {
	public:
		explicit Visitor( Stream *s ) : stream_(s->weak_.GetWeakPtr()) {}
		~Visitor() override
		{
			if ( Stream *s = stream_.get() ) s->Gone();
		}
		void OnCanRead() override
		{
			base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
										 base::BindOnce(&Stream::Receive, stream_));
		}
		void OnCanWrite() override
		{
			base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
										 base::BindOnce(&Stream::Send, stream_));
		}
		void OnResetStreamReceived( quic::WebTransportStreamError e ) override
		{
			if ( Stream *s = stream_.get() ) s->ResetReceived(e);
		}
		void OnStopSendingReceived( quic::WebTransportStreamError e ) override
		{
			if ( Stream *s = stream_.get() ) s->StopSendingReceived(e);
		}
		void OnWriteSideInDataRecvdState() override
		{
			if ( Stream *s = stream_.get() ) s->AllSent();
		}

	private:
		base::WeakPtr<Stream>	stream_;
	};

	Stream( Transport *t, quic::WebTransportStream *out, quic::WebTransportStream *in,
		mojo::ScopedDataPipeConsumerHandle readable, mojo::ScopedDataPipeProducerHandle writable )
		: transport_(t), id_((out != nullptr ? out : in)->GetStreamId()), outgoing_(out), incoming_(in),
		  readable_(std::move(readable)), writable_(std::move(writable)),
		  readable_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
		  writable_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL)
	{
		if ( outgoing_ ) {
			outgoing_->SetVisitor(std::make_unique<Visitor>(this));
			readable_watcher_.Watch(readable_.get(), MOJO_HANDLE_SIGNAL_NEW_DATA_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
						MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
						base::BindRepeating(&Stream::Readable, base::Unretained(this)));
			readable_watcher_.ArmOrNotify();
		}
		if ( incoming_ ) {
			if ( incoming_ != outgoing_ ) incoming_->SetVisitor(std::make_unique<Visitor>(this));
			writable_watcher_.Watch(writable_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
						MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
						base::BindRepeating(&Stream::Writable, base::Unretained(this)));
			writable_watcher_.ArmOrNotify();
		}
	}
	~Stream();

	void FinFromPage()
	{
		fin_from_page_ = true;
		MaySendFin();
	}
	void SetPriority( const webtransport::StreamPriority &p )
	{
		if ( outgoing_ ) outgoing_->SetPriority(p);
	}
	void Abort( uint8_t code )
	{
		if ( !outgoing_ ) return;
		outgoing_->ResetWithUserCode(code);
		outgoing_ = nullptr;
		readable_watcher_.Cancel();
		readable_.reset();
		MayDispose();
	}
	void StopSending( uint8_t code )
	{
		if ( !incoming_ ) return;
		incoming_->SendStopSending(code);
		incoming_ = nullptr;
		writable_watcher_.Cancel();
		writable_.reset();
		MayDispose();
	}

	/* the page's bytes to the QUIC stream */
	void Send()
	{
		MaySendFin();
		while ( readable_ && outgoing_ && outgoing_->CanWrite() ) {
			base::span<const uint8_t> data;
			MojoResult r = readable_->BeginReadData(MOJO_BEGIN_READ_DATA_FLAG_NONE, data);
			if ( r == MOJO_RESULT_SHOULD_WAIT ) {
				readable_watcher_.Arm();
				return;
			}
			if ( r == MOJO_RESULT_FAILED_PRECONDITION ) {
				end_of_pipe_ = true;
				MaySendFin();
				return;
			}
			if ( !outgoing_->Write(std::string_view(reinterpret_cast<const char *>(data.data()), data.size())) ) {
				readable_->EndReadData(0);
				return;
			}
			readable_->EndReadData(data.size());
		}
	}

	/* the QUIC stream's bytes to the page */
	void Receive();

private:
	friend class Visitor;

	void Readable( MojoResult, const mojo::HandleSignalsState & ) { Send(); }
	void Writable( MojoResult, const mojo::HandleSignalsState & ) { Receive(); }

	void MaySendFin()
	{
		if ( !readable_ || !outgoing_ || !end_of_pipe_ || !fin_from_page_ ) return;
		if ( outgoing_->SendFin() ) {
			readable_watcher_.Cancel();
			readable_.reset();
		}
	}

	void Gone();
	void ResetReceived( quic::WebTransportStreamError e );
	void StopSendingReceived( quic::WebTransportStreamError e );
	void AllSent();
	void MayDispose();
	void Dispose();

	Transport				*transport_;
	uint32_t				id_;
	quic::WebTransportStream		*outgoing_;
	quic::WebTransportStream		*incoming_;
	mojo::ScopedDataPipeConsumerHandle	readable_;	/* for outgoing_ */
	mojo::ScopedDataPipeProducerHandle	writable_;	/* for incoming_ */
	mojo::SimpleWatcher			readable_watcher_;
	mojo::SimpleWatcher			writable_watcher_;
	bool					end_of_pipe_ = false;
	bool					fin_from_page_ = false;
	base::WeakPtrFactory<Stream>		weak_{this};
};

/* ---------------------------------------------------------------- a session */

class Transport final : public nm::WebTransport, public net::WebTransportClientVisitor {
public:
	Transport( const GURL &url, const url::Origin &origin, const net::WebTransportParameters &params,
		   mojo::PendingRemote<nm::WebTransportHandshakeClient> handshake )
		: receiver_(this), handshake_(std::move(handshake))
	{
		transport_ = net::CreateWebTransportClient(url, origin, this, net::NetworkAnonymizationKey::CreateSameSite(
								    net::SchemefulSite(origin)),
							    net::handles::kInvalidNetworkHandle, br_net_context(), params);
		handshake_.set_disconnect_handler(base::BindOnce(&Transport::Dispose, base::Unretained(this)));
		transport_->Connect();
	}
	~Transport() override { torn_down_ = true; }

	/* the page's session */
	void SendDatagram( base::span<const uint8_t> data, SendDatagramCallback cb ) override
	{
		datagram_cbs_.push(std::move(cb));
		transport_->session()->SendOrQueueDatagram(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
	}

	void CreateStream( mojo::ScopedDataPipeConsumerHandle readable, mojo::ScopedDataPipeProducerHandle writable,
			   nm::WebTransportStreamPriorityPtr priority, CreateStreamCallback cb ) override
	{
		quic::WebTransportSession *session = transport_->session();
		if ( handshake_ || session == nullptr ) {
			std::move(cb).Run(false, 0);
			return;
		}
		bool bidi = writable.is_valid();
		if ( bidi ? !session->CanOpenNextOutgoingBidirectionalStream()
			  : !session->CanOpenNextOutgoingUnidirectionalStream() ) {
			std::move(cb).Run(false, 0);
			return;
		}
		quic::WebTransportStream *s = bidi ? session->OpenOutgoingBidirectionalStream()
						   : session->OpenOutgoingUnidirectionalStream();
		if ( priority ) s->SetPriority(priority_of(*priority));
		uint32_t id = s->GetStreamId();
		streams_[id] = std::make_unique<Stream>(this, s, bidi ? s : nullptr, std::move(readable), std::move(writable));
		std::move(cb).Run(true, id);
	}

	void AcceptBidirectionalStream( AcceptBidirectionalStreamCallback cb ) override
	{
		bidi_accepts_.push(std::move(cb));
		OnIncomingBidirectionalStreamAvailable();
	}
	void AcceptUnidirectionalStream( AcceptUnidirectionalStreamCallback cb ) override
	{
		uni_accepts_.push(std::move(cb));
		OnIncomingUnidirectionalStreamAvailable();
	}
	void SendFin( uint32_t id ) override
	{
		auto s = streams_.find(id);
		if ( s != streams_.end() ) s->second->FinFromPage();
	}
	void AbortStream( uint32_t id, uint8_t code ) override
	{
		auto s = streams_.find(id);
		if ( s != streams_.end() ) s->second->Abort(code);
	}
	void StopSending( uint32_t id, uint8_t code ) override
	{
		auto s = streams_.find(id);
		if ( s != streams_.end() ) s->second->StopSending(code);
	}
	void SetOutgoingDatagramExpirationDuration( base::TimeDelta d ) override
	{
		if ( torn_down_ || closing_ || transport_->session() == nullptr ) return;
		transport_->session()->SetDatagramMaxTimeInQueue(absl::Microseconds(d.InMicroseconds()));
	}
	void GetStats( GetStatsCallback cb ) override
	{
		if ( torn_down_ || transport_->session() == nullptr ) {
			std::move(cb).Run(nullptr);
			return;
		}
		std::move(cb).Run(stats_of(transport_->session()->GetSessionStats()));
	}
	void Close( nm::WebTransportCloseInfoPtr info ) override
	{
		if ( torn_down_ || closing_ ) return;
		closing_ = true;
		receiver_.reset();
		handshake_.reset();
		client_.reset();
		std::optional<net::WebTransportCloseInfo> close;
		if ( info ) {
			std::string reason = info->reason.Utf8();
			if ( reason.size() > 1024 ) base::TruncateUTF8ToByteSize(reason, 1024, &reason);
			close.emplace(info->code, reason);
		}
		transport_->Close(close);
	}
	void SetStreamPriority( uint32_t id, nm::WebTransportStreamPriorityPtr p ) override
	{
		auto s = streams_.find(id);
		if ( s != streams_.end() ) s->second->SetPriority(priority_of(*p));
	}

	/* the network stack's client */
	void OnLocalNetworkAccessCheck( const net::IPEndPoint &, const net::NetLogWithSource &,
					net::CompletionOnceCallback cb ) override
	{
		std::move(cb).Run(net::OK);
	}
	void OnBeforeConnect( const net::IPEndPoint &address ) override
	{
		if ( torn_down_ || closing_ || !handshake_ ) return;
		handshake_->OnBeforeConnect(address);
	}
	void OnConnected( scoped_refptr<net::HttpResponseHeaders> headers ) override
	{
		if ( torn_down_ || closing_ || !handshake_ ) return;
		auto shown = base::MakeRefCounted<net::HttpResponseHeaders>(headers->raw_headers());
		shown->RemoveHeader("Set-Cookie");
		shown->RemoveHeader("Set-Cookie2");
		printf("browser: webtransport: connected\n");
		handshake_->OnConnectionEstablished(receiver_.BindNewPipeAndPassRemote(), client_.BindNewPipeAndPassReceiver(),
						    shown, blink::String::FromUtf8(transport_->session()->GetNegotiatedSubprotocol().value_or("")),
						    stats_of(transport_->session()->GetSessionStats()));
		handshake_.reset();
		receiver_.set_disconnect_handler(base::BindOnce(&Transport::Dispose, base::Unretained(this)));
		if ( draining_ ) client_->OnDraining();
	}
	void OnConnectionFailed( const net::WebTransportError &e ) override
	{
		if ( torn_down_ || closing_ || !handshake_ ) return;
		printf("browser: webtransport: not connected (%d, %s)\n", e.net_error, e.details.c_str());
		handshake_->OnHandshakeFailed(nm::WebTransportError::New(e.net_error, e.quic_error,
									blink::String::FromUtf8(e.details), e.safe_to_report_details));
		TearDown();
	}
	void OnDraining() override
	{
		if ( torn_down_ || closing_ || draining_ ) return;
		draining_ = true;
		if ( client_.is_bound() ) client_->OnDraining();
	}
	void OnClosed( const std::optional<net::WebTransportCloseInfo> &info ) override
	{
		if ( torn_down_ ) return;
		if ( closing_ ) {
			closing_ = false;
		} else if ( client_.is_bound() ) {
			nm::WebTransportCloseInfoPtr close;
			if ( info ) close = nm::WebTransportCloseInfo::New(info->code, blink::String::FromUtf8(info->reason));
			nm::WebTransportStatsPtr stats;
			if ( transport_ && transport_->session() ) stats = stats_of(transport_->session()->GetSessionStats());
			client_->OnClosed(std::move(close), std::move(stats));
		}
		TearDown();
	}
	void OnError( const net::WebTransportError & ) override
	{
		if ( torn_down_ ) return;
		closing_ = false;
		TearDown();
	}
	void OnIncomingBidirectionalStreamAvailable() override
	{
		if ( torn_down_ || closing_ || !client_.is_bound() ) return;
		while ( !bidi_accepts_.empty() ) {
			quic::WebTransportStream *s = transport_->session()->AcceptIncomingBidirectionalStream();
			if ( s == nullptr ) return;
			mojo::ScopedDataPipeConsumerHandle out_r, in_r;
			mojo::ScopedDataPipeProducerHandle out_w, in_w;
			if ( !Pipe(&out_w, &out_r) || !Pipe(&in_w, &in_r) ) {
				s->ResetDueToInternalError();
				return;
			}
			auto cb = std::move(bidi_accepts_.front());
			bidi_accepts_.pop();
			uint32_t id = s->GetStreamId();
			streams_[id] = std::make_unique<Stream>(this, s, s, std::move(out_r), std::move(in_w));
			std::move(cb).Run(id, std::move(in_r), std::move(out_w));
		}
	}
	void OnIncomingUnidirectionalStreamAvailable() override
	{
		if ( torn_down_ || closing_ || !client_.is_bound() ) return;
		while ( !uni_accepts_.empty() ) {
			quic::WebTransportStream *s = transport_->session()->AcceptIncomingUnidirectionalStream();
			if ( s == nullptr ) return;
			mojo::ScopedDataPipeConsumerHandle in_r;
			mojo::ScopedDataPipeProducerHandle in_w;
			if ( !Pipe(&in_w, &in_r) ) {
				s->ResetDueToInternalError();
				return;
			}
			auto cb = std::move(uni_accepts_.front());
			uni_accepts_.pop();
			uint32_t id = s->GetStreamId();
			streams_[id] = std::make_unique<Stream>(this, nullptr, s, mojo::ScopedDataPipeConsumerHandle(),
								std::move(in_w));
			std::move(cb).Run(id, std::move(in_r));
		}
	}
	void OnDatagramReceived( std::string_view d ) override
	{
		if ( torn_down_ || closing_ || !client_.is_bound() ) return;
		client_->OnDatagramReceived(base::span<const uint8_t>(reinterpret_cast<const uint8_t *>(d.data()), d.size()));
	}
	void OnCanCreateNewOutgoingBidirectionalStream() override {}
	void OnCanCreateNewOutgoingUnidirectionalStream() override {}
	void OnDatagramProcessed( std::optional<quic::DatagramStatus> status ) override
	{
		if ( datagram_cbs_.empty() ) return;
		auto cb = std::move(datagram_cbs_.front());
		datagram_cbs_.pop();
		std::move(cb).Run(status == quic::DATAGRAM_STATUS_SUCCESS);
	}

	/* for the streams */
	nm::WebTransportClient *client() { return client_.is_bound() ? client_.get() : nullptr; }
	bool ending() const { return closing_ || torn_down_; }
	void Forget( uint32_t id ) { streams_.erase(id); }

private:
	static bool Pipe( mojo::ScopedDataPipeProducerHandle *w, mojo::ScopedDataPipeConsumerHandle *r )
	{
		const MojoCreateDataPipeOptions options = {sizeof(options), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1, 256 * 1024};
		return mojo::CreateDataPipe(&options, *w, *r) == MOJO_RESULT_OK;
	}

	void TearDown()
	{
		torn_down_ = true;
		receiver_.reset();
		handshake_.reset();
		client_.reset();
		base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
									 base::BindOnce(&Transport::Dispose, weak_.GetWeakPtr()));
	}

	void Dispose()
	{
		if ( transport_ && !torn_down_ && transport_->session() ) transport_->Close(std::nullopt);
		receiver_.reset();
		streams_.clear();
		delete this;
	}

	std::unique_ptr<net::WebTransportClient>		transport_;
	mojo::Receiver<nm::WebTransport>			receiver_;
	mojo::Remote<nm::WebTransportHandshakeClient>		handshake_;
	mojo::Remote<nm::WebTransportClient>			client_;
	std::map<uint32_t, std::unique_ptr<Stream>>		streams_;
	std::queue<SendDatagramCallback>			datagram_cbs_;
	std::queue<AcceptBidirectionalStreamCallback>		bidi_accepts_;
	std::queue<AcceptUnidirectionalStreamCallback>		uni_accepts_;
	bool							closing_ = false;
	bool							torn_down_ = false;
	bool							draining_ = false;
	base::WeakPtrFactory<Transport>				weak_{this};
};

Stream::~Stream()
{
	quic::WebTransportStream *s = incoming_ != nullptr ? incoming_ : outgoing_;
	if ( s == nullptr || transport_->ending() ) return;
	s->MaybeResetDueToStreamObjectGone();
}

void Stream::Receive()
{
	while ( incoming_ ) {
		quic::WebTransportStream::ReadResult rr;
		if ( incoming_->ReadableBytes() > 0 ) {
			base::span<uint8_t> buf;
			MojoResult r = writable_->BeginWriteData(mojo::DataPipeProducerHandle::kNoSizeHint,
								 MOJO_BEGIN_WRITE_DATA_FLAG_NONE, buf);
			if ( r == MOJO_RESULT_SHOULD_WAIT ) {
				writable_watcher_.Arm();
				return;
			}
			if ( r == MOJO_RESULT_FAILED_PRECONDITION ) {
				writable_watcher_.Cancel();
				writable_.reset();
				incoming_ = nullptr;
				MayDispose();
				return;
			}
			rr = incoming_->Read(absl::MakeSpan(reinterpret_cast<char *>(buf.data()), buf.size()));
			writable_->EndWriteData(rr.bytes_read);
		} else {
			rr = incoming_->Read(absl::Span<char>());
			if ( !rr.fin ) return;
		}
		if ( rr.fin ) {
			if ( auto *c = transport_->client() ) c->OnIncomingStreamClosed(id_, true);
			writable_watcher_.Cancel();
			writable_.reset();
			incoming_ = nullptr;
			MayDispose();
			return;
		}
	}
}

/* the QUIC stream is gone: what of it was still open ends */
void Stream::Gone()
{
	if ( incoming_ ) {
		writable_watcher_.Cancel();
		writable_.reset();
		if ( auto *c = transport_->client() ) c->OnIncomingStreamClosed(id_, false);
		incoming_ = nullptr;
	}
	if ( outgoing_ ) {
		readable_watcher_.Cancel();
		readable_.reset();
		outgoing_ = nullptr;
	}
	MayDispose();
}

void Stream::ResetReceived( quic::WebTransportStreamError e )
{
	if ( auto *c = transport_->client() ) c->OnReceivedResetStream(id_, e);
	incoming_ = nullptr;
	writable_watcher_.Cancel();
	writable_.reset();
	MayDispose();
}

void Stream::StopSendingReceived( quic::WebTransportStreamError e )
{
	if ( auto *c = transport_->client() ) c->OnReceivedStopSending(id_, e);
	outgoing_ = nullptr;
	readable_watcher_.Cancel();
	readable_.reset();
	MayDispose();
}

void Stream::AllSent()
{
	if ( auto *c = transport_->client() ) c->OnOutgoingStreamClosed(id_);
	outgoing_ = nullptr;
	readable_watcher_.Cancel();
	readable_.reset();
	MayDispose();
}

void Stream::MayDispose()
{
	if ( outgoing_ || incoming_ ) return;
	base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&Stream::Dispose, weak_.GetWeakPtr()));
}

void Stream::Dispose()
{
	transport_->Forget(id_);	/* deletes this */
}

/* ---------------------------------------------------------------- the connector */

class Connector final : public bm::WebTransportConnector {
public:
	explicit Connector( std::string origin ) : origin_(url::Origin::Create(GURL(origin))) {}

	void Connect( const blink::KURL &url, blink::Vector<nm::WebTransportCertificateFingerprintPtr> fingerprints,
		      const blink::Vector<blink::String> &protocols, nm::WebTransportCongestionControl cc,
		      std::optional<uint16_t> uni, std::optional<uint16_t> bidi,
		      net::HttpRequestHeaders::HeaderVector headers,
		      mojo::PendingRemote<nm::WebTransportHandshakeClient> client ) override
	{
		net::WebTransportParameters p;
		p.enable_web_transport_http3 = true;
		for ( const blink::String &s : protocols ) p.application_protocols.push_back(s.Utf8());
		switch ( cc ) {
		case nm::WebTransportCongestionControl::kThroughput:
			p.congestion_control_hint = net::WebTransportParameters::CongestionControlHint::kThroughput;
			break;
		case nm::WebTransportCongestionControl::kLowLatency:
			p.congestion_control_hint = net::WebTransportParameters::CongestionControlHint::kLowLatency;
			break;
		default:
			p.congestion_control_hint = net::WebTransportParameters::CongestionControlHint::kDefault;
			break;
		}
		p.anticipated_concurrent_incoming_unidirectional_streams = uni;
		p.anticipated_concurrent_incoming_bidirectional_streams = bidi;
		for ( auto &f : fingerprints ) {
			p.server_certificate_fingerprints.push_back(
				quic::CertificateFingerprint{.algorithm = f->algorithm.Utf8(), .fingerprint = f->fingerprint.Utf8()});
		}
		p.additional_headers = std::move(headers);
		GURL u(url.GetString().Utf8());
		printf("browser: webtransport: %s\n", u.spec().c_str());
		new Transport(u, origin_, p, std::move(client));	/* deletes itself when done */
	}

private:
	url::Origin	origin_;
};

}  // namespace

/* The binder of the frames' broker, once when the engine is up: a connector for the page's origin, on the network's thread */
void br_web_transport_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::WebTransportConnector::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_net_post(base::BindOnce([]( std::string origin, mojo::PendingReceiver<bm::WebTransportConnector> r ) {
				mojo::MakeSelfOwnedReceiver(std::make_unique<Connector>(origin), std::move(r));
			}, br_page_origin(), mojo::PendingReceiver<bm::WebTransportConnector>(std::move(h))));
		}));
}
