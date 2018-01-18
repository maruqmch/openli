/*
 *
 * Copyright (c) 2018 The University of Waikato, Hamilton, New Zealand.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include <unistd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>

#include <libtrace.h>

#include "collector.h"
#include "collector_export.h"
#include "configparser.h"
#include "logger.h"

enum {
    EXP_EPOLL_MQUEUE = 0,
    EXP_EPOLL_TIMER = 1
};

typedef struct exporter_epoll {
    uint8_t type;
    union {
        libtrace_message_queue_t *q;
        export_dest_t *dest;
    } data;
} exporter_epoll_t;

collector_export_t *init_exporter(collector_global_t *glob) {

    collector_export_t *exp = (collector_export_t *)malloc(
            sizeof(collector_export_t));

    exp->glob = glob;
    exp->dests = libtrace_list_init(sizeof(export_dest_t));
    exp->glob->export_epollfd = epoll_create1(0);
    exp->failed_conns = 0;
    return exp;
}

static int connect_single_target(export_dest_t *dest) {

    struct addrinfo hints, *res;
    int sockfd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(dest->details.ipstr, dest->details.portstr, &hints,
                &res) == -1) {
        logger(LOG_DAEMON, "OpenLI: Error while trying to look up %s:%s as an export target -- %s.", dest->details.ipstr, dest->details.portstr, strerror(errno));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (sockfd == -1) {
        logger(LOG_DAEMON, "OpenLI: Error while creating export socket: %s.",
                strerror(errno));
        return -1;
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        if (!dest->failmsg) {
            logger(LOG_DAEMON, "OpenLI: Failed to connect to export target %s:%s -- %s.",
                    dest->details.ipstr, dest->details.portstr, strerror(errno));
            logger(LOG_DAEMON, "OpenLI: Will retry connection periodically.");
            dest->failmsg = 1;
        }

        close(sockfd);
        return -1;
    }

    logger(LOG_DAEMON, "OpenLI: connected to %s:%s successfully.",
            dest->details.ipstr, dest->details.portstr);
    dest->failmsg = 0;

    return sockfd;
}

int connect_export_targets(collector_export_t *exp) {

    int success = 0;
    libtrace_list_node_t *n;
    export_dest_t *d;

    exp->failed_conns = 0;

    n = exp->dests->head;

    while (n) {
        d = (export_dest_t *)n->data;

        if (d->fd != -1) {
            /* Already connected */
            n = n->next;
            success ++;
            continue;
        }

        d->fd = connect_single_target(d);
        if (d->fd != -1) {
            success ++;
        } else {
            exp->failed_conns ++;
        }
        n = n->next;
    }

    /* Return number of targets which we connected to */
    return success;

}

void destroy_exporter(collector_export_t *exp) {
    libtrace_list_node_t *n;
    export_dest_t *d;

    if (exp->glob->export_epollfd != -1) {
        close(exp->glob->export_epollfd);
    }

    /* Close all dest fds */
    n = exp->dests->head;
    while (n) {
        d = (export_dest_t *)n->data;
        if (d->fd != -1) {
            close(d->fd);
        }
        /* Don't free d->details, let the sync thread tidy this up */
        n = n->next;
    }

    libtrace_list_deinit(exp->dests);

    free(exp);
}

static int forward_message(export_dest_t *dest, openli_exportmsg_t *msg) {

    uint32_t enclen = msg->msglen - msg->ipclen;
    int ret;

    if (dest->fd == -1) {
        //dest->fd = connect_single_target(dest);

        if (dest->fd == -1) {
            /* TODO buffer this message for when we are able to connect */
            return 0;
        }
    }

    /* XXX probably be better to replace send()s with sendmmsg?? */

    /* TODO do non-blocking sends, buffer message if EAGAIN */

    ret = send(dest->fd, msg->msgbody, enclen, 0);
    if (ret == -1) {
        logger(LOG_DAEMON, "OpenLI: Error exporting to target %s:%s -- %s.",
                dest->details.ipstr, dest->details.portstr, strerror(errno));
        /* TODO buffer this message for when we are able to reconnect */
        return -1;
    }

    if (msg->ipclen > 0) {
        ret = send(dest->fd, msg->ipcontents, msg->ipclen, 0);
        if (ret == -1) {
            logger(LOG_DAEMON,
                    "OpenLI: Error exporting IP content to target %s:%s -- %s.",
                    dest->details.ipstr, dest->details.portstr,
                    strerror(errno));
            /* TODO buffer this message for when we are able to reconnect */
            return -1;
        }
    }

    return 0;
}

#define MAX_READ_BATCH 25

static int check_epoll_fd(collector_export_t *exp, struct epoll_event *ev) {

    libtrace_message_queue_t *srcq = NULL;
	openli_export_recv_t recvd;
    libtrace_list_node_t *n;
    export_dest_t *dest;
    exporter_epoll_t *epptr = NULL;
    int ret = 0;
    int readmsgs = 0;

    /* Got a message to export */
    if ((ev->events & EPOLLERR) || (ev->events & EPOLLHUP) ||
            !(ev->events & EPOLLIN)) {
        /* Something has gone wrong with a thread -> exporter message
         * queue. This is probably very bad, but we'll try to carry
         * on for now.
         */

        logger(LOG_DAEMON, "OpenLI: Thread lost connection to exporter?");
        return 0;
    }


    epptr = (exporter_epoll_t *)(ev->data.ptr);

    if (epptr->type == EXP_EPOLL_MQUEUE) {
        srcq = epptr->data.q;

        while (readmsgs < MAX_READ_BATCH) {

            if (libtrace_message_queue_try_get(srcq, (void *)(&recvd)) ==
                    LIBTRACE_MQ_FAILED) {
                break;
            }

            if (recvd.type == OPENLI_EXPORT_ETSIREC) {
                readmsgs ++;
                n = exp->dests->head;
                while (n) {
                    dest = (export_dest_t *)(n->data);

                    if (dest->details.destid == recvd.data.toexport.destid) {
                        ret = forward_message(dest, &(recvd.data.toexport));
                        if (ret == -1) {
                            close(dest->fd);
                            dest->fd = -1;
                            break;
                        }
                        break;
                    }
                    n = n->next;
                }

                if (n == NULL) {
                    logger(LOG_DAEMON, "Received a message for export to target %u, but no such target exists??", recvd.data.toexport.destid);
                    ret = -1;
                }
            }

            if (recvd.type == OPENLI_EXPORT_PACKET_FIN) {
                /* All ETSIRECs relating to this packet have been seen, so
                 * we can safely free the packet.
                 */
                trace_destroy_packet(recvd.data.packet);
            }
        }
    }

    if (epptr->type == EXP_EPOLL_TIMER) {
        if (ev->events & EPOLLIN) {
            return 1;
        }
        logger(LOG_DAEMON, "OpenLI: export thread timer has misbehaved.");
        return -1;
    }

    return ret;
}

int exporter_thread_main(collector_export_t *exp) {

	int i, nfds, timerfd;
	struct epoll_event evs[64];
	struct itimerspec its;
    struct epoll_event ev;
    int timerexpired = 0;
    exporter_epoll_t *epoll_ev = NULL;

    /* XXX this could probably be static, but just to be safe... */
    epoll_ev = (exporter_epoll_t *)malloc(sizeof(exporter_epoll_t));
    epoll_ev->type = EXP_EPOLL_TIMER;
    epoll_ev->data.q = NULL;

    ev.data.ptr = epoll_ev;
    ev.events = EPOLLIN | EPOLLET;

    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;

    timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    timerfd_settime(timerfd, 0, &its, NULL);

    if (epoll_ctl(exp->glob->export_epollfd, EPOLL_CTL_ADD, timerfd, &ev) == -1)
    {
        logger(LOG_DAEMON, "OpenLI: failed to add export timer fd to epoll set: %s.", strerror(errno));
        return -1;
    }

    /* Try to connect to any targets which we have buffered records for */
    connect_export_targets(exp);

    /* TODO */


    while (timerexpired == 0) {
    	nfds = epoll_wait(exp->glob->export_epollfd, evs, 64, -1);

        if (nfds < 0) {
            logger(LOG_DAEMON, "OpenLI: error while checking for messages to export: %s.", strerror(errno));
            return -1;
        }

        for (i = 0; i < nfds; i++) {
            timerexpired = check_epoll_fd(exp, &(evs[i]));
            if (timerexpired == -1) {
                break;
            }
        }
    }

    if (epoll_ctl(exp->glob->export_epollfd, EPOLL_CTL_DEL, timerfd, &ev) == -1)
    {
        logger(LOG_DAEMON, "OpenLI: failed to remove export timer fd to epoll set: %s.", strerror(errno));
        return -1;
    }

    free(epoll_ev);
    close(timerfd);
    return 1;

}

void register_export_queue(collector_global_t *glob,
        libtrace_message_queue_t *q) {

    struct epoll_event ev;
    exporter_epoll_t *epoll_ev = (exporter_epoll_t *)malloc(
            sizeof(exporter_epoll_t));

    epoll_ev->type = EXP_EPOLL_MQUEUE;
    epoll_ev->data.q = q;

    ev.data.ptr = (void *)epoll_ev;
    ev.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(glob->export_epollfd, EPOLL_CTL_ADD,
                libtrace_message_queue_get_fd(q), &ev) == -1) {
        /* TODO Do something? */
        logger(LOG_DAEMON, "OpenLI: failed to register export queue: %s",
                strerror(errno));
    }

}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :