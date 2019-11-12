
#include <cstdio>
#include <sapi/sys.hpp>
#include <sapi/inet.hpp>
#include <sapi/var.hpp>
#include <sapi/chrono.hpp>

#include "sl_config.h"

#define NTP_TIMESTAMP_DELTA 2208988800ULL
//#define NTP_TIMESTAMP_DELTA 0

typedef struct MCU_PACK {

	u8 li_vn_mode;      // Eight bits. li, vn, and mode.
	// li.   Two bits.   Leap indicator.
	// vn.   Three bits. Version number of the protocol.
	// mode. Three bits. Client will pick mode 3 for client.

	u8 stratum;         // Eight bits. Stratum level of the local clock.
	u8 poll;            // Eight bits. Maximum interval between successive messages.
	u8 precision;       // Eight bits. Precision of the local clock.

	u32 rootDelay;      // 32 bits. Total round trip delay time.
	u32 rootDispersion; // 32 bits. Max error aloud from primary clock source.
	u32 refId;          // 32 bits. Reference clock identifier.

	u32 refTm_s;        // 32 bits. Reference time-stamp seconds.
	u32 refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

	u32 origTm_s;       // 32 bits. Originate time-stamp seconds.
	u32 origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

	u32 rxTm_s;         // 32 bits. Received time-stamp seconds.
	u32 rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

	u32 txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
	u32 txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.
} ntp_packet;              // Total: 384 bits or 48 bytes.

static time_t get_time_using_network_time_protocol(int retry_count);
static time_t get_time_using_daytime_protocol(int retry_count);
static time_t get_time_using_time_protocol(int retry_count);

static Printer p;

int main(int argc, char * argv[]){
	Cli cli(argc, argv);
	cli.set_publisher(SL_CONFIG_PUBLISHER);
	cli.handle_version();

	p.set_verbose_level( cli.get_option(
									arg::OptionName("verbose")
									)
								);

	time_t t;

	int retry_count = cli.get_option(
				arg::OptionName("retry")
				).to_integer();
	if( retry_count == 0 ){
		retry_count = 20;
	}

	if( cli.get_option(arg::OptionName("daytime")) == "true" ){
		p.info("using daytime procotol");
		t = get_time_using_daytime_protocol(retry_count);
	} else if( cli.get_option(arg::OptionName("time")) == "true" ){
		p.info("using time procotol");
		t = get_time_using_time_protocol(retry_count);
	} else if( cli.get_option(arg::OptionName("ntp")) == "true" ){
		p.info("using network time procotol");
		t = get_time_using_network_time_protocol(retry_count);
	} else {
		cli.show_options();
		exit(1);
	}

	if( t != 0 ){
		String internet_time;
		internet_time = ctime(&t);
		internet_time.replace(
					arg::StringToErase("\n"),
					arg::StringToInsert("")
					);

		internet_time.replace(
					arg::StringToErase("\r"),
					arg::StringToInsert("")
					);

		p.debug("internet time is %s", internet_time.cstring());
	}

	if( t != 0 && cli.get_option(arg::OptionName("sync")) == "true" ){
		if( Time::set_time_of_day(Time(t)) < 0 ){
			p.error("failed to sync device time");
		} else {
			p.info("successfully synced internet time %s", ctime(&t));
		}
	}

	if( t == 0 ){
		//failed to get the time
		p.error("failed to get a timestamp from the internet");
		exit(1);
	}

	return 0;
}

time_t get_time_using_network_time_protocol(int retry_count){
	ntp_packet raw_packet;
	int result;
	u32 i;

	DataReference packet(raw_packet);

	packet.fill(0);
	raw_packet.li_vn_mode = 0x1b; //request the time

	var::Vector<SocketAddressInfo> address_list;
	SocketAddressInfo address_info(SocketAddressInfo::FAMILY_INET,
											 SocketAddressInfo::TYPE_DGRAM,
											 SocketAddressInfo::PROTOCOL_UDP);

	address_list = address_info.fetch_node("time-a-wwv.nist.gov");

	p.debug("fetched %d options", address_list.count());
	for(i=0; i < address_list.count(); i++){

		p.debug("Address info %d %d %d",
				  address_list.at(i).family(),
				  address_list.at(i).type(),
				  address_list.at(i).protocol()
				  );

		SocketAddress socket_address(address_list.at(i), 123);
		Socket socket;
		SocketOption option;

		if( socket.create(
				 arg::SourceSocketAddress(socket_address)
				 ) < 0 ){
			printf("Failed to create socket");
			continue;
		}

		socket << option.ip_time_to_live(56);

		//make socket non-blocking -- may need to try write/read a few times

		p.debug("write %ld bytes on port:%d", packet.size(), socket_address.port());
		packet.to_u32()[0] = 0;
		if( (result = socket.write(
				  arg::SourceData(packet),
				  arg::SourceSocketAddress(socket_address)
				  )) != (int)packet.size() ){
			p.error("failed to send the whole packet (%d, %d)", result, socket.error_number());
			continue;
		}


		Timer::wait_seconds(1);
		int size = packet.size();
		Timer read_timer;
		read_timer.start();
		while( read_timer < MicroTime::from_seconds(1) && (result != size) ){
			p.debug("read %d", packet.size());
			result = socket.read(
						arg::DestinationData(packet),
						arg::DestinationSocketAddress(socket_address)
						);

			chrono::wait_milliseconds(100);
		}

		if( result != size ){
			p.error("response wasn't right (%d, %d)", result, socket.error_number());
			continue;
		}
		break;
	}

	if( i == address_list.count() ){
		return 0;
	}

	//packet->txTm_s = ntohl(packet->txTm_s);
	packet.to_u32()[0] = ntohl(packet.at_u32(0));
	return packet.at_u32(0) - NTP_TIMESTAMP_DELTA;
}

time_t get_time_using_daytime_protocol(int retry_count){
	Data packet( arg::Size(sizeof(u32)) );
	u32 i;
	int result;
	packet.fill(0);
	Data response( arg::Size(256) );

	var::Vector<SocketAddressInfo> address_list;
	SocketAddressInfo address_info(SocketAddressInfo::FAMILY_INET,
											 SocketAddressInfo::TYPE_STREAM,
											 SocketAddressInfo::PROTOCOL_TCP);

	address_list = address_info.fetch_node("time.nist.gov");

	p.debug("fetched %ld options", address_list.count());
	for(i=0; i < address_list.count(); i++){

		p.debug("Address info %d %d %d",
				  address_list.at(i).family(),
				  address_list.at(i).type(),
				  address_list.at(i).protocol()
				  );

		SocketAddress socket_address(address_list.at(i), 13);
		Socket socket;
		SocketOption option;

		if( socket.create(arg::SourceSocketAddress(socket_address)) < 0 ){
			p.error("Failed to create socket");
			continue;
		}

		socket << option.ip_time_to_live(56);

		p.debug("connect to %s:%d", socket_address.address_to_string().cstring(), socket_address.port());
		if( (result = socket.connect(
				  arg::SourceSocketAddress(socket_address)
				  ) ) < 0 ){
			p.error("Failed to connect to socket address -> %s:%d (%d, %d)",
					  socket_address.address_to_string().cstring(),
					  socket_address.port(),
					  socket.error_number(),
					  result);
			continue;
		}

		//if using UDP need to bind

		p.debug("write %ld bytes on port %d", packet.size(), socket_address.port());
		packet.to_u32()[0] = 0;
		if( (result = socket.write(packet)) != (int)packet.size() ){
			p.error("failed to send the whole packet (%d, %d)", result, socket.error_number());
			continue;
		}


		//Timer::wait_seconds(5);
		p.debug("read");
		if( (result = socket.read(response)) <= 0 ){
			p.error("response wasn't right (%d, %d)", result, socket.error_number());
			continue;
		}


		break;
	}

	if( i == address_list.count() ){
		return 0;
	}

	String time_string(response);

	time_string.replace(
				arg::StringToErase("\n"),
				arg::StringToInsert("")
				);

	time_string.replace(
				arg::StringToErase("\r"),
				arg::StringToInsert("")
				);

	p.info("Time String: %s", time_string.cstring());
	Tokenizer time_tokens(
				arg::TokenEncodedString(time_string),
				arg::TokenDelimeters(" ")
				);

	if( time_tokens.count() != 9 ){
		p.error("bad format in time response (" F32U ")", time_tokens.count());
		return 0;
	}

	Tokenizer date_tokens(
				arg::TokenEncodedString(time_tokens.at(1)),
				arg::TokenDelimeters("-")
				);

	if( date_tokens.count() != 3 ){
		p.error("bad format in date response (" F32U ")", date_tokens.count());
		return 0;
	}

	struct tm time_struct;
	memset(&time_struct, 0, sizeof(time_struct));
	time_struct.tm_year = date_tokens.at(0).to_integer() + 100;
	time_struct.tm_mon = date_tokens.at(1).to_integer()-1;
	time_struct.tm_mday = date_tokens.at(2).to_integer();

	Tokenizer clock_tokens(
				arg::TokenEncodedString(time_tokens.at(2)),
				arg::TokenDelimeters(":")
				);


	if( date_tokens.count() != 3 ){
		p.error("bad format in clock response (" F32U ")", clock_tokens.count());
		return 0;
	}
	time_struct.tm_hour = clock_tokens.at(0).to_integer();
	time_struct.tm_min = clock_tokens.at(1).to_integer();
	time_struct.tm_sec = clock_tokens.at(2).to_integer();

	return mktime(&time_struct);
}

time_t get_time_using_time_protocol(int retry_count){
	Data packet( arg::Size(sizeof(u32)) );
	int result;
	u32 i;
	packet.fill(0);


	//packet->li_vn_mode = 0x1b; //request the time

	var::Vector<SocketAddressInfo> address_list;
	SocketAddressInfo address_info(SocketAddressInfo::FAMILY_INET,
											 SocketAddressInfo::TYPE_STREAM,
											 SocketAddressInfo::PROTOCOL_TCP);


	address_list = address_info.fetch_node("time.nist.gov");

	for(i=0; i < address_list.count(); i++){

		SocketAddress socket_address(address_list.at(i), 37);
		Socket socket;

		if( socket.create(arg::SourceSocketAddress(socket_address)) < 0 ){
			p.error("Failed to create socket");
			continue;
		}

		int count = 0;
		do {
			if( (result = socket.connect(
					  arg::SourceSocketAddress(socket_address)
					  )) < 0 ){
				p.error("Failed to connect to socket address -> %s:%d (%d, %d)",
						  socket_address.address_to_string().cstring(),
						  socket_address.port(),
						  socket.error_number(),
						  result);
			} else {


				p.debug("write %ld bytes on port:%d", packet.size(), socket_address.port());
				packet.to_u32()[0] = 0;
				if( (result = socket.write(packet)) != (int)packet.size() ){
					p.error("failed to send the whole packet (%d, %d)", result, socket.error_number());
				} else {
					result = socket.read(
								arg::DestinationData(packet),
								arg::DestinationSocketAddress(socket_address)
								);
				}
			}

			socket.close();

		} while( (result == 0)  && (count++ < retry_count) );

		if( result > 0 ){
			break;
		}
	}

	if( i == address_list.count() ){
		return 0;
	}

	packet.to_u32()[0] = ntohl(packet.at_u32(0));
	return packet.at_u32(0) - NTP_TIMESTAMP_DELTA;
}

/*

dns_tmr: dns_check_entries
dns_tmr: dns_check_entries
dns_tmr: dns_check_entries
dns_tmr: dns_check_entries
dns_tmr: dns_check_entries
INFO:SYS:process_start:/app/ram/sntp
INFO:SYS:process start: execute /app/ram/sntp --time --verbose=debug
INFO:SYS:process_start:returned 1
INFO:SOCKET:SEM Init 0x200182e4 0x20018324
dns_enqueue: "pureaire-a2f7e.firebaseio.com": use DNS entry 0
dns_enqueue: "pureaire-a2f7e.firebaseio.com": use DNS pcb 0
dns_send: dns_servers[0] "pureaire-a2f7e.firebaseio.com": request
sending DNS request ID 36047 for name "pureaire-a2f7e.firebaseio.com" to server 0
ip4_output_if: et0
IP header:
+-------------------------------+
| 4 | 5 |  0x00 |        75     | (v, hl, tos, len)
+-------------------------------+
|        2      |000|       0   | (id, flags, offset)
+-------------------------------+
|  255  |   17  |    0x0000     | (ttl, proto, chksum)
+-------------------------------+
|  192  |  168  |    1  |   16  | (src)
+-------------------------------+
|  192  |  168  |    1  |    1  | (dest)
+-------------------------------+
ip4_output_if: call netif->output()
INFO:SOCKET:write interface
next:0x0
0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0x0 0x80 0xE1 0x0 0x0 0x0 0x8 0x6 0x0 0x1 0x8 0x0 0x6 0x4 0x0 0x1 0x0 0x80 0xE1 0x0 0x0 0x0 0xC0 0xA8 0x1 0x10 0x0 0x0 0x0 0x0 0x0 0x0 0xC0 0xA8 0x1 0x1
INFO:SOCKET:sent:42
INFO:SOCKET:Got 60 bytesINFO:SOCKET:write interface

next:0x0
0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0x0 0x80 0xE1 0x0 0x0 0x0 0x8 0x6 0x0 0x1 0x8 0x0 0x6 0x4 0x0 0x1 0x0 0x80 0xE1 0x0 0x0 0x0 0xC0 0xA8 0x1 0x10 0x0 0x0 0x0 0x0 0x0 0x0 0xC0 0xA8 0x1 0x1
INFO:SOCKET:sent:42
dns_tmr: dns_check_entries
dns_send: dns_servers[0] "pureaire-a2f7e.firebaseio.com": request
sending DNS request ID 36047 for name "pureaire-a2f7e.firebaseio.com" to server 0
ip4_output_if: et0
IP header:
+-------------------------------+
| 4 | 5 |  0x00 |        75     | (v, hl, tos, len)
+-------------------------------+
|        3      |000|       0   | (id, flags, offset)
+-------------------------------+
|  255  |   17  |    0x0000     | (ttl, proto, chksum)
+-------------------------------+
|  192  |  168  |    1  |   16  | (src)
+-------------------------------+
|  192  |  168  |    1  |    1  | (dest)
+-------------------------------+
ip4_output_if: call netif->output()
INFO:SOCKET:write interface
INFO:SOCKET:sent:89
INFO:SOCKET:Got 60 bytes
INFO:SOCKET:Got 105 bytes
ip_input: iphdr->dest 0x1001a8c0 netif->ip_addr 0x1001a8c0 (0x1a8c0, 0x1a8c0, 0x10000000)
ip4_input: packet accepted on interface et
ip4_input:
IP header:
+-------------------------------+
| 4 | 5 |  0x00 |        91     | (v, hl, tos, len)
+-------------------------------+
|        0      |010|       0   | (id, flags, offset)
+-------------------------------+
|   64  |   17  |    0xb730     | (ttl, proto, chksum)
+-------------------------------+
|  192  |  168  |    1  |    1  | (src)
+-------------------------------+
|  192  |  168  |    1  |   16  | (dest)
+-------------------------------+
ip4_input: p->len 91 p->tot_len 91
dns_recv: "pureaire-a2f7e.firebaseio.com": response = 35.201.97.85
dns_tmr: dns_check_entries
dns_tmr: dns_check_entries




*/
