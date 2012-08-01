/*
 *  inet and unix socket functions for qemu
 *
 *  (c) 2008 Gerd Hoffmann <kraxel@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "qemu_socket.h"
#include "qemu-common.h" /* for qemu_isdigit */

#ifndef AI_ADDRCONFIG
# define AI_ADDRCONFIG 0
#endif

static const int on=1, off=0;

/* used temporarely until all users are converted to QemuOpts */
static QemuOptsList dummy_opts = {
    .name = "dummy",
    .head = QTAILQ_HEAD_INITIALIZER(dummy_opts.head),
    .desc = {
        {
            .name = "path",
            .type = QEMU_OPT_STRING,
        },{
            .name = "host",
            .type = QEMU_OPT_STRING,
        },{
            .name = "port",
            .type = QEMU_OPT_STRING,
        },{
            .name = "to",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "ipv4",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "ipv6",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "block",
            .type = QEMU_OPT_BOOL,
        },
        { /* end if list */ }
    },
};

static int inet_getport(struct addrinfo *e)
{
    struct sockaddr_in *i4;
    struct sockaddr_in6 *i6;

    switch (e->ai_family) {
    case PF_INET6:
        i6 = (void*)e->ai_addr;
        return ntohs(i6->sin6_port);
    case PF_INET:
        i4 = (void*)e->ai_addr;
        return ntohs(i4->sin_port);
    default:
        return 0;
    }
}

static void inet_setport(struct addrinfo *e, int port)
{
    struct sockaddr_in *i4;
    struct sockaddr_in6 *i6;

    switch (e->ai_family) {
    case PF_INET6:
        i6 = (void*)e->ai_addr;
        i6->sin6_port = htons(port);
        break;
    case PF_INET:
        i4 = (void*)e->ai_addr;
        i4->sin_port = htons(port);
        break;
    }
}

const char *inet_strfamily(int family)
{
    switch (family) {
    case PF_INET6: return "ipv6";
    case PF_INET:  return "ipv4";
    case PF_UNIX:  return "unix";
    }
    return "unknown";
}

int inet_listen_opts(QemuOpts *opts, int port_offset, Error **errp)
{
    struct addrinfo ai,*res,*e;
    const char *addr;
    char port[33];
    char uaddr[INET6_ADDRSTRLEN+1];
    char uport[33];
    int slisten, rc, to, port_min, port_max, p;

    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    ai.ai_family = PF_UNSPEC;
    ai.ai_socktype = SOCK_STREAM;

    if ((qemu_opt_get(opts, "host") == NULL) ||
        (qemu_opt_get(opts, "port") == NULL)) {
        fprintf(stderr, "%s: host and/or port not specified\n", __FUNCTION__);
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
        return -1;
    }
    pstrcpy(port, sizeof(port), qemu_opt_get(opts, "port"));
    addr = qemu_opt_get(opts, "host");

    to = qemu_opt_get_number(opts, "to", 0);
    if (qemu_opt_get_bool(opts, "ipv4", 0))
        ai.ai_family = PF_INET;
    if (qemu_opt_get_bool(opts, "ipv6", 0))
        ai.ai_family = PF_INET6;

    /* lookup */
    if (port_offset)
        snprintf(port, sizeof(port), "%d", atoi(port) + port_offset);
    rc = getaddrinfo(strlen(addr) ? addr : NULL, port, &ai, &res);
    if (rc != 0) {
        fprintf(stderr,"getaddrinfo(%s,%s): %s\n", addr, port,
                gai_strerror(rc));
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
        return -1;
    }

    /* create socket + bind */
    for (e = res; e != NULL; e = e->ai_next) {
        getnameinfo((struct sockaddr*)e->ai_addr,e->ai_addrlen,
		        uaddr,INET6_ADDRSTRLEN,uport,32,
		        NI_NUMERICHOST | NI_NUMERICSERV);
        slisten = qemu_socket(e->ai_family, e->ai_socktype, e->ai_protocol);
        if (slisten < 0) {
            fprintf(stderr,"%s: socket(%s): %s\n", __FUNCTION__,
                    inet_strfamily(e->ai_family), strerror(errno));
            if (!e->ai_next) {
                error_set(errp, QERR_SOCKET_CREATE_FAILED);
            }
            continue;
        }

        setsockopt(slisten,SOL_SOCKET,SO_REUSEADDR,(void*)&on,sizeof(on));
#ifdef IPV6_V6ONLY
        if (e->ai_family == PF_INET6) {
            /* listen on both ipv4 and ipv6 */
            setsockopt(slisten,IPPROTO_IPV6,IPV6_V6ONLY,(void*)&off,
                sizeof(off));
        }
#endif

        port_min = inet_getport(e);
        port_max = to ? to + port_offset : port_min;
        for (p = port_min; p <= port_max; p++) {
            inet_setport(e, p);
            if (bind(slisten, e->ai_addr, e->ai_addrlen) == 0) {
                goto listen;
            }
            if (p == port_max) {
                fprintf(stderr,"%s: bind(%s,%s,%d): %s\n", __FUNCTION__,
                        inet_strfamily(e->ai_family), uaddr, inet_getport(e),
                        strerror(errno));
                if (!e->ai_next) {
                    error_set(errp, QERR_SOCKET_BIND_FAILED);
                }
            }
        }
        closesocket(slisten);
    }
    fprintf(stderr, "%s: FAILED\n", __FUNCTION__);
    freeaddrinfo(res);
    return -1;

listen:
    if (listen(slisten,1) != 0) {
        error_set(errp, QERR_SOCKET_LISTEN_FAILED);
        perror("listen");
        closesocket(slisten);
        freeaddrinfo(res);
        return -1;
    }
    snprintf(uport, sizeof(uport), "%d", inet_getport(e) - port_offset);
    qemu_opt_set(opts, "host", uaddr);
    qemu_opt_set(opts, "port", uport);
    qemu_opt_set(opts, "ipv6", (e->ai_family == PF_INET6) ? "on" : "off");
    qemu_opt_set(opts, "ipv4", (e->ai_family != PF_INET6) ? "on" : "off");
    freeaddrinfo(res);
    return slisten;
}

int inet_connect_opts(QemuOpts *opts, bool *in_progress, Error **errp)
{
    struct addrinfo ai,*res,*e;
    const char *addr;
    const char *port;
    char uaddr[INET6_ADDRSTRLEN+1];
    char uport[33];
    int sock,rc;
    bool block;

    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_CANONNAME | AI_ADDRCONFIG;
    ai.ai_family = PF_UNSPEC;
    ai.ai_socktype = SOCK_STREAM;

    if (in_progress) {
        *in_progress = false;
    }

    addr = qemu_opt_get(opts, "host");
    port = qemu_opt_get(opts, "port");
    block = qemu_opt_get_bool(opts, "block", 0);
    if (addr == NULL || port == NULL) {
        fprintf(stderr, "inet_connect: host and/or port not specified\n");
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
        return -1;
    }

    if (qemu_opt_get_bool(opts, "ipv4", 0))
        ai.ai_family = PF_INET;
    if (qemu_opt_get_bool(opts, "ipv6", 0))
        ai.ai_family = PF_INET6;

    /* lookup */
    if (0 != (rc = getaddrinfo(addr, port, &ai, &res))) {
        fprintf(stderr,"getaddrinfo(%s,%s): %s\n", addr, port,
                gai_strerror(rc));
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
	return -1;
    }

    for (e = res; e != NULL; e = e->ai_next) {
        if (getnameinfo((struct sockaddr*)e->ai_addr,e->ai_addrlen,
                            uaddr,INET6_ADDRSTRLEN,uport,32,
                            NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
            fprintf(stderr,"%s: getnameinfo: oops\n", __FUNCTION__);
            continue;
        }
        sock = qemu_socket(e->ai_family, e->ai_socktype, e->ai_protocol);
        if (sock < 0) {
            fprintf(stderr,"%s: socket(%s): %s\n", __FUNCTION__,
            inet_strfamily(e->ai_family), strerror(errno));
            continue;
        }
        setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,(void*)&on,sizeof(on));
        if (!block) {
            socket_set_nonblock(sock);
        }
        /* connect to peer */
        do {
            rc = 0;
            if (connect(sock, e->ai_addr, e->ai_addrlen) < 0) {
                rc = -socket_error();
            }
        } while (rc == -EINTR);

  #ifdef _WIN32
        if (!block && (rc == -EINPROGRESS || rc == -EWOULDBLOCK
                       || rc == -WSAEALREADY)) {
  #else
        if (!block && (rc == -EINPROGRESS)) {
  #endif
            if (in_progress) {
                *in_progress = true;
            }

            error_set(errp, QERR_SOCKET_CONNECT_IN_PROGRESS);
        } else if (rc < 0) {
            if (NULL == e->ai_next)
                fprintf(stderr, "%s: connect(%s,%s,%s,%s): %s\n", __FUNCTION__,
                        inet_strfamily(e->ai_family),
                        e->ai_canonname, uaddr, uport, strerror(errno));
            closesocket(sock);
            continue;
        }
        freeaddrinfo(res);
        return sock;
    }
    error_set(errp, QERR_SOCKET_CONNECT_FAILED);
    freeaddrinfo(res);
    return -1;
}

int inet_dgram_opts(QemuOpts *opts)
{
    struct addrinfo ai, *peer = NULL, *local = NULL;
    const char *addr;
    const char *port;
    char uaddr[INET6_ADDRSTRLEN+1];
    char uport[33];
    int sock = -1, rc;

    /* lookup peer addr */
    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_CANONNAME | AI_ADDRCONFIG;
    ai.ai_family = PF_UNSPEC;
    ai.ai_socktype = SOCK_DGRAM;

    addr = qemu_opt_get(opts, "host");
    port = qemu_opt_get(opts, "port");
    if (addr == NULL || strlen(addr) == 0) {
        addr = "localhost";
    }
    if (port == NULL || strlen(port) == 0) {
        fprintf(stderr, "inet_dgram: port not specified\n");
        return -1;
    }

    if (qemu_opt_get_bool(opts, "ipv4", 0))
        ai.ai_family = PF_INET;
    if (qemu_opt_get_bool(opts, "ipv6", 0))
        ai.ai_family = PF_INET6;

    if (0 != (rc = getaddrinfo(addr, port, &ai, &peer))) {
        fprintf(stderr,"getaddrinfo(%s,%s): %s\n", addr, port,
                gai_strerror(rc));
	return -1;
    }

    /* lookup local addr */
    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_PASSIVE;
    ai.ai_family = peer->ai_family;
    ai.ai_socktype = SOCK_DGRAM;

    addr = qemu_opt_get(opts, "localaddr");
    port = qemu_opt_get(opts, "localport");
    if (addr == NULL || strlen(addr) == 0) {
        addr = NULL;
    }
    if (!port || strlen(port) == 0)
        port = "0";

    if (0 != (rc = getaddrinfo(addr, port, &ai, &local))) {
        fprintf(stderr,"getaddrinfo(%s,%s): %s\n", addr, port,
                gai_strerror(rc));
        return -1;
    }

    /* create socket */
    sock = qemu_socket(peer->ai_family, peer->ai_socktype, peer->ai_protocol);
    if (sock < 0) {
        fprintf(stderr,"%s: socket(%s): %s\n", __FUNCTION__,
                inet_strfamily(peer->ai_family), strerror(errno));
        goto err;
    }
    setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,(void*)&on,sizeof(on));

    /* bind socket */
    if (getnameinfo((struct sockaddr*)local->ai_addr,local->ai_addrlen,
                    uaddr,INET6_ADDRSTRLEN,uport,32,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        fprintf(stderr, "%s: getnameinfo: oops\n", __FUNCTION__);
        goto err;
    }
    if (bind(sock, local->ai_addr, local->ai_addrlen) < 0) {
        fprintf(stderr,"%s: bind(%s,%s,%d): OK\n", __FUNCTION__,
                inet_strfamily(local->ai_family), uaddr, inet_getport(local));
        goto err;
    }

    /* connect to peer */
    if (getnameinfo((struct sockaddr*)peer->ai_addr, peer->ai_addrlen,
                    uaddr, INET6_ADDRSTRLEN, uport, 32,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        fprintf(stderr, "%s: getnameinfo: oops\n", __FUNCTION__);
        goto err;
    }
    if (connect(sock,peer->ai_addr,peer->ai_addrlen) < 0) {
        fprintf(stderr, "%s: connect(%s,%s,%s,%s): %s\n", __FUNCTION__,
                inet_strfamily(peer->ai_family),
                peer->ai_canonname, uaddr, uport, strerror(errno));
        goto err;
    }

    freeaddrinfo(local);
    freeaddrinfo(peer);
    return sock;

err:
    if (-1 != sock)
        closesocket(sock);
    if (local)
        freeaddrinfo(local);
    if (peer)
        freeaddrinfo(peer);
    return -1;
}

/* compatibility wrapper */
static int inet_parse(QemuOpts *opts, const char *str)
{
    const char *optstr, *h;
    char addr[64];
    char port[33];
    int pos;

    /* parse address */
    if (str[0] == ':') {
        /* no host given */
        addr[0] = '\0';
        if (1 != sscanf(str,":%32[^,]%n",port,&pos)) {
            fprintf(stderr, "%s: portonly parse error (%s)\n",
                    __FUNCTION__, str);
            return -1;
        }
    } else if (str[0] == '[') {
        /* IPv6 addr */
        if (2 != sscanf(str,"[%64[^]]]:%32[^,]%n",addr,port,&pos)) {
            fprintf(stderr, "%s: ipv6 parse error (%s)\n",
                    __FUNCTION__, str);
            return -1;
        }
        qemu_opt_set(opts, "ipv6", "on");
    } else if (qemu_isdigit(str[0])) {
        /* IPv4 addr */
        if (2 != sscanf(str,"%64[0-9.]:%32[^,]%n",addr,port,&pos)) {
            fprintf(stderr, "%s: ipv4 parse error (%s)\n",
                    __FUNCTION__, str);
            return -1;
        }
        qemu_opt_set(opts, "ipv4", "on");
    } else {
        /* hostname */
        if (2 != sscanf(str,"%64[^:]:%32[^,]%n",addr,port,&pos)) {
            fprintf(stderr, "%s: hostname parse error (%s)\n",
                    __FUNCTION__, str);
            return -1;
        }
    }
    qemu_opt_set(opts, "host", addr);
    qemu_opt_set(opts, "port", port);

    /* parse options */
    optstr = str + pos;
    h = strstr(optstr, ",to=");
    if (h)
        qemu_opt_set(opts, "to", h+4);
    if (strstr(optstr, ",ipv4"))
        qemu_opt_set(opts, "ipv4", "on");
    if (strstr(optstr, ",ipv6"))
        qemu_opt_set(opts, "ipv6", "on");
    return 0;
}

int inet_listen(const char *str, char *ostr, int olen,
                int socktype, int port_offset, Error **errp)
{
    QemuOpts *opts;
    char *optstr;
    int sock = -1;

    opts = qemu_opts_create(&dummy_opts, NULL, 0, NULL);
    if (inet_parse(opts, str) == 0) {
        sock = inet_listen_opts(opts, port_offset, errp);
        if (sock != -1 && ostr) {
            optstr = strchr(str, ',');
            if (qemu_opt_get_bool(opts, "ipv6", 0)) {
                snprintf(ostr, olen, "[%s]:%s%s",
                         qemu_opt_get(opts, "host"),
                         qemu_opt_get(opts, "port"),
                         optstr ? optstr : "");
            } else {
                snprintf(ostr, olen, "%s:%s%s",
                         qemu_opt_get(opts, "host"),
                         qemu_opt_get(opts, "port"),
                         optstr ? optstr : "");
            }
        }
    } else {
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
    }
    qemu_opts_del(opts);
    return sock;
}

int inet_connect(const char *str, bool block, bool *in_progress, Error **errp)
{
    QemuOpts *opts;
    int sock = -1;

    opts = qemu_opts_create(&dummy_opts, NULL, 0, NULL);
    if (inet_parse(opts, str) == 0) {
        if (block) {
            qemu_opt_set(opts, "block", "on");
        }
        sock = inet_connect_opts(opts, in_progress, errp);
    } else {
        error_set(errp, QERR_SOCKET_CREATE_FAILED);
    }
    qemu_opts_del(opts);
    return sock;
}

#ifndef _WIN32

int unix_listen_opts(QemuOpts *opts)
{
    struct sockaddr_un un;
    const char *path = qemu_opt_get(opts, "path");
    int sock, fd;

    sock = qemu_socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket(unix)");
        return -1;
    }

    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    if (path && strlen(path)) {
        snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    } else {
        char *tmpdir = getenv("TMPDIR");
        snprintf(un.sun_path, sizeof(un.sun_path), "%s/qemu-socket-XXXXXX",
                 tmpdir ? tmpdir : "/tmp");
        /*
         * This dummy fd usage silences the mktemp() unsecure warning.
         * Using mkstemp() doesn't make things more secure here
         * though.  bind() complains about existing files, so we have
         * to unlink first and thus re-open the race window.  The
         * worst case possible is bind() failing, i.e. a DoS attack.
         */
        fd = mkstemp(un.sun_path); close(fd);
        qemu_opt_set(opts, "path", un.sun_path);
    }

    unlink(un.sun_path);
    if (bind(sock, (struct sockaddr*) &un, sizeof(un)) < 0) {
        fprintf(stderr, "bind(unix:%s): %s\n", un.sun_path, strerror(errno));
        goto err;
    }
    if (listen(sock, 1) < 0) {
        fprintf(stderr, "listen(unix:%s): %s\n", un.sun_path, strerror(errno));
        goto err;
    }

    return sock;

err:
    closesocket(sock);
    return -1;
}

int unix_connect_opts(QemuOpts *opts)
{
    struct sockaddr_un un;
    const char *path = qemu_opt_get(opts, "path");
    int sock;

    if (NULL == path) {
        fprintf(stderr, "unix connect: no path specified\n");
        return -1;
    }

    sock = qemu_socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket(unix)");
        return -1;
    }

    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    if (connect(sock, (struct sockaddr*) &un, sizeof(un)) < 0) {
        fprintf(stderr, "connect(unix:%s): %s\n", path, strerror(errno));
        close(sock);
	return -1;
    }

    return sock;
}

/* compatibility wrapper */
int unix_listen(const char *str, char *ostr, int olen)
{
    QemuOpts *opts;
    char *path, *optstr;
    int sock, len;

    opts = qemu_opts_create(&dummy_opts, NULL, 0, NULL);

    optstr = strchr(str, ',');
    if (optstr) {
        len = optstr - str;
        if (len) {
            path = g_malloc(len+1);
            snprintf(path, len+1, "%.*s", len, str);
            qemu_opt_set(opts, "path", path);
            g_free(path);
        }
    } else {
        qemu_opt_set(opts, "path", str);
    }

    sock = unix_listen_opts(opts);

    if (sock != -1 && ostr)
        snprintf(ostr, olen, "%s%s", qemu_opt_get(opts, "path"), optstr ? optstr : "");
    qemu_opts_del(opts);
    return sock;
}

int unix_connect(const char *path)
{
    QemuOpts *opts;
    int sock;

    opts = qemu_opts_create(&dummy_opts, NULL, 0, NULL);
    qemu_opt_set(opts, "path", path);
    sock = unix_connect_opts(opts);
    qemu_opts_del(opts);
    return sock;
}

#else

int unix_listen_opts(QemuOpts *opts)
{
    fprintf(stderr, "unix sockets are not available on windows\n");
    errno = ENOTSUP;
    return -1;
}

int unix_connect_opts(QemuOpts *opts)
{
    fprintf(stderr, "unix sockets are not available on windows\n");
    errno = ENOTSUP;
    return -1;
}

int unix_listen(const char *path, char *ostr, int olen)
{
    fprintf(stderr, "unix sockets are not available on windows\n");
    errno = ENOTSUP;
    return -1;
}

int unix_connect(const char *path)
{
    fprintf(stderr, "unix sockets are not available on windows\n");
    errno = ENOTSUP;
    return -1;
}

#endif

#ifdef _WIN32
static void socket_cleanup(void)
{
    WSACleanup();
}
#endif

int socket_init(void)
{
#ifdef _WIN32
    WSADATA Data;
    int ret, err;

    ret = WSAStartup(MAKEWORD(2,2), &Data);
    if (ret != 0) {
        err = WSAGetLastError();
        fprintf(stderr, "WSAStartup: %d\n", err);
        return -1;
    }
    atexit(socket_cleanup);
#endif
    return 0;
}
