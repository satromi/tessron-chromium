/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	usb_hid_tessronos.cc
 *	ブラウザ: WebHID, WebUSB and the Gamepad API, answered for the
 *	devices the system lets a program reach (design 14.4.8).
 *
 *	The interfaces are Blink's own (navigator.hid, navigator.usb,
 *	navigator.getGamepads). The system's USB stack drives its devices by
 *	its own class drivers (keyboards and pointers, storage, audio,
 *	cameras) and lets a program have a device none of them took (SVC
 *	class 12, port/net/br_usb.cc), so:
 *
 *	  - WebUSB: those devices, chosen on a panel by requestDevice, with
 *	    their descriptors, requests on the control endpoint, alternate
 *	    settings, halts cleared, a reset, and transfers on bulk,
 *	    interrupt and isochronous endpoints (a packet a service
 *	    interval);
 *	  - WebHID: the HID interfaces of those devices (the keyboards and
 *	    pointers are the class driver's), chosen on a panel, their
 *	    collections from Chromium's parser of report descriptors, input
 *	    reports from their interrupt endpoints, output and feature
 *	    reports out;
 *	  - the Gamepad API: the HID interfaces of those devices whose
 *	    collection is a joystick or a game pad, opened when they are
 *	    plugged in (the devices are looked at every second) and read on
 *	    a thread each. A report's buttons (the button page), axes (the
 *	    generic desktop's X to Wheel, from -1 to 1) and hat switches
 *	    (two axes each) are put in the order of their usages, and the
 *	    standard mapping Chromium has for the vendor and product is
 *	    applied when there is one. As in Chrome, a page sees a gamepad
 *	    only once one of its buttons has been pressed. A vibration is
 *	    not supported.
 *
 *	The services are on the services' thread.
 */

#include <stdio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "device/gamepad/gamepad_standard_mappings.h"
#include "device/gamepad/public/cpp/gamepad.h"
#include "device/gamepad/public/mojom/gamepad.mojom-blink.h"
#include "device/gamepad/public/mojom/gamepad_hardware_buffer.h"
#include "services/device/public/cpp/hid/hid_report_descriptor.h"
#include "services/device/public/mojom/hid.mojom.h"
#include "services/device/public/mojom/hid.mojom-blink.h"
#include "services/device/public/mojom/usb_device.mojom-blink.h"
#include "services/device/public/mojom/usb_manager_client.mojom-blink.h"
#include "third_party/blink/public/mojom/hid/hid.mojom-blink.h"
#include "third_party/blink/public/mojom/usb/web_usb_service.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../../br.h"
#include "../net/br_usb.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;
namespace dm = device::mojom::blink;

/* ---------------------------------------------------------------- WebHID */

/*
 * A HID interface of a device no class driver took: where its reports
 * come and go, and its report descriptor
 */
struct HidIf {
	int			dev;		/* the system's number of the device */
	int			ifno;
	int			in_ep, out_ep;	/* interrupt endpoints, 0 none */
	std::vector<uint8_t>	desc;		/* the report descriptor */
	BrUsbDevice		usb;
};

std::string hid_guid( int dev, int ifno )
{
	return "tessronos-hid-" + std::to_string(dev) + "-" + std::to_string(ifno);
}

std::map<std::string, std::vector<uint8_t>>	g_hid_desc;	/* the report descriptors read, by interface */

/* The HID interfaces of the devices a program may use; their report descriptors read */
std::vector<HidIf> hid_interfaces()
{
	std::vector<HidIf> out;
	for ( auto &d : br_usb_list() ) {
		std::vector<uint8_t> head = br_usb_descriptor(d.dev, 2, 0, 9);
		if ( head.size() < 9 ) continue;
		std::vector<uint8_t> c = br_usb_descriptor(d.dev, 2, 0, head[2] | ( head[3] << 8 ));
		std::vector<HidIf> ifs;
		HidIf *cur = nullptr;
		int desc_len = 0;
		std::vector<int> lens;
		for ( size_t i = c.empty() ? 0 : c[0]; i + 2 <= c.size() && c[i] >= 2 && i + c[i] <= c.size(); i += c[i] ) {
			const uint8_t *p = &c[i];
			if ( p[1] == 4 && p[0] >= 9 ) {
				cur = nullptr;
				if ( p[3] == 0 && p[5] == 3 ) {
					HidIf h{d.dev, p[2], 0, 0, {}, d};
					ifs.push_back(h);
					lens.push_back(0);
					cur = &ifs.back();
				}
			} else if ( cur != nullptr && p[1] == 0x21 && p[0] >= 9 ) {
				for ( int k = 0; k < p[5] && 6 + 3 * k + 2 < p[0]; k++ ) {
					if ( p[6 + 3 * k] == 0x22 ) desc_len = p[7 + 3 * k] | ( p[8 + 3 * k] << 8 );
				}
				lens.back() = desc_len;
			} else if ( cur != nullptr && p[1] == 5 && p[0] >= 7 && ( p[3] & 3 ) == 3 ) {
				if ( p[2] & 0x80 ) {
					if ( cur->in_ep == 0 ) cur->in_ep = p[2];
				} else if ( cur->out_ep == 0 ) {
					cur->out_ep = p[2];
				}
			}
		}
		if ( ifs.empty() ) continue;
		/*
		 * The report descriptors: GET_DESCRIPTOR to each interface, the
		 * device opened for it, and kept; a device a page has open is
		 * listed with what was read before
		 */
		bool all = true;
		for ( auto &h : ifs ) all = all && g_hid_desc.count(hid_guid(h.dev, h.ifno)) != 0;
		int ud = all ? 0 : br_usb_open(d.dev);
		for ( size_t k = 0; k < ifs.size() && ud > 0; k++ ) {
			int len = lens[k] > 0 ? lens[k] : 4096;
			uint8_t setup[8] = { 0x81, 6, 0, 0x22, (uint8_t)ifs[k].ifno, 0, (uint8_t)len, (uint8_t)( len >> 8 ) };
			std::vector<uint8_t> b((size_t)len);
			int n = br_usb_control(ud, setup, b.data(), len, 1000);
			if ( n > 0 ) {
				b.resize((size_t)n);
				g_hid_desc[hid_guid(ifs[k].dev, ifs[k].ifno)] = std::move(b);
			}
		}
		if ( ud > 0 ) br_usb_close(ud);
		for ( auto &h : ifs ) {
			auto it = g_hid_desc.find(hid_guid(h.dev, h.ifno));
			if ( it == g_hid_desc.end() ) continue;
			h.desc = it->second;
			out.push_back(h);
		}
	}
	return out;
}

/*
 * What a HID interface is, as WebHID has it: its collections and report
 * sizes from Chromium's parser of report descriptors, made in the
 * mojom's own variant and carried over into Blink's by its bytes
 */
dm::HidDeviceInfoPtr hid_info( const HidIf &h )
{
	auto info = device::mojom::HidDeviceInfo::New();
	info->guid = hid_guid(h.dev, h.ifno);
	info->physical_device_id = "tessronos-usb-" + std::to_string(h.dev);
	info->vendor_id = h.usb.vendor;
	info->product_id = h.usb.product;
	info->product_name = h.usb.name;
	info->bus_type = device::mojom::HidBusType::kHIDBusTypeUSB;
	info->report_descriptor = h.desc;
	size_t in = 0, out = 0, feature = 0;
	device::HidReportDescriptor parsed(h.desc);
	parsed.GetDetails(&info->collections, &info->has_report_id, &in, &out, &feature);
	info->max_input_report_size = in;
	info->max_output_report_size = out;
	info->max_feature_report_size = feature;
	info->device_node = info->guid;
	std::vector<uint8_t> bytes = device::mojom::HidDeviceInfo::Serialize(&info);
	dm::HidDeviceInfoPtr blink_info;
	if ( !dm::HidDeviceInfo::Deserialize(bytes, &blink_info) ) return nullptr;
	return blink_info;
}

/* A filter of requestDevice: whether the interface passes it */
bool hid_matches( const dm::HidDeviceInfoPtr &info, const bm::HidDeviceFilterPtr &f )
{
	if ( f->device_ids ) {
		if ( f->device_ids->is_vendor() && f->device_ids->get_vendor() != info->vendor_id ) return false;
		if ( f->device_ids->is_vendor_and_product()
		     && ( f->device_ids->get_vendor_and_product()->vendor != info->vendor_id
			  || f->device_ids->get_vendor_and_product()->product != info->product_id ) ) {
			return false;
		}
	}
	if ( f->usage ) {
		bool any = false;
		for ( auto &c : info->collections ) {
			if ( f->usage->is_page() ) {
				any = any || c->usage->usage_page == f->usage->get_page();
			} else {
				any = any || ( c->usage->usage_page == f->usage->get_usage_and_page()->usage_page
					       && c->usage->usage == f->usage->get_usage_and_page()->usage );
			}
		}
		if ( !any ) return false;
	}
	return true;
}

/*
 * A connection to a HID interface: the device opened for it. The
 * interface's input reports are read on a thread of their own, from its
 * interrupt endpoint, and told to the page as they come; reports and
 * feature reports go out on the endpoint or the control endpoint
 * (SET_REPORT, GET_REPORT).
 */
class HidConn final : public dm::HidConnection {
public:
	HidConn( int ud, const HidIf &h, bool ids, mojo::PendingRemote<dm::HidConnectionClient> client )
		: ud_(ud), h_(h), ids_(ids), client_(std::move(client))
	{
		quit_ = std::make_shared<std::atomic<bool>>(false);
		if ( h_.in_ep != 0 ) {
			std::shared_ptr<std::atomic<bool>> quit = quit_;
			base::WeakPtr<HidConn> self = weak_.GetWeakPtr();
			int ud0 = ud_, ep = h_.in_ep;
			bool ids0 = ids_;
			std::thread([quit, self, ud0, ep, ids0]() {
				std::vector<uint8_t> b(1024);
				while ( !quit->load() ) {
					int n = br_usb_transfer(ud0, ep, b.data(), (int)b.size(), 200);
					if ( n == kBrUsbTimeout ) continue;
					if ( n < 0 ) break;
					uint8_t id = 0;
					std::vector<uint8_t> r(b.begin(), b.begin() + n);
					if ( ids0 && !r.empty() ) {
						id = r[0];
						r.erase(r.begin());
					}
					br_on_service_thread(base::BindOnce([]( base::WeakPtr<HidConn> c, uint8_t id, std::vector<uint8_t> r ) {
						if ( c ) c->client_->OnInputReport(id, blink::Vector<uint8_t>(r));
					}, self, id, std::move(r)));
				}
			}).detach();
		}
	}

	~HidConn() override
	{
		quit_->store(true);
		br_usb_close(ud_);
		printf("browser: hid: device %d closed\n", h_.dev);
	}

	/* the reports go to the client as they come */
	void Read( ReadCallback cb ) override { std::move(cb).Run(false, 0, std::nullopt); }

	void Write( uint8_t id, const blink::Vector<uint8_t> &buffer, WriteCallback cb ) override
	{
		std::vector<uint8_t> d;
		if ( id != 0 ) d.push_back(id);
		d.insert(d.end(), buffer.begin(), buffer.end());
		int ud = ud_, ep = h_.out_ep, ifno = h_.ifno;
		std::thread([ud, ep, ifno, id, d = std::move(d), cb = std::move(cb)]() mutable {
			int n;
			if ( ep != 0 ) {
				n = br_usb_transfer(ud, ep, d.data(), (int)d.size(), 1000);
			} else {
				uint8_t setup[8] = { 0x21, 0x09, id, 2, (uint8_t)ifno, 0, (uint8_t)d.size(), (uint8_t)( d.size() >> 8 ) };
				n = br_usb_control(ud, setup, d.data(), (int)d.size(), 1000);
			}
			br_on_service_thread(base::BindOnce(std::move(cb), n >= 0));
		}).detach();
	}

	/* the report as WebHID gives it: its report ID first, 0 for a device without them */
	void GetFeatureReport( uint8_t id, GetFeatureReportCallback cb ) override
	{
		int ud = ud_, ifno = h_.ifno;
		bool ids = ids_;
		std::thread([ud, ifno, id, ids, cb = std::move(cb)]() mutable {
			std::vector<uint8_t> b(4096);
			uint8_t setup[8] = { 0xA1, 0x01, id, 3, (uint8_t)ifno, 0, 0x00, 0x10 };
			int n = br_usb_control(ud, setup, b.data(), (int)b.size(), 1000);
			std::optional<blink::Vector<uint8_t>> out;
			if ( n >= 0 ) {
				blink::Vector<uint8_t> v;
				if ( !ids ) v.push_back(0);
				v.Append(b.data(), (blink::wtf_size_t)n);
				out = std::move(v);
			}
			br_on_service_thread(base::BindOnce([]( GetFeatureReportCallback cb, bool ok,
								std::optional<blink::Vector<uint8_t>> out ) {
				std::move(cb).Run(ok, std::move(out));
			}, std::move(cb), n >= 0, std::move(out)));
		}).detach();
	}

	void SendFeatureReport( uint8_t id, const blink::Vector<uint8_t> &buffer, SendFeatureReportCallback cb ) override
	{
		std::vector<uint8_t> d;
		if ( id != 0 ) d.push_back(id);
		d.insert(d.end(), buffer.begin(), buffer.end());
		int ud = ud_, ifno = h_.ifno;
		std::thread([ud, ifno, id, d = std::move(d), cb = std::move(cb)]() mutable {
			uint8_t setup[8] = { 0x21, 0x09, id, 3, (uint8_t)ifno, 0, (uint8_t)d.size(), (uint8_t)( d.size() >> 8 ) };
			int n = br_usb_control(ud, setup, d.data(), (int)d.size(), 1000);
			br_on_service_thread(base::BindOnce(std::move(cb), n >= 0));
		}).detach();
	}

private:
	int						ud_;
	HidIf						h_;
	bool						ids_;
	mojo::Remote<dm::HidConnectionClient>		client_;
	std::shared_ptr<std::atomic<bool>>		quit_;
	base::WeakPtrFactory<HidConn>			weak_{this};
};

/*
 * navigator.hid: the HID interfaces of the devices no class driver of
 * the system took (the keyboards and pointers are the system's, and are
 * not listed). requestDevice lists the interfaces its filters pass on a
 * panel, and the one chosen is the page's (by its origin) while the
 * browser runs. Its reports are not held back as Chrome holds back a
 * keyboard's or a mouse's: no interface listed is one the system reads.
 */
class Hid final : public bm::HidService {
public:
	void Bind( mojo::PendingReceiver<bm::HidService> r ) { receivers_.Add(this, std::move(r), br_page_origin()); }

	void RegisterClient( mojo::PendingAssociatedRemote<dm::HidManagerClient> ) override {}

	void GetDevices( GetDevicesCallback cb ) override
	{
		blink::Vector<dm::HidDeviceInfoPtr> out;
		std::string origin = receivers_.current_context();
		for ( auto &h : hid_interfaces() ) {
			if ( granted_.count(origin + "\n" + hid_guid(h.dev, h.ifno)) == 0 ) continue;
			if ( auto info = hid_info(h) ) out.push_back(std::move(info));
		}
		std::move(cb).Run(std::move(out));
	}

	void RequestDevice( blink::Vector<bm::HidDeviceFilterPtr> filters, blink::Vector<bm::HidDeviceFilterPtr> exclusions,
			    RequestDeviceCallback cb ) override
	{
		std::vector<HidIf> ifs;
		std::vector<std::string> names;
		for ( auto &h : hid_interfaces() ) {
			dm::HidDeviceInfoPtr info = hid_info(h);
			if ( !info ) continue;
			bool pass = filters.empty(), out = false;
			for ( auto &f : filters ) pass = pass || hid_matches(info, f);
			for ( auto &f : exclusions ) out = out || hid_matches(info, f);
			if ( !pass || out ) continue;
			char id[16];
			snprintf(id, sizeof(id), "%04x:%04x", h.usb.vendor, h.usb.product);
			names.push_back(( h.usb.name.empty() ? std::string("HID") : h.usb.name ) + "  (" + id + ")");
			ifs.push_back(h);
		}
		if ( ifs.empty() ) {
			printf("browser: hid: no device to choose\n");
			std::move(cb).Run(blink::Vector<dm::HidDeviceInfoPtr>());
			return;
		}
		std::string origin = receivers_.current_context();
		base::WeakPtr<Hid> self = weak_.GetWeakPtr();
		std::thread([self, origin, ifs, names, cb = std::move(cb)]() mutable {
			int k = br_choose(names, 0);
			br_on_service_thread(base::BindOnce(&Hid::Chosen, self, origin,
							    k >= 0 ? hid_guid(ifs[(size_t)k].dev, ifs[(size_t)k].ifno) : std::string(),
							    std::move(cb)));
		}).detach();
	}

	void Connect( const blink::String &guid, mojo::PendingRemote<dm::HidConnectionClient> client, ConnectCallback cb ) override
	{
		std::string g = guid.Utf8();
		if ( granted_.count(receivers_.current_context() + "\n" + g) == 0 ) {
			std::move(cb).Run(mojo::NullRemote());
			return;
		}
		for ( auto &h : hid_interfaces() ) {
			if ( hid_guid(h.dev, h.ifno) != g ) continue;
			dm::HidDeviceInfoPtr info = hid_info(h);
			int ud = br_usb_open(h.dev);
			if ( ud <= 0 || !info ) {
				printf("browser: hid: device %d not opened (%d)\n", h.dev, ud);
				if ( ud > 0 ) br_usb_close(ud);
				break;
			}
			printf("browser: hid: device %d opened, interface %d\n", h.dev, h.ifno);
			mojo::PendingRemote<dm::HidConnection> remote;
			mojo::MakeSelfOwnedReceiver(std::make_unique<HidConn>(ud, h, info->has_report_id, std::move(client)),
						    remote.InitWithNewPipeAndPassReceiver());
			std::move(cb).Run(std::move(remote));
			return;
		}
		std::move(cb).Run(mojo::NullRemote());
	}

	void Forget( dm::HidDeviceInfoPtr info, ForgetCallback cb ) override
	{
		granted_.erase(receivers_.current_context() + "\n" + info->guid.Utf8());
		std::move(cb).Run();
	}

private:
	void Chosen( std::string origin, std::string guid, RequestDeviceCallback cb )
	{
		blink::Vector<dm::HidDeviceInfoPtr> out;
		for ( auto &h : hid_interfaces() ) {
			if ( guid.empty() || hid_guid(h.dev, h.ifno) != guid ) continue;
			granted_.insert(origin + "\n" + guid);
			printf("browser: hid: %s (%04x:%04x) interface %d chosen\n", h.usb.name.c_str(), h.usb.vendor,
			       h.usb.product, h.ifno);
			if ( auto info = hid_info(h) ) out.push_back(std::move(info));
		}
		std::move(cb).Run(std::move(out));
	}

	mojo::ReceiverSet<bm::HidService, std::string>	receivers_;
	std::set<std::string>				granted_;	/* origin, then the interface's guid */
	base::WeakPtrFactory<Hid>			weak_{this};
};

/* ---------------------------------------------------------------- WebUSB */

/* A transfer's outcome as WebUSB has it */
dm::UsbTransferStatus status_of( int n )
{
	if ( n >= 0 ) return dm::UsbTransferStatus::COMPLETED;
	switch ( n ) {
	case kBrUsbTimeout:	return dm::UsbTransferStatus::TIMEOUT;
	case kBrUsbStall:	return dm::UsbTransferStatus::STALLED;
	case kBrUsbGone:	return dm::UsbTransferStatus::DISCONNECT;
	default:		return dm::UsbTransferStatus::TRANSFER_ERROR;
	}
}

std::string usb_guid( int dev )
{
	return "tessronos-usb-" + std::to_string(dev);
}

/* A string descriptor as text (its first language); null when there is none */
blink::String usb_string( int dev, int index )
{
	if ( index == 0 ) return blink::String();
	std::vector<uint8_t> s = br_usb_descriptor(dev, 3, index, 255);
	if ( s.size() < 2 || s[1] != 3 ) return blink::String();
	std::u16string t;
	for ( size_t i = 2; i + 1 < s.size() && i + 1 < s[0]; i += 2 ) t += (char16_t)( s[i] | ( s[i + 1] << 8 ) );
	return blink::String(t);
}

/* What a device is, from its descriptors: the device's, its configuration's and its strings */
dm::UsbDeviceInfoPtr usb_info( const BrUsbDevice &d )
{
	std::vector<uint8_t> dd = br_usb_descriptor(d.dev, 1, 0, 18);
	if ( dd.size() < 18 ) return nullptr;
	auto info = dm::UsbDeviceInfo::New();
	info->guid = blink::String::FromUtf8(usb_guid(d.dev));
	info->usb_version_major = dd[3];
	info->usb_version_minor = dd[2] >> 4;
	info->usb_version_subminor = dd[2] & 15;
	info->class_code = dd[4];
	info->subclass_code = dd[5];
	info->protocol_code = dd[6];
	info->bus_number = d.hc;
	info->port_number = d.port;
	info->vendor_id = (uint16_t)( dd[8] | ( dd[9] << 8 ) );
	info->product_id = (uint16_t)( dd[10] | ( dd[11] << 8 ) );
	info->device_version_major = dd[13];
	info->device_version_minor = dd[12] >> 4;
	info->device_version_subminor = dd[12] & 15;
	info->manufacturer_name = usb_string(d.dev, dd[14]);
	info->product_name = usb_string(d.dev, dd[15]);
	if ( info->product_name.IsNull() && !d.name.empty() ) info->product_name = blink::String::FromUtf8(d.name);
	info->serial_number = usb_string(d.dev, dd[16]);

	std::vector<uint8_t> head = br_usb_descriptor(d.dev, 2, 0, 9);
	if ( head.size() < 9 ) return info;
	std::vector<uint8_t> c = br_usb_descriptor(d.dev, 2, 0, head[2] | ( head[3] << 8 ));
	if ( c.size() < 9 ) return info;
	auto cfg = dm::UsbConfigurationInfo::New();
	cfg->configuration_value = c[5];
	cfg->self_powered = ( c[7] & 0x40 ) != 0;
	cfg->remote_wakeup = ( c[7] & 0x20 ) != 0;
	cfg->maximum_power = c[8];
	dm::UsbAlternateInterfaceInfo *alt = nullptr;
	for ( size_t i = c[0]; i + 2 <= c.size() && c[i] >= 2 && i + c[i] <= c.size(); i += c[i] ) {
		const uint8_t *p = &c[i];
		if ( p[1] == 4 && p[0] >= 9 ) {				/* an interface's setting */
			dm::UsbInterfaceInfo *itf = nullptr;
			for ( auto &x : cfg->interfaces ) {
				if ( x->interface_number == p[2] ) itf = x.get();
			}
			if ( itf == nullptr ) {
				auto n = dm::UsbInterfaceInfo::New();
				n->interface_number = p[2];
				n->first_interface = p[2];
				cfg->interfaces.push_back(std::move(n));
				itf = cfg->interfaces.back().get();
			}
			auto a = dm::UsbAlternateInterfaceInfo::New();
			a->alternate_setting = p[3];
			a->class_code = p[5];
			a->subclass_code = p[6];
			a->protocol_code = p[7];
			a->interface_name = usb_string(d.dev, p[8]);
			itf->alternates.push_back(std::move(a));
			alt = itf->alternates.back().get();
		} else if ( p[1] == 5 && p[0] >= 7 && alt != nullptr ) {	/* an endpoint of it */
			auto e = dm::UsbEndpointInfo::New();
			e->endpoint_number = p[2] & 15;
			e->direction = ( p[2] & 0x80 ) ? dm::UsbTransferDirection::INBOUND : dm::UsbTransferDirection::OUTBOUND;
			e->type = (dm::UsbTransferType)( p[3] & 3 );
			e->packet_size = (uint32_t)( ( p[4] | ( p[5] << 8 ) ) & 0x7ff );
			e->synchronization_type = (dm::UsbSynchronizationType)( ( p[3] >> 2 ) & 3 );
			e->usage_type = (dm::UsbUsageType)( ( p[3] >> 4 ) & 3 );
			e->polling_interval = p[6];
			alt->endpoints.push_back(std::move(e));
		} else if ( alt != nullptr ) {
			alt->extra_data.Append(p, p[0]);
		}
	}
	info->active_configuration = cfg->configuration_value;
	info->configurations.push_back(std::move(cfg));
	return info;
}

/* A filter of requestDevice: whether the device passes it */
bool usb_matches( const BrUsbDevice &d, const dm::UsbDeviceFilterPtr &f )
{
	if ( f->vendor_id && *f->vendor_id != d.vendor ) return false;
	if ( f->product_id && *f->product_id != d.product ) return false;
	if ( f->class_code && *f->class_code != d.cls ) return false;
	if ( f->subclass_code && *f->subclass_code != d.subclass ) return false;
	if ( f->protocol_code && *f->protocol_code != d.protocol ) return false;
	return true;
}

/*
 * A device a page opened: the system's number for it. Every call that
 * waits for the device runs on a thread of its own and answers on the
 * services' thread.
 */
class UsbDev final : public dm::UsbDevice {
public:
	explicit UsbDev( int dev ) : dev_(dev) {}
	~UsbDev() override
	{
		if ( ud_ > 0 ) br_usb_close(ud_);
	}

	void Open( OpenCallback cb ) override
	{
		if ( ud_ > 0 ) {
			std::move(cb).Run(dm::UsbOpenDeviceResult::NewError(dm::UsbOpenDeviceError::ALREADY_OPEN));
			return;
		}
		ud_ = br_usb_open(dev_);
		if ( ud_ <= 0 ) {
			printf("browser: usb: device %d not opened (%d)\n", dev_, ud_);
			int er = ud_;
			ud_ = 0;
			std::move(cb).Run(dm::UsbOpenDeviceResult::NewError(er == kBrUsbBusy ? dm::UsbOpenDeviceError::ALREADY_OPEN
											     : dm::UsbOpenDeviceError::ACCESS_DENIED));
			return;
		}
		printf("browser: usb: device %d opened\n", dev_);
		std::move(cb).Run(dm::UsbOpenDeviceResult::NewSuccess(dm::UsbOpenDeviceSuccess::OK));
	}

	void Close( CloseCallback cb ) override
	{
		if ( ud_ > 0 ) br_usb_close(ud_);
		ud_ = 0;
		std::move(cb).Run();
	}

	/* the configuration the system set when it took the device is the only one */
	void SetConfiguration( uint8_t value, SetConfigurationCallback cb ) override
	{
		std::move(cb).Run(ud_ > 0 && value == config_);
	}

	void ClaimInterface( uint8_t, ClaimInterfaceCallback cb ) override
	{
		std::move(cb).Run(ud_ > 0 ? dm::UsbClaimInterfaceResult::kSuccess : dm::UsbClaimInterfaceResult::kFailure);
	}

	void ReleaseInterface( uint8_t, ReleaseInterfaceCallback cb ) override { std::move(cb).Run(ud_ > 0); }

	void SetInterfaceAlternateSetting( uint8_t ifno, uint8_t alt, SetInterfaceAlternateSettingCallback cb ) override
	{
		int ud = ud_;
		std::thread([ud, ifno, alt, cb = std::move(cb)]() mutable {
			bool ok = ud > 0 && br_usb_alternate(ud, ifno, alt);
			br_on_service_thread(base::BindOnce(std::move(cb), ok));
		}).detach();
	}

	/* the device's port reset, the device addressed and configured again; its interfaces' settings back to 0 */
	void Reset( ResetCallback cb ) override
	{
		int ud = ud_;
		std::thread([ud, cb = std::move(cb)]() mutable {
			bool ok = ud > 0 && br_usb_reset(ud);
			br_on_service_thread(base::BindOnce(std::move(cb), ok));
		}).detach();
	}

	void ClearHalt( dm::UsbTransferDirection dir, uint8_t ep, ClearHaltCallback cb ) override
	{
		int ud = ud_, addr = ( dir == dm::UsbTransferDirection::INBOUND ? 0x80 : 0 ) | ( ep & 15 );
		std::thread([ud, addr, cb = std::move(cb)]() mutable {
			bool ok = ud > 0 && br_usb_clear_halt(ud, addr);
			br_on_service_thread(base::BindOnce(std::move(cb), ok));
		}).detach();
	}

	void ControlTransferIn( dm::UsbControlTransferParamsPtr p, uint32_t length, uint32_t timeout,
				ControlTransferInCallback cb ) override
	{
		uint8_t setup[8];
		Setup(setup, p, true, length);
		int ud = ud_;
		std::thread([ud, length, timeout, setup0 = std::vector<uint8_t>(setup, setup + 8), cb = std::move(cb)]() mutable {
			std::vector<uint8_t> data(length);
			int n = ud > 0 ? br_usb_control(ud, setup0.data(), data.data(), (int)length, (int)timeout) : kBrUsbGone;
			data.resize(n > 0 ? (size_t)n : 0);
			br_on_service_thread(base::BindOnce([]( ControlTransferInCallback cb, int n, std::vector<uint8_t> d ) {
				std::move(cb).Run(status_of(n), base::span<const uint8_t>(d));
			}, std::move(cb), n, std::move(data)));
		}).detach();
	}

	void ControlTransferOut( dm::UsbControlTransferParamsPtr p, base::span<const uint8_t> data, uint32_t timeout,
				 ControlTransferOutCallback cb ) override
	{
		uint8_t setup[8];
		Setup(setup, p, false, (uint32_t)data.size());
		int ud = ud_;
		std::thread([ud, timeout, setup0 = std::vector<uint8_t>(setup, setup + 8),
			     d = std::vector<uint8_t>(data.begin(), data.end()), cb = std::move(cb)]() mutable {
			int n = ud > 0 ? br_usb_control(ud, setup0.data(), d.data(), (int)d.size(), (int)timeout) : kBrUsbGone;
			br_on_service_thread(base::BindOnce([]( ControlTransferOutCallback cb, int n ) {
				std::move(cb).Run(status_of(n));
			}, std::move(cb), n));
		}).detach();
	}

	void GenericTransferIn( uint8_t ep, uint32_t length, uint32_t timeout, GenericTransferInCallback cb ) override
	{
		int ud = ud_;
		std::thread([ud, ep, length, timeout, cb = std::move(cb)]() mutable {
			std::vector<uint8_t> data(length);
			int n = ud > 0 ? br_usb_transfer(ud, 0x80 | ( ep & 15 ), data.data(), (int)length, (int)timeout) : kBrUsbGone;
			data.resize(n > 0 ? (size_t)n : 0);
			br_on_service_thread(base::BindOnce([]( GenericTransferInCallback cb, int n, std::vector<uint8_t> d ) {
				std::move(cb).Run(status_of(n), base::span<const uint8_t>(d));
			}, std::move(cb), n, std::move(data)));
		}).detach();
	}

	void GenericTransferOut( uint8_t ep, base::span<const uint8_t> data, uint32_t timeout,
				 GenericTransferOutCallback cb ) override
	{
		int ud = ud_;
		std::thread([ud, ep, timeout, d = std::vector<uint8_t>(data.begin(), data.end()), cb = std::move(cb)]() mutable {
			int n = ud > 0 ? br_usb_transfer(ud, ep & 15, d.data(), (int)d.size(), (int)timeout) : kBrUsbGone;
			br_on_service_thread(base::BindOnce([]( GenericTransferOutCallback cb, int n ) {
				std::move(cb).Run(status_of(n));
			}, std::move(cb), n));
		}).detach();
	}

	/*
	 * Isochronous: a packet a service interval. IN, the packets as they
	 * come, each into its place of the buffer (as long as it was asked
	 * for), the time-out the first one's; OUT, each packet put in line.
	 */
	void IsochronousTransferIn( uint8_t ep, const blink::Vector<uint32_t> &lengths, uint32_t timeout,
				    IsochronousTransferInCallback cb ) override
	{
		int ud = ud_;
		std::vector<uint32_t> lens(lengths.begin(), lengths.end());
		std::thread([ud, ep, lens, timeout, cb = std::move(cb)]() mutable {
			size_t total = 0;
			for ( uint32_t l : lens ) total += l;
			std::vector<uint8_t> data(total);
			std::vector<std::pair<int, uint32_t>> got;	/* each packet: its outcome, its bytes */
			size_t at = 0;
			for ( size_t i = 0; i < lens.size(); i++ ) {
				int n = ud > 0 ? br_usb_transfer(ud, 0x80 | ( ep & 15 ), data.data() + at, (int)lens[i],
								 i == 0 ? (int)timeout : 100)
					       : kBrUsbGone;
				got.push_back({ n, n > 0 ? (uint32_t)n : 0 });
				at += lens[i];
			}
			br_on_service_thread(base::BindOnce([]( IsochronousTransferInCallback cb, std::vector<uint8_t> d,
								std::vector<uint32_t> lens, std::vector<std::pair<int, uint32_t>> got ) {
				blink::Vector<dm::UsbIsochronousPacketPtr> v;
				for ( size_t i = 0; i < lens.size(); i++ ) {
					auto k = dm::UsbIsochronousPacket::New();
					k->length = lens[i];
					k->transferred_length = got[i].second;
					k->status = status_of(got[i].first);
					v.push_back(std::move(k));
				}
				std::move(cb).Run(base::span<const uint8_t>(d), std::move(v));
			}, std::move(cb), std::move(data), std::move(lens), std::move(got)));
		}).detach();
	}
	void IsochronousTransferOut( uint8_t ep, base::span<const uint8_t> data, const blink::Vector<uint32_t> &lengths,
				     uint32_t timeout, IsochronousTransferOutCallback cb ) override
	{
		int ud = ud_;
		std::vector<uint32_t> lens(lengths.begin(), lengths.end());
		std::thread([ud, ep, lens, timeout, d = std::vector<uint8_t>(data.begin(), data.end()), cb = std::move(cb)]() mutable {
			std::vector<std::pair<int, uint32_t>> sent;
			size_t at = 0;
			for ( uint32_t l : lens ) {
				int n = ( ud > 0 && at + l <= d.size() )
					? br_usb_transfer(ud, ep & 15, d.data() + at, (int)l, (int)timeout) : kBrUsbGone;
				sent.push_back({ n, n > 0 ? (uint32_t)n : 0 });
				at += l;
			}
			br_on_service_thread(base::BindOnce([]( IsochronousTransferOutCallback cb, std::vector<uint32_t> lens,
								std::vector<std::pair<int, uint32_t>> sent ) {
				blink::Vector<dm::UsbIsochronousPacketPtr> v;
				for ( size_t i = 0; i < lens.size(); i++ ) {
					auto k = dm::UsbIsochronousPacket::New();
					k->length = lens[i];
					k->transferred_length = sent[i].second;
					k->status = status_of(sent[i].first);
					v.push_back(std::move(k));
				}
				std::move(cb).Run(std::move(v));
			}, std::move(cb), std::move(lens), std::move(sent)));
		}).detach();
	}

private:
	static void Setup( uint8_t *s, const dm::UsbControlTransferParamsPtr &p, bool in, uint32_t length )
	{
		s[0] = (uint8_t)( ( in ? 0x80 : 0 ) | ( (int)p->type << 5 ) | (int)p->recipient );
		s[1] = p->request;
		s[2] = (uint8_t)p->value;
		s[3] = (uint8_t)( p->value >> 8 );
		s[4] = (uint8_t)p->index;
		s[5] = (uint8_t)( p->index >> 8 );
		s[6] = (uint8_t)length;
		s[7] = (uint8_t)( length >> 8 );
	}

	int	dev_;
	int	ud_ = 0;
	uint8_t	config_ = 1;
};

/*
 * navigator.usb: the devices no class driver of the system took. A page
 * is given a device it was let have: requestDevice lists the devices
 * its filters pass on a panel, and the one chosen is the page's (by its
 * origin) while the browser runs.
 */
class Usb final : public bm::WebUsbService {
public:
	void Bind( mojo::PendingReceiver<bm::WebUsbService> r ) { receivers_.Add(this, std::move(r), br_page_origin()); }

	void GetDevices( GetDevicesCallback cb ) override
	{
		blink::Vector<dm::UsbDeviceInfoPtr> out;
		std::string origin = receivers_.current_context();
		for ( auto &d : br_usb_list() ) {
			if ( granted_.count(origin + "\n" + usb_guid(d.dev)) == 0 ) continue;
			if ( auto info = usb_info(d) ) out.push_back(std::move(info));
		}
		std::move(cb).Run(std::move(out));
	}

	void GetDevice( const blink::String &guid, mojo::PendingReceiver<dm::UsbDevice> r ) override
	{
		std::string g = guid.Utf8(), origin = receivers_.current_context();
		if ( granted_.count(origin + "\n" + g) == 0 ) return;
		for ( auto &d : br_usb_list() ) {
			if ( usb_guid(d.dev) == g ) {
				mojo::MakeSelfOwnedReceiver(std::make_unique<UsbDev>(d.dev), std::move(r));
				return;
			}
		}
	}

	void GetPermission( bm::WebUsbRequestDeviceOptionsPtr options, GetPermissionCallback cb ) override
	{
		std::vector<BrUsbDevice> devs;
		for ( auto &d : br_usb_list() ) {
			bool pass = options->filters.empty(), out = false;
			for ( auto &f : options->filters ) pass = pass || usb_matches(d, f);
			for ( auto &f : options->exclusion_filters ) out = out || usb_matches(d, f);
			if ( pass && !out ) devs.push_back(d);
		}
		if ( devs.empty() ) {
			printf("browser: usb: no device to choose\n");
			std::move(cb).Run(nullptr);
			return;
		}
		std::vector<std::string> names;
		for ( auto &d : devs ) {
			char id[16];
			snprintf(id, sizeof(id), "%04x:%04x", d.vendor, d.product);
			names.push_back(( d.name.empty() ? std::string("USB") : d.name ) + "  (" + id + ")");
		}
		std::string origin = receivers_.current_context();
		base::WeakPtr<Usb> self = weak_.GetWeakPtr();
		std::thread([self, origin, devs, names, cb = std::move(cb)]() mutable {
			int k = br_choose(names, 0);
			br_on_service_thread(base::BindOnce(&Usb::Chosen, self, origin, k >= 0 ? devs[(size_t)k].dev : 0,
							    std::move(cb)));
		}).detach();
	}

	void ForgetDevice( const blink::String &guid, ForgetDeviceCallback cb ) override
	{
		granted_.erase(receivers_.current_context() + "\n" + guid.Utf8());
		std::move(cb).Run();
	}

	void SetClient( mojo::PendingAssociatedRemote<dm::UsbDeviceManagerClient> ) override {}

private:
	void Chosen( std::string origin, int dev, GetPermissionCallback cb )
	{
		for ( auto &d : br_usb_list() ) {
			if ( dev != 0 && d.dev == dev ) {
				granted_.insert(origin + "\n" + usb_guid(dev));
				printf("browser: usb: %s (%04x:%04x) chosen\n", d.name.c_str(), d.vendor, d.product);
				std::move(cb).Run(usb_info(d));
				return;
			}
		}
		std::move(cb).Run(nullptr);
	}

	mojo::ReceiverSet<bm::WebUsbService, std::string>	receivers_;
	std::set<std::string>					granted_;	/* origin, then the device's guid */
	base::WeakPtrFactory<Usb>				weak_{this};
};

/* ---------------------------------------------------------------- gamepads */

/*
 * A field of a gamepad's input report: where its bits are, and what it
 * is (a button, an axis, a hat switch), from the report descriptor
 */
struct PadField {
	int		report_id;
	int		bit, size;	/* from the start of the report, after its ID */
	bool		is_signed;
	int64_t		lmin, lmax;
	uint32_t	usage;		/* the page in the upper 16 bits */
};

/* A gamepad: a HID interface whose collection is a joystick or a game pad */
struct Pad {
	HidIf				hid;
	int				ud = -1;
	bool				has_report_id = false;
	std::vector<PadField>		buttons, axes, hats;	/* in the order of their usages */
	device::GamepadStandardMappingFunction	mapper = nullptr;
	std::u16string			id;
	bool				gesture = false;	/* a button was pressed: the page may see it */
	bool				gone = false;
};

constexpr uint32_t kGenericDesktop = 0x01, kButtonPage = 0x09;

/* The fields of a gamepad's input reports, from the parser of report descriptors */
void pad_fields( Pad *p, const std::vector<device::mojom::HidCollectionInfoPtr> &cols )
{
	for ( const auto &c : cols ) {
		for ( const auto &r : c->input_reports ) {
			int bit = 0;
			for ( const auto &it : r->items ) {
				int n = (int)it->report_count, sz = (int)it->report_size;
				if ( !it->is_constant && it->is_variable ) {
					for ( int k = 0; k < n; k++ ) {
						uint32_t usage = 0;
						if ( it->is_range ) {
							usage = ( (uint32_t)it->usage_minimum->usage_page << 16 )
								| (uint32_t)( it->usage_minimum->usage + k );
						} else if ( !it->usages.empty() ) {
							const auto &u = it->usages[std::min<size_t>((size_t)k, it->usages.size() - 1)];
							usage = ( (uint32_t)u->usage_page << 16 ) | u->usage;
						}
						PadField f{ (int)r->report_id, bit + k * sz, sz, it->logical_minimum < 0,
							    it->logical_minimum, it->logical_maximum, usage };
						uint32_t page = usage >> 16, u = usage & 0xffff;
						if ( page == kButtonPage ) {
							p->buttons.push_back(f);
						} else if ( page == kGenericDesktop && u == 0x39 ) {
							p->hats.push_back(f);
						} else if ( page == kGenericDesktop && ( ( u >= 0x30 && u <= 0x38 ) || u == 0x40 ) ) {
							p->axes.push_back(f);
						} else if ( page == 0x02 && ( u == 0xc4 || u == 0xc5 ) ) {
							p->axes.push_back(f);		/* accelerator, brake */
						}
					}
				}
				bit += n * sz;
			}
		}
	}
	auto by_usage = []( const PadField &a, const PadField &b ) { return a.usage < b.usage; };
	std::stable_sort(p->buttons.begin(), p->buttons.end(), by_usage);
	std::stable_sort(p->axes.begin(), p->axes.end(), by_usage);
}

/* A field's value in a report (the bytes after its ID) */
int64_t field_value( const PadField &f, const uint8_t *b, int len )
{
	uint64_t v = 0;
	for ( int i = 0; i < f.size; i++ ) {
		int at = f.bit + i;
		if ( at / 8 >= len ) return f.lmin;
		if ( b[at / 8] & ( 1 << ( at % 8 ) ) ) v |= (uint64_t)1 << i;
	}
	if ( f.is_signed && f.size > 0 && f.size < 64 && ( v & ( (uint64_t)1 << ( f.size - 1 ) ) ) ) {
		v |= ~(uint64_t)0 << f.size;
	}
	return (int64_t)v;
}

/*
 * A report put into the gamepad's raw state: the buttons, then the axes
 * from -1 to 1, then each hat switch as two axes (x, then y), the order
 * the mappings of standard gamepads are written for
 */
void pad_report( Pad *p, const uint8_t *data, int len, device::Gamepad *raw )
{
	int id = 0;
	if ( p->has_report_id && len > 0 ) {
		id = data[0];
		data++;
		len--;
	}
	unsigned nb = 0, na = 0;
	for ( const PadField &f : p->buttons ) {
		if ( nb >= device::Gamepad::kButtonsLengthCap ) break;
		if ( f.report_id != id ) {
			nb++;
			continue;
		}
		bool on = field_value(f, data, len) > f.lmin;
		raw->buttons[nb].pressed = on;
		raw->buttons[nb].touched = on;
		raw->buttons[nb].value = on ? 1.0 : 0.0;
		raw->buttons[nb].used = true;
		nb++;
	}
	for ( const PadField &f : p->axes ) {
		if ( na >= device::Gamepad::kAxesLengthCap ) break;
		if ( f.report_id == id && f.lmax > f.lmin ) {
			double v = (double)( field_value(f, data, len) - f.lmin ) / (double)( f.lmax - f.lmin );
			raw->axes[na] = std::max(-1.0, std::min(1.0, 2 * v - 1));
		}
		raw->axes_used |= 1u << na;
		na++;
	}
	static const int kHatX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, kHatY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	for ( const PadField &f : p->hats ) {
		if ( na + 2 > device::Gamepad::kAxesLengthCap ) break;
		if ( f.report_id == id ) {
			int64_t v = field_value(f, data, len) - f.lmin, n = f.lmax - f.lmin + 1;
			int dir = ( v >= 0 && v < n && n > 0 ) ? (int)( v * 8 / n ) : -1;
			raw->axes[na] = dir < 0 ? 0 : kHatX[dir];
			raw->axes[na + 1] = dir < 0 ? 0 : kHatY[dir];
		}
		raw->axes_used |= 3u << na;
		na += 2;
	}
	raw->buttons_length = nb;
	raw->axes_length = na;
}

/* Whether a person has used the gamepad: a button pressed, or a stick moved far */
bool pad_gesture( const device::Gamepad &g )
{
	for ( unsigned i = 0; i < g.buttons_length; i++ ) {
		if ( g.buttons[i].pressed ) return true;
	}
	return false;
}

class Gamepads final : public dm::GamepadMonitor {
public:
	void Bind( mojo::PendingReceiver<dm::GamepadMonitor> r ) { receivers_.Add(this, std::move(r)); }

	/* the gamepads' memory, and the gamepads plugged in looked for every second from now on */
	void GamepadStartPolling( GamepadStartPollingCallback cb ) override
	{
		if ( !made_ ) {
			made_ = true;
			base::MappedReadOnlyRegion m = base::ReadOnlySharedMemoryRegion::Create(sizeof(device::GamepadHardwareBuffer));
			if ( m.IsValid() ) {
				buffer_ = new ( m.mapping.memory() ) device::GamepadHardwareBuffer();
				region_ = std::move(m.region);
				mapping_ = std::move(m.mapping);
			} else {
				printf("browser: gamepad: no shared memory for the gamepads\n");
			}
			Scan();
			std::thread([this]() {
				for ( ;; ) {
					std::this_thread::sleep_for(std::chrono::seconds(1));
					br_on_service_thread(base::BindOnce(&Gamepads::Scan, base::Unretained(this)));
				}
			}).detach();
		}
		std::move(cb).Run(region_.IsValid() ? region_.Duplicate() : base::ReadOnlySharedMemoryRegion());
	}
	void GamepadStopPolling( GamepadStopPollingCallback cb ) override { std::move(cb).Run(); }
	void SetObserver( mojo::PendingRemote<dm::GamepadObserver> o ) override { observers_.Add(std::move(o)); }

private:
	/* The devices: a gamepad not seen before opened and read; one that went taken away */
	void Scan()
	{
		std::vector<BrUsbDevice> list = br_usb_list();
		std::string sig;
		for ( auto &d : list ) sig += std::to_string(d.dev) + ",";
		if ( sig == listed_ ) return;
		listed_ = sig;
		for ( auto &h : hid_interfaces() ) {
			bool have = false;
			for ( auto &p : pads_ ) have = have || ( p && !p->gone && p->hid.dev == h.dev && p->hid.ifno == h.ifno );
			if ( have || h.in_ep == 0 ) continue;
			std::vector<device::mojom::HidCollectionInfoPtr> cols;
			bool has_id = false;
			size_t in = 0, out = 0, feature = 0;
			device::HidReportDescriptor(h.desc).GetDetails(&cols, &has_id, &in, &out, &feature);
			bool is_pad = false;
			for ( auto &c : cols ) {
				is_pad = is_pad || ( c->usage->usage_page == kGenericDesktop
						     && ( c->usage->usage == 0x04 || c->usage->usage == 0x05 ) );
			}
			if ( !is_pad ) continue;
			int slot = -1;
			for ( int i = 0; i < (int)device::Gamepads::kItemsLengthCap && slot < 0; i++ ) {
				if ( !pads_[(size_t)i] || pads_[(size_t)i]->gone ) slot = i;
			}
			if ( slot < 0 ) break;
			auto p = std::make_unique<Pad>();
			p->hid = h;
			p->has_report_id = has_id;
			pad_fields(p.get(), cols);
			p->ud = br_usb_open(h.dev);
			if ( p->ud <= 0 ) {
				printf("browser: gamepad: %s cannot be opened (%d)\n", h.usb.name.c_str(), p->ud);
				continue;
			}
			p->mapper = device::GetGamepadStandardMappingFunction(h.usb.name, h.usb.vendor, h.usb.product, 0,
									      h.usb.release, device::GAMEPAD_BUS_USB,
									      device::kGamepadDriverUnknown);
			char vp[64];
			snprintf(vp, sizeof(vp), "%sVendor: %04x Product: %04x)", p->mapper ? "STANDARD GAMEPAD " : "",
				 h.usb.vendor, h.usb.product);
			p->id = base::UTF8ToUTF16(( h.usb.name.empty() ? std::string("USB Gamepad") : h.usb.name ) + " (" + vp);
			printf("browser: gamepad: %s, %zu buttons, %zu axes, %zu hats, %s mapping, in place %d\n",
			       h.usb.name.c_str(), p->buttons.size(), p->axes.size(), p->hats.size(),
			       p->mapper ? "the standard" : "no", slot);
			Pad *pp = p.get();
			pads_[(size_t)slot] = std::move(p);
			std::thread(&Gamepads::Read, this, pp, slot).detach();
		}
	}

	/* A gamepad's input reports, read until it goes */
	void Read( Pad *p, int slot )
	{
		device::Gamepad raw;
		std::vector<uint8_t> buf(64);
		for ( ;; ) {
			int n = br_usb_transfer(p->ud, p->hid.in_ep, buf.data(), (int)buf.size(), 500);
			if ( n == kBrUsbTimeout ) continue;
			if ( n < 0 ) break;
			pad_report(p, buf.data(), n, &raw);
			raw.timestamp = ( base::TimeTicks::Now() - base::TimeTicks() ).InMicroseconds();
			device::Gamepad shown = raw;
			if ( p->mapper ) {
				p->mapper(raw, &shown);
				shown.mapping = device::GamepadMapping::kStandard;
			}
			shown.SetID(p->id);
			shown.connected = true;
			bool first = !p->gesture && pad_gesture(raw);
			if ( first ) p->gesture = true;
			if ( p->gesture ) Put(slot, shown, first ? 1 : 0);
		}
		printf("browser: gamepad: %s went away\n", p->hid.usb.name.c_str());
		device::Gamepad none;
		Put(slot, none, -1);
		br_usb_close(p->ud);
		p->gone = true;
	}

	/* A place of the shared memory written; the pages told of a gamepad come (1) or gone (-1) */
	void Put( int slot, const device::Gamepad &g, int told )
	{
		if ( buffer_ != nullptr ) {
			std::lock_guard<std::mutex> hold(write_lock_);
			buffer_->seqlock.WriteBegin();
			buffer_->data.items[(size_t)slot] = g;
			buffer_->seqlock.WriteEnd();
		}
		if ( told != 0 ) {
			br_on_service_thread(base::BindOnce([]( Gamepads *self, int slot, device::Gamepad g, int told ) {
				for ( auto &o : self->observers_ ) {
					if ( told > 0 ) {
						o->GamepadConnected((uint32_t)slot, g);
					} else {
						o->GamepadDisconnected((uint32_t)slot, g);
					}
				}
			}, base::Unretained(this), slot, g, told));
		}
	}

	mojo::ReceiverSet<dm::GamepadMonitor>		receivers_;
	mojo::RemoteSet<dm::GamepadObserver>		observers_;
	bool						made_ = false;
	base::ReadOnlySharedMemoryRegion		region_;
	base::WritableSharedMemoryMapping		mapping_;
	device::GamepadHardwareBuffer			*buffer_ = nullptr;
	std::mutex					write_lock_;
	std::string					listed_;
	std::array<std::unique_ptr<Pad>, device::Gamepads::kItemsLengthCap>	pads_;
};

class Haptics final : public dm::GamepadHapticsManager {
public:
	void Bind( mojo::PendingReceiver<dm::GamepadHapticsManager> r ) { receivers_.Add(this, std::move(r)); }
	void PlayVibrationEffectOnce( uint32_t, dm::GamepadHapticEffectType, dm::GamepadEffectParametersPtr,
				      PlayVibrationEffectOnceCallback cb ) override
	{
		std::move(cb).Run(dm::GamepadHapticsResult::GamepadHapticsResultNotSupported);
	}
	void ResetVibrationActuator( uint32_t, ResetVibrationActuatorCallback cb ) override
	{
		std::move(cb).Run(dm::GamepadHapticsResult::GamepadHapticsResultNotSupported);
	}

private:
	mojo::ReceiverSet<dm::GamepadHapticsManager>	receivers_;
};

template <typename Interface, typename Service>
void Binder()
{
	static Service *svc;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<Interface> r ) {
				if ( svc == nullptr ) svc = new Service();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<Interface>(std::move(h))));
		}));
}

}  // namespace

/* The binders of the frames' broker, once when the engine is up */
void br_usb_hid_init()
{
	Binder<bm::HidService, Hid>();
	Binder<bm::WebUsbService, Usb>();
	Binder<dm::GamepadMonitor, Gamepads>();
	Binder<dm::GamepadHapticsManager, Haptics>();
}
