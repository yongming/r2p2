/*
 * MIT License
 *
 * Copyright (c) 2019-2021 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <r2p2/api-internal.h>
#include <r2p2/mempool.h>
#ifdef WITH_TIMESTAMPING
static_assert(LINUX, "Timestamping supported only in Linux");
#include <r2p2/r2p2-linux.h>
#include <r2p2/timestamping.h>
#endif

#define POOL_SIZE 1024
#define min(a, b) ((a) < (b)) ? (a) : (b)

static recv_fn rfn;

static __thread struct fixed_mempool *client_pairs;
static __thread struct fixed_mempool *server_pairs;
static __thread struct fixed_linked_list pending_client_pairs = {0};
static __thread struct fixed_linked_list pending_server_pairs = {0};
static __thread struct iovec to_app_iovec[0xFF]; // change this to 0xFF;

static struct r2p2_client_pair *alloc_client_pair(void)
{
	struct r2p2_client_pair *cp;

	cp = alloc_object(client_pairs);
	assert(cp);

	bzero(cp, sizeof(struct r2p2_client_pair));

	return cp;
}

static void free_client_pair(struct r2p2_client_pair *cp)
{
	generic_buffer gb;

	// Free the received reply
	gb = cp->reply.head_buffer;
	while (gb != NULL) {
		free_buffer(gb);
		gb = get_buffer_next(gb);
	}

#ifdef LINUX
	// Free the request sent
	gb = cp->request.head_buffer;
	while (gb != NULL) {
		free_buffer(gb);
		gb = get_buffer_next(gb);
	}
#endif

	// Free the socket in linux on anything implementation specific
	if (cp->on_free)
		cp->on_free(cp->impl_data);

	free_object(cp);
}

static struct r2p2_server_pair *alloc_server_pair(void)
{
	struct r2p2_server_pair *sp;

	sp = alloc_object(server_pairs);
	assert(sp);

	bzero(sp, sizeof(struct r2p2_server_pair));

	return sp;
}

static void free_server_pair(struct r2p2_server_pair *sp)
{
	generic_buffer gb;

	// Free the recv message buffers
	gb = sp->request.head_buffer;
	while (gb != NULL) {
		free_buffer(gb);
		gb = get_buffer_next(gb);
	}

// Free the reply sent
#ifdef LINUX
	gb = sp->reply.head_buffer;
	while (gb != NULL) {
		free_buffer(gb);
		gb = get_buffer_next(gb);
	}
#endif

	free_object(sp);
}

static void add_to_pending_client_pairs(struct r2p2_client_pair *cp)
{
	struct fixed_obj *fo = get_object_meta(cp);
	add_to_list(&pending_client_pairs, fo);
}

static void add_to_pending_server_pairs(struct r2p2_server_pair *sp)
{
	struct fixed_obj *fo = get_object_meta(sp);
	add_to_list(&pending_server_pairs, fo);
}

static void remove_from_pending_server_pairs(struct r2p2_server_pair *sp)
{
	struct fixed_obj *fo = get_object_meta(sp);
	remove_from_list(&pending_server_pairs, fo);
}

static void remove_from_pending_client_pairs(struct r2p2_client_pair *cp)
{
	struct fixed_obj *fo = get_object_meta(cp);
	remove_from_list(&pending_client_pairs, fo);
}

static struct r2p2_server_pair *
find_in_pending_server_pairs(uint16_t req_id, struct r2p2_host_tuple *sender)
{
	struct r2p2_server_pair *sp;
	struct fixed_obj *fo;

	fo = pending_server_pairs.head;
	while (fo) {
		sp = (struct r2p2_server_pair *)fo->elem;
		if ((sp->request.sender.ip == sender->ip) &&
			(sp->request.sender.port == sender->port) &&
			(sp->request.req_id == req_id))
			return sp;
		fo = (struct fixed_obj *)fo->next;
	}
	return NULL;
}

static struct r2p2_client_pair *
find_in_pending_client_pairs(uint16_t req_id, struct r2p2_host_tuple *sender)
{
	struct r2p2_client_pair *cp;
	struct fixed_obj *fo;

	fo = pending_client_pairs.head;
	// FIXME: inlcude ip too
	while (fo) {
		cp = (struct r2p2_client_pair *)fo->elem;
		if ((cp->request.sender.port == sender->port) &&
			(cp->request.req_id == req_id))
			return cp;
		fo = (struct fixed_obj *)fo->next;
	}
	printf("Request not found\n");
	return NULL;
}

static int prepare_to_app_iovec(struct r2p2_msg *msg)
{
	generic_buffer gb;
	char *buf;
	int len, iovcnt = 0;

	gb = msg->head_buffer;
	while (gb != NULL) {
		buf = get_buffer_payload(gb);
		assert(buf);
		len = get_buffer_payload_size(gb);
		to_app_iovec[iovcnt].iov_base = ((struct r2p2_header *)buf) + 1;
		to_app_iovec[iovcnt++].iov_len = len - sizeof(struct r2p2_header);
		gb = get_buffer_next(gb);
		assert(iovcnt < 0xFF);
	}
	return iovcnt;
}

static void forward_request(struct r2p2_server_pair *sp)
{
	int iovcnt;

	iovcnt = prepare_to_app_iovec(&sp->request);
	rfn((long)sp, to_app_iovec, iovcnt);
}

static void r2p2_msg_add_payload(struct r2p2_msg *msg, generic_buffer gb)
{
	if (msg->tail_buffer) {
		chain_buffers(msg->tail_buffer, gb);
		msg->tail_buffer = gb;
	} else {
		assert(msg->head_buffer == NULL);
		assert(msg->tail_buffer == NULL);
		msg->head_buffer = gb;
		msg->tail_buffer = gb;
	}
}

void r2p2_prepare_msg(struct r2p2_msg *msg, struct iovec *iov, int iovcnt,
					  uint8_t req_type, uint8_t policy, uint16_t req_id)
{
	unsigned int iov_idx, bufferleft, copied, tocopy, buffer_cnt, total_payload,
		single_packet_msg, is_first;
	struct r2p2_header *r2p2h;
	generic_buffer gb, new_gb;
	char *target, *src;

	// Compute the total payload
	total_payload = 0;
	for (int i = 0; i < iovcnt; i++)
		total_payload += iov[i].iov_len;

	if (total_payload <= PAYLOAD_SIZE)
		single_packet_msg = 1;
	else
		single_packet_msg = 0;

	iov_idx = 0;
	bufferleft = 0;
	copied = 0;
	gb = NULL;
	buffer_cnt = 0;
	is_first = 1;
	while (iov_idx < (unsigned int)iovcnt) {
		if (!bufferleft) {
			new_gb = get_buffer();
			assert(new_gb);
			// Set the last buffer to full size
			if (gb) {
				if (is_first) {
					set_buffer_payload_size(gb, MIN_PAYLOAD_SIZE +
													sizeof(struct r2p2_header));
					is_first = 0;
				} else
					set_buffer_payload_size(gb, PAYLOAD_SIZE +
													sizeof(struct r2p2_header));
			}
			r2p2_msg_add_payload(msg, new_gb);
			gb = new_gb;
			target = get_buffer_payload(gb);
			if (is_first && !single_packet_msg)
				bufferleft = MIN_PAYLOAD_SIZE;
			else
				bufferleft = PAYLOAD_SIZE;
			// FIX the header
			r2p2h = (struct r2p2_header *)target;
			bzero(r2p2h, sizeof(struct r2p2_header));
			r2p2h->magic = MAGIC;
			r2p2h->rid = req_id;
			r2p2h->header_size = sizeof(struct r2p2_header);
			r2p2h->type_policy = (req_type << 4) | (0x0F & policy);
			r2p2h->p_order = buffer_cnt++;
			r2p2h->flags = 0;
			target += sizeof(struct r2p2_header);
		}
		src = iov[iov_idx].iov_base;
		tocopy = min(bufferleft, iov[iov_idx].iov_len - copied);
		memcpy(target, &src[copied], tocopy);
		copied += tocopy;
		bufferleft -= tocopy;
		target += tocopy;
		if (copied == iov[iov_idx].iov_len) {
			iov_idx++;
			copied = 0;
		}
	}

	// Set the len of the last buffer
	set_buffer_payload_size(gb, PAYLOAD_SIZE + sizeof(struct r2p2_header) -
									bufferleft);

	// Fix the header of the first and last packet
	r2p2h = (struct r2p2_header *)get_buffer_payload(msg->head_buffer);
	r2p2h->flags |= F_FLAG;
	r2p2h->p_order = buffer_cnt;
	r2p2h = (struct r2p2_header *)get_buffer_payload(msg->tail_buffer);
	r2p2h->flags |= L_FLAG;

	msg->req_id = req_id;
}

static void handle_response(generic_buffer gb, int len,
							struct r2p2_header *r2p2h,
							struct r2p2_host_tuple *source,
#ifdef WITH_TIMESTAMPING
							struct r2p2_host_tuple *local_host,
							const struct timespec *last_rx_timestamp)
#else
							struct r2p2_host_tuple *local_host)
#endif
{
	struct r2p2_client_pair *cp;
	int iovcnt;
	generic_buffer rest_to_send;

	cp = find_in_pending_client_pairs(r2p2h->rid, local_host);
	if (!cp) {
		// printf("Expired timer?\n");
		free_buffer(gb);
		return;
	}

#ifdef WITH_TIMESTAMPING
	// Update ctx rx_timestamp if bigger than the current one.
	if (last_rx_timestamp != NULL && last_rx_timestamp->tv_sec != 0 &&
		is_smaller_than(&cp->ctx->rx_timestamp, last_rx_timestamp)) {
		cp->ctx->rx_timestamp = *last_rx_timestamp;
	}
#endif

	cp->reply.sender = *source;
	if (cp->state == R2P2_W_RESPONSE) {
		set_buffer_payload_size(gb, len);
		r2p2_msg_add_payload(&cp->reply, gb);

		if (is_first(r2p2h)) {
			cp->reply_expected_packets = r2p2h->p_order;
			cp->reply_received_packets = 1;
		} else {
			if (r2p2h->p_order != cp->reply_received_packets++) {
				printf("OOF in response\n");
				cp->ctx->error_cb(cp->ctx->arg, -1);
				remove_from_pending_client_pairs(cp);
				free_client_pair(cp);
				return;
			}
		}

		// Is it full msg? Should I call the application?
		if (!is_last(r2p2h)) {
			return;
		}
		if (cp->timer)
			disarm_timer(cp->timer);
		if (cp->reply_received_packets != cp->reply_expected_packets) {
			printf("Wrong total size in response\n");
			cp->ctx->error_cb(cp->ctx->arg, -1);
			remove_from_pending_client_pairs(cp);
			free_client_pair(cp);
			return;
		}
		iovcnt = prepare_to_app_iovec(&cp->reply);

#ifdef WITH_TIMESTAMPING
		// Extract tx timestamp if it wasn't there (due to packet order)
		if (cp->ctx->rx_timestamp.tv_sec != 0 &&
			cp->ctx->tx_timestamp.tv_sec == 0) {
			extract_tx_timestamp(((struct r2p2_socket *)cp->impl_data)->fd,
								 &cp->ctx->tx_timestamp);
		}
#endif

		cp->ctx->success_cb((long)cp, cp->ctx->arg, to_app_iovec, iovcnt);
	} else {
		// Send the rest packets
		assert(cp->state == R2P2_W_ACK);
		assert(len == (sizeof(struct r2p2_header) + 3));
		free_buffer(gb);

#ifdef LINUX
		rest_to_send = get_buffer_next(cp->request.head_buffer);
#else
		rest_to_send = cp->request.head_buffer;
#endif
		buf_list_send(rest_to_send, &cp->reply.sender, cp->impl_data);

		cp->state = R2P2_W_RESPONSE;
	}
}

static void handle_request(generic_buffer gb, int len,
						   struct r2p2_header *r2p2h,
						   struct r2p2_host_tuple *source)
{
	struct r2p2_server_pair *sp;
	uint16_t req_id;
	char ack_payload[] = "ACK";
	struct iovec ack;
	struct r2p2_msg ack_msg = {0};

	req_id = r2p2h->rid;
	if (is_first(r2p2h)) {
		/*
		 * FIXME
		 * Consider the case that an old request with the same id and
		 * src ip port is already there
		 * remove before starting the new one
		 */
		sp = alloc_server_pair();
		assert(sp);
		sp->request.sender = *source;
		sp->request.req_id = req_id;
		sp->request_expected_packets = r2p2h->p_order;
		sp->request_received_packets = 1;
		if (!is_last(r2p2h)) {
			// add to pending request
			add_to_pending_server_pairs(sp);

			// send ACK
			ack.iov_base = ack_payload;
			ack.iov_len = 3;
			r2p2_prepare_msg(&ack_msg, &ack, 1, ACK_MSG, FIXED_ROUTE, req_id);
			buf_list_send(ack_msg.head_buffer, source, NULL);
#ifdef LINUX
			free_buffer(ack_msg.head_buffer);
#endif
		}
	} else {
		// find in pending msgs
		sp = find_in_pending_server_pairs(req_id, source);
		assert(sp);
		if (r2p2h->p_order != sp->request_received_packets++) {
			printf("OOF in request\n");
			remove_from_pending_server_pairs(sp);
			free_server_pair(sp);
			return;
		}
	}
	set_buffer_payload_size(gb, len);
	r2p2_msg_add_payload(&sp->request, gb);

	if (!is_last(r2p2h))
		return;

	if (sp->request_received_packets != sp->request_expected_packets) {
		printf("Wrong total size in request\n");
		remove_from_pending_server_pairs(sp);
		free_server_pair(sp);
		return;
	}
	assert(rfn);
	forward_request(sp);
}

void handle_incoming_pck(generic_buffer gb, int len,
						 struct r2p2_host_tuple *source,
#ifdef WITH_TIMESTAMPING
						 struct r2p2_host_tuple *local_host,
						 const struct timespec *last_rx_timestamp)
#else
						 struct r2p2_host_tuple *local_host)
#endif
{
	struct r2p2_header *r2p2h;
	char *buf;

	if ((unsigned)len < sizeof(struct r2p2_header))
		printf("I received %d\n", len);
	assert((unsigned)len >= sizeof(struct r2p2_header));
	buf = get_buffer_payload(gb);
	r2p2h = (struct r2p2_header *)buf;

	if (is_response(r2p2h))
#ifdef WITH_TIMESTAMPING
		handle_response(gb, len, r2p2h, source, local_host, last_rx_timestamp);
#else
		handle_response(gb, len, r2p2h, source, local_host);
#endif
	else
		handle_request(gb, len, r2p2h, source);
}

int r2p2_backend_init_per_core(void)
{
	time_t t;

	client_pairs = create_mempool(POOL_SIZE, sizeof(struct r2p2_client_pair));
	assert(client_pairs);
	server_pairs = create_mempool(POOL_SIZE, sizeof(struct r2p2_server_pair));
	assert(server_pairs);

	srand((unsigned)time(&t));

	return 0;
}

void timer_triggered(struct r2p2_client_pair *cp)
{
	struct fixed_obj *fo = get_object_meta(cp);
	if (!fo->taken)
		return;

	assert(cp->ctx->timeout_cb);
	cp->ctx->timeout_cb(cp->ctx->arg);

	remove_from_pending_client_pairs(cp);
	free_client_pair(cp);
}

/*
 * API
 */
void r2p2_send_response(long handle, struct iovec *iov, int iovcnt)
{
	struct r2p2_server_pair *sp;

	sp = (struct r2p2_server_pair *)handle;
	r2p2_prepare_msg(&sp->reply, iov, iovcnt, RESPONSE_MSG, FIXED_ROUTE,
					 sp->request.req_id);
	buf_list_send(sp->reply.head_buffer, &sp->request.sender, NULL);

	// Notify router
	router_notify();

	remove_from_pending_server_pairs(sp);
	free_server_pair(sp);
}

void r2p2_send_req(struct iovec *iov, int iovcnt, struct r2p2_ctx *ctx)
{
	generic_buffer second_buffer;
	struct r2p2_client_pair *cp;
	uint16_t rid;

	cp = alloc_client_pair();
	assert(cp);
	cp->ctx = ctx;

	if (prepare_to_send(cp)) {
		free_client_pair(cp);
		return;
	}

	rid = rand();
	r2p2_prepare_msg(&cp->request, iov, iovcnt, REQUEST_MSG,
					 ctx->routing_policy, rid);
	cp->state = cp->request.head_buffer == cp->request.tail_buffer
					? R2P2_W_RESPONSE
					: R2P2_W_ACK;

	add_to_pending_client_pairs(cp);

	// Send only the first packet
	second_buffer = get_buffer_next(cp->request.head_buffer);
	chain_buffers(cp->request.head_buffer, NULL);
	buf_list_send(cp->request.head_buffer, ctx->destination, cp->impl_data);
#ifdef LINUX
	chain_buffers(cp->request.head_buffer, second_buffer);
#else
	cp->request.head_buffer = second_buffer;
#endif
}

void r2p2_recv_resp_done(long handle)
{
	struct r2p2_client_pair *cp = (struct r2p2_client_pair *)handle;

	remove_from_pending_client_pairs(cp);
	free_client_pair(cp);
}

void r2p2_set_recv_cb(recv_fn fn)
{
	rfn = fn;
}