/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	serial_tessronos.cc
 *	ブラウザ: Web Serial (navigator.serial) on the system's serial ports
 *	(design 14.4.9).
 *
 *	What a browser's process does for Blink's modules/serial:
 *
 *	  - requestPort() lists the machine's ports on a panel and the user
 *	    chooses one; the page may use that port (and see it in
 *	    getPorts()) while the browser runs, until it forgets it;
 *	  - open() opens the port (br_serial.cc, SVC class 11) at the
 *	    speed asked for. The port's data is read on a thread of its own
 *	    while the page reads, and handed to the page's data pipe on the
 *	    services' thread; what the page writes is taken from its pipe
 *	    there and sent by a thread of its own, so a slow port holds up
 *	    neither the services nor the page.
 *	  - The system's ports have only the data lines: there are no
 *	    control signals to set or read, and the format is 8 data bits,
 *	    no parity, one stop bit.
 *
 *	The services are on the services' thread.
 */

#include <stdio.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/device/public/mojom/serial.mojom-blink.h"
#include "third_party/blink/public/mojom/serial/serial.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../../br.h"
#include "../net/br_serial.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;
namespace dm = device::mojom::blink;

constexpr int		kChunk = 4096;		/* bytes moved at a time */
constexpr size_t	kHeld = 64 * 1024;	/* bytes read and not yet in the page's pipe */
constexpr int		kReadWait = 100;	/* ms a read of the port waits */

/* ---------------------------------------------------------------- an open port */

class Port final : public dm::SerialPort {
public:
	Port( int port, uint32_t speed, mojo::PendingRemote<dm::SerialPortClient> client )
		: port_(port), speed_(speed), client_(std::move(client)),
		  out_watch_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
		  in_watch_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL)
	{
		self_ = weak_.GetWeakPtr();
		state_ = std::make_shared<Shared>();
		std::shared_ptr<Shared> st = state_;
		base::WeakPtr<Port> self = self_;
		int p = port_;
		reader_ = std::thread([st, self, p]() { Read(st, self, p); });
		writer_ = std::thread([st, self, p]() { Write(st, self, p); });
	}

	~Port() override { Stop(); }

	void StartWriting( mojo::ScopedDataPipeConsumerHandle consumer ) override
	{
		in_watch_.Cancel();
		in_ = std::move(consumer);
		in_watch_.Watch(in_.get(), MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
				base::BindRepeating(&Port::Take, base::Unretained(this)));
		in_watch_.ArmOrNotify();
	}

	void StartReading( mojo::ScopedDataPipeProducerHandle producer ) override
	{
		out_watch_.Cancel();
		out_ = std::move(producer);
		out_watch_.Watch(out_.get(), MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
				 base::BindRepeating(&Port::Give, base::Unretained(this)));
		state_->reading = true;
		Give(MOJO_RESULT_OK);
	}

	void Flush( dm::SerialPortFlushMode mode, FlushCallback cb ) override
	{
		if ( mode == dm::SerialPortFlushMode::kReceive ) {
			held_.clear();
			std::lock_guard<std::mutex> l(state_->lock);
			state_->held = 0;
		} else if ( mode == dm::SerialPortFlushMode::kTransmit ) {
			std::lock_guard<std::mutex> l(state_->lock);
			state_->sending.clear();
		}
		std::move(cb).Run();
	}

	void Drain( DrainCallback cb ) override
	{
		drains_.push_back(std::move(cb));
		Sent();
	}

	void GetControlSignals( GetControlSignalsCallback cb ) override
	{
		std::move(cb).Run(dm::SerialPortControlSignals::New(false, false, false, false));
	}

	void SetControlSignals( dm::SerialHostControlSignalsPtr, SetControlSignalsCallback cb ) override
	{
		std::move(cb).Run(true);
	}

	void ConfigurePort( dm::SerialConnectionOptionsPtr o, ConfigurePortCallback cb ) override
	{
		bool ok = true;
		if ( o->bitrate != 0 ) {
			ok = br_serial_speed(port_, o->bitrate);
			if ( ok ) speed_ = o->bitrate;
		}
		std::move(cb).Run(ok);
	}

	void GetPortInfo( GetPortInfoCallback cb ) override
	{
		auto info = dm::SerialConnectionInfo::New();
		info->bitrate = speed_;
		info->data_bits = dm::SerialDataBits::EIGHT;
		info->parity_bit = dm::SerialParityBit::NO_PARITY;
		info->stop_bits = dm::SerialStopBits::ONE;
		info->cts_flow_control = false;
		std::move(cb).Run(std::move(info));
	}

	void Close( bool, CloseCallback cb ) override
	{
		Stop();
		std::move(cb).Run();
	}

private:
	/* what the threads and the services' thread share */
	struct Shared {
		std::mutex			lock;
		std::condition_variable		wake;
		std::atomic<bool>		stop{false};
		std::atomic<bool>		reading{false};
		size_t				held = 0;		/* read, not yet in the pipe */
		std::deque<std::vector<uint8_t>> sending;		/* to be written */
		bool				busy = false;		/* a write under way */
	};

	/* the reading thread: what comes in, while the page reads and has room */
	static void Read( std::shared_ptr<Shared> st, base::WeakPtr<Port> self, int port )
	{
		std::vector<uint8_t> buf(256);
		while ( !st->stop ) {
			bool room;
			{
				std::lock_guard<std::mutex> l(st->lock);
				room = st->held < kHeld;
			}
			if ( !st->reading || !room ) {
				std::unique_lock<std::mutex> l(st->lock);
				st->wake.wait_for(l, std::chrono::milliseconds(kReadWait));
				continue;
			}
			int n = br_serial_read(port, buf.data(), (int)buf.size(), kReadWait);
			if ( st->stop ) break;
			if ( n < 0 ) {
				printf("browser: serial: read error %d\n", n);
				br_on_service_thread(base::BindOnce(&Port::Failed, self, true));
				break;
			}
			if ( n == 0 ) continue;
			{
				std::lock_guard<std::mutex> l(st->lock);
				st->held += (size_t)n;
			}
			br_on_service_thread(base::BindOnce(&Port::Came, self,
							    std::vector<uint8_t>(buf.begin(), buf.begin() + n)));
		}
	}

	/* the writing thread: what the page wrote, in turn */
	static void Write( std::shared_ptr<Shared> st, base::WeakPtr<Port> self, int port )
	{
		for ( ;; ) {
			std::vector<uint8_t> b;
			{
				std::unique_lock<std::mutex> l(st->lock);
				st->wake.wait(l, [&]() { return st->stop || !st->sending.empty(); });
				if ( st->stop ) return;
				b = std::move(st->sending.front());
				st->sending.pop_front();
				st->busy = true;
			}
			int n = br_serial_write(port, b.data(), (int)b.size());
			{
				std::lock_guard<std::mutex> l(st->lock);
				st->busy = false;
			}
			if ( n < 0 ) {
				printf("browser: serial: write error %d\n", n);
				br_on_service_thread(base::BindOnce(&Port::Failed, self, false));
				return;
			}
			br_on_service_thread(base::BindOnce(&Port::Sent, self));
		}
	}

	/* bytes read: into the page's pipe, as it has room */
	void Came( std::vector<uint8_t> b )
	{
		held_.insert(held_.end(), b.begin(), b.end());
		Give(MOJO_RESULT_OK);
	}

	void Give( MojoResult r )
	{
		if ( !out_.is_valid() ) return;
		if ( r != MOJO_RESULT_OK ) {
			Unread();
			return;
		}
		while ( !held_.empty() ) {
			size_t done = 0;
			MojoResult w = out_->WriteData(base::span<const uint8_t>(held_.data(), held_.size()),
						      MOJO_WRITE_DATA_FLAG_NONE, done);
			if ( w == MOJO_RESULT_SHOULD_WAIT ) {
				out_watch_.ArmOrNotify();
				return;
			}
			if ( w != MOJO_RESULT_OK ) {
				Unread();
				return;
			}
			held_.erase(held_.begin(), held_.begin() + (long)done);
			{
				std::lock_guard<std::mutex> l(state_->lock);
				state_->held = held_.size();
			}
			state_->wake.notify_all();
		}
		out_watch_.ArmOrNotify();
	}

	/* the page stopped reading (its stream cancelled, or the port closed) */
	void Unread()
	{
		out_watch_.Cancel();
		out_.reset();
		state_->reading = false;
	}

	/* what the page wrote: to the writing thread */
	void Take( MojoResult r )
	{
		if ( !in_.is_valid() ) return;
		if ( r != MOJO_RESULT_OK ) {
			in_watch_.Cancel();
			in_.reset();
			return;
		}
		{
			std::lock_guard<std::mutex> l(state_->lock);
			if ( !state_->sending.empty() || state_->busy ) return;	/* again when that is sent */
		}
		base::span<const uint8_t> buf;
		MojoResult b = in_->BeginReadData(MOJO_BEGIN_READ_DATA_FLAG_NONE, buf);
		if ( b == MOJO_RESULT_SHOULD_WAIT ) {
			in_watch_.ArmOrNotify();
			return;
		}
		if ( b != MOJO_RESULT_OK ) {
			in_watch_.Cancel();
			in_.reset();
			return;
		}
		size_t n = buf.size() < (size_t)kChunk ? buf.size() : (size_t)kChunk;
		std::vector<uint8_t> chunk(buf.begin(), buf.begin() + (long)n);
		in_->EndReadData(n);
		{
			std::lock_guard<std::mutex> l(state_->lock);
			state_->sending.push_back(std::move(chunk));
		}
		state_->wake.notify_all();
	}

	/* a chunk went out: the next, and the drains when all has */
	void Sent()
	{
		bool idle;
		{
			std::lock_guard<std::mutex> l(state_->lock);
			idle = state_->sending.empty() && !state_->busy;
		}
		if ( !idle ) return;
		if ( in_.is_valid() ) {
			in_watch_.ArmOrNotify();
		}
		std::vector<DrainCallback> d = std::move(drains_);
		drains_.clear();
		for ( auto &cb : d ) std::move(cb).Run();
	}

	void Failed( bool reading )
	{
		if ( reading ) {
			Unread();
			client_->OnReadError(dm::SerialReceiveError::SYSTEM_ERROR);
		} else {
			in_watch_.Cancel();
			in_.reset();
			client_->OnSendError(dm::SerialSendError::SYSTEM_ERROR);
		}
	}

	void Stop()
	{
		if ( port_ <= 0 ) return;
		state_->stop = true;
		state_->wake.notify_all();
		if ( reader_.joinable() ) reader_.join();
		if ( writer_.joinable() ) writer_.join();
		out_watch_.Cancel();
		in_watch_.Cancel();
		out_.reset();
		in_.reset();
		br_serial_close(port_);
		port_ = 0;
		std::vector<DrainCallback> d = std::move(drains_);
		drains_.clear();
		for ( auto &cb : d ) std::move(cb).Run();
	}

	int					port_;
	uint32_t				speed_;
	mojo::Remote<dm::SerialPortClient>	client_;
	mojo::ScopedDataPipeProducerHandle	out_;
	mojo::ScopedDataPipeConsumerHandle	in_;
	mojo::SimpleWatcher			out_watch_;
	mojo::SimpleWatcher			in_watch_;
	std::vector<uint8_t>			held_;
	std::vector<DrainCallback>		drains_;
	std::shared_ptr<Shared>			state_;
	std::thread				reader_;
	std::thread				writer_;
	base::WeakPtr<Port>			self_;
	base::WeakPtrFactory<Port>		weak_{this};
};

/* ---------------------------------------------------------------- the service */

struct Granted {
	std::string		origin;
	std::string		name;		/* the device */
};

class Serial final : public bm::SerialService {
public:
	void Bind( mojo::PendingReceiver<bm::SerialService> r ) { receivers_.Add(this, std::move(r), br_page_origin()); }

	void SetClient( mojo::PendingRemote<bm::SerialServiceClient> ) override {}

	void GetPorts( GetPortsCallback cb ) override
	{
		blink::Vector<bm::SerialPortInfoPtr> out;
		std::vector<BrSerialPort> ports = br_serial_list();
		for ( auto &g : granted_ ) {
			if ( g.second.origin != receivers_.current_context() ) continue;
			bool there = false;
			for ( auto &p : ports ) there = there || p.name == g.second.name;
			out.push_back(Info(g.first, there));
		}
		std::move(cb).Run(std::move(out));
	}

	void RequestPort( blink::Vector<bm::SerialPortFilterPtr> filters, const blink::Vector<blink::String> &,
			  RequestPortCallback cb ) override
	{
		std::vector<BrSerialPort> ports = br_serial_list();
		/* the system's ports are no USB devices: a filter of vendor or product leaves none */
		bool usb_only = !filters.empty();
		for ( auto &f : filters ) {
			if ( !f->has_vendor_id && !f->has_product_id && !f->bluetooth_service_class_id ) usb_only = false;
		}
		if ( ports.empty() || usb_only ) {
			printf("browser: serial: no port to choose\n");
			std::move(cb).Run(nullptr);
			return;
		}
		std::vector<std::string> names;
		for ( auto &p : ports ) {
			names.push_back(p.name + "  " + p.label + ( p.console ? "(コンソール)" : "" ));
		}
		std::string origin = receivers_.current_context();
		base::WeakPtr<Serial> self = weak_.GetWeakPtr();
		std::thread([self, origin, ports, names, cb = std::move(cb)]() mutable {
			int k = br_choose(names, 0);
			br_on_service_thread(base::BindOnce(&Serial::Chosen, self, origin,
							    k >= 0 ? ports[(size_t)k].name : std::string(), std::move(cb)));
		}).detach();
	}

	void OpenPort( const base::UnguessableToken &token, dm::SerialConnectionOptionsPtr o,
		       mojo::PendingRemote<dm::SerialPortClient> client, OpenPortCallback cb ) override
	{
		auto g = granted_.find(token);
		if ( g == granted_.end() || g->second.origin != receivers_.current_context() ) {
			std::move(cb).Run(mojo::NullRemote());
			return;
		}
		int port = br_serial_open(g->second.name);
		if ( port <= 0 ) {
			printf("browser: serial: %s not opened (%d)\n", g->second.name.c_str(), port);
			std::move(cb).Run(mojo::NullRemote());
			return;
		}
		uint32_t speed = o->bitrate;
		if ( speed != 0 && !br_serial_speed(port, speed) ) {
			br_serial_close(port);
			std::move(cb).Run(mojo::NullRemote());
			return;
		}
		if ( speed == 0 ) {
			for ( auto &p : br_serial_list() ) {
				if ( p.name == g->second.name ) speed = p.speed;
			}
		}
		printf("browser: serial: %s opened at %u\n", g->second.name.c_str(), (unsigned)speed);
		mojo::PendingRemote<dm::SerialPort> remote;
		mojo::MakeSelfOwnedReceiver(std::make_unique<Port>(port, speed, std::move(client)),
					    remote.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(std::move(remote));
	}

	void ForgetPort( const base::UnguessableToken &token, ForgetPortCallback cb ) override
	{
		auto g = granted_.find(token);
		if ( g != granted_.end() && g->second.origin == receivers_.current_context() ) {
			granted_.erase(g);
		}
		std::move(cb).Run();
	}

private:
	static bm::SerialPortInfoPtr Info( const base::UnguessableToken &token, bool connected )
	{
		auto info = bm::SerialPortInfo::New();
		info->token = token;
		info->connected = connected;
		return info;
	}

	void Chosen( std::string origin, std::string name, RequestPortCallback cb )
	{
		if ( name.empty() ) {
			std::move(cb).Run(nullptr);
			return;
		}
		/* the same port chosen again by the same origin keeps its token */
		for ( auto &g : granted_ ) {
			if ( g.second.origin == origin && g.second.name == name ) {
				std::move(cb).Run(Info(g.first, true));
				return;
			}
		}
		base::UnguessableToken token = base::UnguessableToken::Create();
		granted_[token] = Granted{origin, name};
		printf("browser: serial: %s chosen\n", name.c_str());
		std::move(cb).Run(Info(token, true));
	}

	mojo::ReceiverSet<bm::SerialService, std::string>	receivers_;
	std::map<base::UnguessableToken, Granted>		granted_;
	base::WeakPtrFactory<Serial>				weak_{this};
};

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_serial_init()
{
	static Serial *svc;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::SerialService::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::SerialService> r ) {
				if ( svc == nullptr ) svc = new Serial();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::SerialService>(std::move(h))));
		}));
}
