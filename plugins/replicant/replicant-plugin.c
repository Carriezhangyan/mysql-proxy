/* Copyright (C) 2008 MySQL AB */ 
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h> /* FIONREAD */
#elif defined(WIN32)
#include <windows.h>
#include <winsock2.h>
#include <io.h>
#define ioctl ioctlsocket

#define STDERR_FILENO 2
#else
#include <unistd.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-binlog.h"
#include "network-mysqld-packet.h"
#include "sys-pedantic.h"
#include "glib-ext.h"

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

/**
 * we have two phases
 * - getting the binglog-pos with SHOW MASTER STATUS
 * - running the BINLOG_DUMP
 *
 * - split binlog stream into multiple streams based on
 *   lua-script and push the streams into the slaves
 *   - thread-ids
 *   - server-id
 *   - database name
 *   - table-names
 * - rewrite binlogs as delayed streams (listening port per delay)
 *
 * - chaining of replicants is desired
 *   a delayed replicator can feed a splitter or the other way around
 *
 * - we have to maintain the last know position per backend, perhaps 
 *   we want to maintain this in lua land and use the tbl2str functions
 *
 * - we may want to share the config
 *
 * - we have to parse the binlog stream and should also provide a 
 *   binlog reading library
 *
 *
 */
typedef struct {
	enum { REPCLIENT_BINLOG_GET_POS, REPCLIENT_BINLOG_DUMP } state;
	char *binlog_file;
	int binlog_pos;
} plugin_con_state;

struct chassis_plugin_config {
	gchar *master_address;                   /**< listening address of the proxy */

	gchar *mysqld_username;
	gchar *mysqld_password;

	gchar **read_binlogs;

	network_mysqld_con *listen_con;
};


static plugin_con_state *plugin_con_state_init() {
	plugin_con_state *st;

	st = g_new0(plugin_con_state, 1);

	return st;
}

static void plugin_con_state_free(plugin_con_state *st) {
	if (!st) return;

	if (st->binlog_file) g_free(st->binlog_file);

	g_free(st);
}

/**
 * decode the result-set of SHOW MASTER STATUS
 */
static int network_mysqld_resultset_master_status(chassis *UNUSED_PARAM(chas), network_mysqld_con *con) {
	GList *chunk;
	int i;
	network_socket *sock = con->client;
	plugin_con_state *st = con->plugin_con_state;
	GPtrArray *fields;

	/* scan the resultset */
	chunk = sock->send_queue->chunks->head;

	fields = network_mysqld_proto_fielddefs_new();
	chunk = network_mysqld_proto_get_fielddefs(chunk, fields);

	/* a data row */
	while (NULL != (chunk = chunk->next)) {
		network_packet packet;
		gint8 status;

		packet.data = chunk->data;
		packet.offset = 0;

		status = network_mysqld_proto_get_int8(&packet);

		/* if we find the 2nd EOF packet we are done */
		if (status == MYSQLD_PACKET_EOF &&
		    packet.data->len < 10) break;

		for (i = 0; i < fields->len; i++) {
			guint64 field_len = network_mysqld_proto_get_lenenc_int(&packet);

			if (i == 0) {
				g_assert(field_len > 0);
				/* Position */
				
				if (st->binlog_file) g_free(st->binlog_file);

				st->binlog_file = network_mysqld_proto_get_string_len(&packet, field_len);
			} else if (i == 1) {
				/* is a string */
				gchar *num;

				g_assert(field_len > 0);

				st->binlog_pos = 0;

				num = network_mysqld_proto_get_string_len(&packet, field_len);
				st->binlog_pos = g_ascii_strtoull(num, NULL, 10);
			} else {
				network_mysqld_proto_skip(&packet, field_len);
			}
		}

		g_message("reading binlog from: binlog-file: %s, binlog-pos: %d", 
				st->binlog_file, st->binlog_pos);
	}

	return 0;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_handshake) {
	network_packet packet;
	network_socket *recv_sock;
	network_socket *send_sock = NULL;
	chassis_plugin_config *config = con->config;
	network_mysqld_auth_challenge *shake;
	network_mysqld_auth_response  *auth;
	GString *auth_packet;
	
	recv_sock = con->server;
	send_sock = con->server;

	/* there should only be on packet */

	packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
	packet.offset = 0;

	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) {
		/**
		 * packet is too short, looks nasty.
		 *
		 * report an error and let the core send a error to the 
		 * client
		 */

		return NETWORK_SOCKET_ERROR;
	}

	shake = network_mysqld_auth_challenge_new();
	network_mysqld_proto_get_auth_challenge(&packet, shake);

	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

	recv_sock->packet_len = PACKET_LEN_UNSET;

	/* build the auth packet */
	auth_packet = g_string_new(NULL);

	auth = network_mysqld_auth_response_new();

	auth->capabilities = shake->capabilities;
	auth->charset      = shake->charset;

	if (config->mysqld_username) {
		g_string_append(auth->username, config->mysqld_username);
	}

	if (config->mysqld_password) {
		network_mysqld_proto_scramble(auth->response, shake->challenge, config->mysqld_password);
	}

	network_mysqld_proto_append_auth_response(auth_packet, auth);

	network_mysqld_queue_append(send_sock->send_queue, S(auth_packet), send_sock->packet_id + 1);

	network_mysqld_auth_response_free(auth);
	network_mysqld_auth_challenge_free(shake);

	con->state = CON_STATE_SEND_AUTH;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_auth_result) {
	GString *packet;
	GList *chunk;
	network_socket *recv_sock;
	network_socket *send_sock = NULL;

	const char query_packet[] = 
		"\x03"                    /* COM_QUERY */
		"SHOW MASTER STATUS"
		;

	recv_sock = con->server;

	chunk = recv_sock->recv_queue->chunks->tail;
	packet = chunk->data;

	/* we aren't finished yet */
	if (packet->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS;

	/* the auth should be fine */
	switch (packet->str[NET_HEADER_SIZE + 0]) {
	case MYSQLD_PACKET_ERR:
		g_assert(packet->str[NET_HEADER_SIZE + 3] == '#');

		g_critical("%s.%d: error: %d", 
				__FILE__, __LINE__,
				packet->str[NET_HEADER_SIZE + 1] | (packet->str[NET_HEADER_SIZE + 2] << 8));

		return NETWORK_SOCKET_ERROR;
	case MYSQLD_PACKET_OK: 
		break; 
	default:
		g_error("%s.%d: packet should be (OK|ERR), got: %02x",
				__FILE__, __LINE__,
				packet->str[NET_HEADER_SIZE + 0]);

		return NETWORK_SOCKET_ERROR;
	} 

	g_string_free(chunk->data, TRUE);
	recv_sock->packet_len = PACKET_LEN_UNSET;

	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);

	send_sock = con->server;
	network_mysqld_queue_append(send_sock->send_queue, query_packet, sizeof(query_packet) - 1, 0);

	con->state = CON_STATE_SEND_QUERY;

	return NETWORK_SOCKET_SUCCESS;
}

/**
 * inject a COM_BINLOG_DUMP after we have sent our SHOW MASTER STATUS
 */
NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_query_result) {
	/* let's send the
	 *
	 * ask the server for the current binlog-file|pos and dump everything from there
	 *  
	 * - COM_BINLOG_DUMP
	 *   - 4byte pos
	 *   - 2byte flags (BINLOG_DUMP_NON_BLOCK)
	 *   - 4byte slave-server-id
	 *   - nul-term binlog name
	 *
	 * we don't need:
	 * - COM_REGISTER_SLAVE
	 *   - 4byte server-id
	 *   - nul-term host
	 *   - nul-term user
	 *   - nul-term password
	 *   - 2byte port
	 *   - 4byte recovery rank
	 *   - 4byte master-id
	 */
	network_packet packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;
	int is_finished = 0;
	plugin_con_state *st = con->plugin_con_state;
	gint8 status;

	recv_sock = con->server;
	send_sock = con->client;

	chunk = recv_sock->recv_queue->chunks->tail;
	packet.data = chunk->data;
	packet.offset = 0;

	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS; /* packet isn't finished yet */
#if 0
	g_message("%s.%d: packet-len: %08x, packet-id: %d, command: COM_(%02x)", 
			__FILE__, __LINE__,
			recv_sock->packet_len,
			recv_sock->packet_id,
			con->parse.command
		);
#endif						

	is_finished = network_mysqld_proto_get_query_result(&packet, con);

	switch (con->parse.command) {
	case COM_BINLOG_DUMP:
		packet.offset = 0;

		network_mysqld_proto_skip_network_header(&packet);

		status = network_mysqld_proto_get_int8(&packet);

		switch (status) {
		case MYSQLD_PACKET_OK: {
			/* looks like the binlog dump started */
			network_mysqld_binlog *binlog;
			network_mysqld_binlog_event *event;

			binlog = network_mysqld_binlog_new();
			event = network_mysqld_binlog_event_new();

			network_mysqld_proto_skip_network_header(&packet);
			network_mysqld_proto_get_binlog_status(&packet);
			network_mysqld_proto_get_binlog_event_header(&packet, event);
			network_mysqld_proto_get_binlog_event(&packet, binlog, event);

			/* do something */

			network_mysqld_binlog_event_free(event);
			network_mysqld_binlog_free(binlog);

			break; }
		default:
			break;
		}
		break;
	default:
		break;
	}

	network_mysqld_queue_append(send_sock->send_queue, 
			packet.data->str + NET_HEADER_SIZE, 
			packet.data->len - NET_HEADER_SIZE, 
			recv_sock->packet_id);

	/* ... */
	if (is_finished) {
		/**
		 * the resultset handler might decide to trash the send-queue
		 * 
		 * */
		GString *query_packet;
		int my_server_id = 2;
		network_mysqld_binlog_dump *dump;
		GString *s_packet;

		switch (st->state) {
		case REPCLIENT_BINLOG_GET_POS:
			/* parse the result-set and get the 1st and 2nd column */

			network_mysqld_resultset_master_status(chas, con);

			/* remove all packets */
			while ((s_packet = g_queue_pop_head(send_sock->send_queue->chunks))) g_string_free(s_packet, TRUE);

			st->state = REPCLIENT_BINLOG_DUMP;

			dump = network_mysqld_binlog_dump_new();
			dump->binlog_pos  = st->binlog_pos;
			dump->server_id   = my_server_id;
			dump->binlog_file = g_strdup(st->binlog_file);

			query_packet = g_string_new(NULL);

			network_mysqld_proto_append_binlog_dump(query_packet, dump);
		       	
			send_sock = con->server;
			network_mysqld_queue_append(send_sock->send_queue, query_packet->str, query_packet->len, 0);

			network_mysqld_binlog_dump_free(dump);
		
			g_string_free(query_packet, TRUE);
		
			con->state = CON_STATE_SEND_QUERY;

			break;
		case REPCLIENT_BINLOG_DUMP:
			/* remove all packets */

			/* trash the packets for the injection query */
			while ((s_packet = g_queue_pop_head(send_sock->send_queue->chunks))) g_string_free(s_packet, TRUE);

			con->state = CON_STATE_READ_QUERY_RESULT;
			break;
		}
	}

	if (chunk->data) g_string_free(chunk->data, TRUE);
	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);

	recv_sock->packet_len = PACKET_LEN_UNSET;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_connect_server) {
	chassis_plugin_config *config = con->config;
	gchar *address = config->master_address;

	con->server = network_socket_init();

	if (0 != network_address_set_address(&(con->server->addr), address)) {
		return -1;
	}
    
	/* FIXME ... add non-blocking support (getsockopt()) */

	if (0 != network_socket_connect(con->server)) {
		return -1;
	}

	con->state = CON_STATE_SEND_HANDSHAKE;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_init) {
	g_assert(con->plugin_con_state == NULL);

	con->plugin_con_state = plugin_con_state_init();
	
	con->state = CON_STATE_CONNECT_SERVER;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_cleanup) {
	if (con->plugin_con_state == NULL) return NETWORK_SOCKET_SUCCESS;

	plugin_con_state_free(con->plugin_con_state);
	
	con->plugin_con_state = NULL;

	return NETWORK_SOCKET_SUCCESS;
}

int network_mysqld_repclient_connection_init(chassis G_GNUC_UNUSED *chas, network_mysqld_con *con) {
	con->plugins.con_init                      = repclient_init;
	con->plugins.con_connect_server            = repclient_connect_server;
	con->plugins.con_read_handshake            = repclient_read_handshake;
	con->plugins.con_read_auth_result          = repclient_read_auth_result;
	con->plugins.con_read_query_result         = repclient_read_query_result;
	con->plugins.con_cleanup                   = repclient_cleanup;

	return 0;
}

chassis_plugin_config * network_mysqld_replicant_plugin_init(void) {
	chassis_plugin_config *config;

	config = g_new0(chassis_plugin_config, 1);

	return config;
}

void network_mysqld_replicant_plugin_free(chassis_plugin_config *config) {
	if (config->listen_con) {
		/**
		 * the connection will be free()ed by the network_mysqld_free()
		 */
#if 0
		event_del(&(config->listen_con->server->event));
		network_mysqld_con_free(config->listen_con);
#endif
	}

	if (config->master_address) {
		/* free the global scope */
		g_free(config->master_address);
	}

	if (config->mysqld_username) g_free(config->mysqld_username);
	if (config->mysqld_password) g_free(config->mysqld_password);
	if (config->read_binlogs) g_strfreev(config->read_binlogs);

	g_free(config);
}

/**
 * plugin options 
 */
static GOptionEntry * network_mysqld_replicant_plugin_get_options(chassis_plugin_config *config) {
	guint i;

	/* make sure it isn't collected */
	static GOptionEntry config_entries[] = 
	{
		{ "replicant-master-address",            0, 0, G_OPTION_ARG_STRING, NULL, "... (default: :4040)", "<host:port>" },
		{ "replicant-username",                  0, 0, G_OPTION_ARG_STRING, NULL, "username", "" },
		{ "replicant-password",                  0, 0, G_OPTION_ARG_STRING, NULL, "password", "" },
		{ "replicant-read-binlogs",              0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, "binlog files", "" },
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	i = 0;
	config_entries[i++].arg_data = &(config->master_address);
	config_entries[i++].arg_data = &(config->mysqld_username);
	config_entries[i++].arg_data = &(config->mysqld_password);
	config_entries[i++].arg_data = &(config->read_binlogs);
	
	return config_entries;
}

#if 1 
static void dump_str(const char *msg, const unsigned char *s, size_t len) {
	GString *hex;
	size_t i;
		
       	hex = g_string_new(NULL);

	for (i = 0; i < len; i++) {
		g_string_append_printf(hex, "%02x", s[i]);

		if ((i + 1) % 16 == 0) {
			int j;
			g_string_append(hex, "  ");
			for (j = i - 15; j <= i; j++) {
				g_string_append_c(hex, g_ascii_isprint(s[j]) ? s[j] : '.');
			}
			g_string_append(hex, "\n  ");
		} else {
			g_string_append_c(hex, ' ');
		}
	}

	if (i % 16 != 0) {
		/* fill up the line */
		int j;

		for (j = 0; j < 16 - (i % 16); j++) {
			g_string_append(hex, "   ");
		}

		g_string_append(hex, " ");
		for (j = i - (len % 16); j <= i; j++) {
			g_string_append_c(hex, g_ascii_isprint(s[j]) ? s[j] : '.');
		}
	}

	g_message("(%s):\n  %s", msg, hex->str);

	g_string_free(hex, TRUE);
}
#endif

int network_mysqld_binlog_event_print(network_mysqld_binlog_event *event) {
	guint i;
	int metadata_offset = 0;
#if 1
	g_message("%s: timestamp = %u, type = %u, server-id = %u, size = %u, pos = %u, flags = %04x",
			G_STRLOC,
			event->timestamp,
			event->event_type,
			event->server_id,
			event->event_size,
			event->log_pos,
			event->flags);
#endif

	switch (event->event_type) {
	case QUERY_EVENT: /* 2 */
#if 1
		g_message("%s: QUERY: thread_id = %d, exec_time = %d, error-code = %d\ndb = %s, query = %s",
				G_STRLOC,
				event->event.query_event.thread_id,
				event->event.query_event.exec_time,
				event->event.query_event.error_code,
				event->event.query_event.db_name ? event->event.query_event.db_name : "(null)",
				event->event.query_event.query ? event->event.query_event.query : "(null)"
			 );
#endif
		break;
	case STOP_EVENT:
		break;
	case TABLE_MAP_EVENT:
		g_message("%s: (table-definition) table-id = %"G_GUINT64_FORMAT", flags = %04x, db = %s, table = %s",
				G_STRLOC,
				event->event.table_map_event.table_id,
				event->event.table_map_event.flags,
				event->event.table_map_event.db_name ? event->event.table_map_event.db_name : "(null)",
				event->event.table_map_event.table_name ? event->event.table_map_event.table_name : "(null)"
			 );
		g_message("%s: (table-definition) columns = %d",
				G_STRLOC,
				event->event.table_map_event.columns_len
			 );

		/* the metadata is field specific */
		for (i = 0; i < event->event.table_map_event.columns_len; i++) {
			MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();
			enum enum_field_types col_type;

			col_type = event->event.table_map_event.columns[i];

			/* the meta-data depends on the type,
			 *
			 * string has 2 byte field-length
			 * floats have precision
			 * ints have display length
			 * */
			switch (col_type) {
			case MYSQL_TYPE_STRING: /* 254 */
				/* byte 0: real_type 
				 * byte 1: field-length
				 */
				field->type = event->event.table_map_event.metadata[metadata_offset + 0];
				field->length = event->event.table_map_event.metadata[metadata_offset + 1];
				metadata_offset += 2;
				break;
			case MYSQL_TYPE_VAR_STRING:
				/* 2 byte length (int2store)
				 */
				field->type = col_type;
				field->length = 
					((guchar)event->event.table_map_event.metadata[metadata_offset + 0]) |
					((guchar)event->event.table_map_event.metadata[metadata_offset + 1]) << 8;
				metadata_offset += 2;
				break;
			case MYSQL_TYPE_BLOB: /* 252 */
				field->type = col_type;
				metadata_offset += 1; /* the packlength (1 .. 4) */
				break;
			case MYSQL_TYPE_DECIMAL:
				field->type = col_type;
				metadata_offset += 2;
				/**
				 * byte 0: precisions
				 * byte 1: decimals
				 */
				break;
			case MYSQL_TYPE_DOUBLE:
			case MYSQL_TYPE_FLOAT:
				field->type = col_type;
				/* pack-length */
				metadata_offset += 1;
				break;
			case MYSQL_TYPE_ENUM:
				/* real-type (ENUM|SET)
				 * pack-length
				 */
				field->type = event->event.table_map_event.metadata[metadata_offset + 0];
				metadata_offset += 2;
				break;
			case MYSQL_TYPE_BIT:
				metadata_offset += 2;
				break;
			default:
				field->type = col_type;
				metadata_offset += 0;
				break;
			}

			g_message("%s: (column-definition) [%d] type = %d, length = %lu",
					G_STRLOC,
					i,
					field->type,
					field->length
				 );

			network_mysqld_proto_fielddef_free(field);
		}
		break;
	case FORMAT_DESCRIPTION_EVENT: /* 15 */
		break;
	case INTVAR_EVENT: /* 5 */
	 	break;
	case XID_EVENT: /* 16 */
		break;
	case ROTATE_EVENT: /* 4 */
		break;
	default:
		g_message("%s: unknown event-type: %d",
				G_STRLOC,
				event->event_type);
		return -1;
	}
	return 0;
}

int replicate_binlog_dump_file(const char *filename) {
	int fd;
	char binlog_header[4];
	network_packet *packet;
	network_mysqld_binlog *binlog;
	network_mysqld_binlog_event *event;

	if (-1 == (fd = g_open(filename, O_RDONLY, 0))) {
		g_critical("%s: opening '%s' failed: %s",
				G_STRLOC,
				filename,
				g_strerror(errno));
		return -1;
	}

	if (4 != read(fd, binlog_header, 4)) {
		g_return_val_if_reached(-1);
	}

	if (binlog_header[0] != '\xfe' ||
	    binlog_header[1] != 'b' ||
	    binlog_header[2] != 'i' ||
	    binlog_header[3] != 'n') {

		g_critical("%s: binlog-header should be: %02x%02x%02x%02x, got %02x%02x%02x%02x",
				G_STRLOC,
				'\xfe', 'b', 'i', 'n',
				binlog_header[0],
				binlog_header[1],
				binlog_header[2],
				binlog_header[3]
				);

		g_return_val_if_reached(-1);
	}

	packet = network_packet_new();
	packet->data = g_string_new(NULL);
	g_string_set_size(packet->data, 19 + 1);

	binlog = network_mysqld_binlog_new();

	/* next are the events, without the mysql packet header */
	while (19 == (packet->data->len = read(fd, packet->data->str, 19))) {
		gssize len;
		packet->data->str[packet->data->len] = '\0'; /* term the string */

		g_assert_cmpint(packet->data->len, ==, 19);

		event = network_mysqld_binlog_event_new();
		network_mysqld_proto_get_binlog_event_header(packet, event);

		g_assert_cmpint(event->event_size, >=, 19);

		g_string_set_size(packet->data, event->event_size); /* resize the string */
		packet->data->len = 19;

		len = read(fd, packet->data->str + 19, event->event_size - 19);

		if (-1 == len) {
			g_critical("%s: lseek(..., %d, ...) failed: %s",
					G_STRLOC,
					event->event_size - 19,
					g_strerror(errno));
			return -1;
		}
		g_assert_cmpint(len, ==, event->event_size - 19);
		g_assert_cmpint(packet->data->len, ==, 19);
		packet->data->len += len;
		g_assert_cmpint(packet->data->len, ==, event->event_size);
		
		if (network_mysqld_proto_get_binlog_event(packet, binlog, event)) {
			dump_str(G_STRLOC, packet->data->str + 19, packet->data->len - 19);
		} else if (network_mysqld_binlog_event_print(event)) {
			/* ignore it */
		}
	
		network_mysqld_binlog_event_free(event);

		packet->offset = 0;
	}
	g_string_free(packet->data, TRUE);
	network_packet_free(packet);

	network_mysqld_binlog_free(binlog);

	close(fd);

	return 0;
}

/**
 * init the plugin with the parsed config
 */
int network_mysqld_replicant_plugin_apply_config(chassis G_GNUC_UNUSED *chas, chassis_plugin_config *config) {
	if (!config->master_address) config->master_address = g_strdup(":4040");
	if (!config->mysqld_username) config->mysqld_username = g_strdup("repl");
	if (!config->mysqld_password) config->mysqld_password = g_strdup("");

	if (config->read_binlogs) {
		int i;

		/* we have a list of filenames we shall decode */
		for (i = 0; config->read_binlogs[i]; i++) {
			char *filename = config->read_binlogs[i];

			replicate_binlog_dump_file(filename);
		}

		/* we are done, shutdown */
		chassis_set_shutdown();
	}

	return 0;
}

G_MODULE_EXPORT int plugin_init(chassis_plugin *p) {
	p->magic        = CHASSIS_PLUGIN_MAGIC;
	p->name         = g_strdup("replicant");
	p->version		= g_strdup("0.7.0");
	/* append the our init function to the init-hook-list */

	p->init         = network_mysqld_replicant_plugin_init;
	p->get_options  = network_mysqld_replicant_plugin_get_options;
	p->apply_config = network_mysqld_replicant_plugin_apply_config;
	p->destroy      = network_mysqld_replicant_plugin_free;

	return 0;
}
