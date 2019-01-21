#include "homa_impl.h"
#define KSELFTEST_NOT_MAIN 1
#include "kselftest_harness.h"
#include "ccutils.h"
#include "mock.h"
#include "utils.h"

FIXTURE(homa_outgoing) {
	__be32 client_ip;
	int client_port;
	__be32 server_ip;
	int server_port;
	__u64 rpcid;
	struct homa homa;
	struct homa_sock hsk;
	struct sockaddr_in server_addr;
};
FIXTURE_SETUP(homa_outgoing)
{
	self->client_ip = unit_get_in_addr("196.168.0.1");
	self->client_port = 40000;
	self->server_ip = unit_get_in_addr("1.2.3.4");
	self->server_port = 99;
	self->rpcid = 12345;
	homa_init(&self->homa);
	mock_cycles = 10000;
	atomic_long_set(&self->homa.link_idle_time, 10000);
	self->homa.cycles_per_kbyte = 1000;
	self->homa.flags |= HOMA_FLAG_DONT_THROTTLE;
	mock_sock_init(&self->hsk, &self->homa, self->client_port,
			self->server_port);
	self->server_addr.sin_family = AF_INET;
	self->server_addr.sin_addr.s_addr = self->server_ip;
	self->server_addr.sin_port = htons(self->server_port);
	unit_log_clear();
}
FIXTURE_TEARDOWN(homa_outgoing)
{
	homa_destroy(&self->homa);
	unit_teardown();
}

TEST_F(homa_outgoing, homa_message_out_init_basics)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	EXPECT_EQ(3000, crpc->msgout.granted);
	EXPECT_EQ(1, unit_list_length(&self->hsk.client_rpcs));
	EXPECT_STREQ("csum_and_copy_from_iter_full copied 1400 bytes; "
		"csum_and_copy_from_iter_full copied 1400 bytes; "
		"csum_and_copy_from_iter_full copied 200 bytes", unit_log_get());
	unit_log_clear();
	unit_log_message_out_packets(&crpc->msgout, 1);
	EXPECT_STREQ("DATA from 0.0.0.0:40000, dport 99, id 1, length 1428, "
			"message_length 3000, offset 0, unscheduled 10000, "
			"cutoff_version 0; "
		     "DATA from 0.0.0.0:40000, dport 99, id 1, length 1428, "
			"message_length 3000, offset 1400, unscheduled 10000, "
			"cutoff_version 0; "
		     "DATA from 0.0.0.0:40000, dport 99, id 1, length 228, "
			"message_length 3000, offset 2800, unscheduled 10000, "
			"cutoff_version 0",
		     unit_log_get());
}
TEST_F(homa_outgoing, homa_message_out_init__message_too_long)
{
	mock_alloc_skb_errors = 2;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 2000000, NULL);
	EXPECT_TRUE(IS_ERR(crpc));
	EXPECT_EQ(EINVAL, -PTR_ERR(crpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.client_rpcs));
}
TEST_F(homa_outgoing, homa_message_out_init__cant_alloc_skb)
{
	mock_alloc_skb_errors = 2;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_TRUE(IS_ERR(crpc));
	EXPECT_EQ(ENOMEM, -PTR_ERR(crpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.client_rpcs));
}
TEST_F(homa_outgoing, homa_message_out_init__cant_copy_data)
{
	mock_copy_data_errors = 2;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_TRUE(IS_ERR(crpc));
	EXPECT_EQ(EFAULT, -PTR_ERR(crpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.client_rpcs));
}

TEST_F(homa_outgoing, homa_message_out_reset)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk, RPC_OUTGOING,
		self->client_ip, self->server_ip, self->server_port,
		1111, 3000, 100);
	EXPECT_FALSE(IS_ERR(crpc));
	homa_xmit_data(crpc);
	EXPECT_EQ(4200, crpc->msgout.next_offset);
	crpc->msgout.granted = 0;
	homa_message_out_reset(&crpc->msgout);
	EXPECT_EQ(0, crpc->msgout.next_offset);
	EXPECT_EQ(3000, crpc->msgout.granted);
	EXPECT_EQ(crpc->msgout.packets, crpc->msgout.next_packet);
}

TEST_F(homa_outgoing, homa_set_priority)
{
	struct sk_buff *skb = alloc_skb(HOMA_SKB_SIZE, GFP_KERNEL);
	homa_set_priority(skb, 0);
	EXPECT_EQ(1, (skb->vlan_tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT);
	
	homa_set_priority(skb, 1);
	EXPECT_EQ(0, (skb->vlan_tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT);
	
	homa_set_priority(skb, 7);
	EXPECT_EQ(7, (skb->vlan_tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT);
	kfree_skb(skb);
}

TEST_F(homa_outgoing, homa_xmit_control__server_request)
{
	struct homa_rpc *srpc;
	struct grant_header h;
	
	srpc = unit_server_rpc(&self->hsk, RPC_INCOMING, self->client_ip,
		self->server_ip, self->client_port, 1111, 10000, 10000);
	EXPECT_NE(NULL, srpc);
	
	h.offset = htonl(12345);
	h.priority = 4;
	mock_xmit_log_verbose = 1;
	EXPECT_EQ(0, homa_xmit_control(GRANT, &h, sizeof(h), srpc));
	EXPECT_STREQ("xmit GRANT from 0.0.0.0:99, dport 40000, id 1111, "
			"length 48 prio 7, offset 12345, grant_prio 4",
			unit_log_get());
}
TEST_F(homa_outgoing, homa_xmit_control__client_response)
{
	struct homa_rpc *crpc;
	struct grant_header h;
	
	crpc = unit_client_rpc(&self->hsk, RPC_INCOMING, self->client_ip,
		self->server_ip, self->server_port, 1111, 100, 10000);
	EXPECT_NE(NULL, crpc);
	unit_log_clear();
	
	h.offset = htonl(12345);
	h.priority = 4;
	mock_xmit_log_verbose = 1;
	EXPECT_EQ(0, homa_xmit_control(GRANT, &h, sizeof(h), crpc));
	EXPECT_STREQ("xmit GRANT from 0.0.0.0:40000, dport 99, id 1111, "
			"length 48 prio 7, offset 12345, grant_prio 4",
			unit_log_get());
}

TEST_F(homa_outgoing, __homa_xmit_control__cant_alloc_skb)
{
	struct homa_rpc *srpc;
	struct grant_header h;
	
	srpc = unit_server_rpc(&self->hsk, RPC_INCOMING, self->client_ip,
		self->server_ip, self->client_port, 1111, 10000, 10000);
	EXPECT_NE(NULL, srpc);
	
	h.common.type = GRANT;
	h.offset = htonl(12345);
	h.priority = 4;
	mock_xmit_log_verbose = 1;
	mock_alloc_skb_errors = 1;
	EXPECT_EQ(ENOBUFS, -__homa_xmit_control(&h, sizeof(h), srpc->peer,
			&self->hsk));
	EXPECT_STREQ("", unit_log_get());
}
TEST_F(homa_outgoing, __homa_xmit_control__pad_packet)
{
	struct homa_rpc *srpc;
	struct grant_header h;
	
	srpc = unit_server_rpc(&self->hsk, RPC_INCOMING, self->client_ip,
		self->server_ip, self->client_port, 1111, 10000, 10000);
	EXPECT_NE(NULL, srpc);
	
	h.offset = htonl(12345);
	h.priority = 4;
	mock_xmit_log_verbose = 1;
	EXPECT_EQ(0, homa_xmit_control(GRANT, &h, sizeof(h), srpc));
	EXPECT_SUBSTR("length 48", unit_log_get());
}
TEST_F(homa_outgoing, __homa_xmit_control__ip_queue_xmit_error)
{
	struct homa_rpc *srpc;
	struct grant_header h;
	
	srpc = unit_server_rpc(&self->hsk, RPC_INCOMING, self->client_ip,
		self->server_ip, self->client_port, 1111, 10000, 10000);
	EXPECT_NE(NULL, srpc);
	
	h.offset = htonl(12345);
	h.priority = 4;
	mock_xmit_log_verbose = 1;
	mock_ip_queue_xmit_errors = 1;
	EXPECT_EQ(ENETDOWN, -homa_xmit_control(GRANT, &h, sizeof(h), srpc));
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(1, unit_get_metrics()->control_xmit_errors);
}

TEST_F(homa_outgoing, homa_xmit_data__basics)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 6000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	crpc->msgout.sched_priority = 2;
	crpc->msgout.unscheduled = 2000;
	crpc->msgout.granted = 5000;
	homa_peer_set_cutoffs(crpc->peer, INT_MAX, 0, 0, 0, 0, INT_MAX,
			7000, 0);
	unit_log_clear();
	homa_xmit_data(crpc);
	EXPECT_STREQ("xmit DATA 0/6000 P6; "
		"xmit DATA 1400/6000 P6; "
		"xmit DATA 2800/6000 P2; "
		"xmit DATA 4200/6000 P2", unit_log_get());
}
TEST_F(homa_outgoing, homa_xmit_data__below_throttle_min)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 200, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	atomic_long_set(&self->homa.link_idle_time, 11000);
	self->homa.max_nic_queue_cycles = 500;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	homa_xmit_data(crpc);
	EXPECT_STREQ("xmit DATA 0/200 P6", unit_log_get());
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("", unit_log_get());
}
TEST_F(homa_outgoing, homa_xmit_data__throttle_limit)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 6000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	atomic_long_set(&self->homa.link_idle_time, 11000);
	self->homa.max_nic_queue_cycles = 3000;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	homa_xmit_data(crpc);
	EXPECT_STREQ("xmit DATA 0/6000 P6; "
		"xmit DATA 1400/6000 P6", unit_log_get());
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 1, next_offset 2800", unit_log_get());
}
TEST_F(homa_outgoing, homa_xmit_data__skip_shared_skbuffs)
{
	struct sk_buff *skb;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 5000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	skb = *homa_next_skb(crpc->msgout.packets);
	skb_get(skb);
	skb_get(*homa_next_skb(skb));
	unit_log_clear();
	homa_xmit_data(crpc);
	EXPECT_STREQ("xmit DATA 0/5000 P6; xmit DATA 4200/5000 P6", 
			unit_log_get());
	EXPECT_EQ(5600, crpc->msgout.next_offset);
	kfree_skb(skb);
	kfree_skb(*homa_next_skb(skb));
}

TEST_F(homa_outgoing, __homa_xmit_data__update_cutoff_version)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 1000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	crpc->peer->cutoff_version = htons(123);
	mock_xmit_log_verbose = 1;
	unit_log_clear();
	homa_xmit_data(crpc);
	EXPECT_SUBSTR("cutoff_version 123", unit_log_get());
}
TEST_F(homa_outgoing, __homa_xmit_data__fill_dst)
{
	int old_refcount;
	struct dst_entry *dst;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 1000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	dst = crpc->peer->dst;
	old_refcount = dst->__refcnt.counter;
	
	/* First transmission: must fill dst. */
	homa_xmit_data(crpc);
	EXPECT_STREQ("xmit DATA 0/1000 P6", unit_log_get());
	EXPECT_EQ(dst, skb_dst(crpc->msgout.packets));
	EXPECT_EQ(old_refcount+1, dst->__refcnt.counter);
	
	/* Second transmission: dst already set. */
	unit_log_clear();
	__homa_xmit_data(crpc->msgout.packets, crpc);
	EXPECT_STREQ("xmit DATA 0/1000 P6", unit_log_get());
	EXPECT_EQ(old_refcount+1, dst->__refcnt.counter);
}
TEST_F(homa_outgoing, __homa_xmit_data__strip_old_headers)
{
	struct sk_buff *skb;
	unsigned char *old_data;
	int old_len;
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 1000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	skb = crpc->msgout.next_packet;
	old_data = skb->data;
	old_len = skb->len;
	__skb_push(skb, 10);
	homa_xmit_data(crpc);
        EXPECT_EQ(old_data, skb->data);
	EXPECT_EQ(old_len, skb->len);
}
TEST_F(homa_outgoing, __homa_xmit_data__transmit_error)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 1000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	mock_ip_queue_xmit_errors = 1;
	homa_xmit_data(crpc);
	EXPECT_EQ(1, unit_get_metrics()->data_xmit_errors);
}
TEST_F(homa_outgoing, __homa_xmit_data__update_idle_time)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 500 - sizeof(struct data_header)
			- HOMA_MAX_IPV4_HEADER - HOMA_VLAN_HEADER
			- HOMA_ETH_OVERHEAD, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	atomic_long_set(&self->homa.link_idle_time, 9000);
	self->homa.max_nic_queue_cycles = 100000;
	homa_xmit_data(crpc);
	EXPECT_EQ(10500, atomic_long_read(&self->homa.link_idle_time));
}

TEST_F(homa_outgoing, homa_resend_data__basics)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	homa_resend_data(crpc, 1000, 5000, 5);
	EXPECT_STREQ("xmit DATA retrans 0/10000 P5; "
			"xmit DATA retrans 1400/10000 P5; "
			"xmit DATA retrans 2800/10000 P5; "
			"xmit DATA retrans 4200/10000 P5", unit_log_get());
	
	
	unit_log_clear();
	homa_resend_data(crpc, 1400, 2800, 7);
	EXPECT_STREQ("xmit DATA retrans 1400/10000 P7", unit_log_get());
	EXPECT_EQ(5, unit_get_metrics()->resent_packets);
	EXPECT_EQ(5, unit_get_metrics()->packets_sent[0]);
}
TEST_F(homa_outgoing, homa_resend_data__skip_shared_skbuffs)
{
	struct homa_rpc *crpc = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	unit_log_clear();
	skb_get(crpc->msgout.packets);
	homa_resend_data(crpc, 1000, 5000, 5);
	EXPECT_STREQ("xmit DATA retrans 1400/10000 P5; "
			"xmit DATA retrans 2800/10000 P5; "
			"xmit DATA retrans 4200/10000 P5", unit_log_get());
	kfree_skb(crpc->msgout.packets);
}

TEST_F(homa_outgoing, homa_outgoing_sysctl_changed)
{
	self->homa.link_mbps = 10000;
	homa_outgoing_sysctl_changed(&self->homa);
	EXPECT_EQ(800, self->homa.cycles_per_kbyte);
	
	self->homa.link_mbps = 1000;
	homa_outgoing_sysctl_changed(&self->homa);
	EXPECT_EQ(8000, self->homa.cycles_per_kbyte);
	
	self->homa.link_mbps = 40000;
	homa_outgoing_sysctl_changed(&self->homa);
	EXPECT_EQ(200, self->homa.cycles_per_kbyte);
	
	self->homa.max_nic_queue_ns = 200;
	cpu_khz = 2000000;
	homa_outgoing_sysctl_changed(&self->homa);
	EXPECT_EQ(400, self->homa.max_nic_queue_cycles);
}

TEST_F(homa_outgoing, homa_update_idle_time)
{
	atomic_long_set(&self->homa.link_idle_time, 10000);
	mock_cycles = 5000;
	homa_update_idle_time(&self->homa, 1000);
	EXPECT_EQ(11104, atomic_long_read(&self->homa.link_idle_time));
	
	atomic_long_set(&self->homa.link_idle_time, 10000);
	mock_cycles = 20000;
	homa_update_idle_time(&self->homa, 200);
	EXPECT_EQ(20304, atomic_long_read(&self->homa.link_idle_time));
}

/* Don't know how to unit test homa_pacer_main... */

TEST_F(homa_outgoing, homa_pacer_xmit__basics)
{
	struct homa_rpc *crpc1 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 5000, NULL);
	struct homa_rpc *crpc2 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	struct homa_rpc *crpc3 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 15000, NULL);
	EXPECT_FALSE(IS_ERR(crpc1));
	EXPECT_FALSE(IS_ERR(crpc2));
	EXPECT_FALSE(IS_ERR(crpc3));
	homa_add_to_throttled(crpc1);
	homa_add_to_throttled(crpc2);
	homa_add_to_throttled(crpc3);
	self->homa.max_nic_queue_cycles = 2000;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	unit_log_clear();
	homa_pacer_xmit(&self->homa);
	EXPECT_STREQ("xmit DATA 0/5000 P6; xmit DATA 1400/5000 P6",
		unit_log_get());
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 1, next_offset 2800; "
		"request 2, next_offset 0; "
		"request 3, next_offset 0", unit_log_get());
}
TEST_F(homa_outgoing, homa_pacer_xmit__queue_empty)
{
	self->homa.max_nic_queue_cycles = 2000;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	unit_log_clear();
	homa_pacer_xmit(&self->homa);
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("", unit_log_get());
}
TEST_F(homa_outgoing, homa_pacer_xmit__socket_locked)
{
	struct homa_rpc *crpc1 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 5000, NULL);
	EXPECT_FALSE(IS_ERR(crpc1));
	homa_add_to_throttled(crpc1);
	self->homa.max_nic_queue_cycles = 2000;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	self->hsk.inet.sk.sk_lock.owned = 1;
	unit_log_clear();
	homa_pacer_xmit(&self->homa);
	homa_pacer_xmit(&self->homa);
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 1, next_offset 0", unit_log_get());
}
TEST_F(homa_outgoing, homa_pacer_xmit__remove_from_queue)
{
	struct homa_rpc *crpc1 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 1000, NULL);
	struct homa_rpc *crpc2 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	EXPECT_FALSE(IS_ERR(crpc1));
	EXPECT_FALSE(IS_ERR(crpc2));
	homa_add_to_throttled(crpc1);
	homa_add_to_throttled(crpc2);
	self->homa.max_nic_queue_cycles = 2000;
	self->homa.flags &= ~HOMA_FLAG_DONT_THROTTLE;
	unit_log_clear();
	homa_pacer_xmit(&self->homa);
	EXPECT_STREQ("xmit DATA 0/1000 P6", unit_log_get());
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 2, next_offset 0", unit_log_get());
	EXPECT_TRUE(list_empty(&crpc1->throttled_links));
}

/* Don't know how to unit test homa_pacer_stop... */

TEST_F(homa_outgoing, homa_add_to_throttled)
{
	struct homa_rpc *crpc1 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	struct homa_rpc *crpc2 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 5000, NULL);
	struct homa_rpc *crpc3 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 15000, NULL);
	struct homa_rpc *crpc4 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 12000, NULL);
	struct homa_rpc *crpc5 = homa_rpc_new_client(&self->hsk,
			&self->server_addr, 10000, NULL);
	EXPECT_FALSE(IS_ERR(crpc1));
	EXPECT_FALSE(IS_ERR(crpc5));
	
	/* Basics: add one RPC. */
	homa_add_to_throttled(crpc1);
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 1, next_offset 0", unit_log_get());
	
	/* Check priority ordering. */
	homa_add_to_throttled(crpc2);
	homa_add_to_throttled(crpc3);
	homa_add_to_throttled(crpc4);
	homa_add_to_throttled(crpc5);
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 2, next_offset 0; "
		"request 1, next_offset 0; "
		"request 5, next_offset 0; "
		"request 4, next_offset 0; "
		"request 3, next_offset 0", unit_log_get());
	
	/* Don't reinsert if already present. */
	homa_add_to_throttled(crpc1);
	unit_log_clear();
	unit_log_throttled(&self->homa);
	EXPECT_STREQ("request 2, next_offset 0; "
		"request 1, next_offset 0; "
		"request 5, next_offset 0; "
		"request 4, next_offset 0; "
		"request 3, next_offset 0", unit_log_get());
}