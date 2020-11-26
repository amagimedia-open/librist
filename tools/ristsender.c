/* librist. Copyright 2020 SipRadius LLC. All right reserved.
 * Author: Sergio Ammirata, Ph.D. <sergio@ammirata.net>
 */

#include <librist/librist.h>
#include <librist/udpsocket.h>
#include "librist/version.h"
#ifdef USE_MBEDTLS
#include "librist/librist_srp.h"
#include "srp_shared.h"
#endif
#include "vcs_version.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "getopt-shim.h"
#include <stdbool.h>
#include <signal.h>
#include "common/attributes.h"
#include "risturlhelp.h"
#include "rist-private.h"
#include <stdatomic.h>
#include "yamlparse.h"

#if defined(_WIN32) || defined(_WIN64)
# define strtok_r strtok_s
#define MSG_DONTWAIT (0)
#endif

#define RIST_MARK_UNUSED(unused_param) ((void)(unused_param))

#define RISTSENDER_VERSION "2"

#define MAX_INPUT_COUNT 10
#define MAX_OUTPUT_COUNT 10

static int signalReceived = 0;
static struct rist_logging_settings *logging_settings;

struct rist_callback_object {
	int sd;
	struct evsocket_ctx *evctx;
	struct rist_ctx *receiver_ctx;
	struct rist_ctx *sender_ctx;
	const struct rist_udp_config *udp_config;
	uint8_t recv[RIST_MAX_PACKET_SIZE];
};

struct receive_thread_object {
	int sd;
	struct rist_ctx *ctx[MAX_OUTPUT_COUNT];
	const struct rist_udp_config *udp_config;
	uint8_t recv[RIST_MAX_PACKET_SIZE];
};

struct rist_sender_args {
	struct rist_ctx *ctx;	
	char* token;
	int profile;
	enum rist_log_level loglevel;
	int encryption_type;
	char* shared_secret;
	int buffer_size;
	int statsinterval;
	uint16_t stream_id;
};

static struct option long_options[] = {
{ "file",      required_argument, NULL, 'f' },
{ "inputurl",        required_argument, NULL, 'i' },
{ "outputurl",       required_argument, NULL, 'o' },
{ "buffer",          required_argument, NULL, 'b' },
{ "secret",          required_argument, NULL, 's' },
{ "encryption-type", required_argument, NULL, 'e' },
{ "profile",         required_argument, NULL, 'p' },
{ "null-packet-deletion",  no_argument, NULL, 'n' },
{ "tun",             required_argument, NULL, 't' },
{ "stats",           required_argument, NULL, 'S' },
{ "verbose-level",   required_argument, NULL, 'v' },
#ifdef USE_MBEDTLS
{ "srpfile",         required_argument, NULL, 'F' },
#endif
{ "help",            no_argument,       NULL, 'h' },
{ "help-url",        no_argument,       NULL, 'u' },
{ 0, 0, 0, 0 },
};

const char help_str[] = "Usage: %s [OPTIONS] \nWhere OPTIONS are:\n"
"       -f | --file name.yaml                   * | YAML config file                                         |\n"
"       -i | --inputurl  udp://... or rtp://... * | Comma separated list of input udp or rtp URLs            |\n"
"       -o | --outputurl rist://...             * | Comma separated list of output rist URLs                 |\n"
"       -b | --buffer value                       | Default buffer size for packet retransmissions           |\n"
"       -s | --secret PWD                         | Default pre-shared encryption secret                     |\n"
"       -e | --encryption-type TYPE               | Default Encryption type (0, 128 = AES-128, 256 = AES-256)|\n"
"       -p | --profile number                     | Rist profile (0 = simple, 1 = main, 2 = advanced)        |\n"
"       -n | --null-packet-deletion               | Enable NPD, receiver needs to support this!              |\n"
"       -S | --statsinterval value (ms)           | Interval at which stats get printed, 0 to disable        |\n"
"       -v | --verbose-level value                | To disable logging: -1, log levels match syslog levels   |\n"
#ifdef USE_MBEDTLS
"       -F | --srpfile filepath                   | When in listening mode, use this file to hold the list   |\n"
"                                                 | of usernames and passwords to validate against. Use the  |\n"
"                                                 | ristsrppasswd tool to create the line entries.           |\n"
#endif
"       -h | --help                               | Show this help                                           |\n"
"       -u | --help-url                           | Show all the possible url options                        |\n"
"   * == mandatory value \n"
"Default values: %s \n"
"       --profile 1               \\\n"
"       --statsinterval 1000      \\\n"
"       --verbose-level 6         \n";

/*
static uint64_t risttools_convertRTPtoNTP(uint32_t i_rtp)
{
	uint64_t i_ntp;
    int32_t clock = 90000;
    i_ntp = (uint64_t)i_rtp << 32;
    i_ntp /= clock;
	return i_ntp;
}
*/

#ifdef USE_MBEDTLS
	FILE *srpfile = NULL;
#endif

static void input_udp_recv(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
	struct rist_callback_object *callback_object = (void *) arg;
	RIST_MARK_UNUSED(evctx);
	RIST_MARK_UNUSED(revents);
	RIST_MARK_UNUSED(fd);

	ssize_t recv_bufsize = -1;
	struct sockaddr_in addr4 = {0};
	struct sockaddr_in6 addr6 = {0};
	//struct sockaddr *addr;
	uint8_t *recv_buf = callback_object->recv;

	uint16_t address_family = (uint16_t)callback_object->udp_config->address_family;
	if (address_family == AF_INET6) {
		socklen_t addrlen = sizeof(struct sockaddr_in6);
		recv_bufsize = udpsocket_recvfrom(callback_object->sd, recv_buf, RIST_MAX_PACKET_SIZE, MSG_DONTWAIT, (struct sockaddr *) &addr6, &addrlen);
		//addr = (struct sockaddr *) &addr6;
	} else {
		socklen_t addrlen = sizeof(struct sockaddr_in);
		recv_bufsize = udpsocket_recvfrom(callback_object->sd, recv_buf, RIST_MAX_PACKET_SIZE, MSG_DONTWAIT, (struct sockaddr *) &addr4, &addrlen);
		//addr = (struct sockaddr *) &addr4;
	}

	if (recv_bufsize > 0) {
		ssize_t offset = 0;
		struct rist_data_block data_block = { 0 };
		// Delegate ts_ntp to the library by default.
		// If we wanted to be more accurate, we could use the kernel nic capture timestamp (linux)
		data_block.ts_ntp = 0;
		data_block.flags = 0;
		if (callback_object->udp_config->rtp_timestamp && recv_bufsize > 12)
		{
			// Extract timestamp from rtp header
			//uint32_t rtp_time = (recv_buf[4] << 24) | (recv_buf[5] << 16) | (recv_buf[6] << 8) | recv_buf[7];
			// Convert to NTP (assumes 90Khz)
			//data_block.ts_ntp = risttools_convertRTPtoNTP(rtp_time);
			// TODO: Figure out why this does not work (commenting out for now)
		}
		if (callback_object->udp_config->rtp_sequence && recv_bufsize > 12)
		{
			// Extract sequence number from rtp header
			//data_block.seq = (uint64_t)((recv_buf[2] << 8) | recv_buf[3]);
			//data_block.flags = RIST_DATA_FLAGS_USE_SEQ;
			// TODO: Figure out why this does not work (commenting out for now)
		}
		if (callback_object->udp_config->rtp && recv_bufsize > 12)
			offset = 12; // TODO: check for header extensions and remove them as well
		data_block.payload = recv_buf + offset;
		data_block.payload_len = recv_bufsize - offset;
		int w = rist_sender_data_write(callback_object->sender_ctx, &data_block);
		// TODO: report error?
		(void) w;
	}
	else
	{
		// EWOULDBLOCK = EAGAIN = 11 would be the most common recoverable error (if any)
		if (errno != EWOULDBLOCK)
			rist_log(logging_settings, RIST_LOG_ERROR, "Input receive failed: errno=%d, ret=%d, socket=%d\n", errno, recv_bufsize, callback_object->sd);
	}
}

static void input_udp_sockerr(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
	struct rist_callback_object *callback_object = (void *) arg;
	RIST_MARK_UNUSED(evctx);
	RIST_MARK_UNUSED(revents);
	RIST_MARK_UNUSED(fd);
	rist_log(logging_settings, RIST_LOG_ERROR, "Socket error on sd=%d, stream-id=%d !\n", callback_object->sd, callback_object->udp_config->stream_id);
}

static void usage(char *cmd)
{
	rist_log(logging_settings, RIST_LOG_INFO, "%s%s version %s libRIST library: %s API version: %s\n", cmd, help_str, LIBRIST_VERSION, librist_version(), librist_api_version());
	exit(1);
}

static int cb_auth_connect(void *arg, const char* connecting_ip, uint16_t connecting_port, const char* local_ip, uint16_t local_port, struct rist_peer *peer)
{
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	char message[500];
	int ret = snprintf(message, 500, "auth,%s:%d,%s:%d", connecting_ip, connecting_port, local_ip, local_port);
	rist_log(logging_settings, RIST_LOG_INFO,"Peer has been authenticated, sending auth message: %s\n", message);
	struct rist_oob_block oob_block;
	oob_block.peer = peer;
	oob_block.payload = message;
	oob_block.payload_len = ret;
	rist_oob_write(ctx, &oob_block);
	return 0;
}

static int cb_auth_disconnect(void *arg, struct rist_peer *peer)
{
	(void)peer;
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	(void)ctx;
	return 0;
}

static int cb_recv_oob(void *arg, const struct rist_oob_block *oob_block)
{
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	(void)ctx;
	if (oob_block->payload_len > 4 && strncmp(oob_block->payload, "auth,", 5) == 0) {
		rist_log(logging_settings, RIST_LOG_INFO,"Out-of-band data received: %.*s\n", (int)oob_block->payload_len, (char *)oob_block->payload);
	}
	return 0;
}

static int cb_stats(void *arg, const struct rist_stats *stats_container)
{
	(void)arg;
	rist_log(logging_settings, RIST_LOG_INFO, "%s\n\n", stats_container->stats_json);
	rist_stats_free(stats_container);
	return 0;
}

static void intHandler(int signal)
{
	rist_log(logging_settings, RIST_LOG_INFO, "Signal %d received\n", signal);
	signalReceived = signal;
}

static struct rist_peer* setup_rist_peer(struct rist_sender_args *setup)
{
	if (rist_stats_callback_set(setup->ctx, setup->statsinterval, cb_stats, NULL) == -1) {
		rist_log(logging_settings, RIST_LOG_ERROR, "Could not enable stats callback\n");
		return NULL;
	}

	if (rist_auth_handler_set(setup->ctx, cb_auth_connect, cb_auth_disconnect, setup->ctx) < 0) {
		rist_log(logging_settings, RIST_LOG_ERROR, "Could not initialize rist auth handler\n");
		return NULL;
	}

	if (setup->profile != RIST_PROFILE_SIMPLE) {
		if (rist_oob_callback_set(setup->ctx, cb_recv_oob, setup->ctx) == -1) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Could not enable out-of-band data\n");
			return NULL;
		}
	}

	if (rist_stats_callback_set(setup->ctx, setup->statsinterval, cb_stats, NULL) == -1) {
		rist_log(logging_settings, RIST_LOG_ERROR, "Could not enable stats callback\n");
		return NULL;
	}

	// Rely on the library to parse the url
	const struct rist_peer_config *peer_config_link = NULL;
	if (rist_parse_address(setup->token, (void *)&peer_config_link))
	{
		rist_log(logging_settings, RIST_LOG_ERROR, "Could not parse peer options for sender: %s\n", setup->token);
		return NULL;
	}

	/* Process overrides */
	struct rist_peer_config *overrides_peer_config = (void *)peer_config_link;
	if (setup->shared_secret && peer_config_link->secret[0] == 0) {
		strncpy(overrides_peer_config->secret, setup->shared_secret, RIST_MAX_STRING_SHORT -1);
		if (setup->encryption_type)
			overrides_peer_config->key_size = setup->encryption_type;
		else if (!overrides_peer_config->key_size)
			overrides_peer_config->key_size = 128;
	}
	if (setup->buffer_size) {
		overrides_peer_config->recovery_length_min = setup->buffer_size;
		overrides_peer_config->recovery_length_max = setup->buffer_size;
	}
	if (setup->stream_id) {
		if (setup->stream_id % 2 != 0) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Error parsing peer options for sender: %s, stream-id (%d) must be even!\n\n", setup->token, setup->stream_id);
			return NULL;
		}
		else {
			overrides_peer_config->virt_dst_port = setup->stream_id;
		}
	}

	/* Print config */
	rist_log(logging_settings, RIST_LOG_INFO, "Link configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
		peer_config_link->recovery_maxbitrate, peer_config_link->recovery_length_min, peer_config_link->recovery_length_max, 
		peer_config_link->recovery_reorder_buffer, peer_config_link->recovery_rtt_min, peer_config_link->recovery_rtt_max,
		peer_config_link->congestion_control_mode, peer_config_link->min_retries, peer_config_link->max_retries);

	struct rist_peer *peer;
	if (rist_peer_create(setup->ctx, &peer, peer_config_link) == -1) {
		rist_log(logging_settings, RIST_LOG_ERROR, "Could not add peer connector to %s\n", peer_config_link->address);
		free((void *)peer_config_link);
		return NULL;
	}

#ifdef USE_MBEDTLS
	int srp_error = 0;
	if (setup->profile != RIST_PROFILE_SIMPLE) {
		if (strlen(peer_config_link->srp_username) > 0 && strlen(peer_config_link->srp_password) > 0)
		{
			srp_error = rist_enable_eap_srp(peer, peer_config_link->srp_username, peer_config_link->srp_password, NULL, NULL);
			if (srp_error)
				rist_log(logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP for peer\n", srp_error);
		}
		if (srpfile)
		{
			srp_error = rist_enable_eap_srp(peer, NULL, NULL, user_verifier_lookup, srpfile);
			if (srp_error)
				rist_log(logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP global authenticator, file %s\n", srp_error, srpfile);
		}
	}
	else
		rist_log(logging_settings, RIST_LOG_WARN, "SRP Authentication is not available for Rist Simple Profile\n");
#endif

	free((void *)peer_config_link);

	return peer;
}

static PTHREAD_START_FUNC(input_loop, arg)
{
	struct rist_callback_object *callback_object = (void *) arg;
	// This is my main loop (one thread per receiver)
	while (!signalReceived) {
		if (callback_object->receiver_ctx)
		{
			// RIST receiver
			const struct rist_data_block *b;
			int queue_size = rist_receiver_data_read(callback_object->receiver_ctx, &b, 5);
			if (queue_size > 0) {
				if (queue_size % 10 == 0 || queue_size > 50)
					rist_log(logging_settings, RIST_LOG_WARN, "Falling behind on rist_receiver_data_read: %d\n", queue_size);
				if (b && b->payload) {
					int w = rist_sender_data_write(callback_object->sender_ctx, b);
					// TODO: report error?
					(void) w;
				}
			}
		}
		else
		{
			// UDP recevier. Infinite wait, 100 socket events
			evsocket_loop_single(callback_object->evctx, 5, 100);
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	int option_index;
	struct rist_callback_object callback_object[MAX_INPUT_COUNT] = { 0 };
	struct evsocket_event *event[MAX_INPUT_COUNT];
	char *inputurl = NULL;
	char *outputurl = NULL;
	char *oobtun = NULL;
	char *shared_secret = NULL;
	int buffer_size = 0;
	char *yamlfile = NULL;
	int encryption_type = 0;
	int statsinterval = 1000;
	enum rist_profile profile = RIST_PROFILE_MAIN;
	enum rist_log_level loglevel = RIST_LOG_INFO;
	bool npd = false;
	struct rist_sender_args peer_args;

	rist_tools_config_object * yaml_config = malloc(sizeof(rist_tools_config_object));

	for (size_t i = 0; i < MAX_INPUT_COUNT; i++)
		event[i] = NULL;

#ifdef _WIN32
#define STDERR_FILENO 2
    signal(SIGINT, intHandler);
    signal(SIGTERM, intHandler);
    signal(SIGABRT, intHandler);
#else
	struct sigaction act = { {0} };
	act.sa_handler = intHandler;
	sigaction(SIGINT, &act, NULL);
#endif

	if (rist_logging_set(&logging_settings, loglevel, NULL, NULL, NULL, stderr) != 0) {
		fprintf(stderr,"Failed to setup logging!\n");
		exit(1);
	}

	rist_log(logging_settings, RIST_LOG_INFO, "Starting ristsender version: %s libRIST library: %s API version: %s\n", LIBRIST_VERSION, librist_version(), librist_api_version());

	while ((c = (char)getopt_long(argc, argv, "f:i:o:b:s:e:t:p:S:F:v:hun", long_options, &option_index)) != -1) {
		switch (c) {
		case 'f':
			yamlfile = strdup(optarg);
			if (!parse_yaml(yamlfile,yaml_config)){
				fprintf(stderr,"Could not import yaml file %s\n",yamlfile);
				exit(1);
			}
			inputurl = yaml_config->input_url;
			outputurl = yaml_config->output_url;
			buffer_size = yaml_config->buffer;
			shared_secret = yaml_config->secret;
			encryption_type = yaml_config->encryption_type;
			oobtun = yaml_config->tunnel_interface;
			profile = yaml_config->profile;
			statsinterval = yaml_config->stats_interval;
		break;
		case 'i':
			inputurl = strdup(optarg);
		break;
		case 'o':
			outputurl = strdup(optarg);
		break;
		case 'b':
			buffer_size = atoi(optarg);
		break;
		case 's':
			shared_secret = strdup(optarg);
		break;
		case 'e':
			encryption_type = atoi(optarg);
		break;
		case 't':
			oobtun = strdup(optarg);
		break;
		case 'p':
			profile = atoi(optarg);
		break;
		case 'S':
			statsinterval = atoi(optarg);
		break;
		case 'v':
			loglevel = atoi(optarg);
			if (rist_logging_set(&logging_settings, loglevel, NULL, NULL, NULL, stderr) != 0) {
				fprintf(stderr,"Failed to setup logging!\n");
				exit(1);
			}
		break;
#ifdef USE_MBEDTLS
		case 'F':
			srpfile = fopen(optarg, "r");
			if (!srpfile)
				return 1;
			break;
#endif
		case 'u':
			rist_log(logging_settings, RIST_LOG_INFO, "%s", help_urlstr);
			exit(1);
		break;
		case 'n':
			npd = true;
			break;
		case 'h':
			/* Fall through */
		default:
			usage(argv[0]);
		break;
		}
	}

	if (inputurl == NULL || outputurl == NULL) {
		usage(argv[0]);
	}

	if (argc < 2) {
		usage(argv[0]);
	}

	peer_args.loglevel = loglevel;
	peer_args.profile = profile;
	peer_args.encryption_type = encryption_type;
	peer_args.shared_secret = shared_secret;
	peer_args.buffer_size = buffer_size;
	peer_args.statsinterval = statsinterval;

	// Setup the input udp/rist objects: listen to the given address(es)
	int32_t stream_id_check[MAX_INPUT_COUNT];
	for (size_t j = 0; j < MAX_INPUT_COUNT; j++)
		stream_id_check[j] = -1;
	struct evsocket_ctx *evctx = NULL;
	bool atleast_one_socket_opened = false;
	char *saveptrinput;
	char *inputtoken = strtok_r(inputurl, ",", &saveptrinput);
	for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
		if (!inputtoken)
			break;

		// First parse extra url and parameters
		const struct rist_udp_config *udp_config = NULL;
		if (rist_parse_udp_address(inputtoken, &udp_config)) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Could not parse inputurl %s\n", inputtoken);
			goto next;
		}

		// Check for duplicate stream-ids and reject the entire config if we have any dups
		for (size_t j = 0; j < MAX_INPUT_COUNT; j++) {
			if (stream_id_check[j] == -1) {
				stream_id_check[j] = (int32_t)udp_config->stream_id;
				rist_log(logging_settings, RIST_LOG_INFO, "Assigning stream-id %d to this input\n", udp_config->stream_id);
				break;
			} else if ((uint16_t)stream_id_check[j] == udp_config->stream_id) {
				rist_log(logging_settings, RIST_LOG_ERROR, "Every input must have a unique stream-id (%d) when you multiplex\n", udp_config->stream_id);
				goto shutdown;
			}
		}

		// Setup the output rist objects (a brand new instance per receiver)
		char *saveptroutput;
		char *outputtoken = strtok_r(outputurl, ",", &saveptroutput);
		// All output peers should be on the same context per receiver
		if (rist_sender_create(&callback_object[i].sender_ctx, peer_args.profile, 0, logging_settings) != 0) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Could not create rist sender context\n");
			break;
		}
		if (npd) {
			if (profile == RIST_PROFILE_SIMPLE)
				rist_log(logging_settings, RIST_LOG_INFO, "NULL packet deletion enabled on SIMPLE profile. This is non-compliant but might work if receiver supports it (librist does)\n");
			else
				rist_log(logging_settings, RIST_LOG_INFO, "NULL packet deletion enabled. Support for this feature is not guaranteed to be present on receivers. Please make sure the receiver supports it (librist does)\n");
			if (rist_sender_npd_enable(callback_object[i].sender_ctx) != 0) {
				rist_log(logging_settings, RIST_LOG_ERROR, "Failed to enable null packet deletion\n");
			}
		}
		for (size_t j = 0; j < MAX_OUTPUT_COUNT; j++) {
			peer_args.token = outputtoken;
			peer_args.ctx = callback_object[i].sender_ctx;
			peer_args.stream_id = udp_config->stream_id;
			struct rist_peer *peer = setup_rist_peer(&peer_args);
			if (peer == NULL)
				goto shutdown;

			outputtoken = strtok_r(NULL, ",", &saveptroutput);
			if (!outputtoken)
				break;
		}

		if (strcmp(udp_config->prefix, "rist") == 0) {
			// This is a rist input (new context for each listener)
			if (rist_receiver_create(&callback_object[i].receiver_ctx, peer_args.profile, logging_settings) != 0) {
				rist_log(logging_settings, RIST_LOG_ERROR, "Could not create rist receiver context\n");
				goto next;
			}
			peer_args.token = inputtoken;
			peer_args.ctx = callback_object[i].receiver_ctx;
			struct rist_peer *peer = setup_rist_peer(&peer_args);
			if (peer == NULL)
				atleast_one_socket_opened = true;
		}
		else {
			if(!evctx)
				evctx = evsocket_create();
			// This is a udp input, i.e. 127.0.0.1:5000
			char hostname[200] = {0};
			int inputlisten;
			uint16_t inputport;
			if (udpsocket_parse_url((void *)udp_config->address, hostname, 200, &inputport, &inputlisten) || !inputport || strlen(hostname) == 0) {
				rist_log(logging_settings, RIST_LOG_ERROR, "Could not parse input url %s\n", inputtoken);
				goto next;
			}
			rist_log(logging_settings, RIST_LOG_INFO, "URL parsed successfully: Host %s, Port %d\n", (char *) hostname, inputport);

			callback_object[i].sd = udpsocket_open_bind(hostname, inputport, udp_config->miface);
			if (callback_object[i].sd <= 0) {
				rist_log(logging_settings, RIST_LOG_ERROR, "Could not bind to: Host %s, Port %d, miface %s.\n",
					(char *) hostname, inputport, udp_config->miface);
				goto next;
			} else {
				udpsocket_set_nonblocking(callback_object[i].sd);
				rist_log(logging_settings, RIST_LOG_INFO, "Input socket is open and bound %s:%d\n", (char *) hostname, inputport);
				atleast_one_socket_opened = true;
			}
			callback_object[i].udp_config = udp_config;

			callback_object[i].evctx = evctx;
			event[i] = evsocket_addevent(callback_object[i].evctx, callback_object[i].sd, EVSOCKET_EV_READ, input_udp_recv, input_udp_sockerr, 
				(void *)&callback_object[i]);
		}

next:
		inputtoken = strtok_r(NULL, ",", &saveptrinput);
	}

	if (!atleast_one_socket_opened) {
		goto shutdown;
	}

	pthread_t thread_main_loop[MAX_INPUT_COUNT+1] = { 0 };

	if (evctx && pthread_create(&thread_main_loop[0], NULL, input_loop, (void *)callback_object) != 0)
	{
		fprintf(stderr, "Could not start udp receiver thread\n");
		goto shutdown;
	}

	for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
		if (callback_object[i].sender_ctx && rist_start(callback_object[i].sender_ctx) == -1) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Could not start rist sender\n");
			goto shutdown;
		}
		if (callback_object[i].receiver_ctx && rist_start(callback_object[i].receiver_ctx) == -1) {
			rist_log(logging_settings, RIST_LOG_ERROR, "Could not start rist receiver\n");
			goto shutdown;
		}
		if (callback_object[i].receiver_ctx && pthread_create(&thread_main_loop[i+1], NULL, input_loop, (void *)callback_object) != 0)
		{
			fprintf(stderr, "Could not start send rist receiver thread\n");
			goto shutdown;
		}
	}

#ifdef _WIN32
		system("pause");
#else
		pause();
#endif

shutdown:
	for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
		// Remove socket events
		if (event[i])
			evsocket_delevent(callback_object[i].evctx, event[i]);
		// Free udp_config object
		if ((void *)callback_object[i].udp_config)
			free((void *)(callback_object[i].udp_config));
		// Cleanup rist listeners
		if (callback_object[i].receiver_ctx)
			rist_destroy(callback_object[i].receiver_ctx);
		// Cleanup rist sender and their peers
		if (callback_object[i].sender_ctx)
			rist_destroy(callback_object[i].sender_ctx);
	}

	for (size_t i = 0; i <= MAX_INPUT_COUNT; i++) {
		if (thread_main_loop[i])
			pthread_join(thread_main_loop[i], NULL);
	}

	if (inputurl)
		free(inputurl);
	if (outputurl)
		free(outputurl);
	if (oobtun)
		free(oobtun);
	if (shared_secret)
		free(shared_secret);
	free(logging_settings);
	free(yaml_config);

	return 0;
}
