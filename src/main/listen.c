/*
 * socket.c	Handle socket stuff
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2005  The FreeRADIUS server project
 * Copyright 2005  Alan DeKok <aland@ox.org>
 */

#include "autoconf.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "radiusd.h"
#include "rad_assert.h"
#include "conffile.h"
#include "token.h"

#include <sys/resource.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef WITH_UDPFROMTO
#include "udpfromto.h"
#endif

#include "radius_snmp.h"
#include "request_list.h"

static time_t start_time = 0;
static int last_proxy_port = 0;

/*
 *	FIXME: Delete this crap!
 */
extern time_t time_now;
extern int auth_port;

static uint32_t server_ip = 0; /* INADDR_ANY */

/*
 *	Process and reply to a server-status request.
 *	Like rad_authenticate and rad_accounting this should
 *	live in it's own file but it's so small we don't bother.
 */
static int rad_status_server(REQUEST *request)
{
	char		reply_msg[64];
	time_t		t;
	VALUE_PAIR	*vp;

	/*
	 *	Reply with an ACK. We might want to add some more
	 *	interesting reply attributes, such as server uptime.
	 */
	t = request->timestamp - start_time;
	sprintf(reply_msg, "FreeRADIUS up %d day%s, %02d:%02d",
		(int)(t / 86400), (t / 86400) == 1 ? "" : "s",
		(int)((t / 3600) % 24), (int)(t / 60) % 60);
	request->reply->code = PW_AUTHENTICATION_ACK;

	vp = pairmake("Reply-Message", reply_msg, T_OP_SET);
	pairadd(&request->reply->vps, vp); /* don't need to check if !vp */

	return 0;
}

/*
 *	Check for dups, etc.  Common to Access-Request &&
 *	Accounting-Request packets.
 */
static int common_checks(rad_listen_t *listener,
			 RADIUS_PACKET *packet, REQUEST **prequest,
			 const uint8_t *secret)
{
	REQUEST	*curreq;
	static int request_num_counter = 0;
	char buffer[128];

	/*
	 *	If there is no existing request of id, code, etc.,
	 *	then we can return, and let it be processed.
	 */
	if ((curreq = rl_find(packet)) == NULL) {
		/*
		 *	Count the total number of requests, to see if
		 *	there are too many.  If so, return with an
		 *	error.
		 */
		if (mainconfig.max_requests) {
			int request_count = rl_num_requests();

			/*
			 *	This is a new request.  Let's see if
			 *	it makes us go over our configured
			 *	bounds.
			 */
			if (request_count > mainconfig.max_requests) {
				radlog(L_ERR, "Dropping request (%d is too many): "
				       "from client %s port %d - ID: %d", request_count,
				       client_name(&packet->src_ipaddr),
				       packet->src_port, packet->id);
				radlog(L_INFO, "WARNING: Please check the radiusd.conf file.\n"
				       "\tThe value for 'max_requests' is probably set too low.\n");
				return 0;
			} /* else there were a small number of requests */
		} /* else there was no configured limit for requests */

		/*
		 *	FIXME: Add checks for system load.  If the
		 *	system is busy, start dropping requests...
		 *
		 *	We can probably keep some statistics
		 *	ourselves...  if there are more requests
		 *	coming in than we can handle, start dropping
		 *	some.
		 */

	/*
	 *	The current request isn't finished, which
	 *	means that the NAS sent us a new packet, while
	 *	we are still processing the old request.
	 */
	} else if (!curreq->finished) {
		/*
		 *	If the authentication vectors are identical,
		 *	then the NAS is re-transmitting it, trying to
		 *	kick us into responding to the request.
		 */
		if (memcmp(curreq->packet->vector, packet->vector,
			   sizeof(packet->vector)) == 0) {
			RAD_SNMP_INC(rad_snmp.auth.total_dup_requests);

			/*
			 *	It's not finished because the request
			 *	was proxied, but there was no reply
			 *	from the home server.
			 */
			if (curreq->proxy && !curreq->proxy_reply) {
				/*
				 *	We're taking care of sending
				 *	duplicate proxied packets, so
				 *	we ignore any duplicate
				 *	requests from the NAS.
				 *
				 *	FIXME: Make it ALWAYS synchronous!
				 */
				if (!mainconfig.proxy_synchronous) {
					RAD_SNMP_TYPE_INC(listener, total_packets_dropped);

					DEBUG2("Ignoring duplicate packet from client "
					       "%s port %d - ID: %d, due to outstanding proxied request %d.",
					       client_name(&packet->src_ipaddr),
					       packet->src_port, packet->id,
					       curreq->number);
					return 0;

					/*
					 *	We ARE proxying the request,
					 *	and we have NOT received a
					 *	proxy reply yet, and we ARE
					 *	doing synchronous proxying.
					 *
					 *	In that case, go kick
					 *	the home RADIUS server
					 *	again.
					 */
				} else {
					DEBUG2("Sending duplicate proxied request to home server %s port %d - ID: %d",
					       inet_ntop(curreq->proxy->dst_ipaddr.af,
							 &curreq->proxy->dst_ipaddr.ipaddr,
							 buffer, sizeof(buffer)),					       curreq->proxy->dst_port,

					       curreq->proxy->id);
				}
				curreq->proxy_next_try = time_now + mainconfig.proxy_retry_delay;
				rad_send(curreq->proxy, curreq->packet,
					 curreq->proxysecret);
				return 0;
			} /* else the packet was not proxied */

			/*
			 *	Someone's still working on it, so we
			 *	ignore the duplicate request.
			 */
			radlog(L_ERR, "Discarding duplicate request from "
			       "client %s port %d - ID: %d due to unfinished request %d",
			       client_name(&packet->src_ipaddr),
			       packet->src_port, packet->id,
			       curreq->number);
			return 0;
		} /* else the authentication vectors were different */

		/*
		 *	The authentication vectors are different, so
		 *	the NAS has given up on us, as we've taken too
		 *	long to process the request.  This is a
		 *	SERIOUS problem!
		 */
		RAD_SNMP_TYPE_INC(listener, total_packets_dropped);

		radlog(L_ERR, "Dropping conflicting packet from "
		       "client %s port %d - ID: %d due to unfinished request %d",
		       client_name(&packet->src_ipaddr),
		       packet->src_port, packet->id,
		       curreq->number);
		return 0;
		
		/*
		 *	The old request is finished.  We now check the
		 *	authentication vectors.  If the client has sent us a
		 *	request with identical code && ID, but different
		 *	vector, then they MUST have gotten our response, so we
		 *	can delete the original request, and process the new
		 *	one.
		 *
		 *	If the vectors are the same, then it's a duplicate
		 *	request, and we can send a duplicate reply.
		 */
	} else if (memcmp(curreq->packet->vector, packet->vector,
			  sizeof(packet->vector)) == 0) {
		RAD_SNMP_INC(rad_snmp.auth.total_dup_requests);

		/*
		 *	If the packet has been delayed, then silently
		 *	send a response, and clear the delayed flag.
		 *
		 *	Note that this means if the NAS kicks us while
		 *	we're delaying a reject, then the reject may
		 *	be sent sooner than otherwise.
		 *
		 *	This COULD be construed as a bug.  Maybe what
		 *	we want to do is to ignore the duplicate
		 *	packet, and send the reject later.
		 */
		if (curreq->options & RAD_REQUEST_OPTION_DELAYED_REJECT) {
			curreq->options &= ~RAD_REQUEST_OPTION_DELAYED_REJECT;
			rad_send(curreq->reply, curreq->packet, curreq->secret);
			return 0;
		}

		/*
		 *	Maybe we've saved a reply packet.  If so,
		 *	re-send it.  Otherwise, just complain.
		 */
		if (curreq->reply->code != 0) {
			DEBUG2("Sending duplicate reply "
			       "to client %s port %d - ID: %d",
			       client_name(&packet->src_ipaddr),
			       packet->src_port, packet->id);
			rad_send(curreq->reply, curreq->packet, curreq->secret);
			return 0;
		}

		/*
		 *	Else we never sent a reply to the NAS,
		 *	as we decided somehow we didn't like the request.
		 *
		 *	This shouldn't happen, in general...
		 */
		DEBUG2("Discarding duplicate request from client %s port %d - ID: %d",
		       client_name(&packet->src_ipaddr),
		       packet->src_port, packet->id);
		return 0;
	} /* else the vectors were different, so we discard the old request. */

	/*
	 *	'packet' has the same source IP, source port, code,
	 *	and Id as 'curreq', but a different authentication
	 *	vector.  We can therefore delete 'curreq', as we were
	 *	only keeping it around to send out duplicate replies,
	 *	if the first reply got lost in the network.
	 */
	if (curreq) rl_delete(curreq);

	/*
	 *	A unique per-request counter.
	 */
	
	curreq = request_alloc(); /* never fails */
	curreq->packet = packet;
	curreq->number = request_num_counter++;
	strNcpy(curreq->secret, secret,
		sizeof(curreq->secret));
	
	/*
	 *	Remember the request in the list.
	 */
	rl_add(curreq);
	
	/*
	 *	ADD IN "server identifier" from "listen"
	 *	directive!
	 */
	
	/*
	 *	The request passes many of our sanity checks.
	 *	From here on in, if anything goes wrong, we
	 *	send a reject message, instead of dropping the
	 *	packet.
	 *
	 *	Build the reply template from the request
	 *	template.
		 */
	rad_assert(curreq->reply == NULL);
	if ((curreq->reply = rad_alloc(0)) == NULL) {
		radlog(L_ERR, "No memory");
		exit(1);
	}

	curreq->reply->sockfd = curreq->packet->sockfd;
	curreq->reply->dst_ipaddr = curreq->packet->src_ipaddr;
	curreq->reply->src_ipaddr = curreq->packet->dst_ipaddr;
	curreq->reply->dst_port = curreq->packet->src_port;
	curreq->reply->src_port = curreq->packet->dst_port;
	curreq->reply->id = curreq->packet->id;
	curreq->reply->code = 0; /* UNKNOWN code */
	memcpy(curreq->reply->vector, curreq->packet->vector,
	       sizeof(curreq->reply->vector));
	curreq->reply->vps = NULL;
	curreq->reply->data = NULL;
	curreq->reply->data_len = 0;

	*prequest = curreq;
	return 1;
}


/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
  */
static int auth_socket_recv(rad_listen_t *listener,
			    RAD_REQUEST_FUNP *pfun, REQUEST **prequest)
{
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	char		buffer[128];
	RADCLIENT	*cl;

	packet = rad_recv(listener->fd);
	if (!packet) {
		radlog(L_ERR, "%s", librad_errstr);
		return 0;
	}

	RAD_SNMP_TYPE_INC(listener, total_requests); /* FIXME: auth specific */

	if ((cl = client_find(&packet->src_ipaddr)) == NULL) {
		RAD_SNMP_TYPE_INC(listener, total_invalid_requests);
		
		radlog(L_ERR, "Ignoring request from unknown client %s port %d",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
		       packet->src_port);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	Some sanity checks, based on the packet code.
	 */
	switch(packet->code) {
	case PW_AUTHENTICATION_REQUEST:
		fun = rad_authenticate;
		break;
		
	case PW_STATUS_SERVER:
		if (!mainconfig.status_server) {
			DEBUG("WARNING: Ignoring Status-Server request due to security configuration");
			rad_free(&packet);
			return 0;
		}
		fun = rad_status_server;
		break;

	default:
		RAD_SNMP_INC(rad_snmp.auth.total_unknown_types);
		
		radlog(L_ERR, "Invalid packet code %d sent to authentication port from client %s port %d "
		       "- ID %d : IGNORED", packet->code,
		       client_name(&packet->src_ipaddr),
		       packet->src_port, packet->id);
		rad_free(&packet);
		return 0;
		break;
	} /* switch over packet types */
	
	if (!common_checks(listener, packet, prequest, cl->secret)) {
		rad_free(&packet);
		return 0;
	}

	*pfun = fun;
	return 1;
}


/*
 *	Receive packets from an accounting socket
 */
static int acct_socket_recv(rad_listen_t *listener,
	RAD_REQUEST_FUNP *pfun, REQUEST **prequest)
{
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	char		buffer[128];
	RADCLIENT	*cl;
	
	packet = rad_recv(listener->fd);
	if (!packet) {
		radlog(L_ERR, "%s", librad_errstr);
		return 0;
	}
	
	RAD_SNMP_TYPE_INC(listener, total_requests); /* FIXME: acct-specific */

	if ((cl = client_find(&packet->src_ipaddr)) == NULL) {
		RAD_SNMP_TYPE_INC(listener, total_invalid_requests);
		
		radlog(L_ERR, "Ignoring request from unknown client %s port %d",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
		       packet->src_port);
		rad_free(&packet);
		return 0;
	}

	switch(packet->code) {
	case PW_ACCOUNTING_REQUEST:
		fun = rad_accounting;
		break;
		
	default:
		/*
		 *	FIXME: Update MIB for packet types?
		 */
		radlog(L_ERR, "Invalid packet code %d sent to a accounting port "
		       "from client %s port %d - ID %d : IGNORED",
		       packet->code,
		       client_name(&packet->src_ipaddr),
		       packet->src_port, packet->id);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	FIXME: Accounting duplicates should be handled
	 *	differently than authentication duplicates.
	 */
	if (!common_checks(listener, packet, prequest, cl->secret)) {
		rad_free(&packet);
		return 0;
	}

	*pfun = fun;
	return 1;
}


/*
 *	Recieve packets from a proxy socket.
 */
static int proxy_socket_recv(rad_listen_t *listener,
			      RAD_REQUEST_FUNP *pfun, REQUEST **prequest)
{
	REALM		*cl;
	REQUEST		*oldreq;
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	char		buffer[128];
	
	packet = rad_recv(listener->fd);
	if (!packet) {
		radlog(L_ERR, "%s", librad_errstr);
		return 0;
	}

	/*
	 *	Unsupported stuff
	 */
	if (packet->src_ipaddr.af != AF_INET) {
		rad_assert("PROXY IPV6 NOT SUPPORTED" == NULL);
	}
	
	/*
	 *	FIXME: Add support for home servers!
	 */
	if ((cl = realm_findbyaddr(packet->src_ipaddr.ipaddr.ip4addr.s_addr,
				   packet->src_port)) == NULL) {
		radlog(L_ERR, "Ignoring request from unknown home server %s port %d",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
			       packet->src_port);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	FIXME: Client MIB updates?
	 */
	switch(packet->code) {
	case PW_AUTHENTICATION_ACK:
	case PW_ACCESS_CHALLENGE:
	case PW_AUTHENTICATION_REJECT:
		fun = rad_authenticate;
		break;
		
	case PW_ACCOUNTING_RESPONSE:
		fun = rad_accounting;
		break;
		
	default:
		/*
		 *	FIXME: Update MIB for packet types?
		 */
		radlog(L_ERR, "Invalid packet code %d sent to a proxy port "
		       "from home server %s port %d - ID %d : IGNORED",
		       packet->code,
		       ip_ntoh(&packet->src_ipaddr, buffer, sizeof(buffer)),
		       packet->src_port, packet->id);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	Find the original request in the request list
	 */
	oldreq = rl_find_proxy(packet);

	/*
	 *	If we haven't found the original request which was
	 *	sent, to get this reply.  Complain, and discard this
	 *	request, as there's no way for us to send it to a NAS.
	 */
	if (!oldreq) {
		radlog(L_PROXY, "No outstanding request was found for proxy reply from home server %s port %d - ID %d",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
		       packet->src_port, packet->id);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	The proxy reply has arrived too late, as the original
	 *	(old) request has timed out, been rejected, and marked
	 *	as finished.  The client has already received a
	 *	response, so there is nothing that can be done. Delete
	 *	the tardy reply from the home server, and return nothing.
	 */
	if ((oldreq->reply->code != 0) ||
	    (oldreq->finished)) {
		radlog(L_ERR, "Reply from home server %s port %d  - ID: %d arrived too late for request %d. Try increasing 'retry_delay' or 'max_request_time'",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
		       packet->src_port, packet->id,
		       oldreq->number);
		rad_free(&packet);
		return 0;
	}

	/*
	 *	If there is already a reply, maybe this one is a
	 *	duplicate?
	 */
	if (oldreq->proxy_reply) {
		if (memcmp(oldreq->proxy_reply->vector,
			   packet->vector,
			   sizeof(oldreq->proxy_reply->vector)) == 0) {
			radlog(L_ERR, "Discarding duplicate reply from home server %s port %d  - ID: %d for request %d",
			       inet_ntop(packet->src_ipaddr.af,
					 &packet->src_ipaddr.ipaddr,
					 buffer, sizeof(buffer)),
			       packet->src_port, packet->id,
			       oldreq->number);
		} else {
			/*
			 *	? The home server gave us a new proxy
			 *	reply, which doesn't match the old
			 *	one.  Delete it.
			 */
			DEBUG2("Ignoring conflicting proxy reply");
		}

		/*
		 *	We've already received a reply, so
		 *	we discard this one, as we don't want
		 *	to do duplicate work.
		 */
		rad_free(&packet);
		return 0;
	} /* else there wasn't a proxy reply yet, so we can process it */

	/*
	 *	 Refresh the old request, and update it with the proxy
	 *	 reply.
	 *
	 *	? Can we delete the proxy request here?  * Is there
	 *	any more need for it?
	 *
	 *	FIXME: we probably shouldn't be updating the time
	 *	stamp here.
	 */
	oldreq->timestamp = time_now;
	oldreq->proxy_reply = packet;

	/*
	 *	FIXME: we should really verify the digest here,
	 *	before marking this packet as a valid response.
	 *
	 *	This is a security problem, I think...
	 */

	/*
	 *	Now that we've verified the packet IS actually from
	 *	that home server, and not forged, we can go mark the
	 *	entries for this home server as active.
	 *
	 *	If we had done this check in the 'find realm by IP address'
	 *	function, then an attacker could force us to use a home
	 *	server which was inactive, by forging reply packets
	 *	which didn't match any request.  We would think that
	 *	the reply meant the home server was active, would
	 *	re-activate the realms, and THEN bounce the packet
	 *	as garbage.
	 */
	for (cl = mainconfig.realms; cl != NULL; cl = cl->next) {
		if (oldreq->proxy_reply->src_ipaddr.af != cl->ipaddr.af) continue;
		if (cl->ipaddr.af != AF_INET) continue; /* FIXME */

		if (oldreq->proxy_reply->src_ipaddr.ipaddr.ip4addr.s_addr == cl->ipaddr.ipaddr.ip4addr.s_addr) {
			if (oldreq->proxy_reply->src_port == cl->auth_port) {
				cl->active = TRUE;
				cl->last_reply = oldreq->timestamp;
			} else if (oldreq->proxy_reply->src_port == cl->acct_port) {
				cl->acct_active = TRUE;
				cl->last_reply = oldreq->timestamp;
			}
		}
	}

	rad_assert(fun != NULL);
	*pfun = fun;
	*prequest = oldreq;

	return 1;
}

/*
 *	Free a linked list of listeners;
 */
void listen_free(rad_listen_t **head)
{
	rad_listen_t *list;

	if (!head || !*head) return;

	list = *head;
	while (list) {
		rad_listen_t *next = list->next;
		
		/*
		 *	The code below may have eaten the FD.
		 */
		if (list->fd >= 0) close(list->fd);
		free(list);
		
		list = next;
	}

	*head = NULL;
}


/*
 *	Binds a listener to a socket.
 */
static int listen_bind(rad_listen_t *this)
{
	struct sockaddr salocal;
	socklen_t	salen;
	rad_listen_t	**last;

	switch (this->type) {
	case RAD_LISTEN_AUTH:
		this->recv = auth_socket_recv;
		break;

	case RAD_LISTEN_ACCT:
		this->recv = acct_socket_recv;
		break;

	case RAD_LISTEN_PROXY:
		this->recv = proxy_socket_recv;
		break;

	default:
		rad_assert(0 == 1);
		return -1;
	}

	/*
	 *	If the port is zero, then it means the appropriate
	 *	thing from /etc/services.
	 */
	if (this->port == 0) {
		struct servent	*svp;

		switch (this->type) {
		case RAD_LISTEN_AUTH:
			svp = getservbyname ("radius", "udp");
			if (svp != NULL) {
				this->port = ntohs(svp->s_port);
			} else {
				this->port = PW_AUTH_UDP_PORT;
			}
			break;

		case RAD_LISTEN_ACCT:
			svp = getservbyname ("radacct", "udp");
			if (svp != NULL) {
				this->port = ntohs(svp->s_port);
			} else {
				this->port = PW_ACCT_UDP_PORT;
			}
			break;

		default:
			radlog(L_ERR|L_CONS, "ERROR: Non-fatal internal sanity check failed in bind.");
			return -1;
		}
	}

	/*
	 *	Find it in the old list, AFTER updating the port.  If
	 *	it's there, use that, rather than creating a new
	 *	socket.  This allows HUP's to re-use the old sockets,
	 *	which means that packets waiting in the socket queue
	 *	don't get lost.  */
	for (last = &mainconfig.listen;
	     *last != NULL;
	     last = &((*last)->next)) {
		if ((this->type == (*last)->type) &&
		    (this->port == (*last)->port) &&
		    (this->ipaddr.af == (*last)->ipaddr.af) &&
		    (memcmp(&this->ipaddr.ipaddr,
			    &(*last)->ipaddr.ipaddr,
			    ((this->ipaddr.af == AF_INET) ?
			     sizeof(this->ipaddr.ipaddr.ip4addr) :
			     sizeof(this->ipaddr.ipaddr.ip6addr))))) {
			this->fd = (*last)->fd;
			(*last)->fd = -1;
			return 0;
		}
	}

	/*
	 *	Create the socket.
	 */
	this->fd = socket(this->ipaddr.af, SOCK_DGRAM, 0);
	if (this->fd < 0) {
		radlog(L_ERR|L_CONS, "ERROR: Failed to open socket: %s",
		       strerror(errno));
		return -1;
	}
	

#ifdef WITH_UDPFROMTO
	/*
	 *	Initialize udpfromto for all sockets.
	 */
	if (udpfromto_init(this->fd) != 0) {
		radlog(L_ERR|L_CONS, "ERROR: udpfromto init failed.");
	}
#endif

	if (this->ipaddr.af == AF_INET) {
		struct sockaddr_in *sa;

		sa = (struct sockaddr_in *) &salocal;
		memset(sa, 0, sizeof(salocal));
		sa->sin_family = AF_INET;
		sa->sin_addr = this->ipaddr.ipaddr.ip4addr;
		sa->sin_port = htons(this->port);
		salen = sizeof(*sa);

#ifdef HAVE_STRUCT_SOCKADDR_IN6
	} else if (this->ipaddr.af == AF_INET6) {
		struct sockaddr_in6 *sa;

		sa = (struct sockaddr_in6 *) &salocal;
		memset(sa, 0, sizeof(salocal));
		sa->sin6_family = AF_INET6;
		sa->sin6_addr = this->ipaddr.ipaddr.ip6addr;
		sa->sin6_port = htons(this->port);
		salen = sizeof(*sa);
#endif
	} else {
		radlog(L_ERR|L_CONS, "ERROR: Unsupported protocol family %d",
		       this->ipaddr.af);
		close(this->fd);
		this->fd = -1;
		return -1;
	}

	if (bind(this->fd, &salocal, salen) < 0) {
		char buffer[128];

		radlog(L_ERR|L_CONS, "ERROR: Bind to %s port %d failed: %s",
		       inet_ntop(this->ipaddr.af, &this->ipaddr.ipaddr,
				 buffer, sizeof(buffer)),
		       this->port, strerror(errno));
				 
		close(this->fd);
		this->fd = -1;
		return -1;
	}

	return 0;
}


/*
 *	Externally visible function for creating a new proxy LISTENER.
 *
 *	For now, don't take ipaddr or port.
 */
int proxy_new_listener(void)
{
	int port;
	rad_listen_t *this;

	this = rad_malloc(sizeof(*this));

	memset(this, 0, sizeof(*this));

	this->ipaddr.af = AF_INET;
	this->ipaddr.ipaddr.ip4addr.s_addr = server_ip;
	this->type = RAD_LISTEN_PROXY;

	/*
	 *	Proxying was not previously defined: die.
	 */
	if (last_proxy_port == 0) return -1;

	/*
	 *	Keep going until we find an unused port.
	 */
	for (port = last_proxy_port + 1; port < 64000; port++) {
		this->port = port;
		if (listen_bind(this) == 0) {
			rad_listen_t **last;

			last_proxy_port = port;

			/*
			 *	Add the new listener to the list of
			 *	listeners.
			 */
			for (last = &mainconfig.listen;
			     *last != NULL;
			     last = &((*last)->next)) {
				/* do nothing */
			}

			*last = this;
			return this->fd;
		}
	}

	return -1;
}


/*
 *	Hack the OLD way of listening on a socket.
 */
static int old_listen_init(rad_listen_t **head)
{
	CONF_PAIR	*cp;
	rad_listen_t 	*this, **last;

	/*
	 *	No IP from the command-line, look for bind_address.
	 */
	if (mainconfig.myip.af == AF_UNSPEC) {
		cp = cf_pair_find(mainconfig.config, "bind_address");
		if (!cp) return 0;
	}

	last = head;

	this = rad_malloc(sizeof(*this));
	memset(this, 0, sizeof(*this));

	/*
	 *	Create the authentication socket.
	 */
	this->ipaddr.af = AF_INET;
       	this->ipaddr.ipaddr.ip4addr.s_addr = server_ip;
	this->type = RAD_LISTEN_AUTH;
	this->port = auth_port;

	if (listen_bind(this) < 0) {
		radlog(L_CONS|L_ERR, "There appears to be another RADIUS server running on the authentication port %d", this->port);
		free(this);
		return -1;
	}
	auth_port = this->port;	/* may have been updated in listen_bind */
	*last = this;
	last = &(this->next);

	/*
	 *  Open Accounting Socket.
	 *
	 *  If we haven't already gotten acct_port from /etc/services,
	 *  then make it auth_port + 1.
	 */
	this = rad_malloc(sizeof(*this));
	memset(this, 0, sizeof(*this));

	/*
	 *	Create the accounting socket.
	 *
	 *	The accounting port is always the authentication port + 1
	 */
	this->ipaddr.af = AF_INET;
       	this->ipaddr.ipaddr.ip4addr.s_addr = server_ip;
	this->type = RAD_LISTEN_ACCT;
	this->port = auth_port + 1;

	if (listen_bind(this) < 0) {
		radlog(L_CONS|L_ERR, "There appears to be another RADIUS server running on the accounting port %d", this->port);
		free(this);
		return -1;
	}
	*last = this;

	return 0;
}

/*
 *	FIXME: Get rid of this...
 */
static const CONF_PARSER bind_config[] = {
	{ "bind_address", PW_TYPE_IPADDR, 0, &server_ip, "*" },
	{ NULL, -1, 0, NULL, NULL }
};


static const LRAD_NAME_NUMBER listen_compare[] = {
	{ "auth",	RAD_LISTEN_AUTH },
	{ "acct",	RAD_LISTEN_ACCT },
	{ NULL, 0 },
};


/*
 *	Generate a list of listeners.  Takes an input list of
 *	listeners, too, so we don't close sockets with waiting packets.
 */
int listen_init(const char *filename, rad_listen_t **head)
{
	CONF_SECTION	*cs;
	rad_listen_t	**last;
	char		buffer[128];
	rad_listen_t	*this;

	if (start_time != 0) start_time = time(NULL);

	/*
	 *	Read bind_address into server_ip, but only if an IP
	 *	isn't specified on the command-line
	 */
	if (mainconfig.myip.af == AF_UNSPEC) {
		cf_section_parse(mainconfig.config, NULL, bind_config);
	} else if (mainconfig.myip.af == AF_INET) {
		server_ip = mainconfig.myip.ipaddr.ip4addr.s_addr; /* over-ride server_ip */
	} else if (mainconfig.myip.af == AF_INET6) {
		rad_assert(0 == 1); /* IPv6 unsupported */
	} else rad_assert(0 == 1);

	if (old_listen_init(head) < 0) {
		exit(1);
	}

	/*
	 *	Add to the end of the list.
	 */
	for (last = head; *last != NULL; last = &((*last)->next)) {
		/* do nothing */
	}

    	/*
	 *	Walk through the "listen" directives, but ONLY if there
	 *	was no address specified on the command-line.
	 */
	if (mainconfig.myip.af != AF_UNSPEC) goto do_proxy;

	for (cs = cf_subsection_find_next(mainconfig.config,
					  NULL, "listen");
	     cs != NULL;
	     cs = cf_subsection_find_next(mainconfig.config,
					  cs, "listen")) {
		int		rcode, type;
		char		*listen_type;
		int		listen_port;
		int		lineno = cf_section_lineno(cs);
		lrad_ipaddr_t	ipaddr;

		listen_port = 0;
		listen_type = NULL;
		
		/*
		 *	Try IPv4 first
		 */
		ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_NONE);
		rcode = cf_item_parse(cs, "ipaddr", PW_TYPE_IPADDR,
				      &ipaddr.ipaddr.ip4addr, NULL);
		if (rcode < 0) return -1;

		if (rcode == 0) { /* successfully parsed IPv4 */
			ipaddr.af = AF_INET;

		} else {	/* maybe IPv6? */
			rcode = cf_item_parse(cs, "ipv6addr", PW_TYPE_IPV6ADDR,
					      &ipaddr.ipaddr.ip6addr, NULL);
			if (rcode < 0) return -1;

			if (rcode == 1) {
				radlog(L_ERR, "%s[%d]: No address specified in listen section",
				       filename, lineno);
				return -1;
			}
			ipaddr.af = AF_INET6;
		}


		rcode = cf_item_parse(cs, "port", PW_TYPE_INTEGER,
				      &listen_port, "0");
		if (rcode < 0) return -1;

		rcode = cf_item_parse(cs, "type", PW_TYPE_STRING_PTR,
				      &listen_type, "");
		if (rcode < 0) return -1;
		if (rcode == 1) {
			free(listen_type);
			radlog(L_ERR, "%s[%d]: No type specified in listen section",
			       filename, lineno);
			return -1;
		}

		type = lrad_str2int(listen_compare, listen_type,
				    RAD_LISTEN_NONE);
		if (type == RAD_LISTEN_NONE) {
			radlog(L_CONS|L_ERR, "%s[%d]: Invalid type in listen section.",
			       filename, lineno);
			return -1;
		}

		this = rad_malloc(sizeof(*this));
		this->ipaddr = ipaddr;
		this->type = type;
		this->port = listen_port;

		/*
		 *	And bind it to the port.
		 */
		if (listen_bind(this) < 0) {
			radlog(L_CONS|L_ERR, "%s[%d]: Error binding to port for %s port %d",
			       filename, cf_section_lineno(cs),
			       ip_ntoh(&this->ipaddr, buffer, sizeof(buffer)),
			       this->port);
			free(this);
			return -1;
		}

		*last = this;
		last = &(this->next);		
	}

	/*
	 *	If we're proxying requests, open the proxy FD.
	 *	Otherwise, don't do anything.
	 */
 do_proxy:
	if (mainconfig.proxy_requests == TRUE) {
		int		port = -1;
		rad_listen_t	*auth;

		/*
		 *	Find the first authentication port,
		 *	and use it
		 */
		for (auth = *head; auth != NULL; auth = auth->next) {
			if (auth->type == RAD_LISTEN_AUTH) {
				port = auth->port + 2;
				break;
			}
		}

		/*
		 *	Not found, pick an accounting port.
		 */
		if (port < 0) for (auth = *head; auth != NULL; auth = auth->next) {
			if (auth->type == RAD_LISTEN_ACCT) {
				port = auth->port + 1;
				break;
			}
		}

		/*
		 *	Still no port.  Don't do anything.
		 */
		if (port < 0) {
			return 0;
		}

		this = rad_malloc(sizeof(*this));
		memset(this, 0, sizeof(*this));
		
		/*
		 *	Create the proxy socket.
		 */
		this->ipaddr.af = AF_INET;
		this->ipaddr.ipaddr.ip4addr.s_addr = server_ip;
		this->type = RAD_LISTEN_PROXY;

		/*
		 *	Try to find a proxy port (value doesn't matter)
		 */
		for (this->port = port;
		     this->port < 64000;
		     this->port++) {
			if (listen_bind(this) == 0) {
				last_proxy_port = this->port;
				*last = this;
				return 0;
			}
		}

		radlog(L_ERR|L_CONS, "Failed to open socket for proxying");
		free(this);
		return -1;
	}

	return 0;
}

