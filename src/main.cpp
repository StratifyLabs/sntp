
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

static time_t get_time_using_network_time_protocol();
static time_t get_time_using_daytime_protocol();
static time_t get_time_using_time_protocol();

static Printer p;

int main(int argc, char * argv[]){
	Cli cli(argc, argv);
	cli.set_publisher(SL_CONFIG_PUBLISHER);
	cli.handle_version();

	p.set_verbose_level( cli.get_option("verbose") );

	time_t t;

	if( cli.get_option("daytime") == "true" ){
		p.info("using daytime procotol");
		t = get_time_using_daytime_protocol();
	} else if( cli.get_option("time") == "true" ){
		p.info("using time procotol");
		t = get_time_using_time_protocol();
	} else {
		p.info("using network time procotol");
		t = get_time_using_network_time_protocol();
	}

	if( t != 0 ){
		p.debug("internet time is %s", ctime(&t));
	}

	if( t != 0 && cli.get_option("sync") == "true" ){
		if( Time::set_time_of_day(t) < 0 ){
			p.error("failed to sync device time");
		} else {
			p.info("successfully synced internet time %s", ctime(&t));
		}
	}

	if( t == 0 ){
		//failed to get the time
		exit(1);
	}

	return 0;
}

time_t get_time_using_network_time_protocol(){
	StructuredData<ntp_packet> packet;
	int result;
	u32 i;

	packet.fill(0);
	packet->li_vn_mode = 0x1b; //request the time

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


		if( socket.create(socket_address) < 0 ){
			printf("Failed to create socket");
			continue;
		}

		socket << option.ip_time_to_live(56);

		//make socket non-blocking -- may need to try write/read a few times

		p.debug("write %ld bytes on port %d", packet.size(), socket_address.port());
		packet.to_u32()[0] = 0;
		if( (result = socket.write(packet, socket_address)) != (int)packet.size() ){
			p.error("failed to send the whole packet (%d, %d)", result, socket.error_number());
			continue;
		}


		//Timer::wait_seconds(5);
		p.debug("read");
		int size = packet.size();
		if( (result = socket.read(packet, socket_address)) != size ){
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

time_t get_time_using_daytime_protocol(){
	Data packet(sizeof(u32));
	u32 i;
	int result;
	packet.fill(0);
	Data response(256);

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

		if( socket.create(socket_address) < 0 ){
			p.error("Failed to create socket");
			continue;
		}

		socket << option.ip_time_to_live(56);

		p.debug("connect to %s:%d", socket_address.address_to_string().cstring(), socket_address.port());
		if( (result = socket.connect(socket_address)) < 0 ){
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

	String time_string(response.to_char(), response.size());

	time_string.replace("\n", "");
	time_string.replace("\r", "");

	p.info("Time String: %s", time_string.cstring());
	Tokenizer time_tokens(time_string, " ");

	if( time_tokens.count() != 9 ){
		p.error("bad format in time response (" F32U ")", time_tokens.count());
		return 0;
	}

	Tokenizer date_tokens(time_tokens.at(1), "-");
	if( date_tokens.count() != 3 ){
		p.error("bad format in date response (" F32U ")", date_tokens.count());
		return 0;
	}

	struct tm time_struct;
	memset(&time_struct, 0, sizeof(time_struct));
	time_struct.tm_year = date_tokens.at(0).to_integer() + 100;
	time_struct.tm_mon = date_tokens.at(1).to_integer()-1;
	time_struct.tm_mday = date_tokens.at(2).to_integer();

	Tokenizer clock_tokens(time_tokens.at(2), ":");
	if( date_tokens.count() != 3 ){
		p.error("bad format in clock response (" F32U ")", clock_tokens.count());
		return 0;
	}
	time_struct.tm_hour = clock_tokens.at(0).to_integer();
	time_struct.tm_min = clock_tokens.at(1).to_integer();
	time_struct.tm_sec = clock_tokens.at(2).to_integer();

	return mktime(&time_struct);
}

time_t get_time_using_time_protocol(){
	Data packet(sizeof(u32));
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

		if( socket.create(socket_address) < 0 ){
			p.error("Failed to create socket");
			continue;
		}

		if( (result = socket.connect(socket_address)) < 0 ){
			p.error("Failed to connect to socket address -> %s:%d (%d, %d)",
					 socket_address.address_to_string().cstring(),
					 socket_address.port(),
					 socket.error_number(),
					 result);
			continue;
		}


		p.debug("write %ld bytes on port %d", packet.size(), socket_address.port());
		packet.to_u32()[0] = 0;
		if( (result = socket.write(packet)) != (int)packet.size() ){
			p.error("failed to send the whole packet (%d, %d)", result, socket.error_number());
			continue;
		}

		p.debug("read");
		int size = packet.size();
		if( (result = socket.read(packet)) != size ){
			p.error("response wasn't right (%d, %d)", result, socket.error_number());
			continue;
		}
		break;
	}

	if( i == address_list.count() ){
		return 0;
	}

	packet.to_u32()[0] = ntohl(packet.at_u32(0));
	return packet.at_u32(0) - NTP_TIMESTAMP_DELTA;
}
