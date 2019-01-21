/* This file contains functions related to the sender side of message
 * transmission. It also contains utility functions for sending packets.
 */

#include "homa_impl.h"

/**
 * set_priority() - Arrange for a packet to have a VLAN header that
 * specifies a priority for the packet.
 * @skb:        The packet was priority should be set.
 * @priority:   Priority level for the packet, in the range 0 (for lowest
 *              priority) to 7 ( for highest priority).
 */
inline static void set_priority(struct sk_buff *skb, int priority)
{
	/* The priority values stored in the VLAN header are weird, in that
	 * the value 0 is not the lowest priority; this table maps from
	 * "sensible" values as provided by the @priority argument to the
	 * corresponding value for the VLAN header. See the IEEE P802.1
	 * standard for details.
	 */
	static int tci[] = {
		(1 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(0 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(2 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(3 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(4 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(5 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(6 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT,
		(7 << VLAN_PRIO_SHIFT) | VLAN_TAG_PRESENT
	};
	skb->vlan_proto = htons(0x8100);
	skb->vlan_tci = tci[priority];
}

/**
 * homa_message_out_init() - Initialize a homa_message_out, including copying
 * message data from user space into sk_buffs.
 * @msgout:    Struct to initialize; current contents are assumed to be garbage.
 * @hsk:       Socket from which message will be sent.
 * @iter:      Info about the request buffer in user space.
 * @len:       Total length of the message.
 * @dest:      Describes the host to which the RPC will be sent.
 * @dport:     Port on @dest where the server is listening (destination).
 * @sport:     Port of the client (source).
 * @id:        Unique identifier for the message's RPC (relative to sport).
 * 
 * Return:   Either 0 (for success) or a negative errno value.
 */
int homa_message_out_init(struct homa_message_out *msgout,
		struct homa_sock *hsk, struct iov_iter *iter, size_t len,
		struct homa_peer *dest, __u16 dport, __u16 sport, __u64 id)
{
	int bytes_left;
	struct sk_buff *skb;
	int err;
	struct sk_buff **last_link = &msgout->packets;
	
	msgout->length = len;
	msgout->packets = NULL;
	msgout->next_packet = NULL;
	msgout->next_offset = 0;
	
	/* This is a temporary guess; must handle better in the future. */
	msgout->unscheduled = hsk->homa->rtt_bytes;
	msgout->granted = msgout->unscheduled;
	if (msgout->granted > msgout->length)
		msgout->granted = msgout->length;
	msgout->sched_priority = 0;
	
	/* Copy message data from user space and form packet buffers. */
	if (unlikely(len > HOMA_MAX_MESSAGE_LENGTH)) {
		err = -EINVAL;
		goto error;
	}
	for (bytes_left = len, last_link = &msgout->packets; bytes_left > 0;
			bytes_left -= HOMA_MAX_DATA_PER_PACKET) {
		struct data_header *h;
		__u32 cur_size = HOMA_MAX_DATA_PER_PACKET;
		if (likely(cur_size > bytes_left)) {
			cur_size = bytes_left;
		}
		skb = alloc_skb(HOMA_SKB_SIZE, GFP_KERNEL);
		if (unlikely(!skb)) {
			err = -ENOMEM;
			goto error;
		}
		skb_reserve(skb, HOMA_SKB_RESERVE);
		skb_reset_transport_header(skb);
		h = (struct data_header *) skb_put(skb, sizeof(*h));
		h->common.sport = htons(sport);
		h->common.dport = htons(dport);
		h->common.id = id;
		h->common.type = DATA;
		h->message_length = htonl(msgout->length);
		h->offset = htonl(msgout->length - bytes_left);
		h->unscheduled = htonl(msgout->unscheduled);
		h->cutoff_version = dest->cutoff_version;
		h->retransmit = 0;
		err = skb_add_data_nocache((struct sock *) hsk, skb, iter,
				cur_size);
		if (unlikely(err != 0)) {
			kfree_skb(skb);
			goto error;
		}
		*last_link = skb;
		last_link = homa_next_skb(skb);
		*last_link = NULL;
	}
	msgout->next_packet = msgout->packets;
	return 0;
	
    error:
	homa_message_out_destroy(msgout);
	return err;
}

/**
 * homa_message_out_reset() - Reset a homa_message_out to its initial state,
 * as if no packets had been sent. Data for the message is preserved.
 * @msgout:    Struct to reset. Must have been successfully initialized in
 *             the past, and some packets may have been transmitted since
 *             then.
 */
void homa_message_out_reset(struct homa_message_out *msgout)
{
	msgout->next_packet = msgout->packets;
	msgout->next_offset = 0;
	msgout->granted = msgout->unscheduled;
	if (msgout->granted > msgout->length)
		msgout->granted = msgout->length;
}

/**
 * homa_message_out_destroy() - Destructor for homa_message_out.
 * @msgout:       Structure to clean up.
 */
void homa_message_out_destroy(struct homa_message_out *msgout)
{
	struct sk_buff *skb, *next;
	if (msgout->length < 0)
		return;
	for (skb = msgout->packets; skb !=  NULL; skb = next) {
		next = *homa_next_skb(skb);
		kfree_skb(skb);
	}
	msgout->packets = NULL;
}

/**
 * homa_set_priority() - Arrange for a packet to have a VLAN header that
 * specifies a priority for the packet.
 * @skb:        The packet was priority should be set.
 * @priority:   Priority level for the packet, in the range 0 (for lowest
 *              priority) to 7 ( for highest priority).
 */
void homa_set_priority(struct sk_buff *skb, int priority)
{
	set_priority(skb, priority);
}

/**
 * homa_xmit_control() - Send a control packet to the other end of an RPC.
 * @type:      Packet type, such as DATA.
 * @contents:  Address of buffer containing the contents of the packet.
 *             Only information after the common header must be valid;
 *             the common header will be filled in by this function.
 * @length:    Length of @contents (including the common header).
 * @rpc:       The packet will go to the socket that handles the other end
 *             of this RPC. Addressing info for the packet, including all of
 *             the fields of common_header except type, will be set from this.
 * 
 * Return:     Either zero (for success), or a negative errno value if there
 *             was a problem.
 */
int homa_xmit_control(enum homa_packet_type type, void *contents,
	size_t length, struct homa_rpc *rpc)
{
	struct common_header *h = (struct common_header *) contents;
	h->type = type;
	if (rpc->is_client) {
		h->sport = htons(rpc->hsk->client_port);
	} else {
		h->sport = htons(rpc->hsk->server_port);
	}
	h->dport = htons(rpc->dport);
	h->id = rpc->id;
	return __homa_xmit_control(contents, length, rpc->peer, rpc->hsk);
}

/**
 * __homa_xmit_control() - Lower-level version of homa_xmit_control: sends
 * a control packet.
 * @contents:  Address of buffer containing the contents of the packet.
 *             The caller must have filled in all of the information,
 *             including the common header.
 * @length:    Length of @contents.
 * @peer:      Destination to which the packet will be sent.
 * @hsk:       Socket via which the packet will be sent.
 * 
 * Return:     Either zero (for success), or a negative errno value if there
 *             was a problem.
 */
int __homa_xmit_control(void *contents, size_t length, struct homa_peer *peer,
		struct homa_sock *hsk)
{
	struct common_header *h;
	int extra_bytes;
	int result;
	struct sk_buff *skb = alloc_skb(HOMA_SKB_SIZE, GFP_KERNEL);
	if (unlikely(!skb))
		return -ENOBUFS;
	skb_reserve(skb, HOMA_SKB_RESERVE);
	skb_reset_transport_header(skb);
	h = (struct common_header *) skb_put(skb, length);
	memcpy(h, contents, length);
	extra_bytes = HOMA_MAX_HEADER - length;
	if (extra_bytes > 0)
		memset(skb_put(skb, extra_bytes), 0, extra_bytes);
	set_priority(skb, hsk->homa->max_prio);
	dst_hold(peer->dst);
	skb_dst_set(skb, peer->dst);
	skb_get(skb);
	result = ip_queue_xmit((struct sock *) hsk, skb, &peer->flow);
	if (unlikely(result != 0)) {
		INC_METRIC(control_xmit_errors, 1);
		
		/* It appears that ip_queue_xmit frees skbuffs after
		 * errors; the following code is to raise an alert if
		 * this isn't actually the case. The extra skb_get above
		 * and kfree_skb below are needed to do the check
		 * accurately (otherwise the buffer could be freed and
		 * its memory used for some other purpose, resulting in
		 * a bogus "reference count").
		 */
		if (refcount_read(&skb->users) > 1)
			printk(KERN_NOTICE "ip_queue_xmit didn't free "
					"Homa control packet after error\n");
	}
	kfree_skb(skb);
	INC_METRIC(packets_sent[h->type - DATA], 1);
	return result;
}

/**
 * homa_xmit_data() - If an RPC has outbound data packets that are permitted
 * to be transmitted according to the scheduling mechanism, arrange for
 * them to be sent (some may be sent immediately; others will be sent
 * later by the pacer thread).
 * @rpc:    RPC to check for transmittable packets.
 */
void homa_xmit_data(struct homa_rpc *rpc)
{
	while ((rpc->msgout.next_offset < rpc->msgout.granted)
			&& rpc->msgout.next_packet) {
		int priority;
		struct sk_buff *skb = rpc->msgout.next_packet;
		struct data_header *h = (struct data_header *)
				skb_transport_header(skb);
		struct homa *homa = rpc->hsk->homa;
		
		if (((rpc->msgout.length - rpc->msgout.next_offset)
				> homa->throttle_min_bytes)
				&& ((get_cycles() + homa->max_nic_queue_cycles)
				< atomic_long_read(&homa->link_idle_time))
				&& !(homa->flags & HOMA_FLAG_DONT_THROTTLE)) {
			homa_add_to_throttled(rpc);
			return;
		}
		
		rpc->msgout.next_packet = *homa_next_skb(skb);
		if (rpc->msgout.next_offset < rpc->msgout.unscheduled) {
			priority = homa_unsched_priority(rpc->peer,
					rpc->msgout.length);
		} else {
			priority = rpc->msgout.sched_priority;
		}
		rpc->msgout.next_offset += HOMA_MAX_DATA_PER_PACKET;
		
		if (skb_shared(skb)) {
			/* The packet is still being transmitted due to a
			 * previous call to this function; no need to do
			 * anything here (and it may not be safe to retransmit
			 * it, or modify it, in this state).
			 */
			continue;
		}
		set_priority(skb, priority);
		
		/* Reset retransmit in case the packet was previously
		 * retransmitted but we're now restarting from the
		 * beginning.
		 */
		h->retransmit = 0;
		
		__homa_xmit_data(skb, rpc);
	}
}

/**
 * __homa_xmit_data() - Handles packet transmission stuff that is common
 * to homa_xmit_data and homa_resend_data.
 * @skb:    Packet to be sent. Will be freed, either by the underlying
 *          transmission code, or by this function if an error occurs.
 * @sk:     Socket over which to send the packet.
 * @peer:   Information about the packet's destination.
 */
void __homa_xmit_data(struct sk_buff *skb, struct homa_rpc *rpc)
{
	int err;
	struct data_header *h = (struct data_header *)
			skb_transport_header(skb);

	/* Update cutoff_version in case it has changed since the
	 * message was initially created.
	 */
	h->cutoff_version = rpc->peer->cutoff_version;

	skb_get(skb);
	
	/* Fill in the skb's dst if it isn't already set (for original
	 * transmission, it's never set already; for retransmits, it
	 * may or may not have been cleared by ip_queue_xmit, depending
	 * on IFF_XMIT_DST_RELEASE flag).
	 */
	if (skb_dst(skb) == NULL) {
		dst_hold(rpc->peer->dst);
		skb_dst_set(skb, rpc->peer->dst);
	}

	/* Strip headers in front of the transport header (needed if
	 * the packet is being retransmitted).
	 */
	if (skb_transport_offset(skb) > 0)
		skb_pull(skb, skb_transport_offset(skb));
	err = ip_queue_xmit((struct sock *) rpc->hsk, skb, &rpc->peer->flow);
	if (err) {
		INC_METRIC(data_xmit_errors, 1);
		
		/* It appears that ip_queue_xmit frees skbuffs after
		 * errors; the following code raises an alert if this
		 * isn't actually the case.
		 */
		if (refcount_read(&skb->users) > 1) {
			printk(KERN_NOTICE "ip_queue_xmit didn't free "
					"Homa data packet after error\n");
			kfree_skb(skb);
		}
	}
	homa_update_idle_time(rpc->hsk->homa,
			skb->tail - skb->transport_header);
	INC_METRIC(packets_sent[0], 1);
}

/**
 * homa_resend_data() - This function is invoked as part of handling RESEND
 * requests. It retransmits the packets containing a given range of bytes
 * from a message.
 * @msgout:   Message containing the packets.
 * @start:    Offset within @msgout of the first byte to retransmit.
 * @end:      Offset within @msgout of the byte just after the last one
 *            to retransmit.
 * @sk:       Socket to use for transmission.
 * @peer:     Information about the destination.
 * @priority: Priority level to use for the retransmitted data packets.
 */
void homa_resend_data(struct homa_rpc *rpc, int start, int end,
		int priority)
{
	struct sk_buff *skb;
	
	for (skb = rpc->msgout.packets; skb !=  NULL; skb = *homa_next_skb(skb)) {
		struct data_header *h = (struct data_header *)
				skb_transport_header(skb);
		int offset = ntohl(h->offset);
		
		if ((offset + HOMA_MAX_DATA_PER_PACKET) <= start)
			continue;
		if (offset >= end)
			break;
		/* See comments in homa_xmit_data for code below. */
		if (skb_shared(skb))
			continue;
		h->retransmit = 1;
		set_priority(skb, priority);
		__homa_xmit_data(skb, rpc);
		INC_METRIC(resent_packets, 1);
	}
}

/**
 * homa_outgoing_sysctl_changed() - Invoked whenever a sysctl value is changed;
 * any output-related parameters that depend on sysctl-settable values.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_outgoing_sysctl_changed(struct homa *homa)
{
	/* Code below is written carefully to avoid integer underflow or
	 * overflow under expected usage patterns. Be careful when changing!
	 */
	__u64 tmp;
	homa->cycles_per_kbyte = (8*(__u64) cpu_khz)/homa->link_mbps;
	tmp = homa->max_nic_queue_ns;
	tmp = (tmp*cpu_khz)/1000000;
	homa->max_nic_queue_cycles = tmp;
}

/**
 * homa_update_idle_time() - This function is invoked whenever a packet
 * is queued for transmission; it updates homa->link_idle_time to reflect
 * the new transmission.
 * @homa:    Overall data about the Homa protocol implementation.
 * @bytes:   Number of bytes in the packet that was just transmitted,
 *           not including IP or Ethernet headers.
 */
void homa_update_idle_time(struct homa *homa, int bytes)
{
	__u64 old_idle, new_idle, clock;
	int cycles_for_packet;
	
	bytes += HOMA_MAX_IPV4_HEADER + HOMA_VLAN_HEADER + HOMA_ETH_OVERHEAD;
	cycles_for_packet = (bytes*homa->cycles_per_kbyte)/1000;
	while (1) {
		clock = get_cycles();
		old_idle = atomic_long_read(&homa->link_idle_time);
		if (old_idle < clock)
			new_idle = clock + cycles_for_packet;
		else
			new_idle = old_idle + cycles_for_packet;
		if (atomic_long_cmpxchg_relaxed(&homa->link_idle_time, old_idle,
				new_idle) == old_idle)
			break;
	}
}

/**
 * homa_pacer_thread() - Top-level function for the pacer thread.
 * @transportInfo:  Pointer to struct homa.
 * @return:         Always 0.
 */
int homa_pacer_main(void *transportInfo)
{
	cycles_t start, now;
	struct homa *homa = (struct homa *) transportInfo;
	
	start = get_cycles();
	while (1) {
		if (homa->pacer_exit) {
			break;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_first_or_null_rcu(&homa->throttled_rpcs,
				struct homa_rpc, throttled_links) == NULL) {
			INC_METRIC(pacer_cycles, get_cycles() - start);
			schedule();
			start = get_cycles();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		homa_pacer_xmit(homa);
		now = get_cycles();
		INC_METRIC(pacer_cycles, now - start);
		start = now;
		
	}
	do_exit(0);
	return 0;
}

/**
 * homa_pacer_xmit() - Wait until we can send at least one packet from
 * the throttled list, then send as many packets as possible from the
 * highest priority message.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_pacer_xmit(struct homa *homa)
{
	struct homa_rpc *rpc;
	
	while ((get_cycles() + homa->max_nic_queue_cycles)
			< atomic_long_read(&homa->link_idle_time)) {}
	rcu_read_lock();
	rpc = list_first_or_null_rcu(&homa->throttled_rpcs, struct homa_rpc,
		throttled_links);
	if (rpc != NULL) {
		struct sock *sk = (struct sock *) rpc->hsk;
		bh_lock_sock_nested(sk);

		/* Once we've locked the socket we can release the RCU read
		 * lock: the socket can't go away now. */
		rcu_read_unlock();
		if (unlikely(sock_owned_by_user(sk))) {
			bh_unlock_sock(sk);
			return;
		}
		homa_xmit_data(rpc);
		if ((rpc->msgout.next_offset >= rpc->msgout.granted)
				|| !rpc->msgout.next_packet) {
			spin_lock_bh(&homa->throttle_lock);
			if (!list_empty(&rpc->throttled_links)) {
				list_del_rcu(&rpc->throttled_links);

				/* Note: this reinitialization is only safe
				 * because the pacer only looks at the first
				 * element of the list, rather than traversing
				 * it (and besides, we know the pacer isn't
				 * active concurrently, since this code *is*
				 * the pacer). It would not be safe under more
				 * general usage patterns.
				 */
				INIT_LIST_HEAD_RCU(&rpc->throttled_links);
			}
			spin_unlock_bh(&homa->throttle_lock);
		}
		bh_unlock_sock(sk);
	} else {
		rcu_read_unlock();
	}
}

/**
 * homa_pacer_stop() - Will cause the pacer thread to exit (waking it up
 * if necessary); doesn't return until after the pacer thread has exited.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_pacer_stop(struct homa *homa)
{
	homa->pacer_exit = true;
	wake_up_process(homa->pacer_kthread);
	kthread_stop(homa->pacer_kthread);
	homa->pacer_kthread = NULL;
}

/**
 * homa_add_to_throttled() - Make sure that an RPC is on the throttled list
 * and wake up the pacer thread if necessary.
 * @rpc:     RPC with outbound packets that have been granted but can't be
 *           sent because of NIC queue restrictions.
 */
void homa_add_to_throttled(struct homa_rpc *rpc)
{
	struct homa *homa = rpc->hsk->homa;
	struct homa_rpc *candidate;

	if (!list_empty(&rpc->throttled_links)) {
		return;
	}
	spin_lock_bh(&homa->throttle_lock);
	list_for_each_entry_rcu(candidate, &homa->throttled_rpcs,
			throttled_links) {
		if ((candidate->msgout.length - candidate->msgout.next_offset)
				> (rpc->msgout.length - rpc->msgout.next_offset)) {
			list_add_tail_rcu(&rpc->throttled_links,
					&candidate->throttled_links);
			goto done;
		}
	}
	list_add_tail_rcu(&rpc->throttled_links, &homa->throttled_rpcs);
done:
	spin_unlock_bh(&homa->throttle_lock);
	wake_up_process(homa->pacer_kthread);
}