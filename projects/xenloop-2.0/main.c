/*
 *  XenLoop -- A High Performance Inter-VM Network Loopback 
 *
 *  Installation and Usage instructions
 *
 *  Authors: 
 *  	Jian Wang - Binghamton University (jianwang@cs.binghamton.edu)
 *  	Kartik Gopalan - Binghamton University (kartik@cs.binghamton.edu)
 *
 *  Copyright (C) 2007-2009 Kartik Gopalan, Jian Wang
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/genhd.h>

#include <xen/hypercall.h>
#include <xen/driver_util.h>
#include <xen/gnttab.h>
#include <xen/evtchn.h>
#include <xen/xenbus.h>

#include <net/sock.h>
#include <net/inet_sock.h>
#include <net/protocol.h>
#include <linux/in.h>
#include <linux/skbuff.h>
#include <net/neighbour.h>
#include <net/dst.h>
#include <linux/if_ether.h>
#include <net/inet_common.h>
#include <linux/inetdevice.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/genhd.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include "main.h"
#include "debug.h"
#include "bififo.h"
#include "maptable.h"


extern int 	init_hash_table(HashTable *, char *);  
extern void 	clean_table(HashTable *);
extern void 	insert_table(HashTable *, void *, u8); 
extern void*	lookup_table(HashTable *, void *); 
extern void     update_table(HashTable *,u8 *, int);
extern void	mark_suspend(HashTable *);
extern int	has_suspend_entry(HashTable *);
extern void	clean_suspended_entries(HashTable * ht);
extern void 	notify_all_bfs(HashTable * ht);
extern void	check_timeout(HashTable * ht);

static domid_t my_domid;
static u8 my_macs[MAX_MAC_NUM][ETH_ALEN];
static u8 num_of_macs = 0;
static u8 freezed = 0;
struct net_device *NIC = NULL;
static int if_drops = 0;
static int if_over = 0;
static int if_fifo = 0;
static int if_total = 0;
static skb_queue_t out_queue;
static skb_queue_t pending_free;

static int xenloop_connect(message_t *msg, Entry *e); 
static int xenloop_listen(Entry *e);
static struct task_struct *suspend_thread = NULL;
DECLARE_WAIT_QUEUE_HEAD(swq);
static struct task_struct *pending_thread = NULL;
DECLARE_WAIT_QUEUE_HEAD(pending_wq);

HashTable mac_domid_map;


static int  write_xenstore(int status)
{
	int err = 1;

	err = xenbus_printf(XBT_NIL, "xenloop", "xenloop","%d", status);
        if (err) {
		EPRINTK( "writing xenstore xenloop status failed, err = %d \n", err);
	}
	return err;
}

static domid_t get_my_domid(void) 
{
	char *domidstr;
	domid_t domid;

	domidstr = xenbus_read(XBT_NIL, "domid", "", NULL);
	if ( IS_ERR(domidstr) ) {
		EPRINTK("xenbus_read error\n");
		return PTR_ERR(domidstr);
	}

	domid = (domid_t) simple_strtoul(domidstr, NULL, 10);

	kfree(domidstr);

	return domid;
}


int store_mac(char* mac) 
{
	char *pEnd = mac;
	int i;

	for (i=0; i < (ETH_ALEN-1); i++) {
		my_macs[(int)num_of_macs][i] = simple_strtol(pEnd, &pEnd, 16);
		pEnd++;
	}

	my_macs[(int)num_of_macs][ETH_ALEN-1] = simple_strtol(pEnd, NULL, 16);
	
	num_of_macs++;
	
	return 0;
}



static int probe_vifs(void)
{
        int err = 0;
        char **dir;
	char * path, *macstr;
        unsigned int i, dir_n;

        dir = xenbus_directory(XBT_NIL, "device/vif", "", &dir_n);
        if (IS_ERR(dir))
                return PTR_ERR(dir);

        for (i = 0; i < dir_n; i++) {

		path = kasprintf(GFP_KERNEL, "device/vif/%s", dir[i]);

		if (!path) {
			EPRINTK("kasprintf failed dir[%d]=%s \n", i, dir[i]);
			err = -ENOMEM;
			goto out;
		}

		macstr = xenbus_read(XBT_NIL, path, "mac", NULL);
		if ( IS_ERR(macstr) ) {
			EPRINTK("xenbus_read error path=%s \n", path);
			err = PTR_ERR(macstr);
			kfree(path);
			goto out;
		}

		store_mac(macstr);
		DB("device/vif/%s/mac path=%s ==> %s\n", dir[i], path, macstr);

		
		kfree(macstr);
		kfree(path);
	}

out:
	kfree(dir);
	return err;
}

void session_update(message_t* msg) 
{
	int i, found = 0;
	u8 mac_count = msg->mac_count;
	Entry * e;

	for(i=0; i<mac_count; i++) {
		if (memcmp(msg->mac[i], my_macs[0], ETH_ALEN) == 0) {
			found = 1;
			break;
		}
	}
	if (!found) return;
	
	for(i=0; i<mac_count; i++) {
		if (memcmp(msg->mac[i], my_macs[0], ETH_ALEN) == 0)
			continue;

		if (!(e = lookup_table(&mac_domid_map, msg->mac[i]))) {

			insert_table(&mac_domid_map, msg->mac[i], msg->guest_domids[i]);

			DPRINTK("Added one new guest mac = " MAC_FMT  " Domid=%d.\n", \
			   MAC_NTOA(msg->mac[i]), msg->guest_domids[i]);

		} else 
			e->timestamp = jiffies;
	}

	update_table(&mac_domid_map, (u8*)msg->mac, msg->mac_count);
}

Entry *pre_check_msg(message_t *msg)
{
	Entry *e = NULL;
	if(msg->mac_count > 1){
		EPRINTK("warning more than one mac\n");
	}

	if (!(e = lookup_table(&mac_domid_map, msg->mac[0]))) {
		EPRINTK("lookup table failed\n");
	}

	return e;
}

int session_recv(struct sk_buff * skb, net_device * dev, packet_type * pt, net_device * d)
{
	int ret = NET_RX_SUCCESS;
	message_t * msg = NULL;
	Entry *e;
	
	TRACE_ENTRY;

	BUG_ON(!skb);

	msg = (message_t *)skb->data;
	BUG_ON(!msg);
	
	skb_linearize(skb);
	
	switch(msg->type) {
		case XENLOOP_MSG_TYPE_SESSION_DISCOVER:
			if (!freezed)
				session_update(msg);
			break;
		case XENLOOP_MSG_TYPE_CREATE_CHN:
			e = pre_check_msg(msg);
			if(!e)	goto out;

			ret = xenloop_connect(msg, e);
			break;
		case XENLOOP_MSG_TYPE_CREATE_ACK:
			e = pre_check_msg(msg);
			if(!e)	goto out;
			
			e->status = XENLOOP_STATUS_CONNECTED;
			if (e->ack_timer)
				del_timer_sync(e->ack_timer);
			DPRINTK("LISTENER status changed to XENLOOP_STATUS_CONNECTED!!!\n");
			break;
		default:
			EPRINTK("session_recv(): unknown msg type %d\n", msg->type);
	}
	
out:		
	kfree_skb(skb);
	TRACE_EXIT;
	return ret; 
}


static packet_type xenloop_ptype = {
	.type		= __constant_htons(ETH_P_TIDC),
	.func 		= session_recv,
	.dev 		= NULL,
	.af_packet_priv = NULL,
};



inline void net_send(struct sk_buff * skb, u8 * dest) 
{
	ethhdr * eth;
	int ret;

	skb->nh.raw = skb->data; 

	skb->len = headers;
	skb->data_len = 0;
	skb_shinfo(skb)->nr_frags 	= 0;
	skb_shinfo(skb)->frag_list 	= NULL;
	skb->tail = skb->data + headers;

	skb->dev 	= NIC;
	skb->protocol 	= htons(ETH_P_TIDC);
	eth 		= (ethhdr *) skb->data;
	eth->h_proto 	= htons(ETH_P_TIDC);
	memcpy(eth->h_dest, dest, ETH_ALEN);
	
	memcpy(eth->h_source, NIC->dev_addr, ETH_ALEN);

	if((skb_shinfo(skb) == NULL)) {
		WARN_ON(1);
		TRACE_ERROR;
	}

	SKB_LINEAR_ASSERT(skb);
	
	
	if((ret = dev_queue_xmit(skb))) {
		DB("Non-zero return code: %d %s", ret,
		   skb_shinfo(skb) ? "good" : "bad");

		if_drops++;
		TRACE_ERROR;
	}


}


void send_create_chn_msg(int gref_in, int gref_out, int remote_port, u8 *dest_mac) 
{
	message_t *m;
	struct sk_buff *skb;

	TRACE_ENTRY;

	skb = alloc_skb(headers, GFP_ATOMIC);
	BUG_ON(!skb);

	m = (message_t *) (skb->data + LINK_HDR);

	memset(m, 0, MSGSIZE);
	m->type = XENLOOP_MSG_TYPE_CREATE_CHN;
	m->domid= my_domid;
	m->mac_count = num_of_macs;
	memcpy(m->mac, my_macs, num_of_macs*ETH_ALEN);
	m->gref_in = gref_in;
	m->gref_out = gref_out;
	m->remote_port = remote_port;
 
	net_send(skb, dest_mac);

	TRACE_EXIT;
}

void send_create_ack_msg(u8 *dest_mac) 
{
	message_t *m;
	struct sk_buff *skb;

	TRACE_ENTRY;

	BUG_ON(!(skb = alloc_skb(headers, GFP_ATOMIC)));

	m = (message_t *) (skb->data + LINK_HDR);

	memset(m, 0, MSGSIZE);
	m->type = XENLOOP_MSG_TYPE_CREATE_ACK;
	m->domid= my_domid;
	m->mac_count = num_of_macs;
	memcpy(m->mac, my_macs, num_of_macs*ETH_ALEN);
 
	net_send(skb, dest_mac);

	TRACE_EXIT;
}

static void ack_timeout(ulong data) 
{
	Entry *e = (void *)data;
	bf_handle_t *bfl;

	TRACE_ENTRY;

	BUG_ON(!e);
	BUG_ON(!e->listen_flag);

	
	if( e->status == XENLOOP_STATUS_CONNECTED ) 
		return;

	BUG_ON(e->status != XENLOOP_STATUS_LISTEN);

 	bfl = e->bfh;
	BUG_ON(!bfl);

	if(e->retry_count < MAX_RETRY_COUNT ) {
		
		send_create_chn_msg(BF_GREF_IN(bfl),		\
					BF_GREF_OUT(bfl),	\
					BF_EVT_PORT(bfl),	\
					e->mac);
		e->retry_count++;
		mod_timer(e->ack_timer, jiffies + XENLOOP_ACK_TIMEOUT*HZ);
	} else {
		if (check_descriptor(e->bfh)) {
			BF_SUSPEND_IN(e->bfh) = 1;
			BF_SUSPEND_OUT(e->bfh) = 1;
		}
		e->status = XENLOOP_STATUS_SUSPEND;
		wake_up_interruptible(&swq);
	}

	TRACE_EXIT;
}



static int xenloop_listen(Entry *e) 
{
	static DEFINE_SPINLOCK(listen_lock);
	unsigned long flag;
	domid_t remote_domid = e->domid; 
	bf_handle_t *bfl = NULL;
	int i;
	bf_data_t *pbf;

	TRACE_ENTRY;

	spin_lock_irqsave(&listen_lock, flag);

	if( e->status != XENLOOP_STATUS_INIT) {
		spin_unlock_irqrestore(&listen_lock, flag);
		TRACE_EXIT;
		return 0;
	}

	e->status = XENLOOP_STATUS_LISTEN;

	spin_unlock_irqrestore(&listen_lock, flag);


	
	bfl = bf_create(remote_domid, XENLOOP_ENTRY_ORDER); 
	if(!bfl) {
		e->status = XENLOOP_STATUS_INIT; 

		EPRINTK("bf_creat failed\n");
		TRACE_ERROR;
		return -1;
	}

	for(i=0; i<=xf_size(bfl->in); i++) {
		pbf = xf_entry(bfl->in, bf_data_t, i);
		pbf->status = BF_FREE;
	}
	for(i=0; i<=xf_size(bfl->out); i++) {
		pbf = xf_entry(bfl->out, bf_data_t, i);
		pbf->status = BF_FREE;
	}

	e->listen_flag = 1;
	e->bfh = bfl;

	
	send_create_chn_msg(BF_GREF_IN(bfl),
				BF_GREF_OUT(bfl),
				BF_EVT_PORT(bfl),
				e->mac);

	
	e->ack_timer = kmalloc(sizeof(struct timer_list), GFP_ATOMIC);
	BUG_ON(!e->ack_timer);
	init_timer(e->ack_timer);
	e->ack_timer->function	= ack_timeout;
	e->ack_timer->expires	= jiffies + XENLOOP_ACK_TIMEOUT*HZ;
	e->ack_timer->data	= (unsigned long)e;
	add_timer(e->ack_timer);

	TRACE_EXIT;
	return 0;
}

static int xenloop_connect(message_t *msg, Entry *e) 
{
	domid_t remote_domid = e->domid; 
	bf_handle_t *bfc = NULL;

	TRACE_ENTRY;

	BUG_ON(!msg);

	if(e->status == XENLOOP_STATUS_CONNECTED) {
		send_create_ack_msg(e->mac);
		TRACE_EXIT;
		return 0;
	}

	
	if(msg->gref_in <= 0 || msg->gref_out <= 0 || msg->remote_port <= 0) {
		EPRINTK("gref_in %d gref_out %d remote_port %d\n", msg->gref_in, msg->gref_out, msg->remote_port);
		goto err;
	}

	
	bfc = bf_connect(remote_domid, msg->gref_out, msg->gref_in,\
				 msg->remote_port);
	if(!bfc) {
		EPRINTK("bf_connect failed\n");
		goto err;
	}

	e->listen_flag = 0;
	e->bfh = bfc;

	e->status = XENLOOP_STATUS_CONNECTED;
	DPRINTK("CONNECTOR status changed to XENLOOP_STATUS_CONNECTED!!!\n");

	
	send_create_ack_msg(e->mac);

	TRACE_EXIT;
	return 0;
err:
	TRACE_ERROR;
	return -1;
}



static int xmit_large_pkt(struct sk_buff *skb, xf_handle_t *xfh)
{
	bf_data_t *mdata;
	char *pback, *pfront, *pfifo;
	int num_entries, ret, len=0, len1=0, len2=0;

	TRACE_ENTRY;
	BUG_ON(!skb);
	BUG_ON(!xfh);

	if( skb->len + sizeof(bf_data_t) > xf_free(xfh)*sizeof(bf_data_t) ) {
		TRACE_EXIT;
		return -1;
	}

	mdata  = xf_entry(xfh, bf_data_t, xf_size(xfh));
	BUG_ON(!mdata);

	mdata->status = BF_WAITING;
	mdata->type = BF_PACKET;
	mdata->pkt_info = skb->len; 

	num_entries = skb->len/sizeof(bf_data_t);
	if (skb->len % sizeof(bf_data_t)) 
		num_entries++;

	pfifo = (char *)xfh->fifo;
	pfront = (char *)xf_entry(xfh, bf_data_t, 0);
	pback = (char *) xf_entry(xfh, bf_data_t, xf_size(xfh) + 1);

	BUG_ON(!pfifo);
	BUG_ON(!pfront);
	BUG_ON(!pback);

	if( pback >= pfront ) {
		len1 = (pfifo + xfh->descriptor->max_data_entries*sizeof(bf_data_t)) - pback;
		len = (len1 >= skb->len) ? skb->len : len1;
		if(skb_copy_bits(skb, 0, pback, len))
			BUG();

		len2 = skb->len - len;
		if( len2  > 0 ) {
			if(skb_copy_bits(skb, len, pfifo, len2))
				BUG();
		}
	} else {
		if(skb_copy_bits(skb, 0, pback, skb->len))
			BUG();
	}

	ret = xf_pushn(xfh, num_entries + 1);
	BUG_ON( ret < 0 );

	TRACE_EXIT;

	return 0;
}

void enqueue(skb_queue_t *Q, struct sk_buff *skb)
{
	if (Q->count++ == 0) {
		Q->head = skb;
		Q->tail = skb;
	} else {
		Q->tail->next = skb;
		Q->tail = skb;
	}
}

void dequeue(skb_queue_t *Q)
{
	if (Q->head != Q->tail)
		Q->head = Q->head->next;
	if (--Q->count == 0) {
		Q->head = NULL;
		Q->tail = NULL;
	}
}

void clean_pending(skb_queue_t *Q) {
	struct sk_buff *skb;
	while (Q->count > 0) {
		skb = Q->head;
		dequeue(Q);
		kfree_skb(skb);
	}
}


inline int xmit_packets(struct sk_buff *skb)
{
	static DEFINE_SPINLOCK(xmit_lock);
	int ret = 0;
	unsigned long flags;

	TRACE_ENTRY;

	BUG_ON( in_irq() );

	spin_lock_irqsave( &xmit_lock, flags );

	if(skb) {
		if ( skb->len + sizeof(bf_data_t) < (1 << XENLOOP_ENTRY_ORDER)*sizeof(bf_data_t) ) 
			enqueue(&out_queue, skb);
		else {
			DB("Packet size greater than total fifo size\n");
			ret = -1;
		}
	}

	while (out_queue.count > 0) {
		int rc;
		Entry *e;

		skb = out_queue.head;
		BUG_ON(!skb);

		e = lookup_table(&mac_domid_map, skb->dst->neighbour->ha);
		BUG_ON(!e);

		rc = xmit_large_pkt(skb, e->bfh->out);

		if (rc < 0) {
			bf_notify(e->bfh->port);
			wake_up_interruptible(&pending_wq);
			break;
		}

		dequeue(&out_queue);

		kfree_skb(skb);
	}

	spin_unlock_irqrestore( &xmit_lock, flags );

	notify_all_bfs(&mac_domid_map);

	TRACE_EXIT;
	return ret;
}



static unsigned int iphook_out(
	unsigned int hooknum, 
	struct sk_buff **pskb,
	const struct net_device *in,
	const struct net_device *out,
	int (*okfn)(struct sk_buff *))
{
	Entry * e;
	int ret = NF_ACCEPT;
	struct sk_buff *skb= *pskb;
        struct dst_entry *dst = skb->dst;
        struct neighbour *neigh = dst->neighbour;

	if_total++;
	if (skb->len > 32768*8)  if_over++;

	
        if (!dst || !neigh) {
		return NF_ACCEPT;
	}
        

	if (!(neigh->nud_state & (NUD_CONNECTED|NUD_DELAY|NUD_PROBE) )) {
		return NF_ACCEPT;
	}
	
	if (!(e = lookup_table(&mac_domid_map, neigh->ha))) {
		return NF_ACCEPT;
	}

	TRACE_ENTRY;
		
	if (check_descriptor(e->bfh) && (BF_SUSPEND_IN(e->bfh) || BF_SUSPEND_OUT(e->bfh))) {
		e->status = XENLOOP_STATUS_SUSPEND;
		wake_up_interruptible(&swq);
		return NF_ACCEPT;
	}

	switch (e->status) {
		case  XENLOOP_STATUS_INIT:
			if( my_domid < e->domid)  {
				xenloop_listen(e);
			}

			TRACE_EXIT;
			return NF_ACCEPT;

		case XENLOOP_STATUS_CONNECTED:
			if( xmit_packets(skb) < 0  ) {
				EPRINTK("Couldn't send packet via bififo. Using network instead\n");
				ret = NF_ACCEPT;
				goto out;
			}
			if_fifo++;
			ret = NF_STOLEN;
			break;

		case XENLOOP_STATUS_LISTEN:
		default:
			TRACE_EXIT;
			return ret;
	}
out:
	TRACE_EXIT;
	return ret;
}



static unsigned int iphook_in(
	unsigned int hooknum, 
	struct sk_buff **pskb,
	const struct net_device *in,
	const struct net_device *out,
	int (*okfn)(struct sk_buff *))
{

	Entry * e;
	int ret = NF_ACCEPT;
	struct sk_buff *skb= *pskb;
	u8 *src_mac = eth_hdr(skb)->h_source;

	if (!(e = lookup_table(&mac_domid_map, src_mac))) {
		return ret;
	}

	if ((e->status == XENLOOP_STATUS_INIT) && (my_domid < e->domid))
		xenloop_listen(e);

	TRACE_EXIT;

        return NF_ACCEPT;
}



struct nf_hook_ops iphook_in_ops = {
	.hook = iphook_in,
	.owner = THIS_MODULE,
	.pf = PF_INET,
	.hooknum = NF_IP_PRE_ROUTING,
	.priority = 10, 
};
struct nf_hook_ops iphook_out_ops = {
	.hook = iphook_out,
	.owner = THIS_MODULE,
	.pf = PF_INET,
	.hooknum = NF_IP_POST_ROUTING,
	.priority = 10, 
};


int net_init(void) 
{

	int ret = 0, i;
	char nic[5];

	TRACE_ENTRY;

	for ( i=0; i<5; i++) {
		sprintf(nic, "eth%d", i);
		NIC = dev_get_by_name(nic);
		if(NIC) break;
	}
	if(!NIC) {
		EPRINTK("Could not find network card %s\n", nic);
		ret = -ENODEV;
		goto out;
	}

	DB("Using interface %s, MTU: %d bytes\n", NIC->name, NIC->mtu);
	
	ret = nf_register_hook(&iphook_out_ops);
	if (ret < 0) {
		EPRINTK("can't register OUT hook.\n");
		goto out;
	} 
	ret = nf_register_hook(&iphook_in_ops);
	if (ret < 0) {
		EPRINTK("can't register OUT hook.\n");
		goto out;
	}


	dev_add_pack(&xenloop_ptype);

out:
	TRACE_EXIT;
	return ret;
}

void net_exit(void) 
{
	TRACE_ENTRY;

	dev_remove_pack(&xenloop_ptype);

	nf_unregister_hook(&iphook_in_ops);
	nf_unregister_hook(&iphook_out_ops);

	if(NIC) dev_put(NIC);

	TRACE_EXIT;
}

void pre_migration(void)
{
	TRACE_ENTRY;

	write_xenstore(0);
	freezed = 1;
	mark_suspend(&mac_domid_map);

	wake_up_interruptible(&swq);
	TRACE_EXIT;
	return;
}

void post_migration(void)
{
	TRACE_ENTRY;

	freezed = 0;
	write_xenstore(1);
	
	TRACE_EXIT;
	return;
}


#define LONG_PENDING_TIMEOUT 1 // seconds
#define SHORT_PENDING_TIMEOUT 1 // jiffies
static int xmit_pending(void *useless)
{
	unsigned long timeout;
	TRACE_ENTRY;
	
	while(!kthread_should_stop()) {
		timeout = out_queue.count ? SHORT_PENDING_TIMEOUT : LONG_PENDING_TIMEOUT*HZ;
		wait_event_interruptible_timeout(pending_wq, (out_queue.count > 0), timeout);
		//if (out_queue.count > 0)
		xmit_packets(NULL);
	}
	TRACE_EXIT;
	return 0;
}

#define SUSPEND_TIMEOUT 5
static int check_suspend(void *useless)
{
	int ret;
	TRACE_ENTRY;
	
	while(!kthread_should_stop()) {
		ret = wait_event_interruptible_timeout(swq, has_suspend_entry(&mac_domid_map), SUSPEND_TIMEOUT*HZ);
		if (ret > 0) {
			clean_suspended_entries(&mac_domid_map);
		} else if (ret == 0) {
			check_timeout(&mac_domid_map);
		}
	}
	TRACE_EXIT;
	return 0;
}


static void suspend_resume_handler(struct xenbus_watch *watch,
                             const char **vec, unsigned int len)
{
        char **dir;
        unsigned int i, dir_n;

#define SR_UNDEFINED 0
#define SR_SUSPENDED 1
#define SR_RESUMED 2
	static int cur_state = SR_UNDEFINED;
	int prev_state = cur_state;


	if( prev_state == SR_UNDEFINED ) {
		cur_state = SR_RESUMED;
		return;
	}

        dir = xenbus_directory(XBT_NIL, "control", "", &dir_n);
        if (IS_ERR(dir)) {
		EPRINTK("ERROR\n");
		return;
	}
                

	cur_state = SR_RESUMED;
        for (i = 0; i < dir_n; i++) {

		if (strcmp(dir[i], "shutdown") != 0)
			continue;

		cur_state = SR_SUSPENDED;
		break;
	}

	if( prev_state == cur_state) 
		goto out; 

	switch(cur_state)  {

	case SR_SUSPENDED:
		pre_migration();
		break;

	case SR_RESUMED:
		post_migration();
		break;

	}

out:
	kfree(dir);
}


static struct xenbus_watch suspend_resume_watch = {
        .node = "control/shutdown",
        .callback = suspend_resume_handler
};

static void __exit xenloop_exit(void) 
{
	
	TRACE_ENTRY;
	
	write_xenstore(0);
	freezed = 1;

	if(pending_thread)
		kthread_stop(pending_thread);

	mark_suspend(&mac_domid_map);

	if(suspend_thread)
		kthread_stop(suspend_thread);
	
	unregister_xenbus_watch(&suspend_resume_watch);

	net_exit();

	clean_table(&mac_domid_map);

	DPRINTK("Exiting xenloop module.\n");
	TRACE_EXIT;
}


static int __init xenloop_init(void) 
{
	int rc = 0;

	TRACE_ENTRY;

	out_queue.head = NULL;
	out_queue.tail = NULL;
	out_queue.count = 0;

	pending_free.head = NULL;
	pending_free.tail = NULL;
	pending_free.count = 0;
	
	
	if(init_hash_table(&mac_domid_map, "MAC_DOMID_MAP_Table") != 0) {
		rc = -ENOMEM;
		goto out;
	}

	my_domid = get_my_domid();
	probe_vifs();

	if ((rc = net_init()) < 0) {
		EPRINTK("session_init(): net_init failed\n");
		clean_table(&mac_domid_map);
		goto out;
	}

	write_xenstore(1);

	rc = register_xenbus_watch(&suspend_resume_watch);
        if (rc) {
                EPRINTK("Failed to set shutdown watcher\n");
        }

	pending_thread = kthread_run(xmit_pending, NULL, "pending");
	if(!pending_thread) {
		xenloop_exit();
		rc = -1;
		goto out;
	}

	suspend_thread = kthread_run(check_suspend, NULL, "suspend");
	if(!suspend_thread) {
		xenloop_exit();
		rc = -1;
		goto out;
	}

	DPRINTK("XENLOOP successfully initialized!\n");
	
out:
	TRACE_EXIT;
	return rc;
}

module_init(xenloop_init);
module_exit(xenloop_exit);

MODULE_LICENSE("GPL");
