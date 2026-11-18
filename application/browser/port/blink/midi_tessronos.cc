/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	midi_tessronos.cc
 *	ブラウザ: Web MIDI on the USB MIDI devices of the machine (design
 *	14.4.8).
 *
 *	A USB MIDI device (the audio class, the MIDI streaming subclass) is
 *	not taken by the system's audio driver, which drives only audio
 *	streaming, so the browser opens it as a program's device (SVC class
 *	12, port/net/br_usb.cc) and speaks USB MIDI on its bulk endpoints
 *	itself:
 *
 *	  - the ports: each embedded jack the class-specific descriptors of an
 *	    endpoint name is a cable of that endpoint; the jacks of the OUT
 *	    endpoint are the page's outputs, those of the IN endpoint its
 *	    inputs, named after the device (and the cable when there are
 *	    several);
 *	  - in: 4-byte event packets read from the IN endpoint on a thread of
 *	    the device's, the bytes of each (as many as its code index says)
 *	    put together into whole messages for each cable
 *	    (midi::MidiMessageQueue) and told to every session;
 *	  - out: a message a page sends is cut into event packets (system
 *	    exclusive three bytes at a time) and written to the OUT endpoint
 *	    on a thread of the device's; the session is told the bytes went.
 *
 *	The devices are looked for when a session starts. Web MIDI's
 *	permission is asked by Blink before a session starts (the
 *	permission service answers on a panel).
 */

#include <stdio.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/midi/midi_message_queue.h"
#include "media/midi/midi_service.mojom-blink.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

#include "../net/br_usb.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace mm = midi::mojom::blink;

/* The bytes of MIDI an event packet carries, by its code index number */
int packet_bytes( uint8_t cin )
{
	static const int kBytes[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
	return kBytes[cin & 15];
}

/* A message the page sends, as the event packets of a cable */
std::vector<uint8_t> packets_of( int cable, const std::vector<uint8_t> &msg )
{
	std::vector<uint8_t> out;
	auto put = [&]( uint8_t cin, const uint8_t *b, int n ) {
		out.push_back((uint8_t)( ( cable << 4 ) | cin ));
		for ( int i = 0; i < 3; i++ ) out.push_back(i < n ? b[i] : 0);
	};
	size_t i = 0;
	while ( i < msg.size() ) {
		uint8_t s = msg[i];
		if ( s == 0xf0 ) {
			/* system exclusive: three bytes a packet, the last one says how many it holds */
			size_t end = i;
			while ( end < msg.size() && msg[end] != 0xf7 ) end++;
			if ( end < msg.size() ) end++;
			while ( end - i > 3 ) {
				put(0x4, &msg[i], 3);
				i += 3;
			}
			int n = (int)( end - i );
			put((uint8_t)( n == 1 ? 0x5 : n == 2 ? 0x6 : 0x7 ), &msg[i], n);
			i = end;
		} else if ( s >= 0xf8 ) {
			put(0xf, &msg[i], 1);
			i += 1;
		} else if ( s >= 0xf0 ) {
			int n = ( s == 0xf2 ) ? 3 : ( s == 0xf1 || s == 0xf3 ) ? 2 : 1;
			n = (int)std::min<size_t>((size_t)n, msg.size() - i);
			put((uint8_t)( n == 3 ? 0x3 : n == 2 ? 0x2 : 0x5 ), &msg[i], n);
			i += (size_t)n;
		} else if ( s >= 0x80 ) {
			int n = ( ( s & 0xf0 ) == 0xc0 || ( s & 0xf0 ) == 0xd0 ) ? 2 : 3;
			n = (int)std::min<size_t>((size_t)n, msg.size() - i);
			put((uint8_t)( s >> 4 ), &msg[i], n);
			i += (size_t)n;
		} else {
			i++;			/* a data byte with no status: Blink never sends one */
		}
	}
	return out;
}

/* A port of the page: a cable of a device's endpoint */
struct Port {
	int		dev;		/* the index in g_devs */
	int		cable;
	std::string	id, name, maker;
};

/* A USB MIDI device opened, with its endpoints and the threads that move its data */
struct Device {
	BrUsbDevice			usb;
	int				ud = -1;
	int				in_ep = 0, out_ep = 0;
	int				in_cables = 0, out_cables = 0;
	std::vector<std::unique_ptr<midi::MidiMessageQueue>>	queues;	/* by cable */
	std::mutex			lock;
	std::condition_variable		wake;
	std::deque<std::pair<int, std::vector<uint8_t>>>	sends;	/* the output port, its bytes */
	bool				stop = false;
};

std::vector<std::unique_ptr<Device>>	g_devs;
std::vector<Port>			g_inputs, g_outputs;
bool					g_looked;

class Session;
std::vector<Session *>			g_sessions;	/* the services' thread's */

/* The MIDI streaming interface of a device's configuration: its bulk endpoints and their cables */
bool midi_interface( const std::vector<uint8_t> &c, Device *d )
{
	bool in_ms = false, found = false;
	int last_ep = 0;
	for ( size_t i = c.empty() ? 0 : c[0]; i + 2 <= c.size() && c[i] >= 2 && i + c[i] <= c.size(); i += c[i] ) {
		const uint8_t *p = &c[i];
		if ( p[1] == 4 && p[0] >= 9 ) {
			in_ms = ( p[5] == 1 && p[6] == 3 );
			found = found || in_ms;
			last_ep = 0;
		} else if ( in_ms && p[1] == 5 && p[0] >= 7 && ( p[3] & 3 ) == 2 ) {
			last_ep = p[2];
			if ( p[2] & 0x80 ) {
				if ( d->in_ep == 0 ) d->in_ep = p[2];
			} else if ( d->out_ep == 0 ) {
				d->out_ep = p[2];
			}
		} else if ( in_ms && p[1] == 0x25 && p[0] >= 4 && p[2] == 1 && last_ep != 0 ) {
			/* MS_GENERAL: the embedded jacks the endpoint carries, one cable each */
			if ( last_ep == d->in_ep ) d->in_cables = p[3];
			if ( last_ep == d->out_ep ) d->out_cables = p[3];
		}
	}
	if ( d->in_ep != 0 && d->in_cables == 0 ) d->in_cables = 1;
	if ( d->out_ep != 0 && d->out_cables == 0 ) d->out_cables = 1;
	return found && ( d->in_ep != 0 || d->out_ep != 0 );
}

void received( int port, std::vector<uint8_t> msg );

/* The IN endpoint of a device read until it is closed */
void reader( Device *d, int first_port )
{
	std::vector<uint8_t> buf(64);
	for ( ;; ) {
		{
			std::lock_guard<std::mutex> hold(d->lock);
			if ( d->stop ) return;
		}
		int n = br_usb_transfer(d->ud, d->in_ep, buf.data(), (int)buf.size(), 500);
		if ( n == kBrUsbTimeout ) continue;
		if ( n < 0 ) {
			printf("browser: midi: %s stopped reading (%d)\n", d->usb.name.c_str(), n);
			return;
		}
		for ( int i = 0; i + 4 <= n; i += 4 ) {
			int cable = buf[(size_t)i] >> 4, len = packet_bytes(buf[(size_t)i]);
			if ( len == 0 || cable >= d->in_cables ) continue;
			midi::MidiMessageQueue *q = d->queues[(size_t)cable].get();
			q->Add(base::span<const uint8_t>(&buf[(size_t)i + 1], (size_t)len));
			for ( ;; ) {
				std::vector<uint8_t> msg;
				q->Get(&msg);
				if ( msg.empty() ) break;
				received(first_port + cable, std::move(msg));
			}
		}
	}
}

void sent( uint32_t bytes );

/* What the pages send to a device, written to its OUT endpoint */
void writer( Device *d )
{
	for ( ;; ) {
		std::pair<int, std::vector<uint8_t>> job;
		{
			std::unique_lock<std::mutex> hold(d->lock);
			d->wake.wait(hold, [&] { return d->stop || !d->sends.empty(); });
			if ( d->stop ) return;
			job = std::move(d->sends.front());
			d->sends.pop_front();
		}
		std::vector<uint8_t> pk = packets_of(job.first, job.second);
		int n = pk.empty() ? 0 : br_usb_transfer(d->ud, d->out_ep, pk.data(), (int)pk.size(), 1000);
		if ( n < 0 ) printf("browser: midi: sending to %s failed (%d)\n", d->usb.name.c_str(), n);
		sent((uint32_t)job.second.size());
	}
}

/* The USB MIDI devices, opened, their ports listed (once, on the services' thread) */
void look_for_devices()
{
	if ( g_looked ) return;
	g_looked = true;
	for ( auto &u : br_usb_list() ) {
		std::vector<uint8_t> head = br_usb_descriptor(u.dev, 2, 0, 9);
		if ( head.size() < 9 ) continue;
		std::vector<uint8_t> c = br_usb_descriptor(u.dev, 2, 0, head[2] | ( head[3] << 8 ));
		auto d = std::make_unique<Device>();
		d->usb = u;
		if ( !midi_interface(c, d.get()) ) continue;
		d->ud = br_usb_open(u.dev);
		if ( d->ud <= 0 ) {
			printf("browser: midi: %s cannot be opened (%d)\n", u.name.c_str(), d->ud);
			continue;
		}
		int index = (int)g_devs.size();
		std::string name = u.name.empty() ? std::string("USB MIDI") : u.name;
		char id[32];
		for ( int k = 0; k < d->in_cables; k++ ) {
			snprintf(id, sizeof(id), "usb-%d-in-%d", u.dev, k);
			g_inputs.push_back({ index, k, id, d->in_cables > 1 ? name + " " + std::to_string(k + 1) : name, "" });
			d->queues.push_back(std::make_unique<midi::MidiMessageQueue>(true));
		}
		for ( int k = 0; k < d->out_cables; k++ ) {
			snprintf(id, sizeof(id), "usb-%d-out-%d", u.dev, k);
			g_outputs.push_back({ index, k, id, d->out_cables > 1 ? name + " " + std::to_string(k + 1) : name, "" });
		}
		printf("browser: midi: %s, %d in and %d out\n", name.c_str(), d->in_cables, d->out_cables);
		Device *dp = d.get();
		int first = (int)g_inputs.size() - d->in_cables;
		g_devs.push_back(std::move(d));
		if ( dp->in_ep != 0 ) std::thread(reader, dp, first).detach();
		if ( dp->out_ep != 0 ) std::thread(writer, dp).detach();
	}
}

mm::PortInfoPtr port_info( const Port &p )
{
	return mm::PortInfo::New(blink::String::FromUtf8(p.id), blink::String::FromUtf8(p.maker),
				 blink::String::FromUtf8(p.name), blink::String("1.0"), mm::PortState::CONNECTED);
}

class Session final : public mm::MidiSession {
public:
	explicit Session( mojo::PendingRemote<mm::MidiSessionClient> client ) : client_(std::move(client))
	{
		g_sessions.push_back(this);
		look_for_devices();
		for ( const Port &p : g_inputs ) client_->AddInputPort(port_info(p));
		for ( const Port &p : g_outputs ) client_->AddOutputPort(port_info(p));
		client_->SessionStarted(mm::Result::OK);
	}

	~Session() override
	{
		g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), this), g_sessions.end());
	}

	void SendData( uint32_t port, const blink::Vector<uint8_t> &data, base::TimeTicks ) override
	{
		if ( port >= g_outputs.size() ) return;
		Device *d = g_devs[(size_t)g_outputs[port].dev].get();
		{
			std::lock_guard<std::mutex> hold(d->lock);
			d->sends.emplace_back(g_outputs[port].cable, std::vector<uint8_t>(data.begin(), data.end()));
		}
		d->wake.notify_one();
	}

	mojo::Remote<mm::MidiSessionClient>	client_;
};

void received( int port, std::vector<uint8_t> msg )
{
	base::TimeTicks now = base::TimeTicks::Now();
	br_on_service_thread(base::BindOnce([]( int port, std::vector<uint8_t> msg, base::TimeTicks t ) {
		for ( Session *s : g_sessions ) s->client_->DataReceived((uint32_t)port, blink::Vector<uint8_t>(msg), t);
	}, port, std::move(msg), now));
}

void sent( uint32_t bytes )
{
	br_on_service_thread(base::BindOnce([]( uint32_t bytes ) {
		for ( Session *s : g_sessions ) s->client_->AcknowledgeSentData(bytes);
	}, bytes));
}

class Midi final : public mm::MidiSessionProvider {
public:
	void Bind( mojo::PendingReceiver<mm::MidiSessionProvider> r ) { receivers_.Add(this, std::move(r)); }

	void StartSession( mojo::PendingReceiver<mm::MidiSession> session,
			   mojo::PendingRemote<mm::MidiSessionClient> client ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<Session>(std::move(client)), std::move(session));
	}

private:
	mojo::ReceiverSet<mm::MidiSessionProvider>	receivers_;
};

Midi	*g_midi;

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_midi_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		mm::MidiSessionProvider::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<mm::MidiSessionProvider> r ) {
				if ( g_midi == nullptr ) g_midi = new Midi();
				g_midi->Bind(std::move(r));
			}, mojo::PendingReceiver<mm::MidiSessionProvider>(std::move(h))));
		}));
}
