/* librist. Copyright 2019 SipRadius LLC. All right reserved.
 * Author: Kuldeep Singh Dhaka <kuldeep@madresistor.com>
 * Author: Sergio Ammirata, Ph.D. <sergio@ammirata.net>
 */

#include <librist.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <stdbool.h>
#include "network.h"

#define INPUT_COUNT 2
#define OUTPUT_COUNT 4

// TODO: add options for flow_id, cname and gre-dst-port

const char help_str[] = "Usage: %s [OPTIONS] \nWhere OPTIONS are:\n"
"       -u | --url ADDRESS:PORT                                          * | Output IP address and port                          |\n"
"       -f | --miface name/index                                         * | Multicast Interface name (linux) or index (win)     |\n"
"       -T | --recovery-type TYPE                                        * | Type of recovery (off, bytes, time)                 |\n"
"       -x | --url2 ADDRESS:PORT                                         * | Second Output IP address and port                   |\n"
"       -q | --miface2 name/index                                        * | Multicast Interface2 name (linux) or index (win)    |\n"
"       -s | --server  rist://@ADDRESS:PORT or rist6://@ADDRESS:PORT     * | Address of local rist server                        |\n"
"       -b | --server2 rist://@ADDRESS:PORT or rist6://@ADDRESS:PORT       | Address of second local rist server                 |\n"
"       -c | --server3 rist://@ADDRESS:PORT or rist6://@ADDRESS:PORT       | Address of third local rist server                  |\n"
"       -d | --server4 rist://@ADDRESS:PORT or rist6://@ADDRESS:PORT       | Address of fourth local rist server                 |\n"
"       -e | --encryption-password PWD                                     | pre-shared encryption password                      |\n"
"       -t | --encryption-type TYPE                                        | encryption type (1 = AES-128, 2 = AES-256)          |\n"
"       -p | --profile number                                              | rist profile (0 = simple, 1 = main)                 |\n"
"       -n | --gre-src-port port                                           | reduced profile src port to forward                 |\n"
"       -v | --verbose-level value                                         | QUIET=-1,INFO=0,ERROR=1,WARN=2,DEBUG=3,SIMULATE=4   |\n"
"       -h | --help                                                        | Show this help                                      |\n"
"  ***** Default peer settings in case the sender is not librist:                                                                |\n"
"       -m | --min-buf ms                                                * | Minimum rist recovery buffer size                   |\n"
"       -M | --max-buf ms                                                * | Maximum rist recovery buffer size                   |\n"
"       -o | --reorder-buf ms                                            * | Reorder buffer size                                 |\n"
"       -r | --min-rtt RTT                                               * | Minimum RTT                                         |\n"
"       -R | --max-rtt RTT                                               * | Maximum RTT                                         |\n"
"       -B | --bloat-mode MODE                                           * | Buffer bloat mitigation mode (slow, fast, fixed)    |\n"
"       -l | --bloat-limit NACK_COUNT                                    * | Buffer bloat min nack count for random discard      |\n"
"       -L | --bloat-hardlimit NACK_COUNT                                * | Buffer bloat max nack count for hard limit discard  |\n"
"       -W | --max-bitrate MBPS                                          * | rist recovery max bitrate (Mbit/s)                  |\n"
"   * == mandatory value \n"
"Default values: %s \n"
"       --recovery-type time      \\\n"
"       --min-buf 1000            \\\n"
"       --max-buf 1000            \\\n"
"       --reorder-buf 25          \\\n"
"       --min-rtt 50              \\\n"
"       --max-rtt 500             \\\n"
"       --max-bitrate 100         \\\n"
"       --encryption-type 1       \\\n"
"       --profile 1               \\\n"
"       --gre-src-port 1971       \\\n"
"       --verbose-level 2         \n";

static struct option long_options[] = {
	{ "url",             required_argument, NULL, 'u' },
	{ "miface",          required_argument, NULL, 'f' },
	{ "url2",            required_argument, NULL, 'x' },
	{ "miface2",         required_argument, NULL, 'q' },
	{ "server",          required_argument, NULL, 's' },
	{ "server2",         required_argument, NULL, 'b' },
	{ "server3",         required_argument, NULL, 'c' },
	{ "server4",         required_argument, NULL, 'd' },
	{ "recovery-type",   required_argument, NULL, 'T' },
	{ "min-buf",         required_argument, NULL, 'm' },
	{ "max-buf",         required_argument, NULL, 'M' },
	{ "reorder-buf",     required_argument, NULL, 'o' },
	{ "min-rtt",         required_argument, NULL, 'r' },
	{ "max-rtt",         required_argument, NULL, 'R' },
	{ "bloat-mode",      required_argument, NULL, 'B' },
	{ "bloat-limit",     required_argument, NULL, 'l' },
	{ "bloat-hardlimit", required_argument, NULL, 'L' },
	{ "max-bitrate",     required_argument, NULL, 'W' },
	{ "encryption-password", required_argument, NULL, 'e' },
	{ "encryption-type", required_argument, NULL, 't' },
	{ "profile",         required_argument, NULL, 'p' },
	{ "gre-src-port",    required_argument, NULL, 'n' },
	{ "verbose-level",   required_argument, NULL, 'v' },
	{ "help",            no_argument,       NULL, 'h' },
	{ 0, 0, 0, 0 },
};

void usage(char *name)
{
	fprintf(stderr, "%s%s", help_str, name);
	exit(1);
}

static int mpeg[INPUT_COUNT];
static struct network_url parsed_url[INPUT_COUNT];

struct rist_port_filter {
	uint16_t src_port;
	uint16_t dst_port;
};

static void cb_recv(void *arg, uint64_t flow_id, const void *buf, size_t len, uint16_t src_port, uint16_t dst_port)
{
	struct rist_port_filter *port_filter = (void *) arg;
	(void) flow_id;

	if (port_filter->src_port != src_port)
		fprintf(stderr, "Source port mistmatch %d != %d\n", port_filter->src_port, src_port);

	for (size_t i = 0; i < INPUT_COUNT; i++) {
		if (mpeg[i] > 0) {
			sendto(mpeg[i], buf, len, 0, (struct sockaddr *)&(parsed_url[i].u),
				sizeof(struct sockaddr_in));
		}
	}
}

int main(int argc, char *argv[])
{
	int option_index;
	char *url[INPUT_COUNT];
	char *miface[INPUT_COUNT];
	char *addr[OUTPUT_COUNT];
	char *shared_secret = NULL;
	char c;
	enum rist_profile profile = RIST_MAIN;
	enum rist_log_level loglevel = RIST_LOG_WARN;
	uint8_t encryption_type = 1;
	enum rist_recovery_mode recovery_mode = RIST_RECOVERY_MODE_TIME;
	uint32_t recovery_maxbitrate = 100;
	uint32_t recovery_maxbitrate_return = 0;
	uint32_t recovery_length_min = 1000;
	uint32_t recovery_length_max = 1000;
	uint32_t recover_reorder_buffer = 25;
	uint32_t recovery_rtt_min = 50;
	uint32_t recovery_rtt_max = 500;
	enum rist_buffer_bloat_mode buffer_bloat_mode = RIST_BUFFER_BLOAT_MODE_OFF;
	uint32_t buffer_bloat_limit = 6;
	uint32_t buffer_bloat_hard_limit = 20;
	struct rist_port_filter port_filter;
	port_filter.src_port = 1971;
	port_filter.dst_port = 1968;

	for (size_t i = 0; i < INPUT_COUNT; i++) {
		url[i] = NULL;
		miface[i] = NULL;
		mpeg[i] = 0;
	}

	for (size_t i = 0; i < OUTPUT_COUNT; i++) {
		addr[i] = NULL;
	}

	while ((c = getopt_long(argc, argv, "u:x:q:v:f:n:e:s:h:b:c:d:m:M:o:r:R:B:l:L:W:t:p:n", long_options, &option_index)) != -1) {
		switch (c) {
		case 'u':
			url[0] = strdup(optarg);
		break;
		case 'x':
			url[1] = strdup(optarg);
		break;
		case 'f':
			miface[0] = strdup(optarg);
		break;
		case 'q':
			miface[1] = strdup(optarg);
		break;
		case 's':
			addr[0] = strdup(optarg);
		break;
		case 'b':
			addr[1] = strdup(optarg);
		break;
		case 'c':
			addr[2] = strdup(optarg);
		break;
		case 'd':
			addr[3] = strdup(optarg);
		break;
		case 'm':
			recovery_length_min = atoi(optarg);
		break;
		case 'M':
			recovery_length_max = atoi(optarg);
		break;
		case 'o':
			recover_reorder_buffer = atoi(optarg);
		break;
		case 'r':
			recovery_rtt_min = atoi(optarg);
		break;
		case 'R':
			recovery_rtt_max = atoi(optarg);
		break;
		case 'B':
			if (!strcmp(optarg, "off")) {
				buffer_bloat_mode = RIST_BUFFER_BLOAT_MODE_OFF;
			} else if (!strcmp(optarg, "normal")) {
				buffer_bloat_mode = RIST_BUFFER_BLOAT_MODE_NORMAL;
			} else if (!strcmp(optarg, "aggressive")) {
				buffer_bloat_mode = RIST_BUFFER_BLOAT_MODE_AGGRESSIVE;
			} else {
				usage(argv[0]);
			}
		break;
		case 'l':
			buffer_bloat_limit = atoi(optarg);
		break;
		case 'L':
			buffer_bloat_hard_limit = atoi(optarg);
		break;
		case 'W':
			recovery_maxbitrate = atoi(optarg);
		break;
		case 't':
			encryption_type = atoi(optarg);
		break;
		case 'p':
			profile = atoi(optarg);
		break;
		case 'n':
			port_filter.src_port = atoi(optarg);
		break;
		case 'e':
			shared_secret = strdup(optarg);
		break;
		case 'v':
			loglevel = atoi(optarg);
		break;
		case 'h':
			/* Fall through */
		default:
			usage(argv[0]);
		break;
		}
	}

	// For some reason under windows the empty len is 1

	bool all_url_null = true;
	for (size_t i = 0; i < INPUT_COUNT; i++) {
		if (url[i] != NULL) {
			all_url_null = false;
			break;
		}
	}

	if (all_url_null) {
		fprintf(stderr, "No address provided\n");
		usage(argv[0]);
	}

	// minimum, first addr need to be provided
	if (addr[0] == NULL) {
		usage(argv[0]);
	}

	if (argc < 3) {
		usage(argv[0]);
	}

	/* rist side */
	fprintf(stderr, "Configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d buffer_bloat=%d (limit:%d, hardlimit:%d)\n",
			recovery_maxbitrate, recovery_length_min, recovery_length_max, recover_reorder_buffer, recovery_rtt_min,
			recovery_rtt_max, buffer_bloat_mode, buffer_bloat_limit, buffer_bloat_hard_limit);

	struct rist_server *ctx;

	if (rist_server_create(&ctx, profile) != 0) {
		fprintf(stderr, "Could not create rist server context\n");
		exit(1);
	}

	const struct rist_peer_config default_peer_config = {
		.address = addr[0],
		.recovery_mode = recovery_mode,
		.recovery_maxbitrate = recovery_maxbitrate,
		.recovery_maxbitrate_return = recovery_maxbitrate_return,
		.recovery_length_min = recovery_length_min,
		.recovery_length_max = recovery_length_max,
		.recover_reorder_buffer = recover_reorder_buffer,
		.recovery_rtt_min = recovery_rtt_min,
		.recovery_rtt_max = recovery_rtt_max,
		.weight = 5,
		.bufferbloat_mode = buffer_bloat_mode,
		.bufferbloat_limit = buffer_bloat_limit,
		.bufferbloat_hard_limit = buffer_bloat_hard_limit
	};

	if (rist_server_init(ctx, &default_peer_config, loglevel) == -1) {
		fprintf(stderr, "Could not init rist server\n");
		exit(1);
	}

	if (shared_secret != NULL) {
		int keysize =  encryption_type == 1 ? 128 : 256;
		if (rist_server_encrypt_enable(ctx, shared_secret, keysize) == -1) {
			fprintf(stderr, "Could not add enable encryption\n");
			exit(1);
		}
	}

	for (size_t i = 1; i < OUTPUT_COUNT; i++) {
		if (addr[i] == NULL) {
			continue;
		}

		if (rist_server_add_peer(ctx, addr[i]) == -1) {
			fprintf(stderr, "Could not init rist server%i\n", (int)(i + 1));
			exit(1);
		}
	}

	/* Mpeg side */
	bool atleast_one_socket_opened = false;
	for (size_t i = 0; i < INPUT_COUNT; i++) {
		if (url[i] == NULL) {
			continue;
		}

		// TODO: support ipv6 destinations
		if (parse_url(url[i], &parsed_url[i]) != 0) {
			fprintf(stderr, "[ERROR] %s / %s\n", parsed_url[i].error, url[i]);
			continue;
		} {
			fprintf(stderr, "[INFO] URL parsed successfully: Host %s, Port %d\n",
				(char *) parsed_url[i].hostname, parsed_url[i].port);
		}

		mpeg[i] = udp_Connect_Simple(AF_INET, -1, miface[i]);
		if (mpeg <= 0) {
			char *msgbuf = malloc(256);
			msgbuf = udp_GetErrorDescription(mpeg[i], msgbuf);
			fprintf(stderr, "[ERROR] Could not connect to: Host %s, Port %d. %s\n",
				(char *) parsed_url[i].hostname, parsed_url[i].port, msgbuf);
			free(msgbuf);
			exit(1);
		}

		fprintf(stderr, "Socket %i is open\n", (int)(i + 1));
		atleast_one_socket_opened = true;
	}

	if (!atleast_one_socket_opened) {
		exit(1);
	}

	/* Start the rist protocol thread */
	if (rist_server_start(ctx, cb_recv, &port_filter)) {
		fprintf(stderr, "Could not start rist server\n");
		exit(1);
	}

	fprintf(stderr, "Pause application?\n");

	pause();

	return 0;
}