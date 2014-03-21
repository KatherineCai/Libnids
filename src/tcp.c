/*
  Copyright (c) 1999 Rafal Wojtczuk <nergal@7bulls.com>. All rights reserved.
  See the file COPYING for license details.
 */

#include <config.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>

#include "checksum.h"
#include "scan.h"
#include "tcp.h"
#include "util.h"
#include "nids.h"
#include "hash.h"

// ������������tcp�ĺ˵ĸ���
#define TCP_CORE_NUM 3

#if ! HAVE_TCP_STATES
enum
{
    TCP_ESTABLISHED = 1,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,
    TCP_CLOSING			/* now a valid state */
};

#endif

#define FIN_SENT 120
#define FIN_CONFIRMED 121
#define COLLECT_cc 1
#define COLLECT_sc 2
#define COLLECT_ccu 4
#define COLLECT_scu 8

#define EXP_SEQ (snd->first_data_seq + rcv->count + rcv->urg_count)

// ��libnids�ж���Ļص���������
extern struct proc_node *tcp_procs;

// ����һ��hash��������������tcp����
static struct tcp_stream **tcp_stream_table;
// ��¼hash���С
static int tcp_stream_table_size;
// ����������������tcp_stream�ڵ�,�����Ƿ�free
static struct tcp_stream *streams_pool[TCP_CORE_NUM];
// ��¼��ǰ����free_streams�⻹�м���tcp�ڵ㣬��¼��Ծ��tcp����
static int tcp_num[TCP_CORE_NUM];
// ��¼���������ٸ�tcp,����ֵӦ���� tcp_stream_table_size * 3 /4
static int max_stream;
// tcp_latest����ָ���������tcp�ڵ㣬tcp_oldest����ָ�����Ƚ�����tcp�ڵ�
// ע�⣬tcp�ڵ㻹��һ��time����
// �������ĳ�ʼ����Ҫ��tcp_int������ִ��
static struct tcp_stream *tcp_latest[TCP_CORE_NUM], *tcp_oldest[TCP_CORE_NUM];
// ������е�tcp�ڵ�
static struct tcp_stream *free_streams[TCP_CORE_NUM];
// ����ԭʼ��ipͷ
static struct ip *ugly_iphdr[TCP_CORE_NUM];
// ����һ����ʱ���У�������ô˹��ܣ�ÿ��tcp���Ĵλ��ֺ󲻻������ͷţ�
// ���Ǽ���ö���ֱ����ʱ���ö����ǰ��ճ�ʱ��ʱ���������еġ�
struct tcp_timeout *nids_tcp_timeouts = 0;


/**
	���:
		h : ��Ҫ�����İ����ӡ�

	����:
		�������İ������е�listȫ�������

	ע��:
		�������������prune_queue�����ǳ����ơ�
		�Ǹ������������������Ǹ���������list���˵�����½�list��ա�
		���һ���þ���������������Ҫ����һ��this_tcphdr��Ϊ������

	- Comment by shashibici 2014/03/07
**/
static void purge_queue(struct half_stream * h)
{
	struct skbuff *tmp, *p = h->list;

	// ������ж���
	while (p)
	{
		free(p->data);
		tmp = p->next;
		free(p);
		p = tmp;
	}

	// ����Ϊ��
	h->list = h->listtail = 0;
	// listռ�õ���ʵ��СҲ��Ϊ0
	h->rmem_alloc = 0;
}


static void
add_tcp_closing_timeout(struct tcp_stream * a_tcp)
{
	struct tcp_timeout *to;
	struct tcp_timeout *newto;

	if (!nids_params.tcp_workarounds)
		return;
	newto = malloc(sizeof (struct tcp_timeout));
	if (!newto)
		nids_params.no_mem("add_tcp_closing_timeout");
	newto->a_tcp = a_tcp;
	newto->timeout.tv_sec = nids_last_pcap_header->ts.tv_sec + 10;
	newto->prev = 0;

	// Ѱ�Ҳ��ͷ�
	for (newto->next = to = nids_tcp_timeouts; to; newto->next = to = to->next)
	{
		if (to->a_tcp == a_tcp)
		{
			free(newto);
			return;
		}
		if (to->timeout.tv_sec > newto->timeout.tv_sec)
			break;
		newto->prev = to;
	}
	if (!newto->prev)
		nids_tcp_timeouts = newto;
	else
		newto->prev->next = newto;
	if (newto->next)
		newto->next->prev = newto;
}


static void
del_tcp_closing_timeout(struct tcp_stream * a_tcp)
{
	struct tcp_timeout *to;

	if (!nids_params.tcp_workarounds)
		return;
	// ��[nids_tcp_timeouts]��Ѱ�Ұ���[a_tcp]�Ľڵ�
	for (to = nids_tcp_timeouts; to; to = to->next)
	{
		if (to->a_tcp == a_tcp)
			break;
	}
	if (!to)
		return;
	if (!to->prev)
		nids_tcp_timeouts = to->next;
	else
		to->prev->next = to->next;
	if (to->next)
		to->next->prev = to->prev;
	free(to);
}


/**
	����:
		a_tcp:  ��Ҫ�ͷŵ�tcp�ڵ�

	˵��:
		��������Ϊ��6���貽�� -- ���list����table��ժ�¡����data����time��ժ�¡�
		ɾ������listener����β����
		1) ִ�к���purge_queue��������еȴ�ȷ�ϵ�tcp����(list������)
		2) ��[tcp_stream_table]��tcp�ڵ�ժ������ʹ֮���ڻ�Ծ
		3) �ͷ�data�ռ�(����Ѿ�ȷ���˵İ����tcp����)
		4) ���ڵ��time������ժ����
		5) ɾ����tcp������listener
		6) �����tcp�ڵ���ص�[free_streams]����ͷ����ʾ��������һ�����еĽڵ��ˡ�

	- Comment by shashibici 2014/03/21
	
**/
void
nids_free_tcp_stream(struct tcp_stream * a_tcp)
{
	int hash_index = a_tcp->hash_index;
	int icore = a_tcp->icore;
	// ע��: ����Ĵ�����ʾ��lurker_node��ʵ�ʹ�����a_tcp��һ��listener.
	// (�����������ڶ�����)
	struct lurker_node *i, *j;

	// ����Ĭ�ϲ���ִ�У���Ϊû��ʹ��workaround����
	del_tcp_closing_timeout(a_tcp);
	// ������ո�tcp���˵�list���У�Ҳ���ǰ���Щ�ȴ�ȷ�ϵı������
	purge_queue(&a_tcp->server);
	purge_queue(&a_tcp->client);

	// ����ǰnodeɾ��������һ��node��prevָ��ָ��ǰnode��ǰһ��node
	if (a_tcp->next_node)
	{
		a_tcp->next_node->prev_node = a_tcp->prev_node;
	}
	// ����ǰnodeɾ��������һ��node��nextָ��ǰnode����һ��node
	if (a_tcp->prev_node)
	{
		a_tcp->prev_node->next_node = a_tcp->next_node;
	}
	else
	{
		// ���atcp->prev_node�ǿյģ�˵���Ѿ�������ͷ������һ��hash����
		tcp_stream_table[hash_index] = a_tcp->next_node;
	}
	// �ͷ�data������ݣ������ݰ������Ѿ�ȷ�ϵ�tcp����
	if (a_tcp->client.data)
	{
		free(a_tcp->client.data);
	}
	if (a_tcp->server.data)
	{
		free(a_tcp->server.data);
	}

	// ��a_tcp��time������ժ����
	if (a_tcp->next_time)
	{
		a_tcp->next_time->prev_time = a_tcp->prev_time;
	}
	if (a_tcp->prev_time)
	{
		a_tcp->prev_time->next_time = a_tcp->next_time;
	}

	// �޸�tcp_latest tcp_oldesָ��
	if (a_tcp == tcp_oldest[icore])
	{
		tcp_oldest[icore] = a_tcp->prev_time;
	}
	if (a_tcp == tcp_latest[icore])
	{
		tcp_latest[icore] = a_tcp->next_time;
	}
	// �ͷ�����a_tcp��listeners
	i = a_tcp->listeners;
	while (i)
	{
		j = i->next;
		free(i);
		i = j;
	}

	// ��a_tcp�ҵ�free_streams��[��ͷ]
	a_tcp->next_free = free_streams[icore];
	free_streams[icore] = a_tcp;

	// ��ǰtcp����-1
	tcp_num[icore]--;
}


/**
	����:
		now :  �ող���İ���ʱ���

	˵��:
		- ���÷���: tcp_check_timeouts(&hdr->ts);
		- ����ʱ��: ��pcapץ��һ������֮����������ô˺�����

		- ����������Ϊ��libnids��[��С����ʱ�䵥λ]��[һ��pcap��ʱ����].
		  Ҳ����˵��ֻ�е�libnids��pcap�����յ�һ����֮��Ż���libnids
		  Ŀǰ��ά���ĸ���tcp�ļ�ʱ��(��Щ��ʱ����nids_tcp_tmieoutsָ��).
		  ������������þ�����libnidsִ������һ������������libnidsֻ��Ҫ��
		  ���ʵ�ʱ������������������ˡ�
		
		- ���������ִ��˼·��:
			-- ���Ƚ���ǰ����ʱ��[now.tv_sec]������[nids_tcp_timeouts]��\
			   �����ڵ��[timeout.tv_sec]��Ƚ�.
			-- ����[nids_tcp_timeouts]�еĽڵ��ǰ���[timeout.tv_sec]�ؼ���
			   �������У�������һ��[timeout.tv_sec] > [now.tv_sec]ʱ�Ϳ���
			   ȷ��֮������нڵ��[timeout.tv_sec]����С��[now.tv_sec]������
			   ����Է����ˡ�
			-- �������[timeout.tv_sec] <= [now.tv_sec]�Ľڵ㣬˵������ڵ�
			   ��Ӧ��tcp�Ѿ���ʱ��(now.tv_sec����ľ��ǵ�ǰʱ��--��ǰ����ʱ��)��
			   ��ʱ��Ҫ����ʱ��tcp�ͷŵ�(�ͷ�֮ǰ����һ��listeners).

		- ��nids_tcp_timeouts�����:
			--  [nids_tcp_timeouts]�����б������һ������[timeout]�ڵ㡣
				��Щ�ڵ���[add_tcp_closing_timeout]�����б���ӵ�
				[nids_tcp_timeouts]�����ϣ���������ķ�ʽ����(����forѭ��).
			--  �ɼ�[nids_tcp_timeouts]�����м�¼�Ķ����Ѿ������Ĵλ���
				��׼���ͷ��˵�tcp�����ǡ�
			--  Ϊʲô��Ҫ����ôһ��[nids_tcp_timeouts]������ֱ���ͷŵ�
				�Ѿ�����Ĵλ��ֵ�һ��tcp��?
			--  ���Ĵλ��ֺ�����ɾ��tcp�ǿ��ǵ��ڲ��õĽ�����client��server
				���ܻ��ٴ�������һ��һģһ����tcp(��ַ��Ԫ����ȫһ����tcp)��
				���˶���.

	- Comment by shashibici 2014/03/21
			
**/
void
tcp_check_timeouts(struct timeval *now)
{

	// tcp_timeout�ṹ����һ�������������е�������a_tcp,Ȼ����pre��next
	// ����������
	struct tcp_timeout *to;
	struct tcp_timeout *next;
	struct lurker_node *i;

	// ����nids_tcp_timeouts����
	for (to = nids_tcp_timeouts; to; to = next)
	{
		// �����ǰ����ʱ��ֵû�г���[to]�м�¼��timeoutֵ��ֱ�ӷ���
		if (now->tv_sec < to->timeout.tv_sec)
			return;
		// ����[to]��Ӧ��tcp״̬����Ϊ[NIDS_TIMED_OUT]
		to->a_tcp->nids_state = NIDS_TIMED_OUT;
		// ��[to]����Ӧ��tcp������listenersִ��һ��Ȼ���ͷ�[to]��Ӧ��tcp
		for (i = to->a_tcp->listeners; i; i = i->next)
			(i->item) (to->a_tcp, &i->data);
		next = to->next;
		nids_free_tcp_stream(to->a_tcp);
	}
}


// ����һ����Ԫ��ĵ�ַ������һ��hash������
// ΪʲôҪ����hash����? ��Ϊͬһ����Ԫ����Թ�������tcp����Щtcp����֯��һ��hash������
static int
mk_hash_index(struct tuple4 addr)
{
	int hash=mkhash(addr.saddr, addr.source, addr.daddr, addr.dest);
	return hash % tcp_stream_table_size;
}


// ���tcp��ͷ��һЩ��Ϣ
static int get_ts(struct tcphdr * this_tcphdr, unsigned int * ts)
{
	int len = 4 * this_tcphdr->th_off;
	unsigned int tmp_ts;
	unsigned char * options = (unsigned char*)(this_tcphdr + 1);
	int ind = 0, ret = 0;
	while (ind <=  len - (int)sizeof (struct tcphdr) - 10 )
		switch (options[ind])
		{
		case 0: /* TCPOPT_EOL */
			return ret;
		case 1: /* TCPOPT_NOP */
			ind++;
			continue;
		case 8: /* TCPOPT_TIMESTAMP */
			memcpy((char*)&tmp_ts, options + ind + 2, 4);
			*ts=ntohl(tmp_ts);
			ret = 1;
			/* no break, intentionally */
		default:
			if (options[ind+1] < 2 ) /* "silly option" */
				return ret;
			ind += options[ind+1];
		}

	return ret;
}


// ��ñ�ͷ��һЩ��Ϣ
static int get_wscale(struct tcphdr * this_tcphdr, unsigned int * ws)
{
	int len = 4 * this_tcphdr->th_off;
	unsigned int tmp_ws;
	unsigned char * options = (unsigned char*)(this_tcphdr + 1);
	int ind = 0, ret = 0;
	*ws=1;
	while (ind <=  len - (int)sizeof (struct tcphdr) - 3 )
		switch (options[ind])
		{
		case 0: /* TCPOPT_EOL */
			return ret;
		case 1: /* TCPOPT_NOP */
			ind++;
			continue;
		case 3: /* TCPOPT_WSCALE */
			tmp_ws=options[ind+2];
			if (tmp_ws>14)
				tmp_ws=14;
			*ws=1<<tmp_ws;
			ret = 1;
			/* no break, intentionally */
		default:
			if (options[ind+1] < 2 ) /* "silly option" */
				return ret;
			ind += options[ind+1];
		}

	return ret;
}


/**
	����:
		this_tcphdr: ָ��һ��tcp����
		this_iphdr:  ָ��һ��ip���ģ��ñ����Ѿ��ɶ��ip�������
		icore:       ָ����ǰ��һ���˴����tcp

	˵��:
		
**/
static void
add_new_tcp(struct tcphdr * this_tcphdr, struct ip * this_iphdr, int icore)
{
	struct tcp_stream *tolink;
	struct tcp_stream *a_tcp;
	int hash_index;
	struct tuple4 addr;

	// ���õ�ַ���hashֵ
	addr.source = ntohs(this_tcphdr->th_sport);
	addr.dest = ntohs(this_tcphdr->th_dport);
	addr.saddr = this_iphdr->ip_src.s_addr;
	addr.daddr = this_iphdr->ip_dst.s_addr;
	
	hash_index = mk_hash_index(addr);

	// ���tcp����������
	if (tcp_num[icore] > max_stream)
	{
		// ����һ��a_tcp�е�listener
		struct lurker_node *i;
		// �������ϵ�tcp��client��״̬
		int orig_client_state=tcp_oldest[icore]->client.state;
		// �������ϵ�tcpΪ[ʱ�䵽�ˣ������ٴ���]��
		tcp_oldest[icore]->nids_state = NIDS_TIMED_OUT;
		// ����ִ��������ϵ�tcp�е�����listener����
		for (i = tcp_oldest[icore]->listeners; i; i = i->next)
		{
			(i->item) (tcp_oldest[icore], &i->data);
		}
		// �����ϵ�tcp�ͷ���(��Ȼ����Ҫ�޸�time�����)�������������tcp_num--
		nids_free_tcp_stream(tcp_oldest[icore]);
		// ���������ϵ�tcp��clientû�гɹ����͹�syn����ôserver�ض�û��
		// TCP_CLOSING.��ʱ��Ҫ��������
		if (orig_client_state!=TCP_SYN_SENT)
		{
			// tcp̫����!
			// FIXME: Ϊʲô����[this_iphdr]?
			nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_TOOMUCH, ugly_iphdr[icore], this_tcphdr);
		}
	}

	// ���free_streams����ͷ
	a_tcp = free_streams[icore];
	if (!a_tcp)
	{
		fprintf(stderr, "gdb me ...\n");
		pause();
	}
	// ��free_streamsͷ�ڵ�ȡ���������ҽ���ȡ�����Ľڵ����a_tcp��
	free_streams[icore] = a_tcp->next_free;
	// tcp_num��������
	tcp_num[icore]++;

	// �ҵ�hash����
	tolink = tcp_stream_table[hash_index];
	// ��a_tcp��ָ�ڴ���գ�Ҳ���ǽ��մ�[free_streams]����ͷժ�����Ľڵ����
	memset(a_tcp, 0, sizeof(struct tcp_stream));
	// ��ʼ��һ��tcp����
	a_tcp->hash_index = hash_index;
	a_tcp->addr = addr;
	// client��������
	a_tcp->client.state = TCP_SYN_SENT;
	// ����client�˵ķ�����ţ�Ϊ��һ����Ҫ���͵���ţ�Ҳ��serverӦ��ACK�����
	a_tcp->client.seq = ntohl(this_tcphdr->th_seq) + 1;
	// ����ͻ��˵�һ�����к�
	a_tcp->client.first_data_seq = a_tcp->client.seq;
	// ��¼client���ڴ�С
	a_tcp->client.window = ntohs(this_tcphdr->th_win);
	// ���ts
	a_tcp->client.ts_on = get_ts(this_tcphdr, &a_tcp->client.curr_ts);
	// ���wscale
	a_tcp->client.wscale_on = get_wscale(this_tcphdr, &a_tcp->client.wscale);
	// ���÷�������Ϊclose
	a_tcp->server.state = TCP_CLOSE;
	// ���ô���ĺ�
	a_tcp->icore = icore;

	// ��a_tcp����hash���Ӧ��������ͷ��������ӵ�hash��
	a_tcp->next_node = tolink;
	a_tcp->prev_node = 0;
	// ��¼��ǰʱ�䣬[nids_last_pcap_header]���Ǹմ�pcap����õİ�
	// ��tcp�ĵ�һ�����ĵ�ʱ������Ϊ���tcp��ʱ��
	a_tcp->ts = nids_last_pcap_header->ts.tv_sec;
	// �����hash���Ѿ���tcp�ڵ��ˣ���ô���ص���ǰ��
	if (tolink)
	{
		tolink->prev_node = a_tcp;
	}
	tcp_stream_table[hash_index] = a_tcp;

	/**
		ע��:
			������һ�δ����ڶ��߳������ºܿ��ܻ�������⡣
			�����Ҫ����һ������

			struct tcp_stream * tcp_latest[TCP_CORE_NUM]
			struct tcp_stream * tcp_oldest[TCP_CORE_NUM]

			ÿһ��tcp_latest��tcp_oldest��ָ�������ǲ�ͬ�ģ�һ������һ��
	**/
	// ��ӵ�time������
	a_tcp->next_time = tcp_latest[icore];
	a_tcp->prev_time = 0;
	// ���oldest�ǿգ���ô����ǰ�������ϵģ����������ϵ�
	if (!tcp_oldest[icore])
		tcp_oldest[icore] = a_tcp;
	// ���latest��Ϊ�գ���ô��latestǰ���Ǹ���Ϊ�ղż�������
	if (tcp_latest[icore])
		(tcp_latest[icore])->prev_time = a_tcp;
	// �ղż���ģ���Ϊlatest
	tcp_latest[icore] = a_tcp;
}


/**
	���:
		rcv    : ���Ľ����ߣ�һ��tcp������
		data   : ָ����Ҫ����buffer������
		datalen: ��Ҫ����buffer�����ݳ���

	����:
		�����㹻�Ŀռ䣬
		��������data��ָ�򳤶�Ϊdatalen�����ݣ�������������Ŀռ��С�
		����ռ���rcv->data��ָ��

	ע��: 
		��������޸���rcv->count��rcv->count_new �� rcv->bufsize
		�����κα�����û���޸ģ�����rcv->offset �� rcv->urg_ptr��
		
	- Comment by shashibici 2014/03/07
	
**/
static void
add2buf(struct half_stream * rcv, char *data, int datalen)
{
	int toalloc;

	// cout - offset ǡ�õ��ڵ�ǰdata�д��ڵ��ֽ���
	// ��������datalen�������ֽ�������Ҫ��� rcv��buffer�Ƿ񹻴�
	// ���������,��Ҫ�������
	if (datalen + rcv->count - rcv->offset > rcv->bufsize)
	{
		// �����û�и�dataָ�����ռ�(��ֻ�����ڸտ�ʼʱ)
		if (!rcv->data)
		{
			// �����ǰ������Ҫ���������
			// С��2048�͵���2048,�����������
			if (datalen < 2048)
				toalloc = 4096;
			else
				toalloc = datalen * 2;
			rcv->data = malloc(toalloc);
			rcv->bufsize = toalloc;
		}
		// �����Ѿ�������,�ⷢ�����յ��������ĵ����
		else
		{
			/* ����ռ������Կ���������һ��"�����Ƶ����㷨" */
			
			// ���������Ҫ��������ݣ��ȵ�ǰ���ܴ�СҪС��
			// ��ôֻ��Ҫ�ڷ��䵱ǰ��ô��Ļ���ռ伴�ɣ�
			// ������£���ʣ��Щ��ռ�������һ��������
			if (datalen < rcv->bufsize)
			{
				toalloc = 2 * rcv->bufsize;
			}
			// ������Ҫ�������Ŀռ䡣
			else
			{
				toalloc = rcv->bufsize + 2*datalen;
			}
			// realloc���·���,
			// ����ѿռ��㹻��ֱ����ԭ�ռ�ά׷��;�����������ɿռ䣬�������ͷ�ԭ�ռ�
			rcv->data = realloc(rcv->data, toalloc);
			rcv->bufsize = toalloc;
		}
		// ���û�з���ɹ�
		if (!rcv->data)
			nids_params.no_mem("add2buf");
	}

	// ���򹻴�ֱ��ִ������
	// (count-offset)��data�����е��������� data+(count-offset)��������������ĩβλ��
	// �����ǽ����������ݣ��ӵ�ĩβ
	memcpy(rcv->data + rcv->count - rcv->offset, data, datalen);
	// �޸ĸոյ��������ݵļ�����:count_new
	rcv->count_new = datalen;
	rcv->count += datalen;

	/*  ע��: ���������û���޸� offset ���������ֻ���޸��� count �������*/
	
}


/**
	���:
		a_tcp  : һ��tcp����
		mask   : ����һ���Ǻţ�����ֵֻ���������һ��
				{	
					COLLECT_cc  = 00000001B, 
					COLLECT_sc  = 00000010B,
					COLLECT_ccu = 0000100B,
					COLLECT_scu = 0001000B
				}

	����:

	 1��������ôһ��Ӧ�ó���:
	 	
	 	- ��һ���������ּ��ϵͳ�У�������ÿһ�ι������᲻�ɱ���ز���һ��
	 	  ��������(����urg��ǵı���)��
	 	- ���ϵͳΪ����ƽ����ʱ����ټ�⿪��������������Щ����urg�ı��ģ�
	 	  ���������ı��Ĳ������ռ�������
	 	- ���ϵͳ���յ�urg����֮�󣬻�����ñ��ĵ��������������ĳ��������
	 	  �Ϳ����жϷ��������繥����
	 	- һ���жϷ��������繤�ߣ����ϵͳΪ���ռ������ߵĸ�����Ϣ�����뽫
	 	  �����е�����tcp���Ķ��ռ���������������һ���ķ�������

	 2��������ĳ����У�Ӧ�ó���(���ǿɳ�֮Ϊ"����")���ܿ��԰�����˼·ʵ��:

		- ÿ����������һ���µ�tcp���ӽ���ʱ��������ִ�����
				a_tcp->client.collect_urg++;
				a_tcp->server.collect_urg++;
	 	  ȷ��libnids��Ϊ��������tcp���������а���urg��ǵı���
	 	- ����һ��������libnids�ϴ���������ʱ���������ж��������ʱ�����
	 	  urg��ǡ��������urg��ǣ���ô�����������������ݰ������������Ƿ���Ҫ
	 	  �������������������rug��ǣ�˵������һ�������İ����������ʴ�ʱһ��������
	 	  �Ѿ������˾���(���������Ժ�libnids�ŻὫ�����İ��͸�����)����ʱ��Ҫ����
	 	  ��������İ�����һ��ȷ��������������
		- ����������������������һ����������������urg���ĺ󣬻�ִ�����������������
				a_tcp->client.collect++;
				a_tcp->server.collect++;
				alarm = true;
		- ��һ�ι�����в�����֮������Ӧ�ý���������ָ���������������urg�ı���
		  ��״̬��������ִ������������������
		  		a_tcp->client.collect--;
		  		a_tcp->server.collect--;
		  		alarm = false;
		- ֮�������ظ��������������������������urg�ı���

	 3��������ĳ����У�libnids�е�ride_lurkers����������������?

	  	- ÿ���յ�һ������a_tcp���ӵ�tcp���ģ�ride_lurkers�ͻᱻ����һ�Ρ�
	  	- �ر���Ҫע��ڶ�������mask,������������ride_lurkers�����õ�ʱ��
	  	  ��λ�õĲ�ͬ������ͬ������libnids��ȷ�յ�һ������urg��ǵı���ʱ��
	  	  �������������е���
	  	  		mask = COLLECT_ccu;
	  	  		ride_lurkers(a_tcp, mask);
	  	  �������˵����libnids��ʱ�յ���һ�����ģ����������client,�����������
	  	  ��һ��������urg��ǵı��ģ�ride_lurkers����Ҫ���ľ���ȥ���е�ע�ắ����
	  	  ��һ�£�����һ��ע�ắ����whatto������mask(�����м�COLLECT_ccu)��һ�µģ�
	  	  ����ҵ����͵������������
	  	- ride_lurkers�ڴ���ĳһ��ע�ắ����whatto��־��ʱ�򣬲��õ���"��򿪹�"����
	  	  ���whatto��ĳһλ���ó�1����������ע�ắ��ϣ������ĳһ���Ͷ�Ӧ�ı��ġ�
	  	  �������ϸ������У�
	  	       maskΪCOLLECT_ccu(0x04),����whatto���룬����whatto�ĵ�������λΪ1
	  	       ��ʱ��������true,���ܹ�ִ��if.
	  	  ���whatto��COLLECT_ccu(0x04)�����ô���ǰ�whatto�ĵ�������λ��Ϊ1��
	  	  ���whatto��COLLECT_ccu(0x04)ȡ�����룬��ô���ǰ�whatto��������λ��0��
		- ���ÿһ��lurker_node���ܹ�ͨ����whatto�ֶ����ж��Լ��ʺ��ڴ�����һ��
		  ���͵ı��ģ������ʺϴ�����һ�����͵ı��ġ���Ҫע����ǣ�ÿһ��lurker_node
		  �ڵ�ʹ�����һ���û�ע���ע�ắ�������е�item�����ָ���û�ע�ắ����ָ�롣

	ע��:
		lurker_node �� proc_node������
		- proc_node�����ǽ��û�ע��ĺ�����֯��������
		- lurker_node �����ÿһ��tcp���Ӷ��Եģ�ÿһ��tcp���Ӷ���һ��lurker_node��
		  ����ṹ������tcp�ͷ���֮�����е�lurker_node����ȫ���ͷš�
		- ��������ô˵��proc_node��ÿһ��ע�ắ���ļң�����ͬ��tcp��Ҫ�õ�ͬһ��ע��
		  ������ʱ����Щtcp����Ҫ��ע�ắ��һ����ʱ�ļң���ʱ�ļҾ���lurker_node.

	- Comment by shashibic 2014/03/07
	
**/
static void
ride_lurkers(struct tcp_stream * a_tcp, char mask)
{
	struct lurker_node *i;
	// collect collect_urg
	char cc, sc, ccu, scu;

	// �������еļ�����
	for (i = a_tcp->listeners; i; i = i->next)
		// �����ǰ������i ��whatto �� mask ��һ��(�����Ϊ1)
		// ��ô��ǰ�����߾��Ǵ������mask����Ӧ�Ķ����ġ�
		if (i->whatto & mask)
		{
			// �����⼸������: cc��sc��ccu��scuҪôΪ0��ҪôΪ1
			cc = a_tcp->client.collect;
			sc = a_tcp->server.collect;
			ccu = a_tcp->client.collect_urg;
			scu = a_tcp->server.collect_urg;

			// ִ�м����ߺ���������ʵ�����û�ע���ĳһ������
			(i->item) (a_tcp, &i->data);

			// �ٴ��ж�a_tcp����Ӧ�ı���Ƿ�仯��
			// �����if��������˵��:�û�����Ӧ��ֵ������
			if (cc < a_tcp->client.collect)
				i->whatto |= COLLECT_cc;
			if (ccu < a_tcp->client.collect_urg)
				i->whatto |= COLLECT_ccu;
			if (sc < a_tcp->server.collect)
				i->whatto |= COLLECT_sc;
			if (scu < a_tcp->server.collect_urg)
				i->whatto |= COLLECT_scu;
			// �����if��������˵��:�û���������Ӧ��ֵ
			if (cc > a_tcp->client.collect)
				i->whatto &= ~COLLECT_cc;
			if (ccu > a_tcp->client.collect_urg)
				i->whatto &= ~COLLECT_ccu;
			if (sc > a_tcp->server.collect)
				i->whatto &= ~COLLECT_sc;
			if (scu > a_tcp->server.collect_urg)
				i->whatto &= ~COLLECT_scu;
		}
}


/**
	���:
		a_tcp : �����ص���tcp����
		rcv   : �����ص��Ľ�����

	����:
		�������������:
		1������н������ġ�
		- ����whattoΪ�������ı�־���Ը���ride_lurker�ص���Ŀ���Ǵ���������ġ�
		- Ȼ�����ride_lurker���������û�ע�ắ���Ļص���
		- ���listener�����Ƿ��м�������Ҫɾ������ɾ�������档
		
		2�����û�н�������
		- ����whattoΪ�������ı�־���Ը���ride_lurker�ص���Ŀ���Ǵ����������ġ�
		- Ȼ�����ride_lurker���������û�ע�ắ���Ļص���
		- ���listener�����Ƿ��м�������Ҫɾ������ɾ�������档

	ע��:
		��2���У���Ȼ��һ��whileѭ��������Ĭ�ϵأ����ѭ��ֻ��ִ��һ�Σ�
		���Ҳ�������one_loop_less��Ϊ��0.

	- Comment by shashibici 2014/03/07
	
**/
static void
notify(struct tcp_stream * a_tcp, struct half_stream * rcv)
{
	struct lurker_node *i, **prev_addr;
	char mask;

	// ������µĽ�����
	if (rcv->count_new_urg)
	{
		// ���������������
		if (!rcv->collect_urg)
			return;
		// �ж���client����server
		if (rcv == &a_tcp->client)
			mask = COLLECT_ccu;
		else
			mask = COLLECT_scu;
		// ִ��maks
		ride_lurkers(a_tcp, mask);
		// ��ת��"ɾ��listeners", ��ִ�������if
		goto prune_listeners;
	}
	// ��������߶������ı��ĸ���Ȥ����ôִ�ж��������ĵĴ���
	if (rcv->collect)
	{
		if (rcv == &a_tcp->client)
			mask = COLLECT_cc;
		else
			mask = COLLECT_sc;
		do
		{
			int total;
			// ���ȼ��㵱ǰbuffer�е��ֽ�����(count-offset)
			// Ȼ���¼�������ֵ
			a_tcp->read = rcv->count - rcv->offset;
			total=a_tcp->read;

			/*
				������Ҫ�ر�ע�⣬ride_lurkers��ص��û���ע�ắ����
				�������û�ע��ĺ����У����п����޸�a_tcp->read�����ֵ��
				���磬���û���ȡ��n���ֽڣ���ô���a_tcp->read���п����޸�Ϊn��
			*/
			ride_lurkers(a_tcp, mask);
			
			// ���count_new>0
			if (a_tcp->read > (total - rcv->count_new))
				rcv->count_new = total-a_tcp->read;
			// ��data���readΪ��ʼ��ַ�����ݣ��ƶ���data��
			if (a_tcp->read > 0)
			{
				memmove(rcv->data, rcv->data + a_tcp->read, 
					    rcv->count - rcv->offset - a_tcp->read);
				
				rcv->offset += a_tcp->read;
			}
		}
		/* ע��: one_loop_less Ĭ�������Ϊ0��Ҳ���ǲ���ѭ��ִ��*/
		while (nids_params.one_loop_less && a_tcp->read>0 && rcv->count_new);
		// we know that if one_loop_less!=0, we have only one callback to notify
		// �ƶ�����֮�� ����count_new
		rcv->count_new=0;
	}
	
prune_listeners:
	prev_addr = &a_tcp->listeners;
	i = a_tcp->listeners;

	// �������е�listener,
	// �����ĳ��listener��whattoΪ0��˵�����Ѿ�û�����ˣ���ôҪɾ��
	while (i)
	{
		if (!i->whatto)
		{
			*prev_addr = i->next;
			free(i);
			i = *prev_addr;
		}
		else
		{
			prev_addr = &i->next;
			i = i->next;
		}
	}
}


/**
	���:
		a_tcp   : ��ǰ���ڴ����tcp
		rcv     : �����ߣ���tcp
		snd     : �����ߣ���tcp
		data    : ָ���ĵ�ָ��
		datalen : �������ĵĳ��ȣ�������data����Ч����
		this_seq: �ӱ���ͷ��ȡ�����ģ���ǰ���ĵ�һ���ֽڵ����
		fin     : �ӱ���ͷ��ȡ�����ģ���ǰ���ĵ�fin���
		urg     : �ӱ���ͷ��ȡ�����ģ���ǰ���ĵ�urg���
		urg_prt : �ӱ���ͷ��ȡ�����ģ���ǰ���ĵ�urg_prt��ֵ

	����:
		lost    : ��¼ǰ���ж����ֽ��Ѿ��Ǳ�ȷ�Ϲ���
		to_copy : ��¼����ָ��֮ǰ�ж�������������
		to_copy2: ��¼����ָ��֮���ֶ�������������

		������ִ�з�Ϊ�����֧:
			(1)�ñ��İ����Ϸ���Ч�Ľ�������;
			(2)�ñ���û�а����Ϸ���Ч�Ľ�������;

		����������������������º�������Ϊ:
		1���յ��ı��İ����Ϸ���Ч�Ľ������ġ�
			����������£���������������Ҫ�����������������:
			1) �ȴ����������֮ǰ����Ч���ݡ����ⲿ��������ӵ�
			   data��ָ��Ļ����У�Ȼ�����notify������
			2) Ȼ��������������ݡ����ⲿ�����ݿ�����data��ָ��
			   �Ļ����У�Ȼ�����notify������
			3) �����������ĺ����Ч���ݡ����ⲿ��������ӵ�
			   data��ָ��Ļ����У�Ȼ�����notify������
		2���յ��ı���û�а����Ϸ���Ч�Ľ������ġ�
			����������£�������ֱ�ӽ���Ч������ӵ�data��ָ���
			�����У�Ȼ�����notify������

		��������ݱ���������֮�󣬺�����ִ��һ��listener�������
		whattoΪ0��listener������˲��������档
		Ҳ����˵���һ��listener��ɾ���ˣ���ô�����tcp�������������ڶ�����
		�ٴα���ӽ����ˡ�

	ע:  "��Ч����"��ָ����ȷ������֮�����Щ�ֽ������ݣ��Ѿ���ȷ���˵�����
		����"��Ч����"��

	- Comment by shashibici. 2014/03/07.

**/
static void
add_from_skb(struct tcp_stream * a_tcp, struct half_stream * rcv,
             struct half_stream * snd,
             u_char *data, int datalen,
             u_int this_seq, char fin, char urg, u_int urg_ptr)
{
	// ��¼����������Ҫ���������ֽ�
	u_int lost = EXP_SEQ - this_seq;
	int to_copy, to_copy2;

	// �����һ�������������ҽ���ָ���ָ���λ�����������ڴ��ģ�
	// ���� (�����߻�û�з�������������ģ� ���߽���ָ���ԭ������ָ�뻹Ҫ��)
	// ��ôִ�������if����������urg_seen �Լ� urg_ptr.
	if (urg && after(urg_ptr, EXP_SEQ - 1) &&
	        (!rcv->urg_seen || after(urg_ptr, rcv->urg_ptr)))
	{
		rcv->urg_ptr = urg_ptr;
		rcv->urg_seen = 1;
	}

	// ��������߿���������������� &&
	// �������ĵĿ�ʼ���ӵ�����һ���ֱ���֮�󣬼�����������Ҫ�Ľ����������� &&
	// ����ָ�벻������ǰ����
	if (rcv->urg_seen && after(rcv->urg_ptr + 1, this_seq + lost) &&
	        before(rcv->urg_ptr, this_seq + datalen))
	{
		// ���ȼ����������֮ǰ����Ч����
		to_copy = rcv->urg_ptr - (this_seq + lost);
		// �����������Ҫ����
		if (to_copy > 0)
		{
			// collect����������¼�Ƿ���������ı���
			// ��0��ʾ���ܣ�0��ʾ��������������
			if (rcv->collect)
			{
				// ������գ���ѵ�ǰ���У�����ָ��֮ǰ��������ӵ�buffer��
				// ���buffer��half_stream�е�һ��dataָ����ָ����ڴ�
				add2buf(rcv, (char *)(data + lost), to_copy);
				notify(a_tcp, rcv);
			}
			// �����ʾ���ն˲������������ݣ���������notify����
			else
			{
				// ֻ�ǰ��������ݵ�������¼һ�£�û����ӵ�buffer��
				rcv->count += to_copy;
				// �޸�offset���
				rcv->offset = rcv->count; /* clear the buffer */
			}
		}

		/* �������Ϲ��̣��Ѿ�������ָ��֮ǰ���������ݿ�������buffer����*/

		// ��rcv->urgdataָ�������Ľ������ݿ�ʼ��λ��
		// �������д���ȼ���:
		/*
			rcv->urgdata = data+(rcv->urg_ptr - this_seq);
		*/
		rcv->urgdata = data[rcv->urg_ptr - this_seq];
		// ������µĽ������ݵ���
		rcv->count_new_urg = 1;
		// ����notify�������ú������ջ�����û�ע��Ļص�����
		notify(a_tcp, rcv);
		// ������֪ͨ�������������ý�����־
		rcv->count_new_urg = 0;
		// ����Ϊ"������û�п�����������"����Ϊ��һ������׼��
		rcv->urg_seen = 0;
		// �޸Ľ������ļ�����
		rcv->urg_count++;
		// �����������ָ����滹�ж����ֽ���Ҫ����
		to_copy2 = this_seq + datalen - rcv->urg_ptr - 1;
		// ������ֽ���Ҫ����
		if (to_copy2 > 0)
		{
			// �����������Ҫ�����������ģ���ô�򿽱�
			if (rcv->collect)
			{
				add2buf(rcv, (char *)(data + lost + to_copy + 1), to_copy2);
				// �ٴε���notify�������ú������ջ�ص��û���ע�ắ��
				// ��ע�⣬��ʱrcv->count_urg_new Ϊ0��
				// �ᱻ������ͨ���Ĵ���
				notify(a_tcp, rcv);
			}
			// ����ֻ��ͳ�ƣ���������Ҳ�����ûص�����
			else
			{
				rcv->count += to_copy2;
				rcv->offset = rcv->count; /* clear the buffer */
			}
		}
	}

	// ����ǰ����û�а����Ϸ���Ч�� "��������"
	// �����������Ĵ���
	else
	{
		// �����ȥ���������ݣ���������
		if (datalen - lost > 0)
		{
			// �����������Ҫ������������
			if (rcv->collect)
			{
				add2buf(rcv, (char *)(data + lost), datalen - lost);
				// û�кϷ��Ľ������ģ���ô�͵�������ͨ���ĵ���notify.
				// ע�⣬��ʱrcv->count_urg_newΪ0.
				notify(a_tcp, rcv);
			}
			// ��������߲������������ģ�������Ҳ���ص��û�ע��ĺ���
			else
			{
				rcv->count += datalen - lost;
				rcv->offset = rcv->count; /* clear the buffer */
			}
		}
	}

	/*  ����Ĺ����ǽ��������� "�Ƿ������������" �����׼�����˷��������
		����Ľ������Ҫ�ǽ��ո��յ�����Ч���Ŀ�����buffer��.

		���潫�Ǿ������������ģ��ж��Ƿ����"����"��Ϣ��Ȼ�������崦��
		��������һ��snd->state��rcv->state.
	*/
	
	if (fin)
	{
		// �������������Ϣ��˵���������Ѿ�������
		snd->state = FIN_SENT;
		// ����������Ѿ�ΪTCP_CLOSING ˵��������Ҳ�Ѿ����͹�fin��
		// ��ô���ǵȴ��ر�����tcp
		if (rcv->state == TCP_CLOSING)
			add_tcp_closing_timeout(a_tcp);
	}
}


/**
	���:
		a_tcp        ��Ҫ�����tcp����
		this_tcphdr  ָ��ղ����tcp����ͷ����ָ��
		sed          ������
		rcv          ������
		data         ָ��ղ����tcp����������ʼ��ַ��ָ��
		datalen      ��Ҫ����Ŀռ��С
		skblen       �ող����tcp���ĵ����ݵĳ���

	����:
		����������ող����tcp����������ӵ�rcv�С�
		rcv������ָ����õ����ֱ���"char *data" �� "skbuff *list"
		dataָ��ָ������Ѿ���ȷ���˵�tcp���ģ�
		list��һ������ͷ����������ʵ���ǽ��շ��Ļ��崰�ڣ���ŵ��ǽ��յ�����û��
		��ȷ�ϵı��ġ�

	ע��:
		�ú����Խ��յ��ı��ķ�����������ۡ�
		1)	���յ��ı��ĵ����С�ڽ��շ���һ��ack����ţ�Ҳ��˵�յ���һ���Ѿ���ȷ��
			���˵ı��ġ�
		2) 	���յ��ı��ĵ����С�ڽ��շ���һ��ack����ţ����Ǽ��ϱ��ĵĳ���֮��
			����ų������շ���һ��ack����ţ�Ҳ����˵�����������һ����û�б�ȷ�ϡ�
		3)	���յ��ı�����Ŵ��ڻ���ڽ��շ���һ��ack����ţ�Ҳ����˵�յ���һ��
			���ģ��ñ����ǵȴ�ȷ�ϵı��ġ�

	- Comment by shashibici 2014/03/07.
**/
static void
tcp_queue(struct tcp_stream * a_tcp, struct tcphdr * this_tcphdr,
          struct half_stream * snd, struct half_stream * rcv,
          char *data, int datalen, int skblen
         )
{
	u_int this_seq = ntohl(this_tcphdr->th_seq);
	struct skbuff *pakiet, *tmp;

	/*
	 * Did we get anything new to ack?
	 */

	// EXP_SEQ��ʾ���շ������ķ��ͷ����кš�
	// this_seq�Ǹող��������������кš�
	// if (this_seq < EXP_SEQ)��ʾ����ǰץ���İ���һ���ط��İ���
	// if (this_seq == EXP_SEQ) ��ʾ����ǰץ���İ���һ�������İ���
	if (!after(this_seq, EXP_SEQ))
	{
		// ��� ��ǰ���ĵ����+���ĵĳ���+(1��0) > ��һ�η�����ack
		// ˵�� �������һ������Ҫ��������ӵ�rcv->data��
		// ע��: ���"һ����"�ĳ���ǡ�þ��� this_seq + datalen - EXP_SEQ,
		// ���this_seq == EXP_SEQ ��ô�����������Ķ���Ҫ��ӵ�rcv->data��
		if (after(this_seq + datalen + (this_tcphdr->th_flags & TH_FIN), EXP_SEQ))
		{
			/* the packet straddles our window end */
			get_ts(this_tcphdr, &snd->curr_ts);
			add_from_skb(a_tcp, rcv, snd, (u_char *)data, datalen, this_seq,
			             (this_tcphdr->th_flags & TH_FIN),
			             (this_tcphdr->th_flags & TH_URG),
			             ntohs(this_tcphdr->th_urp) + this_seq - 1);
			/*
			 * Do we have any old packets to ack that the above
			 * made visible? (Go forward from skb)
			 */
			pakiet = rcv->list;
			// ����rec->list�����������������Ȥ�İ�ֱ���������
			// �������ָ���Ȥ�İ������¸���Ȥ�Ĳ���Ȼ���������
			// һֱ������һ����ȫ����Ȥ�İ����˳�������
			// "����Ȥ"��ָ������ʼ��Ŵ��ڱ�ȷ�Ϲ������(�ð���ȫû��ȷ��)
			// "���ָ���Ȥ"��ָ����ǰһ�����ѱ�ȷ�϶���벿��û��ȷ�ϡ�
			// "������Ȥ"��ָ�������Ѿ���ȷ�Ϲ���
			while (pakiet)
			{
				if (after(pakiet->seq, EXP_SEQ))
					break;
				// �����һ������ ���� �ڶ������� ��Ϊ�棬ִ��if.
				if (after(pakiet->seq + pakiet->len + pakiet->fin, EXP_SEQ))
				{
					add_from_skb(a_tcp, rcv, snd, pakiet->data,
					             pakiet->len, pakiet->seq, pakiet->fin, pakiet->urg,
					             pakiet->urg_ptr + pakiet->seq - 1);
				}
				rcv->rmem_alloc -= pakiet->truesize;
				// ����Ҵ�İ���ǰ����Ϊ�գ�˵�����ǵ�һ����
				if (pakiet->prev)
				{
					pakiet->prev->next = pakiet->next;
				}
				// ����˵���ǵ�һ��
				else
				{
					rcv->list = pakiet->next;
				}
				// ������治Ϊ�գ�˵���������һ��
				if (pakiet->next)
				{
					pakiet->next->prev = pakiet->prev;
				}
				// ���������һ��
				else
				{
					rcv->listtail = pakiet->prev;
				}
				tmp = pakiet->next;
				free(pakiet->data);
				free(pakiet);
				pakiet = tmp;
			}
		}
		else
		{
			// !after((this_seq, EXP_SEQ))Ϊ��  ����
			// after(this_seq + datalen + (this_tcphdr->th_flags & TH_FIN), EXP_SEQ) Ϊ��
			// ��ʾ��ȫ������Ȥ,ֱ�ӷ��ء�
			return;
		}
		
	}
	// ���else�д��� "��ȫ����Ȥ"�İ���
	else
	{
		struct skbuff *p = rcv->listtail;

		pakiet = mknew(struct skbuff);
		// ��ʵ�����ݳ���
		pakiet->truesize = skblen;
		rcv->rmem_alloc += pakiet->truesize;
		// ��һ����ռ�ĳ���
		pakiet->len = datalen;
		// ����һ������ռ�ĳ��ȵĿռ�
		pakiet->data = malloc(datalen);
		// �������ʧ�����ӡ����(���˳�)
		if (!pakiet->data)
			nids_params.no_mem("tcp_queue");
		// ���򿽱�����
		memcpy(pakiet->data, data, datalen);
		// ���û��ֱ�־
		pakiet->fin = (this_tcphdr->th_flags & TH_FIN);
		/* Some Cisco - at least - hardware accept to close a TCP connection
		 * even though packets were lost before the first TCP FIN packet and
		 * never retransmitted; this violates RFC 793, but since it really
		 * happens, it has to be dealt with... The idea is to introduce a 10s
		 * timeout after TCP FIN packets were sent by both sides so that
		 * corresponding libnids resources can be released instead of waiting
		 * for retransmissions which will never happen.  -- Sebastien Raveau
		 */
		 // �������һ������
		if (pakiet->fin)
		{
			// ���÷�����״̬Ϊ�ر�
			snd->state = TCP_CLOSING;
			// ������շ��Ѿ������˻��� ���� ���շ��Ѿ�ȷ���˻���
			if (rcv->state == FIN_SENT || rcv->state == FIN_CONFIRMED)
				// �Ὣ���tcp�ŵ�һ���ȴ��رն����С���ʱ�����˾ͻ�ر�
				add_tcp_closing_timeout(a_tcp);
		}
		// ���ñ�־
		pakiet->seq = this_seq;
		pakiet->urg = (this_tcphdr->th_flags & TH_URG);
		pakiet->urg_ptr = ntohs(this_tcphdr->th_urp);
		// ����ǰ�ı��İ����뵽list���ʵ�λ�ã�ʹ��seq��������
		for (;;)
		{
			// ��������˶�ͷ������ ������һ��list�ڵ�p,����seq��������ǰseq
			if (!p || !after(p->seq, this_seq))
				// ��ô����ֹѭ��
				break;
			// �����ɶ�β���ͷ��������
			p = p->prev;
		}

		// ����ǿգ���ʾ��һ�����յ��İ�����ԭ��list�����а���seq��С
		// ����Ӧ�ò��ڶ�ͷ
		if (!p)
		{
			// ����ǰ�����뵽���ʵ�λ��
			pakiet->prev = 0;
			pakiet->next = rcv->list;
			if (rcv->list)
				rcv->list->prev = pakiet;
			rcv->list = pakiet;
			if (!rcv->listtail)
				rcv->listtail = pakiet;
		}
		// �����ǿգ���ôһ���ҵ���һ�����ʵ�λ��
		// ���Բ��뵽���ʵ�λ��
		else
		{
			pakiet->next = p->next;
			p->next = pakiet;
			pakiet->prev = p;
			if (pakiet->next)
				pakiet->next->prev = pakiet;
			else
				rcv->listtail = pakiet;
		}
	}
}


/**
	���:
		rcv :         ���Ľ�����
		this_tcphdr : ��ǰ�յ��ı��ĵ�ͷ

	����:
		��rcv�е�list����ȫ��ɾ����ԭ����list�������ˡ�
		
	ע��:
		���������tcp.c��ͷ��һ��purge_queue�����ǳ����ƣ�ǧ��ҪŪ��
		�Ǹ�����ֻ��һ���������Ǿ�����Ҫ"����"�İ����ӣ��Ǹ��������ᷢ������
		���Բ���Ҫtcpͷ��

	- Comment by shashibici 2014/03/07
**/

static void
prune_queue(struct half_stream * rcv, struct tcphdr * this_tcphdr, int icore)
{
	struct skbuff *tmp, *p = rcv->list;

	nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_BIGQUEUE, ugly_iphdr[icore], this_tcphdr);
	while (p)
	{
		free(p->data);
		tmp = p->next;
		free(p);
		p = tmp;
	}
	rcv->list = rcv->listtail = 0;
	rcv->rmem_alloc = 0;
}

static void
handle_ack(struct half_stream * snd, u_int acknum)
{
	int ackdiff;

	ackdiff = acknum - snd->ack_seq;
	if (ackdiff > 0)
	{
		snd->ack_seq = acknum;
	}
}
#if 0
static void
check_flags(struct ip * iph, struct tcphdr * th)
{
	u_char flag = *(((u_char *) th) + 13);
	if (flag & 0x40 || flag & 0x80)
		nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_BADFLAGS, iph, th);
//ECN is really the only cause of these warnings...
}
#endif



struct tcp_stream *
find_stream(struct tcphdr * this_tcphdr, struct ip * this_iphdr,
            int *from_client)
{
	struct tuple4 this_addr, reversed;
	struct tcp_stream *a_tcp;

	this_addr.source = ntohs(this_tcphdr->th_sport);
	this_addr.dest = ntohs(this_tcphdr->th_dport);
	this_addr.saddr = this_iphdr->ip_src.s_addr;
	this_addr.daddr = this_iphdr->ip_dst.s_addr;
	a_tcp = nids_find_tcp_stream(&this_addr);
	if (a_tcp)
	{
		*from_client = 1;
		return a_tcp;
	}
	
	reversed.source = ntohs(this_tcphdr->th_dport);
	reversed.dest = ntohs(this_tcphdr->th_sport);
	reversed.saddr = this_iphdr->ip_dst.s_addr;
	reversed.daddr = this_iphdr->ip_src.s_addr;
	a_tcp = nids_find_tcp_stream(&reversed);
	if (a_tcp)
	{
		*from_client = 0;
		return a_tcp;
	}
	return 0;
}



struct tcp_stream *
nids_find_tcp_stream(struct tuple4 *addr)
{
	int hash_index;
	struct tcp_stream *a_tcp;

	hash_index = mk_hash_index(*addr);
	for (a_tcp = tcp_stream_table[hash_index];
	        a_tcp && memcmp(&a_tcp->addr, addr, sizeof (struct tuple4));
	        a_tcp = a_tcp->next_node);
	return a_tcp ? a_tcp : 0;
}

/**
	����:
		a_tcp: ����һ��a_tcp�ҵ�����icore�š�

	����:
		icore�ţ�����ɹ���
		-1�����ʧ��
**/
static int
find_icore(struct tcp_stream * a_tcp)
{
	int icore;
	struct tcp_stream *t_tcp;
	
	for (icore = 0; icore < TCP_CORE_NUM; icore++)
	{
		for (t_tcp = tcp_latest[icore]; t_tcp; t_tcp = t_tcp->next_time)
		{
			if (a_tcp->addr.source == t_tcp->addr.source &&
				a_tcp->addr.dest == t_tcp->addr.dest &&
				a_tcp->addr.saddr == t_tcp->addr.saddr &&
				a_tcp->addr.daddr == t_tcp->addr.daddr)
			{
				return icore;
			}
		}
	}
	fprintf(stderr,"gdb me in find_core ...\n");
	pause();
	return -1;
}

/**
	����:
		�ͷ���������Ŀռ䣬�����������ָ��
**/
void tcp_exit(void)
{
	int i;
	struct lurker_node *j;
	struct tcp_stream *a_tcp, *t_tcp;

	// [streams_pool] �����е�Ԫ��Ҫôȫ��Ϊ0��Ҫô����Ϊ0
	// ���ֻ��Ҫ�жϵ�һ��Ԫ�ؼ��ɡ�
	if (!tcp_stream_table || !(streams_pool[0]))
	{
		return;
	}
	// ��ÿһ��hash��������е�tcp������һ�ͷ�
	for (i = 0; i < tcp_stream_table_size; i++)
	{
		a_tcp = tcp_stream_table[i];
		while(a_tcp)
		{
			t_tcp = a_tcp;
			a_tcp = a_tcp->next_node;
			for (j = t_tcp->listeners; j; j = j->next)
			{
				t_tcp->nids_state = NIDS_EXITING;
				(j->item)(t_tcp, &j->data);
			}
			// ÿ��ѭ���ͷ�һ��tcp�ڵ�
			nids_free_tcp_stream(t_tcp);
		}
	}
	// �ͷ�table�ռ�
	free(tcp_stream_table);
	tcp_stream_table = NULL;
	// �ͷ����е�tcp�ڵ�ռ�
	for (i = 0; i < TCP_CORE_NUM; i++)
	{
		free(streams_pool[i]);
		streams_pool[i] = NULL;
	}
	/* FIXME: anything else we should free? */
	/* yes plz.. */
	for (i = 0; i< TCP_CORE_NUM; i++)
	{
		tcp_latest[i] = tcp_oldest[i] = NULL;
		tcp_num[i] = 0;
	}
}


// ÿ����һ��tcp�����ı��ı����գ��ͻ�����������
// ������������:
//
//   1������pcap��ע����һ���ص�����: nids_pcap_handler,�������Ǹո�
//   ���յ��Ǹ�������·��������������pcap���յ�������·��İ���ʱ�򱻻ص���
//
//   2����nids_pcap_handler�л��������·��������ȡ�������ж��Ƿ�Ϊһ��ip����
//   �����һ��ip���飬�ͻᱣ���� cap_queue ������(���߳�ʱ)��Ȼ��ֱ�ӵ���
//   call_ip_frag_procs ��������������pcap����ģ������򵥴�������������·�����
//
//   3����call_ip_frag_procs�����У������ȵ��������û�ע���ip_frag��������
//   �����Ǹող���İ���������libnids.c�ļ��е�gen_ip_frag_proc������
//   �����Ǹող���İ��� ��һ��ip����
//
//   4����gen_ip_frag_proc�����У����ȴ���һ�´�������ip���飬Ȼ�󽫴������ip��
//   ��Ϊ����������ip_defrag_stub�����������Ǹոմ������ip����(��)
//
//   5����ip_defrag_stub�����У������ip_defrag �������ղŵİ������Ѿ�������һ����
//   �˵ķ��飬�������飬��������γ�һ��������ip���ģ��᷵��һ�� IPF_NEW��
//   ���߻�û����װ��һ��������ip���ģ�����IPF_NOTF��
//   ���߳�������IPF_ISF��
//   
//
//   6���ص�gen_ip_frag_proc�У������IPF_ISF����ֱ�ӷ��أ������������ip_procs
//   �е����к������������û�ע���ip�������������libnids.c�е�gen_ip_proc
//   �����������ǲ���İ����ݣ��Լ����峤�ȡ�
//   
//   7����gen_ip_proc�����У������ip�ϲ�������ͣ�����process_tcp����(�����tcp)��
//   �����ǲ�������ݱ����Լ����峤�ȡ�
// 
//   8����process_tcp�����У����������������һ��tcp��Ȼ�������Ӧ�Ĳ�����
//      ���һ����ʵ���ʱ����� tcp��listeners�Լ� �û�ע���tcp�ص�����
//
//   - Comment by shashibic 2014/03/07
//
void
process_tcp(u_char * data, int skblen, int icore)
{

	/*************   ���Ƚ���tcp���������Լ��   ***************/
	
    //http://blog.sina.com.cn/s/blog_5ceeb9ea0100wy0h.html
    //tcpͷ�����ݽṹ
	struct ip *this_iphdr = (struct ip *)data;
	struct tcphdr *this_tcphdr = (struct tcphdr *)(data + 4 * this_iphdr->ip_hl);
	int datalen, iplen;
	int from_client = 1;
	unsigned int tmp_ts;
	struct tcp_stream *a_tcp;
	struct half_stream *snd, *rcv;

	ugly_iphdr[icore] = this_iphdr;
	//ntohl()�ǽ�һ���޷��ų�����������
	//���ֽ�˳��ת��Ϊ�����ֽ�˳��
	//�������:��Ϊ�����д�С�����⣬����
	//ͳһ�������ֽ�˳���ܹ����λ����Ĳ������ͨ�š�
	iplen = ntohs(this_iphdr->ip_len);//len ���� hlͷ������
	if ((unsigned)iplen < 4 * this_iphdr->ip_hl + sizeof(struct tcphdr))
		//���ip���ݱ�����������С�ĳ���(tcpֻ��ͷ��,������)
	{
		//ϵͳ��ӡ������־
		nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_HDR, this_iphdr,
		                   this_tcphdr);
		return;
	} // ktos sie bawi

	datalen = iplen - 4 * this_iphdr->ip_hl - 4 * this_tcphdr->th_off;
	//th_off TCPͷ����
	if (datalen < 0)
	{
		nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_HDR, this_iphdr,
		                   this_tcphdr);
		return;
	} // ktos sie bawi
    //���ԭip��Ŀ��ip��Ϊ0
	if ((this_iphdr->ip_src.s_addr | this_iphdr->ip_dst.s_addr) == 0)
	{
		nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_HDR, this_iphdr,
		                   this_tcphdr);
		return;
	}
	if (!(this_tcphdr->th_flags & TH_ACK))
		/*���û��th_ack ���������ɨ���Ƿ��й�����*/
		detect_scan(this_iphdr);//̽��ipͷ��
	if (!nids_params.n_tcp_streams) return;
	if (my_tcp_check(this_tcphdr, iplen - 4 * this_iphdr->ip_hl,
	                 this_iphdr->ip_src.s_addr, this_iphdr->ip_dst.s_addr))
	{
		//��������
		nids_params.syslog(NIDS_WARN_TCP, NIDS_WARN_TCP_HDR, this_iphdr,
		                   this_tcphdr);
		//return;
	}

	
	 /*************    ���濪ʼ�ж��Ƿ��һ������     ***************/

	if (!(a_tcp = find_stream(this_tcphdr, this_iphdr, &from_client)))
	{
		// �ҵ�hash���е�tcp
		// �������ִ�У���ô���ǵ�һ������
		if ((this_tcphdr->th_flags & TH_SYN) &&
		        !(this_tcphdr->th_flags & TH_ACK) &&
		        !(this_tcphdr->th_flags & TH_RST))
			add_new_tcp(this_tcphdr, this_iphdr, icore);//
		return;
	}

	/*************    ���濪ʼ�ж��Ƿ�ڶ�������     ***************/

	// �������ִ�������˵���Ѿ�������һ��tcp
	// ʶ�𲢼�¼���ͷ�����շ�
	if (from_client)  //��������û�
	{
		snd = &a_tcp->client;//clientΪ���ͷ�
		rcv = &a_tcp->server;//������Ϊ���շ�
	}
	else  //�����෴
	{
		rcv = &a_tcp->client;
		snd = &a_tcp->server;
	}

	// �ڶ�������Э�鶼��ִ����һ��
	if ((this_tcphdr->th_flags & TH_SYN))  //���SYN==1 ��ʾͬ���ź�
	{
		// �������client ��ô�����ظ��ĵ�һ�����֡�
		if (from_client)
		{
			// [nids_last_pcap_header]��ʾ����ץ����pcap��.
			// ������˳�ʱ���ܣ���������ץ���İ���������һ����+ʱ������,����Ϊ���µİ���ʱ
			if (nids_params.tcp_flow_timeout > 0 &&
			        (nids_last_pcap_header->ts.tv_sec > a_tcp->ts + nids_params.tcp_flow_timeout))
			{
				// ִ�е������棬˵��������ǳ�ʱ�˵�
				// ���������Ȳ��ǻ�Ӧ��Ҳ�������ð�����ô���ֶ����á�
				if (!(this_tcphdr->th_flags & TH_ACK) && !(this_tcphdr->th_flags & TH_RST))
				{
					/************
						ִ�е����if�����������:
						   �������ź�
						&& ����client(Ҳ�����ظ������һ������)
						&& ������ǳ�ʱ�İ�
						&& û��Ӧ����Ϣ
						&& û��������Ϣ.
						-------
						˵����:
						�����ȷʵ��һ����һ������(��Ϊ��û��Ӧ��û������)��
						������һ���������ںܾ���ǰ�ͷ������ˣ�ֻ��·������
						ֱ�����ڲű��յ���
						��ֻ����һ������:ԭ����tcp�Ѿ�ͨ���Ĵλ��ֱ��ͷ��ˣ�
						����a_tcp���������û���������٣�����Ȼ����(���ں���
						�Ĵ�����������)���������Ϊ�˵ȴ���������ķ�����
						��ϸ�������������˿�֮�䣬ԭ����tcp�Ѿ��ͷţ�һ��ʱ��
						����Ҫ����һ��tcp���ӣ���ô���Ҫ��Ϳ��ܾ���[��ʱ]
						��һ������(��Ϊ������������tcp֮��ļ�������ܽ���
						������һ��tcp���������ʺͿ�����һ��[��ʱ]�ı���)��
					************/
					// �ͷ�ԭ����tcp�ռ�(����һ��tcp�������֮�󲢲���
					// �������ͷţ���Ӻ���Ĵ�����Կ���)
					nids_free_tcp_stream(a_tcp);
					// ������һ������һ���������һ���µ�tcp
					// ����find_tcp_stream�ˣ���Ϊ�ող��ͷţ���Ȼ�Ҳ���
					add_new_tcp(this_tcphdr, this_iphdr, icore);//�����µ�tcp
				}//end if

				/***********
					���������ifֱ��ִ����������������:
					   �������ź�
					&& ����client
					&& ������ǳ�ʱ��
					&& �������ACK ���� �������RST.
					----------
					˵����:
					�������������һ���Ϸ��ĵ�һ������(��Ϊ��������
					ACK����RST)��
					���������ζ���ֱ�Ӱ���������
				************/
			}
			/*********
				������������ifֱ��ִ����������������:
				   �������ź�
				&& ����client��
				&& û�г�ʱ
				-------
				˵����:
				client�ں̵ܶ�ʱ����������������������
				���Ǻ���Ȼ���������ִ�е���һ�������tcp�Ѿ�������
				��һ����������(libnids�ڴ����һ�����������ʱ�����
				��table�д�����һ��a_tcp�ڵ�).
				��ˣ�ֱ�Ӷ����������
			************/
			return;
		}

		/**************
			������������ifֱ��ִ�е�����������:
			   ���������һ����������
			&& �����������server
			----------
			˵����:
			���Ȼ�Ǵ�server�˷�����һ����������(Ҳ�п����ǳ�ʱ��),
			��ô���еĿ��ܾ���--�ڶ������֡�
		****************/

		// ���client �ոշ���syn ���� ������û�� ���� ACK==1 ��ô���ǵڶ�������
		// �ο�: add_new_tcp���� �� "TCP/IP��������Э��"
		if (a_tcp->client.state != TCP_SYN_SENT ||
		        a_tcp->server.state != TCP_CLOSE || !(this_tcphdr->th_flags & TH_ACK))
			return;

		/**********
			���ִ�е�����������:
				�����������������
			&&  �����������server
			&&  client�Ѿ�������syn
			&&  server��tcp״̬��close��
			&&  �������ACK���
			----
			˵����:
			��ض���һ��[�ڶ�������]��[�ڶ�������]ǡ������
			�����������	
		**********/
		// ���ҽ����ǵڶ�������(����server��)�Ż�����ִ��
		// ����[add_new_tcp]�����Ե�һ�����ֵĴ�����Կ�����client.seq��ʾ����client
		// ��Ҫ���͵���һ����ţ����server������Ӧ�������������ţ�˵�������ˡ�
		// ��Ϊtcp��ʱ��û������server��������֮ǰ����ţ�Ҳ��������֮�����š�
		// ������֣�˵������һ�����Ϸ��İ�����������Ϊ������ʱ����һ�����������µġ�
		if (a_tcp->client.seq != ntohl(this_tcphdr->th_ack))
			return;
		/**
			ִ�е�����󣬲���Ҫ���������������������Ҫ����:
				serverӦ������ǡ�þ���client�ϴη��������+1.
		**/
		/**
			�����ǽ�pcap����ץ���İ���ʱ�䱣����a_tcp��
			ע�⣬�Ⲣ����tcp�ļ�ʱ���������²�(����������·��)
			��ʱ��.
		**/
		a_tcp->ts = nids_last_pcap_header->ts.tv_sec;
		// ԭ��[server.state]��[TCP_CLOSE]�������޸���
		a_tcp->server.state = TCP_SYN_RECV;
		// ��[server.seq]����Ϊserver����һ����Ҫ���͵���ţ�Ҳ��client�´�Ӧ������
		// ����һ��server�İ�����ʱ�������Ų��ԣ�����Ӧ����
		a_tcp->server.seq = ntohl(this_tcphdr->th_seq) + 1;
		// [server.firstdata]�����������˳�ʼ��,��¼����
		// server�˵ĵ�һ�����
		a_tcp->server.first_data_seq = a_tcp->server.seq;
		/*************
			�����Ǳ�����server�˴˴�Ӧ������(��server�ڴ�
			client���͵���һ����ţ���Ӧ��ǡ�õ���[client.seq]).
			���´�libnids�յ�һ�����������Ǹ���������client
			��ʱ�򣬻����Ǹ����е�[this_tcphdr->th_seq]ֵ:
			
			- ����Ǹ����е�[this_tcphdr->th_seq] < ���ڵ�
			[server.ack_seq]��˵���Ǹ����Ѿ���serverȷ���ˣ�
			ֱ�Ӷ�����

			- ����Ǹ����е�[this_tcphdr->th_seq] >= ���ڵ�
			[server.ack_seq]��˵���Ǹ�����û�б�serverȷ�ϣ�
			��Ҫ��ӵ�[server.data]����[server.list]��
			ǰ�߱����Ѿ�����ȷ���˵ı��ģ����߱�����Խ���
			����û��ȷ�ϵı���
		***********/
		a_tcp->server.ack_seq = ntohl(this_tcphdr->th_ack);
		// ����˴�server����client�Ĵ��ڴ�С
		a_tcp->server.window = ntohs(this_tcphdr->th_win);

		// 
		if (a_tcp->client.ts_on)
		{
			// ����ʱ���
			a_tcp->server.ts_on = get_ts(this_tcphdr, &a_tcp->server.curr_ts);
			if (!a_tcp->server.ts_on)
				a_tcp->client.ts_on = 0;
		}
		// ����client�ǹرյĻ���serverҲҪ�ص�
		else 
		{
			a_tcp->server.ts_on = 0;
		}

		// ��������еĶ�Ӧֵ,wscale��������
		if (a_tcp->client.wscale_on)
		{
			a_tcp->server.wscale_on = get_wscale(this_tcphdr, &a_tcp->server.wscale);
			if (!a_tcp->server.wscale_on)
			{
				a_tcp->client.wscale_on = 0;
				a_tcp->client.wscale  = 1;
				a_tcp->server.wscale = 1;
			}
		}
		else
		{
			a_tcp->server.wscale_on = 0;
			a_tcp->server.wscale = 1;
		}
		return;
	}
	// ����һ��if�ǵڶ�������

	/*************    ���濪ʼ�ж��Ƿ����������     ***************/
	
	//--------------------------------
	// ����ִ���������,���ǵ�һ��Ҳ���ǵڶ�������
	// ������һЩ��������return
	if (
		// ������ (û�����ݲ��������ͬ)
	    ! (  !datalen && ntohl(this_tcphdr->th_seq) == rcv->ack_seq  )
	    &&
	    //  ���͵����в��ڽ��ܵķ�Χ֮�� (�����������޻���ڴ�������)
	    ( !before(ntohl(this_tcphdr->th_seq), rcv->ack_seq + rcv->window*rcv->wscale) ||
	      before(ntohl(this_tcphdr->th_seq) + datalen, rcv->ack_seq)
	    )
	)
		// ��ô����
		return;
	
	// ���򲻷���
	
	// ��������ش��������reset��ִ���������
	if ((this_tcphdr->th_flags & TH_RST))
	{
		// ��������ݴ���׶�
		if (a_tcp->nids_state == NIDS_DATA)
		{
			struct lurker_node *i;
			// �����޸�״̬Ϊreset
			a_tcp->nids_state = NIDS_RESET;
			// Ȼ�������tcp�����м����ߺ���
			for (i = a_tcp->listeners; i; i = i->next)
				(i->item) (a_tcp, &i->data);
		}
		// �ͷŸ�tcp������
		nids_free_tcp_stream(a_tcp);
		return;
	}

	
	/* PAWS check */
	// ���ƻؼ�⣬������ƻأ�ֱ��return
	if (rcv->ts_on && get_ts(this_tcphdr, &tmp_ts) &&
	        before(tmp_ts, snd->curr_ts))
		return;

	// �����жϣ����tcp���ı������Ӧ����Ϣ
	if ((this_tcphdr->th_flags & TH_ACK))
	{
		// ���if�����������Ψһȷ���˱����ǵ���������
		// �ο�: TCP-IP ����1��18��
		if (from_client && a_tcp->client.state == TCP_SYN_SENT &&
		        a_tcp->server.state == TCP_SYN_RECV)
		{
			// ���Ӧ���������ȷ�ģ��Ż�ִ�������if���
			if (ntohl(this_tcphdr->th_ack) == a_tcp->server.seq)
			{
				// �޸Ŀͻ��˵�״̬
				a_tcp->client.state = TCP_ESTABLISHED;
				// �Ѱ��е�ack��¼�������ŵ�client��
				a_tcp->client.ack_seq = ntohl(this_tcphdr->th_ack);
				// ����tcp��ʱ���
				a_tcp->ts = nids_last_pcap_header->ts.tv_sec;

				/*********************************************************************
				������һ�μ��˻����Ŵ���Ĺ���:
					���ȱ�����ȷ��libnidsһ�����յ���һ������client�ĵ��������ֱ��ĲŻ�
					ִ��������һ�δ��룬�յ�����ʱ��ı��Ķ�����ִ������Ĵ���ġ�

					���Կ�����ô��⣬����Ĵ�����һ��tcp���Ӹոս�����ʱ�򣬵�һ�ε���
					�û�ע��Ļص�������

					tcp�����ڽ���ʱ����һ�����ֺ͵ڶ������ֲ���ص��û�ע��ĺ�����
					
					tcp�ڵ���������֮ǰ�������һ������reset�źţ�Ҳ��ص��û�ע��ĺ���
					��������֮������tcp���ᱻ���١�

					������δ�����Ǳ���һ���û�ע��Ļص�������ÿ����һ�����ͽ�����ӵ�
					listener�����С���Ҫ��forѭ��ʵ�֡�
				***********************************************************************/
				
				// Ϊʲô������Ҫ�Ӵ�����?
				// ��Ϊ��Ҫʹ�þֲ����� i j data
				{
					struct proc_node *i;
					struct lurker_node *j;
					void *data;

					// �޸�server�˵�״̬
					a_tcp->server.state = TCP_ESTABLISHED;
					// �޸�tcp��״̬���ոս���
					a_tcp->nids_state = NIDS_JUST_EST;
					// ѭ���ص������û��Ѿ�ע���˵�tcp�ص�����
					for (i = tcp_procs; i; i = i->next)
					{
						// �������������¼�û��Ķ���
						char whatto = 0;
						// ���ȼ�¼ԭ���� client.collect.
						// server.cllect �ȵ�ֵ��
						// Ȼ�����û��Ļص������л������û���ϲ�þ���
						// �Ƿ��޸�client.collect����server.collect.��ֵ��
						char cc = a_tcp->client.collect;
						char sc = a_tcp->server.collect;
						char ccu = a_tcp->client.collect_urg;
						char scu = a_tcp->server.collect_urg;

						/**
							�����û�ע��ĺ�����һ�α�����
							������ע���ʱ a_tcp->nids_state = NIDS_JUST_EST;
							�������û��Ļص������У���Ҫ�������������
								if (a_tcp->nids_state == NIDS_JUST_EST)
								{
									������д��ע�ắ���Ĳ���Ŀ�ģ�����ר�Ŵ���urg���ģ�
									��ôֻ��Ҫ��a_tcp->client.collect_urg++ �Լ�
									a_tcp->server.collect_urg++���ɡ�
								}
							���û����һ��ʼ��ʱ�򣬰�Ŀ��˵�������ô����ص�����
							�Ͳ��ᱻע�ᵽlibnids���У��������Զ���ᱻ�ص��ģ�ֻ����
							һ��ʼ��ʱ�򱻻ص�һ�¡�

							���⣬����ں����Ĵ����У���ĳ��ע�ắ���е�����Ŀ��ָ��
							������ˣ���ô����ص������ͻᱻ����������Ժ���Ҳ�޷����ص��ˡ�
							�����������У������ a_tcp->client.collect_urg ��
							a_tcp->server.collect_urg�����٣�����û�������ı��������ã�
							��ôlibnids����Ϊ���ע�ắ���Ѿ�û�����ü�ֵ�ˣ����ǻ��
							listener�����г������Ժ���Ҳû�л���ӵ�listener�������ˣ�
							���ǽ�����һ���µ�tcp���ӡ�(�ο� notify����)
							
						**/
						(i->item) (a_tcp, &data);

						/* �����û��ص��������޸ģ��ж��û���Ҫ��ʲô�£��Ӷ�
						   ����whatto��*/
						// ����û�������client.collect�Ĵ�С��
						// ˵���û�ϣ�����տͻ��˵���ͨ��
						// ������λwhatto���λ��
						if (cc < a_tcp->client.collect)
							whatto |= COLLECT_cc;
						// ����û�������client.collect_urg�Ĵ�С
						// ˵���û�ϣ�����տͻ��˽�����
						// ������λwhatto �δε�λ��
						if (ccu < a_tcp->client.collect_urg)
							whatto |= COLLECT_ccu;
						// �������ƣ���whatto�Ĳ�ͬλ��Ϊ1
						if (sc < a_tcp->server.collect)
							whatto |= COLLECT_sc; 
						if (scu < a_tcp->server.collect_urg)
							whatto |= COLLECT_scu;

						// Ĭ��Ϊ��,��ִ��
						if (nids_params.one_loop_less)
						{
							if (a_tcp->client.collect >=2)
							{
								a_tcp->client.collect=cc;
								whatto&=~COLLECT_cc;
							}
							if (a_tcp->server.collect >=2 )
							{
								a_tcp->server.collect=sc;
								whatto&=~COLLECT_sc;
							}
						}

						// ����û���Ҫ��ĳЩ���飬��ôwhatto�Ͳ���Ϊ��
						// ����һ��listener���ҹҵ�ͷ
						if (whatto)
						{
							// ����һ��listener�����û�ע��ĺ�����Ϊ
							// ���listener�ĺ�����
							// ���listener����ص���Ӧ��a_tcp��
							j = mknew(struct lurker_node);
							j->item = i->item;
							j->data = data;
							j->whatto = whatto;
							j->next = a_tcp->listeners;
							a_tcp->listeners = j;
						}
					}
					
					// ���û��listener ���ͷŴ�tcp������
					// ��Ϊ���tcp���ü���
					if (!a_tcp->listeners)
					{
						nids_free_tcp_stream(a_tcp);
						return;
					}

					// ����nids_stat����ΪNIDS_DATA
					// ��ʾ�Ѿ������ݵ����ˣ��������ݻ�û�б��û�����
					a_tcp->nids_state = NIDS_DATA;
				}
				/**
					ע��������һ�μ��˻����ŵĴ��룬����ִ�����֮��Ὣ
					a_tcp��״̬��ΪNIDS_DATA,˵���Ѿ��յ�����һ������
					��Ȼ��������Ѿ����ص����������ˡ�
					���״ֻ̬��Ϊ�˸�����libnids���ձ�����һ����־--˵�����tcp
					�Ѿ���������ֽ׶Σ��������ݴ���׶��ˡ�
				**/
				
			}
			// return;
		}
		
	}


	/**
	   ע��: ִ����������䣬process_tcp��û�н�����

	   - ���ȣ���������ִ��֮������ǵ��������ֵĻ��û�ע��Ļص������Ѿ���ִ���ˡ�
	   - Ȼ���ִ������Ĵ��룬����Ĵ����ǽ��ոս��յ������tcp����
	     �����жϣ��Ƿ���Ҫ���浽a_tcp�Ļ������У������Ƿ��ǻ��ֱ��ġ�
	**/
	

	/********** �����Ƕ����ݱ��ĵĴ����������ж��Ƿ��Ĵλ��� ***************/
	/**
	�Ӵ����Ͽ�:
		- ������ݱ�Ϊ��һ�����֣���ô�����洦������return
		- ������ݱ�Ϊ�ڶ������֣���ô�����洦�����Ҳ�� return
		- ������ݱ�Ϊ���������֣���ô�����洦����󲻻�����return�������������ִ��

	�����TCP/IP��Э��淶:
		- �����������ܹ�Я�����ݡ�
		- ������������Я�������ݣ���ô�������process_tcp���������λص��û�ע�ắ��
		  ���������������:
		  1) ��һ�λص���ʱ����"����������"������ɻص����������forѭ���������
		     ����ʱ������������״̬(�����������forѭ��ǰ)��
		     	a_tcp->client.state = TCP_ESTABLISHED;
				a_tcp->server.state = TCP_ESTABLISHED;
				a_tcp->nids_state = NIDS_JUST_EST;
			 ��ʱ�ص��û�ע�ắ���Ľ���ǣ��û���Ϊtcp�ոս���������Щ��ʼ��������
		  2) �ڶ��λص���ʱ����"�յ��±���"������ɻص������������һ�丳ֵ���
		  	 �������֡�
		  	 	a_tcp->nids_state = NIDS_DATA;
		  	 ������Ϊ��������һ��״̬����ô�ڽ������Ĵ�����ʹ��notify�ص��û�ע��
		  	 �ĺ���ʱ���û�����Ϊtcp�Ѿ������ˣ����ҵ�ǰ��һ�����ı���������

	ע: 
		- �����յ��ı����ǵ���������ʱ���Ż���process_tcp�����г������λص�
		  �û����������󣬵�һ�����������forѭ����ɣ��ڶ����������tcp_queue��ɡ�
		- ����ͨ����£����յ�һ�����ģ���process_tcp�����н��ص�һ���û�ע�ắ����
		- ���յ�����������ʱ����ʱ����һ�λص��û�ע�ắ����ͬʱ��������tcpע��
		  һ��listener����������ÿһ���ڵ������һ���û�ע��ĺ��������һ���û�
		  ע�ắ�������ʱ��û�н�
		  		client->collect
		  		client->urg_collect
		  		server->collect
		  		server->urg_collect
		  �ĸ��е��κ�һ����λ����ô����û�д�Ļص�����������ע�ᵽ���tcp��listener
		  �����У������Ժ���Ҳû�л���ע�����������!!
		  ͬ���أ�����û�д�Ļص����������������ĸ����������ó���0����ô��һ�λص�
		  ������������Ļص������ͻ��listener������ɾ��������û�л�������ӻ�ȥ��!!
		  
	**/

	// �ж��Ƿ������Ĵ�����
	// �����if�У����������"������Ĵ�����"�����������رղ��ͷš�
	if ((this_tcphdr->th_flags & TH_ACK))
	{
		// ����ack
		handle_ack(snd, ntohl(this_tcphdr->th_ack));
		// ������շ��������˻�������
		if (rcv->state == FIN_SENT)
			// ���շ���״̬�޸�Ϊ����ȷ��
			rcv->state = FIN_CONFIRMED;
		// ����շ�˫�����Ѿ�ȷ�ϻ��֣���ô���ͷ�tcp
		if (rcv->state == FIN_CONFIRMED && snd->state == FIN_CONFIRMED)
		{
			struct lurker_node *i;
			// �޸�tcp��״̬Ϊclose
			a_tcp->nids_state = NIDS_CLOSE;
			// ����ִ������listener
			for (i = a_tcp->listeners; i; i = i->next)
				(i->item) (a_tcp, &i->data);
			// �ͷ�tcp
			nids_free_tcp_stream(a_tcp);
			return;
		}
	}


	/**
		�������û������Ĵλ��֣���ô�ͻ���������ߣ�������������ǳ��ؼ���if.
		��������������ο� tcp_queue����ע�͡�
	**/
	// ���������Ĵλ��ֵ���������ô��Ҫ������һ������
	// �ܿ��ܾ��Ƿ��뻺���С�
	if (datalen + (this_tcphdr->th_flags & TH_FIN) > 0)
	{
		tcp_queue(a_tcp, this_tcphdr, snd, rcv,
		          (char *) (this_tcphdr) + 4 * this_tcphdr->th_off,
		          datalen, skblen);
	}
	
	// ���ʹ��ڸ���Ϊ��ǰ���ݰ��Ĵ��ڴ�С
	snd->window = ntohs(this_tcphdr->th_win);
	// ������շ����ڴ����65535���ͷŵ�����ռ�õ��ڴ档
	if (rcv->rmem_alloc > 65535)
		prune_queue(rcv, this_tcphdr, icore);
	// ���û�м����ߣ����ͷ�tcp���ӣ������ͷš�
	if (!a_tcp->listeners)
		nids_free_tcp_stream(a_tcp);
}


void
nids_discard(struct tcp_stream * a_tcp, int num)
{
	if (num < a_tcp->read)
		a_tcp->read = num;
}

void
nids_register_tcp(void (*x))
{
	register_callback(&tcp_procs, x);
}

void
nids_unregister_tcp(void (*x))
{
	unregister_callback(&tcp_procs, x);
}

/**
	��ʼ������tcp.c����Ҫ��ʼ����ȫ�ֱ���

	- Written by shashibici 2014/03/21
**/
static inline void
init_tcp_globles()
{
	int i;
	
	for (i = 0; i < TCP_CORE_NUM; i++)
	{
		tcp_oldest[i] = 0;
		tcp_latest[i] = 0;
		tcp_num[0] = 0;
	}
}
	

/**
	parameters:
		size  :  spsecify the size for tcp_stream table.

	returning:
		return 0 if successful and -1 if fialed.

	Note:
		There are 4 steps.
		- Firstly, allocate tcp_stream_table.
		- Second, allocate "steams_pool" and add it to "free_streams".
		- Third, initialize hash.
		- Fourth, free "nids_tcp_timeouts" if necessary.

	------------------------------------------------------------------
	[tcp_stream_table]������Ҫ�ֲ�����ֻ��Ҫ��֤ÿһ��hash���ܹ�ռ�ò�ͬ
	��cache line���ɡ���Ϊ��ͬ���̷߳��ʵı�Ȼ�ǲ�ͬhash�

	[tcp_stream_table_size]������Ҫ�ֲ�������Ϊ����������Ŀ�г��˱�������
	���ʼ���������κ�ʱ����ֻ���ġ�

	[max_stream]������Ҫ�ֲ�������Ϊ����������Ŀ�г��˱������ڻ��ʼ����
	�����κ�ʱ����ֻ���ġ�

	[streams_pool]���Ծֲ���
		struct tcp_stream* streams_pools[3].
	[free_streams]���Ծֲ���
		struct tcp_stream* frees_streams[3]
	ÿһ���˶�Ӧһ�� streams_pools-frees_streams�ԣ����һ������Ҫ����
	�ܶ���޸ġ�

	[nids_tcp_timeouts] ����Ҫ��ᣬ��Ϊlibnids�����в����������ǣ�������Ҫ
	�õ�nids_tcp_timeouts--workarounds��������Ϊ0��
	
	- Comment by shashibici 2014/03/21
	
*/
int
tcp_init(int size)
{
	
	int i,j;
	struct tcp_timeout *tmp;

	// ��ʼ����Դ�ļ��е�����ȫ�ֱ���
	init_tcp_globles();
	
	// return if no tcp
	if (!size) 
		return 0;

	// set tcp table size;
	tcp_stream_table_size = size;
	
	// allocate a set of memory initialized with 0, as tcp_table
	tcp_stream_table = calloc(tcp_stream_table_size, sizeof(char *));
	if (!tcp_stream_table)
	{
		nids_params.no_mem("tcp_init");
		return -1;
	}

	// set max hash factor
	max_stream = 3 * tcp_stream_table_size / 4;

	// allocate tcp_stream nodes
	for (i = 0; i < TCP_CORE_NUM; i++)
	{
		streams_pool[i] = (struct tcp_stream *) malloc((max_stream + 1) * sizeof(struct tcp_stream));
		// �������ʧ�ܣ���ô�ͷŵ�֮ǰ�Ѿ������˵��ڴ�
		if (!streams_pool[i])
		{
			while (i > 0)
			{
				free(streams_pool[i-1]);
				i--;
			}
			nids_params.no_mem("tcp_init");
			return -1;
		}
	}
	
	// Ϊÿһ���˳�ʼ��һ����Ӧ��streams_pool��free_streams����
	for (i = 0; i < TCP_CORE_NUM; i++)
	{
		for (j = 0; j < max_stream; j++)
		{
			streams_pool[i][j].next_free = &(streams_pool[i][j + 1]);
		}
		streams_pool[i][max_stream].next_free = 0;
		// ������ŵ����ʵ�freeָ����
		free_streams[i] = streams_pool[i];
	}

	init_hash();

	// free all nids_tcp_timouts
	while (nids_tcp_timeouts)
	{
		tmp = nids_tcp_timeouts->next;
		free(nids_tcp_timeouts);
		nids_tcp_timeouts = tmp;
	}
	return 0;
}

#if HAVE_ICMPHDR
#define STRUCT_ICMP struct icmphdr
#define ICMP_CODE   code
#define ICMP_TYPE   type
#else
#define STRUCT_ICMP struct icmp
#define ICMP_CODE   icmp_code
#define ICMP_TYPE   icmp_type
#endif

#ifndef ICMP_DEST_UNREACH
#define ICMP_DEST_UNREACH ICMP_UNREACH
#define ICMP_PROT_UNREACH ICMP_UNREACH_PROTOCOL
#define ICMP_PORT_UNREACH ICMP_UNREACH_PORT
#define NR_ICMP_UNREACH   ICMP_MAXTYPE
#endif


void
process_icmp(u_char * data)
{
	struct ip *iph = (struct ip *) data;
	struct ip *orig_ip;
	STRUCT_ICMP *pkt;
	struct tcphdr *th;
	struct half_stream *hlf;
	int match_addr;
	struct tcp_stream *a_tcp;
	struct lurker_node *i;

	int from_client;
	/* we will use unsigned, to suppress warning; we must be careful with
	   possible wrap when substracting
	   the following is ok, as the ip header has already been sanitized */

	// icmp ֱ�ӷ�װ��ip���ϣ�ȥ��ipͷ����icmp��
	// len����icmp�����ݵĳ���
	unsigned int len = ntohs(iph->ip_len) - (iph->ip_hl << 2);

	// ������Ȳ����ϣ����˳�
	if (len < sizeof(STRUCT_ICMP))
		return;
	// data����һ����ipͷ��ip��������ָ�����㣬ȡipͷ֮�����һ����
	pkt = (STRUCT_ICMP *) (data + (iph->ip_hl << 2));
	// ����У��ͣ������򷵻�
	if (ip_compute_csum((char *) pkt, len))
		return;
	// icmp�����Ǳ���"Ŀ�Ĳ��ɴ�",�򷵻�
	if (pkt->ICMP_TYPE != ICMP_DEST_UNREACH)
		return;
	// ������Ŀ�Ĳ��ɴ�

	
	/* ok due to check 7 lines above */
	len -= sizeof(STRUCT_ICMP);
	// sizeof(struct icmp) is not what we want here

	if (len < sizeof(struct ip))
		return;

	// orig_ip�ǳ������ip��ͷ(Ŀ�Ĳ��ɴ��icmp�����ݡ�)
	// �ο�:http://wenku.baidu.com/link?url=7v9LjU1shidls6JHAGDThlZY5ml4GYK25v8On-Fxa6MDwViRtkNOdJGqvBiFSkEzQLOtZ3tlmnKyvSTjKJ1XQoP84nAvNXb9XVHOaEzaiOm
	orig_ip = (struct ip *) (((char *) pkt) + 8);
	// len��icmp��ȥicmp��ͷ����һ���ֵĳ���
	// ����˳���С�ڹ涨�ĳ���(����ip��ͷ+����ip���ݵ�ǰ8�ֽ�)�������
	if (len < (unsigned)(orig_ip->ip_hl << 2) + 8)
		return;
	
	/* subtraction ok due to the check above */
	// len��ȥ����ipͷ�ĳ���
	len -= orig_ip->ip_hl << 2;

	// ����Ƕ˿ڲ��ɴ�
	if (     (pkt->ICMP_CODE & 15) == ICMP_PROT_UNREACH ||
	        (pkt->ICMP_CODE & 15) == ICMP_PORT_UNREACH)
	        // ���ַ�ǶԵ�
		match_addr = 1;
	else
		// �����ַ�Ǵ��
		match_addr = 0;

	
	if (pkt->ICMP_CODE > NR_ICMP_UNREACH)
		return;

	// ��һ�ִ��������Ӧ�÷���
	if (match_addr && (iph->ip_src.s_addr != orig_ip->ip_dst.s_addr))
		return;
	// �����������tcp��ip����ô����������
	if (orig_ip->ip_p != IPPROTO_TCP)
		return;

	// ����������tcp��
	// ����ip��������ͷ����������ݣ���tcpͷ
	th = (struct tcphdr *) (((char *) orig_ip) + (orig_ip->ip_hl << 2));
	// �������û����ôһ��tcp���ӣ������������У�����
	if (!(a_tcp = find_stream(th, orig_ip, &from_client)))
		return;

	/*-----------------------------------------------------
	a_tcp->addr.dest��16λ�ģ� iph->ip_dst.s_addr��32λ��???
	ǰ����һ���˿ںţ�������һ��ip��ַ
	------------------------------------------------------*/	
	if (a_tcp->addr.dest == iph->ip_dst.s_addr)
		hlf = &a_tcp->server;
	else
		hlf = &a_tcp->client;
	/*-------------------------------------------------------
	-------------------------------------------------------*/
	// ����Ѿ����Ͳ��ҽӵ������ˣ��ͷ���
	if (hlf->state != TCP_SYN_SENT && hlf->state != TCP_SYN_RECV)
		return;
	// ���������������ǿ��reset
	a_tcp->nids_state = NIDS_RESET;
	// reset�Ĳ���ʱ���ͷ����ӣ������ڴ�֮ǰҪ����ÿһ��listener
	for (i = a_tcp->listeners; i; i = i->next)
		(i->item) (a_tcp, &i->data);
	nids_free_tcp_stream(a_tcp);
}
