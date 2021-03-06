#include "kernel.h"
#include "net.h"
#include "socket.h"

struct udp_wait_entry {
	struct hlist_head list;
	struct socket *sock;
	struct process *proc;
	struct packet *rcv;
	uint16_t port;
};

#define UDP_HLIST_SIZE 128
struct hlist_head udp_hlist[UDP_HLIST_SIZE];

static inline uint32_t udp_hash(uint16_t port)
{
	return port % UDP_HLIST_SIZE;
}

static struct udp_wait_entry *lookup_entry(uint16_t port)
{
	struct udp_wait_entry *entry;
	uint32_t hash = udp_hash(port);
	list_for_each_entry(entry, &udp_hlist[hash], list)
	{
		if (entry->port == port)
			return entry;
	}
	return NULL;
}

/*
 * Wait for a packet to come in on "port".
 *
 * YOU MUST HAVE ALREADY DISABLED INTERRUPTS BEFORE CALLING THIS FUNCTION.
 *
 * Why? We are about to go into a sleep which we can only wake from by the
 * receipt of a packet. Presumably, you have already sent out the packet which
 * will trigger this receipt. If interrupts are enabled, the packet could be
 * received and handled BEFORE we have entered our sleep.
 */
struct packet *udp_wait(uint16_t port)
{
	struct udp_wait_entry entry;
	uint32_t hash = udp_hash(port);

	entry.sock = NULL;
	entry.proc = current;
	entry.port = port;
	entry.rcv = NULL;
	hlist_insert(&udp_hlist[hash], &entry.list);

	current->flags.pr_ready = 0;

	interrupt_enable();
	schedule();

	hlist_remove(&udp_hlist[hash], &entry.list);
	return entry.rcv;
}

void udp_recv(struct netif *netif, struct packet *pkt)
{
	struct udp_wait_entry *entry;
	uint32_t hash = udp_hash(ntohs(pkt->udp->dst_port));
	/*printf("udp_recv src=%u dst=%u\n", ntohs(pkt->udp->src_port),
	       ntohs(pkt->udp->dst_port));*/
	pkt->al = pkt->tl + sizeof(struct udphdr);
	list_for_each_entry(entry, &udp_hlist[hash], list)
	{
		if (entry->sock) {
			/* TODO: socket may not be connected to this endpoint,
			 * need to better check here */
			list_insert_end(&entry->sock->recvq, &pkt->list);
			wait_list_awaken(&entry->sock->recvwait);
			return;
		} else {
			if (entry->port == ntohs(pkt->udp->dst_port)) {
				entry->rcv = pkt;
				entry->proc->flags.pr_ready = true;
				return;
			}
		}
	}
	puts("nobody was waiting for this packet, freeing\n");
	packet_free(pkt);
}

void udp_send(struct netif *netif, struct packet *pkt, uint32_t src_ip,
              uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
	uint32_t csum;
	uint32_t len_rounded;
	pkt->tl = pkt->al - sizeof(struct udphdr);

	pkt->udp->src_port = src_port;
	pkt->udp->dst_port = dst_port;
	pkt->udp->len = htons(pkt->end - pkt->tl);
	len_rounded = pkt->end - pkt->tl;
	len_rounded = (len_rounded / 2) + (len_rounded % 2);
	pkt->udp->csum = 0;
	csum_init(&csum);
	csum_add(&csum, &src_ip, 2);
	csum_add(&csum, &dst_ip, 2);
	csum_add_value(&csum, ntohs((uint16_t)IPPROTO_UDP));
	csum_add(&csum, &pkt->udp->len, 1);
	csum_add(&csum, (uint16_t *)pkt->udp, len_rounded);
	pkt->udp->csum = csum_finalize(&csum);
	ip_send(netif, pkt, IPPROTO_UDP, src_ip, dst_ip);
}

int udp_reserve(void)
{
	return ip_reserve() + sizeof(struct udphdr);
}

void udp_do_bind(struct socket *sock, const struct sockaddr_in *addr)
{
	struct udp_wait_entry *entry;
	int hash;
	hash = udp_hash(ntohs(addr->sin_port));

	/* NOTE: there's totally a race condition we ignore here where we first
	 * look to see if there is a socket listening on a port, and if not, we
	 * create it. This is fine for now since we're not preemptive, and
	 * interrupt handlers don't go around creating sockets. But if we become
	 * preemptive, a different process could come around and claim the port
	 * in between, and then we'd encounter a fun time. */
	entry = kmalloc(sizeof(struct udp_wait_entry));
	entry->sock = sock;
	entry->port = ntohs(addr->sin_port);
	hlist_insert(&udp_hlist[hash], &entry->list);
	sock->src = *addr;
	sock->flags.sk_bound = 1;
}

static bool udp_bind_to_ephemeral(struct socket *sock)
{
#define EPH_BEGIN 20000
	const uint16_t ephemeral_begin = EPH_BEGIN;
	const uint16_t ephemeral_end = 65535;

	struct sockaddr_in addr;
	static uint16_t i = EPH_BEGIN;
	uint16_t prev;
	uint16_t tries = 100;

	while (tries--) {
		prev = i;
		if (i == ephemeral_end)
			i = ephemeral_begin;
		else
			i = i + 1;

		if (!lookup_entry(htons(prev))) {
			addr.sin_addr.s_addr = 0;
			addr.sin_port = htons(prev);
			udp_do_bind(sock, &addr);
			return true;
		}
	}
	return false;
}

int udp_bind(struct socket *sock, const struct sockaddr *address,
             socklen_t address_len)
{
	int rv;
	struct sockaddr_in addr;

	if (sock->flags.sk_bound)
		return -EINVAL;

	if (address_len != sizeof(struct sockaddr_in))
		return -EINVAL;

	rv = copy_from_user(&addr, address, address_len);
	if (rv < 0)
		return rv;

	if (addr.sin_addr.s_addr != 0 && addr.sin_addr.s_addr != nif.ip)
		return -EADDRNOTAVAIL;

	if (lookup_entry(addr.sin_port))
		return -EADDRINUSE;

	udp_do_bind(sock, &addr);
	return 0;
}

int udp_connect(struct socket *sock, const struct sockaddr *address,
                socklen_t address_len)
{
	int rv;
	struct sockaddr_in addr;

	/* UDP is connectionless. Calling connect() many times is just fine with
	 * us. */

	if (address_len != sizeof(struct sockaddr_in))
		return -EINVAL;

	rv = copy_from_user(&addr, (const void *)address, address_len);
	if (rv < 0)
		return rv;

	sock->dst = addr;
	sock->flags.sk_connected = 1;
	return 0;
}

int udp_sys_send(struct socket *sock, void *data, size_t len, int flags)
{
	int rv, space;
	struct packet *pkt;
	uint32_t src;

	space = udp_reserve();
	if (space + len > MAX_ETH_PKT_SIZE)
		return -EMSGSIZE;

	if (!sock->flags.sk_connected) {
		/* Although UDP sockets do support sending via sockets without
		 * connecting, it is through the sendto() or sendmsg() syscalls.
		 * We require that the socket be connected first. */
		return -EDESTADDRREQ;
	}

	if (!sock->flags.sk_bound) {
		/* Unbound sockets can be sent from -- we just select an unused
		 * ephemeral port and bind to that. */
		if (!udp_bind_to_ephemeral(sock)) {
			return -EADDRINUSE;
		}
	}

	pkt = packet_alloc();
	pkt->app = (void *)&pkt->data + space;
	rv = copy_from_user(pkt->app, data, len);
	if (rv < 0)
		goto error;

	pkt->end = pkt->app + len;

	if (sock->src.sin_addr.s_addr)
		src = sock->src.sin_addr.s_addr;
	else
		src = nif.ip;

	udp_send(&nif, pkt, src, sock->dst.sin_addr.s_addr, sock->src.sin_port,
	         sock->dst.sin_port);
	return len;
error:
	packet_free(pkt);
	return rv;
}

struct packet *socket_recvq_get(struct socket *socket)
{
	struct packet *pkt;
	list_for_each_entry(pkt, &socket->recvq, list)
	{
		return pkt;
	}
	return NULL;
}

int udp_sys_recv(struct socket *sock, void *data, size_t len, int flags)
{
	struct packet *pkt;
	size_t pktlen;
	int rv;

	if (!sock->flags.sk_bound) {
		/* If the socket is not bound, then recv() is somewhat
		 * meaningless. I'm not sure if this is how a real socket
		 * implementation would behave, but we'll raise an error. */
		return -EINVAL;
	}

	/* Get packet or wait for one to come */
	pkt = socket_recvq_get(sock);
	while (!pkt) {
		wait_for(&sock->recvwait);
		pkt = socket_recvq_get(sock);
	}

	/* This may not be standard, but we only allow recv()ing entire packets,
	 * no less. */
	pktlen = (uint32_t)(pkt->end - pkt->al);
	if (pktlen > len)
		return -EMSGSIZE;

	if ((rv = copy_to_user(data, pkt->al, pktlen)) < 0)
		return rv;

	list_remove(&pkt->list);
	packet_free(pkt);
	return pktlen;
}

struct sockops udp_ops = {
	.proto = IPPROTO_UDP,
	.bind = udp_bind,
	.connect = udp_connect,
	.send = udp_sys_send,
	.recv = udp_sys_recv,
};

void udp_init(void)
{
	int i;
	socket_register_proto(&udp_ops);

	for (i = 0; i < UDP_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(udp_hlist[i]);
}
