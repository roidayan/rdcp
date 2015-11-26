// vim: set ai ts=8 sw=8 softtabstop=8:
/*
 * Copyright (c) 2015 Roi Dayan, Slava Shwartsman.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <byteswap.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <dirent.h>
#include <libgen.h>
#include <linux/limits.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/arch.h>
#include "list.h"

static int verbose = 0;

#define VERBOSE_LOG(level, fmt, ...)             \
	if (verbose >= level) {                  \
		printf(fmt, ##__VA_ARGS__);      \
	}

#define uint64_from_ptr(p) (uint64_t)(uintptr_t)(p)
#define ptr_from_int64(p) (void *)(unsigned long)(p)

enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	SEND_METADATA,
	SEND_DATA,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	DISCONNECTED,
	ERROR
};

struct rdma_info {
	int      id;
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

struct metadata_info {
	int version; // protocol version
	long size;
	char src_path[PATH_MAX];
	char dst_path[PATH_MAX];
};

#define RDCP_PORT 7171
#define MAX_TASKS 64
#define MAX_WC 16
#define MAX_WR (MAX_TASKS + 1)
#define BUF_SIZE (64 * 1024)
#define METADATA_WR_ID 0xfffffffffffffffaULL
#define CQ_DEPTH ((MAX_TASKS + 1) * 2)

struct rdcp_task {
	struct rdma_info   buf;
	struct ibv_sge     sgl;
	struct ibv_mr      *mr;
	union {
		struct ibv_recv_wr rq_wr;
		struct ibv_send_wr sq_wr;
	};
	struct list_head   task_list;
};

/*
 * Control block struct.
 */
struct rdcp_cb {
	int server;			/* 0 iff client */
	pthread_t cqthread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	int fd;
	FILE *fp;
	int sent_count;
	int recv_count;
	int use_null;

	struct metadata_info metadata;
	struct ibv_mr *metadata_mr;
	struct ibv_sge metadata_sgl;
	struct ibv_recv_wr md_recv_wr;
	struct ibv_send_wr md_send_wr;

	struct rdcp_task recv_tasks[MAX_TASKS];
	struct rdcp_task send_tasks[MAX_TASKS];
	struct list_head task_free;
	struct list_head task_alloc;

	struct ibv_send_wr rdma_sq_wr[MAX_TASKS];	/* rdma work request record */
	struct ibv_sge rdma_sgl[MAX_TASKS];	/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	struct ibv_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	struct ibv_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	sem_t sem;

	struct sockaddr_storage sin;
	uint16_t port;			/* dst port in NBO */
	int size;			/* ping data size */

	/* CM stuff */
	pthread_t cmthread;
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
};

long long current_timestamp()
{
	long long milliseconds;
	struct timeval te; 
        gettimeofday(&te, NULL); // get current time
	milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // caculate milliseconds
	return milliseconds;
}

static int rdcp_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rdcp_cb *cb = cma_id->context;

	VERBOSE_LOG(3, "cma_event type %s cma_id %p (%s)\n",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "me" : "remote");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			perror("rdma_resolve_route");
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		VERBOSE_LOG(3, "child cma %p\n", cb->child_cm_id);
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		VERBOSE_LOG(3, "ESTABLISHED\n");

		/*
		 * Server will wake up when first RECV completes.
		 */
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		fprintf(stderr, "cma event %s, error %d\n",
			rdma_event_str(event->event), event->status);
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		VERBOSE_LOG(3, "%s DISCONNECT EVENT...\n",
			cb->server ? "server" : "client");
		cb->state = DISCONNECTED;
		// TODO nooo
		sync();
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		fprintf(stderr, "cma detected device removal!!!!\n");
		ret = -1;
		break;

	default:
		fprintf(stderr, "unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

static int server_response(struct rdcp_cb *cb, struct ibv_wc *wc)
{
	int ret;
	int id = wc->wr_id;
	struct ibv_send_wr *bad_wr;
	struct rdcp_task *send_task = &cb->send_tasks[id];
	struct rdcp_task *recv_task = &cb->recv_tasks[id];

	send_task->buf.size = recv_task->buf.size;
	ret = ibv_post_send(cb->qp, &send_task->sq_wr, &bad_wr);
	if (ret) {
		perror("server response error");
		return ret;
	}
	VERBOSE_LOG(1, "server posted go ahead\n");

	return 0;
}

static int server_open_dest(struct rdcp_cb *cb)
{
	DIR *d;

	d = opendir(cb->metadata.dst_path);
	if (d) {
		closedir(d);
		// XXX: can overflow dst_path
		strcat(cb->metadata.dst_path, "/");
		strcat(cb->metadata.dst_path,
			basename(cb->metadata.src_path));
	} else if (ENOENT == errno) {
		// ok. create a file.
	} else if (ENOTDIR == errno){
		// ok. It's not a dir. overwrite
	} else {
		perror("open error");
		return errno;
	}

	printf("Content of metadata src: %s dst: %s\n",
		cb->metadata.src_path,
		cb->metadata.dst_path);
	fflush(stdout);

	cb->fd = open(cb->metadata.dst_path,
			O_WRONLY | O_CREAT | O_TRUNC,
			S_IRUSR | S_IWUSR);
	if (cb->fd < 0) {
		perror("failed to open file");
		return errno;
	}

	VERBOSE_LOG(1, "open fd %d\n", cb->fd);

	return 0;
}

static int server_recv_metadata(struct rdcp_cb *cb, struct ibv_wc *wc)
{
	int ret = 0;

	VERBOSE_LOG(1, "Got metadata, replying\n");
	//TODO: Send protocol version to client to verify
	if (!cb->use_null) {
		ret = server_open_dest(cb);
		if (ret)
			goto out;
	}
	
	ret = ibv_post_send(cb->qp, &cb->md_send_wr, NULL);
	if (ret) {
		perror("failed reply metadata");
		close(cb->fd);
	}

out:
	return ret;
}

static int server_recv(struct rdcp_cb *cb, struct ibv_wc *wc)
{
	struct rdcp_task *task;
	int ret;
	struct ibv_send_wr *bad_wr;
	int i;
	
	if (wc->wr_id == METADATA_WR_ID) {
		return server_recv_metadata(cb, wc);
	}

	i = wc->wr_id;
	task = &cb->recv_tasks[i];

	if (wc->byte_len != sizeof(struct rdma_info)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	cb->remote_rkey = task->buf.rkey;
	cb->remote_addr = task->buf.buf;
	cb->remote_len  = task->buf.size;
	VERBOSE_LOG(1, "Received rkey %x addr %" PRIx64 " len %d from peer\n",
		  cb->remote_rkey, task->buf.buf, cb->remote_len);

	VERBOSE_LOG(1, "rdma read %d %p\n", i, &cb->rdma_buf[i * BUF_SIZE]);

	/* Issue RDMA Read. */
	cb->rdma_sq_wr[i].opcode = IBV_WR_RDMA_READ;
	cb->rdma_sq_wr[i].wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr[i].wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr[i].sg_list->length = cb->remote_len;
	cb->rdma_sq_wr[i].wr_id = i;

	ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr[i], &bad_wr);
	if (ret) {
		perror("post rdma read failed");
		return ret;
	}
	VERBOSE_LOG(3, "server posted rdma read req \n");

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct rdcp_cb *cb, struct ibv_wc *wc)
{
	if (wc->wr_id == METADATA_WR_ID) {
		sem_post(&cb->sem);
		return 0;
	}

	if (wc->byte_len != sizeof(struct rdma_info)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static int rearm_completions(struct rdcp_cb *cb)
{
	int i, ret;
	static int rearm = 0;

	rearm++;
	if (rearm == MAX_TASKS) {
		struct ibv_recv_wr *wr = &cb->recv_tasks[0].rq_wr;
		for (i = 1; i < MAX_TASKS; i++) {
			wr->next = &cb->recv_tasks[i].rq_wr;
			wr = &cb->recv_tasks[i].rq_wr;
		}
		ret = ibv_post_recv(cb->qp, &cb->recv_tasks[0].rq_wr, NULL);
		if (ret) {
			perror("post recv error");
			goto error;
		}
		rearm = 0;
	}

	return 0;

error:
	return ret;
}

static int handle_wc(struct rdcp_cb *cb, struct ibv_wc *wc)
{
	int ret = 0;
	int i;
	int size;

	if (wc->status) {
		if (wc->status != IBV_WC_WR_FLUSH_ERR) {
			fprintf(stderr,
				"cq completion id %lu failed with status %d (%s)\n",
				(unsigned long) wc->wr_id,
				wc->status,
				ibv_wc_status_str(wc->status));
			ret = -1;
		}
		goto error;
	}

	switch (wc->opcode) {
	case IBV_WC_SEND:
		VERBOSE_LOG(3, "send completion\n");
		break;

	case IBV_WC_RDMA_WRITE:
		VERBOSE_LOG(3, "rdma write completion\n");
		cb->state = RDMA_WRITE_COMPLETE;
		sem_post(&cb->sem);
		break;

	case IBV_WC_RDMA_READ:
		VERBOSE_LOG(3, "rdma read completion\n");
		i = wc->wr_id;

		VERBOSE_LOG(1, "fd %d i %d rdma_buf %p\n", cb->fd, i, &cb->rdma_buf[i*BUF_SIZE]);
		if (!cb->use_null) {
			size = write(cb->fd, &cb->rdma_buf[i*BUF_SIZE], cb->recv_tasks[i].buf.size);
		
			if (size < 0) {
			    printf("error writing data\n");
			    ret = size;
			    goto error;
			}
		}
		ret = server_response(cb, wc);
		if (ret)
			goto error;
		break;

	case IBV_WC_RECV:
		VERBOSE_LOG(3, "recv completion\n");
		ret = cb->server ? server_recv(cb, wc) :
				   client_recv(cb, wc);
		if (ret) {
			perror("recv wc error");
			goto error;
		}

		if (wc->wr_id == METADATA_WR_ID)
			break;

		rearm_completions(cb);

		cb->recv_count++;
		if (cb->recv_count >= cb->sent_count)
			sem_post(&cb->sem);
		break;

	default:
		VERBOSE_LOG(3, "unknown!!!!! completion\n");
		ret = -1;
		goto error;
	}

	if (ret) {
		fprintf(stderr, "poll error %d\n", ret);
		goto error;
	}

	return 0;

error:
	if (ret < 0) {
		cb->state = ERROR;
//		if (cb->server)
//			rdma_disconnect(cb->child_cm_id);
//		else
//			rdma_disconnect(cb->cm_id);
	}
	sem_post(&cb->sem);
	return ret;
}

static int rdcp_cq_event_handler(struct rdcp_cb *cb)
{
	struct ibv_wc wcs[MAX_WC];
	int i, n;

	VERBOSE_LOG(3, "poll\n");
	while ((n = ibv_poll_cq(cb->cq, MAX_WC, wcs)) > 0) {
		for (i = 0; i < n; i++) {
			handle_wc(cb, &wcs[i]);
		}
	}

	if (n < 0)
	    return n;

	return 0;
}

static int rdcp_accept(struct rdcp_cb *cb)
{
	int ret;

	VERBOSE_LOG(3, "accepting client connection request\n");

	ret = rdma_accept(cb->child_cm_id, NULL);
	if (ret) {
		perror("rdma_accept");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state == ERROR) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	return 0;
}

static int rdcp_setup_wr(struct rdcp_cb *cb)
{
	int i;
	char* buf = cb->rdma_buf;

	cb->metadata_mr = ibv_reg_mr(cb->pd,
                                            &cb->metadata,
                                            sizeof(struct metadata_info),
                                            IBV_ACCESS_LOCAL_WRITE);
	if (!cb->metadata_mr) {
		fprintf(stderr, "metadata reg_mr failed\n");
		return errno;
	}

	cb->metadata_sgl.addr = uint64_from_ptr(&cb->metadata);
	cb->metadata_sgl.length = sizeof(struct metadata_info);
	cb->metadata_sgl.lkey = cb->metadata_mr->lkey;
	cb->md_send_wr.opcode = IBV_WR_SEND;
	cb->md_send_wr.send_flags = IBV_SEND_SIGNALED;
	cb->md_send_wr.sg_list = &cb->metadata_sgl;
	cb->md_send_wr.num_sge = 1;
	cb->md_send_wr.wr_id = METADATA_WR_ID;

	cb->md_recv_wr.sg_list = &cb->metadata_sgl;
	cb->md_recv_wr.num_sge = 1;
	cb->md_recv_wr.wr_id = METADATA_WR_ID;

	for (i = 0; i < MAX_TASKS; i++) {
		cb->rdma_sgl[i].addr = uint64_from_ptr(buf);
		cb->rdma_sgl[i].length = BUF_SIZE;
		cb->rdma_sgl[i].lkey = cb->rdma_mr->lkey;
		cb->rdma_sq_wr[i].send_flags = IBV_SEND_SIGNALED;
		cb->rdma_sq_wr[i].sg_list = &cb->rdma_sgl[i];
		cb->rdma_sq_wr[i].num_sge = 1;
		buf += BUF_SIZE;
	}

	return 0;
}

static void rdcp_free_buffers(struct rdcp_cb *cb)
{
	int i;

	VERBOSE_LOG(3, "rdcp_free_buffers called on cb %p\n", cb);
	if (cb->start_buf);
		free(cb->start_buf);
	if (cb->rdma_mr)
		ibv_dereg_mr(cb->rdma_mr);
	if (cb->rdma_buf)
		free(cb->rdma_buf);

	for (i = 0; i < MAX_TASKS; i++) {
		struct rdcp_task *recv_task = &cb->recv_tasks[i];
		struct rdcp_task *send_task = &cb->send_tasks[i];

		if (recv_task->mr)
			ibv_dereg_mr(recv_task->mr);
		if (send_task->mr)
			ibv_dereg_mr(send_task->mr);
	}
	if (cb->metadata_mr)
		ibv_dereg_mr(cb->metadata_mr);
//	if (!cb->server) {
		ibv_dereg_mr(cb->start_mr);
		free(cb->start_buf);
//	}
}

static int rdcp_setup_buffers(struct rdcp_cb *cb)
{
	int ret;
	int i;

	VERBOSE_LOG(3, "rdcp_setup_buffers called on cb %p\n", cb);

	// TODO: handle error
	for (i = 0; i < MAX_TASKS; i++) {
	    struct rdcp_task *recv_task = &cb->recv_tasks[i];
	    struct rdcp_task *send_task = &cb->send_tasks[i];

	    recv_task->buf.id = i;
	    send_task->buf.id = i;

	    recv_task->mr = ibv_reg_mr(cb->pd,
					    &recv_task->buf,
					    sizeof(struct rdma_info),
					    IBV_ACCESS_LOCAL_WRITE);
	    if (!recv_task->mr) {
		    fprintf(stderr, "recv_buf reg_mr failed\n");
		    ret = errno;
		    goto error;
	    }
	    recv_task->sgl.addr = uint64_from_ptr(&recv_task->buf);
	    recv_task->sgl.length = sizeof(struct rdma_info);
	    recv_task->sgl.lkey = recv_task->mr->lkey;
	    recv_task->rq_wr.sg_list = &recv_task->sgl;
	    recv_task->rq_wr.num_sge = 1;
	    recv_task->rq_wr.wr_id = i;

	    send_task->mr = ibv_reg_mr(cb->pd,
					    &send_task->buf,
					    sizeof(struct rdma_info),
					    0);
	    if (!send_task->mr) {
		    fprintf(stderr, "send_buf reg_mr failed\n");
		    ret = errno;
		    goto error;
	    }
	}

	cb->rdma_buf = valloc(BUF_SIZE * MAX_TASKS);
	if (!cb->rdma_buf) {
		fprintf(stderr, "rdma_buf alloc failed\n");
		ret = -ENOMEM;
		goto error;
	}
	memset(cb->rdma_buf, 0, BUF_SIZE * MAX_TASKS);

	cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, BUF_SIZE * MAX_TASKS,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		fprintf(stderr, "rdma_buf reg_mr failed\n");
		ret = errno;
		goto error;
	}

	// TODO: server use rdma_buf and not start_buf but still use send_tasks
	// TODO: client use start_buf and not rdma_buf

//	if (!cb->server) {
		char* start_buf;
		cb->start_buf = valloc(BUF_SIZE * MAX_TASKS);
		if (!cb->start_buf) {
			fprintf(stderr, "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto error;
		}
		start_buf = cb->start_buf;

		cb->start_mr = ibv_reg_mr(cb->pd, cb->start_buf, BUF_SIZE * MAX_TASKS,
					  IBV_ACCESS_LOCAL_WRITE | 
					  IBV_ACCESS_REMOTE_READ |
					  IBV_ACCESS_REMOTE_WRITE);
		if (!cb->start_mr) {
			fprintf(stderr, "start_buf reg_mr failed\n");
			ret = errno;
			goto error;
		}

		INIT_LIST_HEAD(&cb->task_free);
		INIT_LIST_HEAD(&cb->task_alloc);

		for (i = 0; i < MAX_TASKS; i++) {
			struct rdcp_task *send_task = &cb->send_tasks[i];

		        list_add_tail(&send_task->task_list, &cb->task_free);
			send_task->buf.buf = uint64_from_ptr(start_buf);
			send_task->buf.size = BUF_SIZE;
			send_task->buf.rkey = cb->start_mr->rkey;

			send_task->sgl.addr = uint64_from_ptr(&send_task->buf);
			send_task->sgl.length = sizeof(struct rdma_info);
			send_task->sgl.lkey = send_task->mr->lkey; 

			start_buf += BUF_SIZE;

			send_task->sq_wr.opcode = IBV_WR_SEND;
			send_task->sq_wr.send_flags = IBV_SEND_SIGNALED;
			send_task->sq_wr.sg_list = &send_task->sgl;
			send_task->sq_wr.num_sge = 1;
			send_task->sq_wr.wr_id = 200 + i;
		}
//	}

	ret = rdcp_setup_wr(cb);
	if (ret)
		goto error;

	VERBOSE_LOG(3, "allocated & registered buffers...\n");
	return 0;

error:
	rdcp_free_buffers(cb);
	return ret;
}

static int rdcp_create_qp(struct rdcp_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	// TODO: check values
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = MAX_WR;
	init_attr.cap.max_recv_wr = MAX_WR;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void rdcp_free_qp(struct rdcp_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}

static int rdcp_setup_qp(struct rdcp_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		// TODO use perror for verbs error prints
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return errno;
	}
	VERBOSE_LOG(3, "created pd %p\n", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		// TODO: use real labels. e.g. err_create_channel or free_pd
		goto free_pd;
	}
	VERBOSE_LOG(3, "created channel %p\n", cb->channel);

	// TODO: do we really need *2 ?
	cb->cq = ibv_create_cq(cm_id->verbs, CQ_DEPTH, cb,
				cb->channel, 0);
	if (!cb->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto free_comp_chan;
	}
	VERBOSE_LOG(3, "created cq %p\n", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto free_cq;
	}

	ret = rdcp_create_qp(cb);
	if (ret) {
		perror("rdma_create_qp");
		goto free_cq;
	}
	VERBOSE_LOG(3, "created qp %p\n", cb->qp);
	return 0;

free_cq:
	ibv_destroy_cq(cb->cq);
free_comp_chan:
	ibv_destroy_comp_channel(cb->channel);
free_pd:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void *cm_thread(void *arg)
{
	struct rdcp_cb *cb = arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(cb->cm_channel, &event);
		if (ret) {
			perror("rdma_get_cm_event");
			exit(ret);
		}
		ret = rdcp_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret)
			exit(ret);
		if (cb->state >= DISCONNECTED) {
			if (cb->server)
				rdma_disconnect(cb->child_cm_id);
			VERBOSE_LOG(1, "post disconnect\n");
			// wakeup cq_thread
			ibv_post_send(cb->qp, &cb->send_tasks[0].sq_wr, NULL);
			exit(ret);
		}
	}
}

static void *cq_thread(void *arg)
{
	struct rdcp_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;
	long long tick1, tick2;
	
	VERBOSE_LOG(3, "cq_thread started.\n");

	while (1) {	
		pthread_testcancel();

		tick1 = current_timestamp();
		VERBOSE_LOG(3, "wait for cq event\n");

		ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
		if (ret) {
			fprintf(stderr, "Failed to get cq event!\n");
			pthread_exit(NULL);
		}

		tick2 = current_timestamp();
		if (tick2 - tick1 > 1000) {
		    VERBOSE_LOG(1, "got pause %f\n", (tick2-tick1)/1000.0);
		}

		if (ev_cq != cb->cq) {
			fprintf(stderr, "Unknown CQ!\n");
			pthread_exit(NULL);
		}

		ibv_ack_cq_events(cb->cq, 1);

		VERBOSE_LOG(3, "handle cq event\n");
		ret = rdcp_cq_event_handler(cb);
		if (ret){
			VERBOSE_LOG(3, "ERROR: handle event, killing cq_thread\n");
			pthread_exit(NULL);
		}

		if (cb->state == ERROR)
		    printf("ERROR state\n");
		if (cb->state >= DISCONNECTED)
		    pthread_exit(NULL);

		ret = ibv_req_notify_cq(cb->cq, 0);
		if (ret) {
			fprintf(stderr, "Failed to set notify!\n");
			pthread_exit(NULL);
		}
	}
}

static int rdcp_test_server(struct rdcp_cb *cb)
{
	int ret = 0;
	
	while (1) {
		/* Wait for client's Start STAG/TO/Len */
		sem_wait(&cb->sem);
	}

	close(cb->fd);
	cb->fd = -1;

	return (cb->state == DISCONNECTED) ? 0 : ret;
}

static int rdcp_bind_server(struct rdcp_cb *cb)
{
	int ret;

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *) &cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *) &cb->sin)->sin6_port = cb->port;

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *) &cb->sin);
	if (ret) {
		perror("rdma_bind_addr");
		return ret;
	}
	VERBOSE_LOG(3, "rdma_bind_addr successful\n");
	VERBOSE_LOG(3, "rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		perror("rdma_listen");
		return ret;
	}

	return 0;
}

static int rdcp_run_server(struct rdcp_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int i, ret;

	ret = rdcp_bind_server(cb);
	if (ret)
		return ret;

	sem_wait(&cb->sem);
	if (cb->state != CONNECT_REQUEST) {
		fprintf(stderr, "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	ret = rdcp_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = rdcp_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rdcp_setup_buffers failed: %d\n", ret);
		goto free_qp;
	}
	
	ret = ibv_post_recv(cb->qp, &cb->md_recv_wr, &bad_wr);
	if (ret) {
		perror("error post recv metadata");
		goto free_buffers;
	}

	for (i = 0; i < MAX_TASKS; i++) {
	    ret = ibv_post_recv(cb->qp, &cb->recv_tasks[i].rq_wr, &bad_wr);
	    if (ret) {
		    fprintf(stderr, "error post recv task %d: %m\n", i);
		    goto free_buffers;
	    }
	}

	pthread_create(&cb->cqthread, NULL, cq_thread, cb);

	ret = rdcp_accept(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		cb->state = ERROR;
		goto join_cq_thread;
	}

	ret = rdcp_test_server(cb);
	if (ret) {
		fprintf(stderr, "rdcp server failed: %d\n", ret);
		goto discon;
	}

	ret = 0;

discon:
	if (cb->child_cm_id) {
		rdma_disconnect(cb->child_cm_id);
		rdma_destroy_id(cb->child_cm_id);
	}
join_cq_thread:
	pthread_join(cb->cqthread, NULL);
free_buffers:
	// TODO: wait for flush
	sleep(1);
	rdcp_free_buffers(cb);
free_qp:
	rdcp_free_qp(cb);
	return ret;
}

static void print_send_status(struct rdcp_cb *cb, long *_total_size, int print)
{
	static long long tick1 = 0, tick2 = 0;
	static long sent_size = 0;
	long total_size = *_total_size;

	if (tick1 == 0)
		tick1 = current_timestamp();

	tick2 = current_timestamp();
	VERBOSE_LOG(3, "tick %lld\n", tick2);

	if (tick2 - tick1 >= 1000 || print) {
		float sec = (tick2 - tick1) / 1000.0;
		long mb = 1024 * 1024;
		long gb = 1024 * mb;
		int percent = 100;
		long eta = 0;
		char size_spec[3] = "MB";
		long div = mb;
		float bytes_per_sec = total_size / sec;

		sent_size += total_size;
		if (cb->metadata.size > 0) {
			percent = sent_size * 100 / cb->metadata.size;
			eta = ((cb->metadata.size - sent_size) / bytes_per_sec);
		}
		if (sent_size > 10 * gb) {
			strcpy(size_spec, "GB");
			div = gb;
		}
		printf("\r%-20s %d%% %5d%s %5dMB/s  %02d:%02d ETA",
			basename(cb->metadata.src_path),
			percent,
			(int)(sent_size / div),
			size_spec,
			(int)(bytes_per_sec / mb),
			(int)(eta / 60),
			(int)(eta % 60));
		fflush(stdout);
		total_size = 0;
		*_total_size = 0;
		tick1 = tick2;
	}
}

static int rdcp_test_client(struct rdcp_cb *cb)
{
	int i, ret = 0;
	struct ibv_send_wr *bad_wr;
	int size;
	long total_size = 0;

	if (!cb->use_null) {
		cb->fd = open(cb->metadata.src_path, O_RDONLY);
		VERBOSE_LOG(1, "open fd %d\n", cb->fd);
		if (cb->fd < 0) {
		    perror("Couldn't open file");
		    goto out;
		}
		cb->fp = fdopen(cb->fd, "rb");
		fseek(cb->fp, 0, SEEK_END);
		cb->metadata.size = ftell(cb->fp);
		if (cb->metadata.size <= 0) {
			perror("size error");
			goto out;
		}
		fseek(cb->fp, 0, SEEK_SET);
	} else {
		cb->metadata.size = 0;
	}

	// send meta
	//
	VERBOSE_LOG(1, "Sending metadata to server\n");
	ret = ibv_post_send(cb->qp, &cb->md_send_wr, &bad_wr);
	sem_wait(&cb->sem);
	// TODO check state we got metata and not error/disconnect

	if (cb->state >= DISCONNECTED) {
		ret = -1;
		goto out;
	}
	// send file
	//
	VERBOSE_LOG(1, "start\n");
	do {
		cb->state = RDMA_READ_ADV;
		cb->recv_count = 0;
		cb->sent_count = 0;

		VERBOSE_LOG(1, "send tasks\n");
		for (i = 0; i < MAX_TASKS; i++) {
			struct rdcp_task *send_task = &cb->send_tasks[i];
			struct rdma_info *info = &send_task->buf;

			if (cb->use_null) {
				size = BUF_SIZE;
			} else {
				size = read(cb->fd, ptr_from_int64(info->buf), BUF_SIZE);
				VERBOSE_LOG(1, "Read size = %d\n", size);
			}

			if (size == 0)
			    break;

			if (size < 0) {
			    perror("error reading file\n");
			    break;
			}

			VERBOSE_LOG(3, "RDMA addr %" PRIx64" rkey %x len %d\n",
				  info->buf, info->rkey, info->size);
			info->size = size;
			ret = ibv_post_send(cb->qp, &send_task->sq_wr, &bad_wr);
			if (ret) {
				perror("send task failed");
				break;
			}
			
			cb->sent_count++;
			total_size += size;
		}

		/* Wait for server to ACK */
		VERBOSE_LOG(1, "wait for server respond\n");
		while (cb->recv_count < i) {
			sem_wait(&cb->sem);
		}

		print_send_status(cb, &total_size, size < BUF_SIZE);
	} while (size > 0);

	printf("\n");
	VERBOSE_LOG(1, "done\n");

out:
	fclose(cb->fp);
	cb->fd = -1;

	return (cb->state == DISCONNECTED) ? 0 : ret;
}

static int rdcp_connect_client(struct rdcp_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;
// if we need retry because we miss recv wr
//	conn_param.rnr_retry_count = 6;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		perror("rdma_connect");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	VERBOSE_LOG(3, "rmda_connect successful\n");
	return 0;
}

static int rdcp_bind_client(struct rdcp_cb *cb)
{
	int ret;

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *) &cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *) &cb->sin)->sin6_port = cb->port;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &cb->sin, 2000);
	if (ret) {
		perror("rdma_resolve_addr");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		fprintf(stderr, "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	VERBOSE_LOG(3, "rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int rdcp_run_client(struct rdcp_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int i, ret;

	ret = rdcp_bind_client(cb);
	if (ret)
		return ret;

	ret = rdcp_setup_qp(cb, cb->cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = rdcp_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rdcp_setup_buffers failed: %d\n", ret);
		goto free_qp;
	}

	ret = ibv_post_recv(cb->qp, &cb->md_recv_wr, &bad_wr);

	for (i = 0; i < MAX_TASKS; i++) {
	    ret = ibv_post_recv(cb->qp, &cb->recv_tasks[i].rq_wr, &bad_wr);
	    if (ret) {
		    fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
		    goto free_buffers;
	    }
	}

	pthread_create(&cb->cqthread, NULL, cq_thread, cb);

	ret = rdcp_connect_client(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		cb->state = ERROR;
		goto join_cq_thread;
	}

	ret = rdcp_test_client(cb);
	if (ret) {
		fprintf(stderr, "rdcp client failed: %d\n", ret);
		goto discon;
	}

	ret = 0;
discon:
	if (cb->cm_id)
		rdma_disconnect(cb->cm_id);
join_cq_thread:
	pthread_join(cb->cqthread, NULL);
	// TODO: wait for flush
	sleep(1);
free_buffers:
	rdcp_free_buffers(cb);
free_qp:
	rdcp_free_qp(cb);

	return ret;
}

static int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("getaddrinfo failed - invalid hostname or IP address\n");
		return ret;
	}

	if (res->ai_family == PF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else
		ret = -1;
	
	freeaddrinfo(res);
	return ret;
}

static void usage()
{
	printf("usage: rdcp -s [-vt] [-p port]\n");
	printf("       rdcp [-vt] [-p port] file1 host2:file2\n\n");
	printf("  -s        server mode\n");
	printf("  -t        null device\n");
	printf("  -v        verbose mode\n");
	printf("  -p port   port\n");
}

int main(int argc, char *argv[])
{
	struct rdcp_cb *cb;
	int op;
	int ret = 0;

	cb = malloc(sizeof(*cb));
	if (!cb)
		return -ENOMEM;

	memset(cb, 0, sizeof(*cb));
	cb->fd = -1;
	cb->server = 0;
	cb->state = IDLE;
	cb->sin.ss_family = AF_INET;
	// TODO: use htons when set in sockaddr and not here
	cb->port = htons(RDCP_PORT);
	sem_init(&cb->sem, 0, 0);

	opterr = 0;
	while ((op = getopt(argc, argv, "Pp:tsv")) != -1) {
		switch (op) {
		case 't':
			cb->use_null = 1;
			break;
		case 'p':
			cb->port = htons(atoi(optarg));
			VERBOSE_LOG(3, "port %d\n", (int) atoi(optarg));
			break;
		case 's':
			cb->server = 1;
			VERBOSE_LOG(3, "server\n");
			break;
		case 'v':
			verbose++;
			VERBOSE_LOG(3, "verbose\n");
			break;
		default:
			usage();
			ret = -EINVAL;
			goto out;
		}
	}
	if (ret)
		goto out;

	if (cb->server) {
		if (optind < argc) {
			usage();
			ret = -EINVAL;
			goto out;
		}
	} else {
		if (optind + 1 >= argc) {
			usage();
			ret = -EINVAL;
			goto out;
		}
		char *p;

		strncpy(cb->metadata.src_path, argv[optind], PATH_MAX);
		optind++;
		p = strchr(argv[optind], ':');
		if (!p) {
			usage();
			ret = -EINVAL;
			goto out;
		}
		*p = '\0';
		ret = get_addr(argv[optind], (struct sockaddr *) &cb->sin);
		strncpy(cb->metadata.dst_path, p+1, PATH_MAX);

		VERBOSE_LOG(1, "addr %s\n", argv[optind]);
		VERBOSE_LOG(1, "src %s\n", cb->metadata.src_path);
		VERBOSE_LOG(1, "dst %s\n", cb->metadata.dst_path);
	}

	if (cb->server == -1) {
		usage();
		ret = -EINVAL;
		goto out;
	}

	cb->cm_channel = rdma_create_event_channel();
	if (!cb->cm_channel) {
		perror("rdma_create_event_channel");
		ret = errno;
		goto out;
	}

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
	if (ret) {
		perror("rdma_create_id");
		goto destroy_event_chan;
	}
	VERBOSE_LOG(3, "created cm_id %p\n", cb->cm_id);

	pthread_create(&cb->cmthread, NULL, cm_thread, cb);

	if (cb->server) {
		ret = rdcp_run_server(cb);
	} else {
		ret = rdcp_run_client(cb);
	}

	VERBOSE_LOG(3, "destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
destroy_event_chan:
	rdma_destroy_event_channel(cb->cm_channel);
out:
	free(cb);
	return ret;
}
