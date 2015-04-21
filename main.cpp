#include <tox/tox.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <json/json.h>
#include "tunnel.h"
#include "control.h"
#include <errno.h>
#include "main.h"

#define BOOTSTRAP_ADDRESS "23.226.230.47"
#define BOOTSTRAP_PORT 33445
#define BOOTSTRAP_KEY "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074"

Tunnel *tunnels[100];
volatile bool keep_running = true;
std::string myip;
int epoll_handle;

void hex_string_to_bin(const char *hex_string, uint8_t *ret)
{
    // byte is represented by exactly 2 hex digits, so lenth of binary string
    // is half of that of the hex one. only hex string with even length
    // valid. the more proper implementation would be to check if strlen(hex_string)
    // is odd and return error code if it is. we assume strlen is even. if it's not
    // then the last byte just won't be written in 'ret'.
    size_t i, len = strlen(hex_string) / 2;
    const char *pos = hex_string;

    for (i = 0; i < len; ++i, pos += 2)
        sscanf(pos, "%2hhx", &ret[i]);

}
void to_hex(char *a, const uint8_t *p, int size) {
	char buffer[3];
	for (int i=0; i<size; i++) {
		int x = snprintf(buffer,3,"%02x",p[i]);
		a[i*2] = buffer[0];
		a[i*2+1] = buffer[1];
	}
}
void MyFriendRequestCallback(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length, void *user_data) {
	char tox_printable_id[TOX_ADDRESS_SIZE * 2 + 1];
	int friendnumber = tox_friend_add_norequest(tox, public_key,NULL);

	memset(tox_printable_id, 0, sizeof(tox_printable_id));
	to_hex(tox_printable_id, public_key,TOX_ADDRESS_SIZE);
	printf("Accepted friend request from %s(%s) as %d\n", tox_printable_id, message, friendnumber);
}
void MyFriendMessageCallback(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message, size_t length, void *user_data) {
	printf("message %d %s\n",friend_number,message);
}
void MyFriendStatusCallback(Tox *tox, uint32_t friend_number, const uint8_t *message, size_t length, void *user_data) {
	printf("status msg #%d %s\n",friend_number,message);
	Json::Reader reader;
	Json::Value root;
	if (reader.parse(std::string((const char *)message,length), root)) {
		Json::Value ip = root["ownip"];
		if (ip.isString()) {
			std::string peerip = ip.asString();
			if (!tunnels[friend_number]) {
				tunnels[friend_number] = new Tunnel(friend_number,myip,peerip);
			}
		}
	} else {
		printf("unable to parse status, ignoring\n");
	}
}
void MyFriendLossyPacket(Tox *tox, uint32_t friend_number, const uint8_t *data, size_t length, void *user_data) {
	if (data[0] == 200) {
		std::cout << "sending to tun" << std::endl;
		if (tunnels[friend_number]) tunnels[friend_number]->processPacket(data+1,length-1);
	}
}
void handle_int(int something) {
	puts("int!");
	keep_running = false;
}
void connection_status(Tox *tox, TOX_CONNECTION connection_status, void *user_data) {
	switch (connection_status) {
	case TOX_CONNECTION_NONE:
		puts("connection lost");
		break;
	case TOX_CONNECTION_TCP:
		puts("tcp connection established");
		break;
	case TOX_CONNECTION_UDP:
		puts("udp connection established");
		break;
	}
}
void saveState(Tox *tox) {
	int size = tox_get_savedata_size(tox);
	uint8_t *savedata = new uint8_t[size];
	tox_get_savedata(tox,savedata);
	int fd = open("savedata",O_TRUNC|O_WRONLY|O_CREAT,0644);
	assert(fd);
	int written = write(fd,savedata,size);
	assert(written == size);
	close(fd);
}
int main(int argc, char **argv) {
	uint8_t *bootstrap_pub_key = new uint8_t[TOX_PUBLIC_KEY_SIZE];
	hex_string_to_bin(BOOTSTRAP_KEY, bootstrap_pub_key);

	epoll_handle = epoll_create(20);
	assert(epoll_handle >= 0);
	
	Control control;

	struct sigaction interupt;
	memset(&interupt,0,sizeof(interupt));
	interupt.sa_handler = &handle_int;
	sigaction(SIGINT,&interupt,NULL);

	for (int i=0; i<100; i++) tunnels[i] = 0;

	assert(argc >= 2);
	myip = argv[1];
	Json::Value root;
	root["ownip"] = myip;
	Json::FastWriter fw;
	
	Tox *my_tox;
	bool want_bootstrap = false;
	int oldstate = open("savedata",O_RDONLY);
	if (oldstate >= 0) {
		struct stat info;
		fstat(oldstate,&info);
		uint8_t *temp = new uint8_t[info.st_size];
		int size = read(oldstate,temp,info.st_size);
		assert(size == info.st_size);
		my_tox = tox_new(NULL,temp,size,NULL);
		delete temp;
	} else {
		/* Create a default Tox */
		my_tox = tox_new(NULL, NULL, 0, NULL);
		want_bootstrap = true;
	}

	uint8_t toxid[TOX_ADDRESS_SIZE];
	tox_self_get_address(my_tox,toxid);
	char tox_printable_id[TOX_ADDRESS_SIZE * 2 + 1];
	memset(tox_printable_id, 0, sizeof(tox_printable_id));
	to_hex(tox_printable_id, toxid,TOX_ADDRESS_SIZE);
	printf("my id is %s and IP is %s\n",tox_printable_id,myip.c_str());
	
	/* Register the callbacks */
	tox_callback_friend_request(my_tox, MyFriendRequestCallback, NULL);
	tox_callback_friend_message(my_tox, MyFriendMessageCallback, NULL);
	tox_callback_friend_status_message(my_tox, MyFriendStatusCallback, NULL);
	tox_callback_friend_lossy_packet(my_tox, MyFriendLossyPacket, NULL);

	/* Define or load some user details for the sake of it */
	struct utsname hostinfo;
	uname(&hostinfo);
	tox_self_set_name(my_tox, (const uint8_t*)hostinfo.nodename, strlen(hostinfo.nodename), NULL); // Sets the username
	std::string json = fw.write(root);
	if (json[json.length()-1] == '\n') json.erase(json.length()-1, 1);
	tox_self_set_status_message(my_tox, (const uint8_t*)json.data(), json.length(), NULL); // Sets the status message

	/* Set the user status to TOX_USER_STATUS_NONE. Other possible values:
	 * TOX_USER_STATUS_AWAY and TOX_USER_STATUS_BUSY */
	tox_self_set_status(my_tox, TOX_USER_STATUS_NONE);

	tox_callback_self_connection_status(my_tox, &connection_status, 0);

	/* Bootstrap from the node defined above */
	if (want_bootstrap) tox_bootstrap(my_tox, BOOTSTRAP_ADDRESS, BOOTSTRAP_PORT, bootstrap_pub_key, NULL);

	if (argc >= 3) {
		const char *peer = argv[2];
		printf("going to connect to %s\n",peer);
		const char *msg = "vpn_test";
		uint8_t peerbinary[TOX_ADDRESS_SIZE];
		TOX_ERR_FRIEND_ADD error;
		hex_string_to_bin(peer,peerbinary);
		tox_friend_add(my_tox, (uint8_t*)peerbinary, (uint8_t*)msg,strlen(msg),&error);
		printf("err code %d\n",error);
		switch (error) {
		case TOX_ERR_FRIEND_ADD_OK:
			puts("no error");
			break;
		case TOX_ERR_FRIEND_ADD_ALREADY_SENT:
			puts("already sent");
			break;
		case TOX_ERR_FRIEND_ADD_BAD_CHECKSUM:
			puts("crc error");
			break;
		}
	}

	while (keep_running) {
		tox_iterate(my_tox); // will call the callback functions defined and registered

		struct epoll_event events[10];
		int count = epoll_wait(epoll_handle, events, 10, tox_iteration_interval(my_tox));
		if (count == -1) std::cout << "epoll error " << strerror(errno);
		else {
			for (int i=0; i<count; i++) {
				EpollTarget *t = (EpollTarget *)events[i].data.ptr;
				t->handleData(events[i],my_tox);
			}
		}
	}
	puts("shutting down");
	saveState(my_tox);
	tox_kill(my_tox);
	return 0;
}