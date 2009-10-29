/* 
        
    *** uWSGI/mod_uwsgi ***

    Copyright 2009 Unbit S.a.s. <info@unbit.it>
        
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.


To compile:
(Linux)
	apxs2 -i -c mod_uwsgi.c
(OSX)
	sudo apxs -i -a -c mod_uwsgi.c
(OSX 64bit)
	sudo apxs -i -a -c -Wc,'-arch x86_64' -Wl,'-arch x86_64' mod_uwsgi.c


Configure:

LoadModule uwsgi_module <path_of_apache_modules>/mod_uwsgi.so
<Location XXX>
	SetHandler uwsgi-handler
</Location>

*/

#include "apr_strings.h"
#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <poll.h>

#define MAX_VARS 64
#define DEFAULT_SOCK "/tmp/uwsgi.sock"

typedef struct {
	union {
		struct sockaddr x_addr ;
		struct sockaddr_un u_addr ;
		struct sockaddr_in i_addr ;
	} s_addr;
	int addr_size;
	union {
		struct sockaddr x_addr ;
		struct sockaddr_un u_addr ;
		struct sockaddr_in i_addr ;
	} s_addr2;
	int addr_size2;
	int socket_timeout;
	char script_name[256];
} uwsgi_cfg;

module AP_MODULE_DECLARE_DATA uwsgi_module;

static int uwsgi_add_var(struct iovec *vec, int i, char *key, char *value, unsigned short *pkt_size) {

	vec[i].iov_base = &vec[i+1].iov_len ;
	vec[i].iov_len = 2 ;
	vec[i+1].iov_base = key ;
	vec[i+1].iov_len = strlen(key) ;
	vec[i+2].iov_base = &vec[i+3].iov_len ;
	vec[i+2].iov_len = 2 ;
	vec[i+3].iov_base = value ;
	vec[i+3].iov_len = strlen(value) ;

	*pkt_size+= vec[i+1].iov_len + vec[i+3].iov_len + 4 ;

	return i+4;
}

static void *uwsgi_server_config(apr_pool_t *p, server_rec *s) {

	uwsgi_cfg *c = (uwsgi_cfg *) apr_pcalloc(p, sizeof(uwsgi_cfg));
	strcpy(c->s_addr.u_addr.sun_path, DEFAULT_SOCK);
        c->s_addr.u_addr.sun_family = AF_UNIX;
	c->addr_size = strlen(DEFAULT_SOCK) + ( (void *)&c->s_addr.u_addr.sun_path - (void *)&c->s_addr ) ;
	c->socket_timeout = 0 ;

	return c;
}

static void *uwsgi_dir_config(apr_pool_t *p, char *dir) {

	uwsgi_cfg *c = (uwsgi_cfg *) apr_pcalloc(p, sizeof(uwsgi_cfg));
	strcpy(c->s_addr.u_addr.sun_path, DEFAULT_SOCK);
        c->s_addr.u_addr.sun_family = AF_UNIX;
        c->addr_size = strlen(DEFAULT_SOCK) + ( (void *)&c->s_addr.u_addr.sun_path - (void *)&c->s_addr ) ;
	c->socket_timeout = 0 ;

	return c;
}

static int uwsgi_handler(request_rec *r) {

	struct pollfd uwsgi_poll;

	uwsgi_cfg *c = ap_get_module_config(r->per_dir_config, &uwsgi_module);

	struct iovec uwsgi_vars[(MAX_VARS*4)+1] ;
	int vecptr = 1 ;
	char pkt_header[4];
	unsigned short pkt_size = 0;
	char buf[4096] ;
	int cnt,i ;
	const apr_array_header_t *headers;
	apr_table_entry_t *h;
	char *penv, *cp;

	apr_bucket_brigade *bb;

	if (strcmp(r->handler, "uwsgi-handler"))
        	return DECLINED;

	cnt = ap_setup_client_block(r, REQUEST_CHUNKED_ERROR);
	if (cnt != OK) {
		return cnt;
	}

	
	if (c == NULL) {
		c = ap_get_module_config(r->server->module_config, &uwsgi_module);
	}
	

	uwsgi_poll.fd = socket(c->s_addr.x_addr.sa_family, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: unable to create socket: %s", strerror(errno));
		return HTTP_INTERNAL_SERVER_ERROR;
	}


	if (connect(uwsgi_poll.fd, (struct sockaddr *) &c->s_addr, c->addr_size ) < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: connect to socket failed: %s", strerror(errno));

		close(uwsgi_poll.fd);

		if (c->addr_size2 > 0) {

			uwsgi_poll.fd = socket(c->s_addr2.x_addr.sa_family, SOCK_STREAM, 0);
			if (uwsgi_poll.fd < 0) {
				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: unable to create failover socket: %s", strerror(errno));
				return HTTP_INTERNAL_SERVER_ERROR;
			}

			if (connect(uwsgi_poll.fd, (struct sockaddr *) &c->s_addr2, c->addr_size2 ) < 0) {

				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: failover connect failed: %s", strerror(errno));

				close(uwsgi_poll.fd);

				return HTTP_INTERNAL_SERVER_ERROR;
			}
		}
		else {
			return HTTP_INTERNAL_SERVER_ERROR;
		}
	}


		
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "REQUEST_METHOD", (char *) r->method, &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "QUERY_STRING", r->args ? r->args : "", &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SERVER_NAME", (char *) ap_get_server_name(r), &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SERVER_PORT", apr_psprintf(r->pool, "%u",ap_get_server_port(r)), &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SERVER_PROTOCOL", r->protocol, &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "REQUEST_URI", r->uri, &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "REMOTE_ADDR", r->connection->remote_ip, &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "REMOTE_USER", r->user ? r->args : "", &pkt_size) ;
	vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "DOCUMENT_ROOT", (char *) ap_document_root(r), &pkt_size) ;

	if (c->script_name[0] == '/') {
		if (c->script_name[1] == 0) {
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SCRIPT_NAME", "", &pkt_size) ;
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "PATH_INFO", r->uri, &pkt_size) ;
		}
		else {
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SCRIPT_NAME", c->script_name, &pkt_size) ;
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "PATH_INFO", r->uri+strlen(c->script_name), &pkt_size) ;
		}
	}
	else {
		if (r->path_info) {
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SCRIPT_NAME", apr_pstrndup(r->pool, r->uri, (strlen(r->uri) - strlen(r->path_info) )) , &pkt_size) ;
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "PATH_INFO", r->path_info, &pkt_size) ;
		}
		else {
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "SCRIPT_NAME", "", &pkt_size) ;
			vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "PATH_INFO", r->uri, &pkt_size) ;
		}
	}

	headers = apr_table_elts(r->headers_in);
	h = (apr_table_entry_t *) headers->elts;

	// check for max vars (a bit ugly)
	cnt = headers->nelts ;
	if (cnt + 11 > MAX_VARS) {
		cnt = MAX_VARS -11;
	}

	for(i=0;i< cnt;i++) {
		if (h[i].key){
			if (!strcasecmp(h[i].key, "Content-Type")) {
				vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "CONTENT_TYPE", h[i].val, &pkt_size) ;
			}
			else if (!strcasecmp(h[i].key, "Content-Length")) {
				vecptr = uwsgi_add_var(uwsgi_vars, vecptr, "CONTENT_LENGTH", h[i].val, &pkt_size) ;
			}
			else {
				penv = apr_pstrcat(r->pool, "HTTP_", h[i].key, NULL);
				for(cp = penv+5; *cp !=0; cp++) {
					if (*cp == '-') {
						*cp = '_';
					}
					else {
						*cp = toupper(*cp);
					}
				}
				vecptr = uwsgi_add_var(uwsgi_vars, vecptr, penv, h[i].val, &pkt_size) ;
			}
		}
	}
	

	uwsgi_vars[0].iov_base = pkt_header;
	uwsgi_vars[0].iov_len = 4;

	pkt_header[0] = 0 ;
	memcpy(pkt_header+1, &pkt_size, 2);
	pkt_header[3] = 0 ;

	cnt = writev( uwsgi_poll.fd, uwsgi_vars, vecptr );
	if (cnt < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: writev() %s", strerror(errno));
		close(uwsgi_poll.fd);
		return HTTP_INTERNAL_SERVER_ERROR;
	}
	else if (cnt != pkt_size+4) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: writev() returned wrong size");
		close(uwsgi_poll.fd);
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	
	if (ap_should_client_block(r)) {
		while ((cnt = ap_get_client_block(r, buf, 4096)) > 0) {
			cnt = send( uwsgi_poll.fd, buf, cnt, 0);
			if (cnt < 0) {
				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: read() client block failed !");
				close(uwsgi_poll.fd);
				return HTTP_INTERNAL_SERVER_ERROR;
			}
		}
	}

	r->assbackwards = 1 ;

	bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);

	uwsgi_poll.events = POLLIN ;

	for(;;) {
		/* put -1 to disable timeout on zero */
		cnt = poll(&uwsgi_poll, 1, (c->socket_timeout*1000)-1) ;
		if (cnt == 0) {
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: recv() timeout");
			break;
		}
		else if (cnt > 0) {
			cnt = recv(uwsgi_poll.fd, buf, 4096, 0) ;
			if (cnt > 0) {
				apr_brigade_write(bb, NULL, NULL, buf, cnt);
			}
			else if (cnt == 0) {
				break;
			}
			else {
				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: recv() %s", strerror(errno));
			}
		}
		else {
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "uwsgi: poll() %s", strerror(errno));
			break;
		}
	}


	close(uwsgi_poll.fd);
	
	return ap_pass_brigade(r->output_filters, bb);;
}

static const char * cmd_uwsgi_force_script_name(cmd_parms *cmd, void *cfg, const char *location) {
	uwsgi_cfg *c = cfg;

	if (strlen(location) <= 255 && location[0] == '/') {
		strcpy(c->script_name, location);
	}
	else {
		return "ignored uWSGIforceScriptName. Invalid location" ;
	}

	return NULL ;

}

static const char * cmd_uwsgi_socket2(cmd_parms *cmd, void *cfg, const char *path) {

        uwsgi_cfg *c ;
	char *tcp_port;

        if (cfg) {
                c = cfg ;
        }
        else {
                c = ap_get_module_config(cmd->server->module_config, &uwsgi_module);
        }

	if (tcp_port = strchr(path, ':')) {
                c->addr_size2 = sizeof(struct sockaddr_in);
                c->s_addr2.i_addr.sin_family = AF_INET;
                c->s_addr2.i_addr.sin_port = htons(atoi(tcp_port+1));
                tcp_port[0] = 0;
                c->s_addr2.i_addr.sin_addr.s_addr = inet_addr(path);
        }
        else if (strlen(path) < 104) {
                strcpy(c->s_addr2.u_addr.sun_path, path);
                c->addr_size2 = strlen(path) + ( (void *)&c->s_addr.u_addr.sun_path - (void *)&c->s_addr ) ;
                // abstract namespace ??
                if (path[0] == '@') {
                        c->s_addr2.u_addr.sun_path[0] = 0 ;
                }
                c->s_addr2.u_addr.sun_family = AF_UNIX;
        }

        return NULL ;
}


static const char * cmd_uwsgi_socket(cmd_parms *cmd, void *cfg, const char *path, const char *timeout) {

	uwsgi_cfg *c ;
	char *tcp_port;

	if (cfg) {
		c = cfg ;
	}
	else {
		c = ap_get_module_config(cmd->server->module_config, &uwsgi_module);
	}

	if (tcp_port = strchr(path, ':')) {
		c->addr_size = sizeof(struct sockaddr_in);
		c->s_addr.i_addr.sin_family = AF_INET;
		c->s_addr.i_addr.sin_port = htons(atoi(tcp_port+1));
		tcp_port[0] = 0;
		c->s_addr.i_addr.sin_addr.s_addr = inet_addr(path);
	}
	else if (strlen(path) < 104) {
		strcpy(c->s_addr.u_addr.sun_path, path);
		c->addr_size = strlen(path) + ( (void *)&c->s_addr.u_addr.sun_path - (void *)&c->s_addr ) ;
		// abstract namespace ??
		if (path[0] == '@') {
			c->s_addr.u_addr.sun_path[0] = 0 ;
		}
		c->s_addr.u_addr.sun_family = AF_UNIX;
	}

	if (timeout) {
		c->socket_timeout = atoi(timeout);
	}


	return NULL ;
}

static const command_rec uwsgi_cmds[] = {
	AP_INIT_TAKE12("uWSGIsocket", cmd_uwsgi_socket, NULL, RSRC_CONF|ACCESS_CONF, "Absolute path and optional timeout in seconds of uwsgi server socket"),	
	AP_INIT_TAKE1("uWSGIsocket2", cmd_uwsgi_socket2, NULL, RSRC_CONF|ACCESS_CONF, "Absolute path of failover uwsgi server socket"),	
	AP_INIT_TAKE1("uWSGIforceScriptName", cmd_uwsgi_force_script_name, NULL, ACCESS_CONF, "Fix for PATH_INFO/SCRIPT_NAME when the location has filesystem correspondence"),	
	{NULL}
};

static void register_hooks(apr_pool_t *p) {
    ap_hook_handler(uwsgi_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA uwsgi_module = {

    STANDARD20_MODULE_STUFF,
    uwsgi_dir_config,
    NULL,
    uwsgi_server_config,
    NULL,
    uwsgi_cmds,
    register_hooks
};
