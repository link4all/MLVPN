/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *   Adapted for mlvpn by Laurent Coustet (c) 2015
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <inttypes.h>
#include <string.h>
#include <sys/queue.h>

#include "reorder.h"
#include "log.h"

#define MARK

/* A generic circular buffer */
struct cir_buffer {
    unsigned int size;   /**< Number of pkts that can be stored */
    unsigned int mask;   /**< [buffer_size - 1]: used for wrap-around */
    unsigned int head;   /**< insertion point in buffer */
    unsigned int tail;   /**< extraction point in buffer */
    mlvpn_pkt_t **pkts;
};

#ifdef MARK

struct pktlist 
{
  mlvpn_pkt_t *pkt;
  struct pktlist *last;
  struct pktlist *next;
};
#endif

/* The reorder buffer data structure itself */
struct mlvpn_reorder_buffer {
    uint64_t min_seqn;  /**< Lowest seq. number that can be in the buffer */
    unsigned int memsize; /**< memory area size of reorder buffer */
    struct cir_buffer ready_buf; /**< temp buffer for dequeued pkts */
    struct cir_buffer order_buf; /**< buffer used to reorder pkts */
    int is_initialized;
  
#ifdef MARK
  struct pktlist *pool;
  struct pktlist *list;
  struct pktlist *tail;
  int list_size;
  int list_size_av;
  int max_size;
#endif
};


#ifdef MARK

struct mlvpn_reorder_buffer *
mlvpn_reorder_init(struct mlvpn_reorder_buffer *b, unsigned int bufsize,
        unsigned int size)
{
  b->max_size=10;
  b->pool=NULL;
  b->list=NULL;
  b->tail=NULL;
  b->list_size=0;
  b->list_size_av=10;
  b->is_initialized = 0;
  
  return b;
}
struct mlvpn_reorder_buffer*
mlvpn_reorder_create(unsigned int size)
{
  struct mlvpn_reorder_buffer *b = malloc(sizeof(struct mlvpn_reorder_buffer));
  mlvpn_reorder_init(b, 0, size);
  return b;
}
void
mlvpn_reorder_reset(struct mlvpn_reorder_buffer *b)
{
  if (b->tail) {
    b->tail->next=b->pool;
    b->pool=b->list;
  }
  b->list=NULL;
  b->tail=NULL;
  b->list_size=0;
}
void mlvpn_reorder_free(struct mlvpn_reorder_buffer *b)
{
  struct pktlist *l,*n;
  for (l=b->list;l;l=n) {n=l->next;free(l);}
  for (l=b->pool;l;l=n) {n=l->next;free(l);}
  free(b);
}

int
mlvpn_reorder_insert(struct mlvpn_reorder_buffer *b, mlvpn_pkt_t *pkt)
{
    struct pktlist *p;
    if (b->pool) {
      p=b->pool;
      b->pool=b->pool->next;
    } else {
      p=malloc(sizeof (struct pktlist));
    }
    p->pkt=pkt;
    
    if (!b->is_initialized) {
        b->min_seqn = pkt->seq;
        b->is_initialized = 1;
        log_debug("reorder", "initial sequence: %"PRIu64"", pkt->seq);
    }

    
    /*
     * calculate the offset from the head pointer we need to go.
     * The subtraction takes care of the sequence number wrapping.
     * For example (using 16-bit for brevity):
     *  min_seqn  = 0xFFFD
     *  pkt_seq   = 0x0010
     *  offset    = 0x0010 - 0xFFFD = 0x13
     * Then we cast to a signed int, if the subtraction ends up in a large
     * number, that will be seen as negative when casted....
     */
    struct pktlist *l;
    for (l=b->list;l && ((int64_t)(pkt->seq - l->pkt->seq)<=0);l=l->next){}
    if (!l) {
      if (b->tail) {
        p->last=b->tail;
        b->tail->next=p;
        b->tail=p;
        p->next=NULL;
      } else {
        p->last=NULL;
        p->next=NULL;
        b->list=p;
        b->tail=p;
      }
    } else {
      p->last=l->last;
      if (p->last) {
        p->last->next=p;
      } else {
        b->list=p;
      }
      p->next=l;
      l->last=p;
    }
    
    b->list_size++;
//    printf("Insert    %lu  list size %d min %lu\n",pkt->seq, b->list_size, b->min_seqn);

    if (b->tail && ((int64_t)(b->min_seqn - b->tail->pkt->seq) > 0)) {
      log_debug("reorder", "got old (insert) consider increasing buffer (%d behind)\n",(int)(b->min_seqn - b->tail->pkt->seq));
    }

    return 0;
}

void mlvpn_reorder_skip(struct mlvpn_reorder_buffer *b)
{
//  printf("number in list %u list_size_av %u min %lu tail %lu\n",b->list_size, b->list_size_av, b->min_seqn, b->tail->pkt->seq);
  if (b->tail) {
    b->min_seqn=b->tail->pkt->seq; // Jump over any hole !!!!
  }
}

  
      
unsigned int
mlvpn_reorder_drain(struct mlvpn_reorder_buffer *b, mlvpn_pkt_t **pkts,
        unsigned max_pkts)
{

  unsigned int drain_cnt = 0;

//  offset = pkt->seq - b->min_seqn;

//  printf("min_seqn %lu, top %lu\n",b->min_seqn, b->list->pkt->seq);

/*  uint64_t old=0,new=0;
  mlvpn_tunnel_t *t;
  old=rtuns.lh_first->seq;
  new=rtuns.lh_first->seq;
  LIST_FOREACH(t, &rtuns, entries) {
    if ((int64_t)(t->seq - old) > 0) old=t->seq;
    if ((int64_t)(new - t->seq) > 0) new=t->seq;
  }
//  No point being behind the oldest tunnel !
  if (b->list && ((int64_t)(old - b->min_seqn) > 0)) {
    printf("Skipping (no old tunnels)  %d\n",(int)(old - b->min_seqn));
    b->min_seqn=b->list->pkt->seq;
  }
  

//    no point being older than the newest thing in - max_size
  if (b->list && ((int64_t)(new - b->min_seqn) > b->max_size)) {
    printf("Skipping (newest is too far ahead) %d\n",(int)(old - b->min_seqn));
    b->min_seqn=b->list->pkt->seq;
  }
*/

/*  
  if (b->list && ((int64_t)(b->list->pkt->seq - b->min_seqn) > b->max_size)) {
    printf("Skipping (whole) %d\n",(int)(b->list->pkt->seq - b->min_seqn));
    b->min_seqn=b->list->pkt->seq; // we have a whole, skip over !!!
  } else
*/
/*  if (b->list && ((int64_t)(b->min_seqn - b->list->pkt->seq) > 0)) {
    printf("got old (drain) %d\n",(int)(b->min_seqn - b->list->pkt->seq));
    b->min_seqn=b->list->pkt->seq; // we have a whole, skip over !!!
  }
*/
  
  while (b->tail && ((b->list_size>((b->list_size_av*2))) || ((int64_t)(b->min_seqn - b->tail->pkt->seq)>=0)) && (drain_cnt < max_pkts)) {
    struct pktlist *l=b->tail;
    pkts[drain_cnt++]=l->pkt;
    if (l->last) {
      b->tail=l->last;
      b->tail->next=NULL;
    } else {
      b->list=NULL;
      b->tail=NULL;
    }
    l->next=b->pool;
    b->pool=l;
    b->list_size--;

    b->min_seqn=l->pkt->seq+1;

// do we need to do this again here?
/*    if (b->list && ((int64_t)(b->list->pkt->seq - b->min_seqn) > b->max_size)) {
      printf("Skipping (hole - b) %d\n",(int)(b->list->pkt->seq - b->min_seqn));
      b->min_seqn=b->list->pkt->seq; // we have a whole, skip over !!!
      }
*/
    
  }
  if (drain_cnt > 1) {
    int last=b->list_size_av;
    b->list_size_av = ((b->list_size_av*9) + (b->list_size + drain_cnt) + 5)/10;
    if (b->list_size_av > 64) {
      b->list_size_av = 64;
      if (b->list_size_av != last ) {
        log_info("reorder", "List size reached limit (64)\n");
      }
    } 
    if (b->list_size_av < 4) {
      b->list_size_av = 4;
      if (b->list_size_av != last ) {
        log_debug("reorder", "List size reached limit (4)\n");
      }
    } 
//    if (b->list_size_av != last ) {
//      log_debug("reorder", "Changed in list_size_av %d\n",b->list_size_av);
//      printf("reorder:  Changed in list_size_av %d\n",b->list_size_av);
//    }
  }
  
//  if (b->tail) b->min_seqn++;
  
//  printf("Drain %d %lu  list size %d min %lu\n",drain_cnt, pkts[0]->seq, b->list_size, b->min_seqn);
  return drain_cnt;
}
  
#else   


// OLD CODE....
void mlvpn_reorder_skip(struct mlvpn_reorder_buffer *b)
{
}

struct mlvpn_reorder_buffer *
mlvpn_reorder_init(struct mlvpn_reorder_buffer *b, unsigned int bufsize,
        unsigned int size)
{
    const unsigned int min_bufsize = sizeof(*b) +
                    (2 * size * sizeof(mlvpn_pkt_t *));
    if (b == NULL) {
        log_crit("reorder", "Invalid reorder buffer parameter: NULL");
        return NULL;
    }
    if (bufsize < min_bufsize) {
        log_crit("reorder", "Invalid reorder buffer memory size: %u, "
            "minimum required: %u", bufsize, min_bufsize);
        return NULL;
    }

    memset(b, 0, bufsize);
    b->memsize = bufsize;
    b->order_buf.size = b->ready_buf.size = size;
    b->order_buf.mask = b->ready_buf.mask = size - 1;
    b->ready_buf.pkts = (void *)&b[1];
    b->order_buf.pkts = (void *)&b[1] + (size * sizeof(b->ready_buf.pkts[0]));

    return b;
}

struct mlvpn_reorder_buffer*
mlvpn_reorder_create(unsigned int size)
{
    struct mlvpn_reorder_buffer *b = NULL;

    const unsigned int bufsize = sizeof(struct mlvpn_reorder_buffer) +
                    (2 * size * sizeof(mlvpn_pkt_t *));
    /* Allocate memory to store the reorder buffer structure. */
    b = calloc(1, bufsize);
    if (b == NULL) {
        log_crit("reorder", "Memzone allocation failed");
    } else {
        mlvpn_reorder_init(b, bufsize, size);
    }
    return b;
}

void
mlvpn_reorder_reset(struct mlvpn_reorder_buffer *b)
{
    mlvpn_reorder_init(b, b->memsize, b->order_buf.size);
}

void
mlvpn_reorder_free(struct mlvpn_reorder_buffer *b)
{
    /* Check user arguments. */
    if (b == NULL)
        return;
    free(b);
}


static unsigned
mlvpn_reorder_fill_overflow(struct mlvpn_reorder_buffer *b, unsigned n)
{
    /*
     * 1. Move all ready pkts that fit to the ready_buf
     * 2. check if we meet the minimum needed (n).
     * 3. If not, then skip any gaps and keep moving.
     * 4. If at any point the ready buffer is full, stop
     * 5. Return the number of positions the order_buf head has moved
     */

    struct cir_buffer *order_buf = &b->order_buf,
            *ready_buf = &b->ready_buf;

    unsigned int order_head_adv = 0;

    /*
     * move at least n packets to ready buffer, assuming ready buffer
     * has room for those packets.
     */
    while (order_head_adv < n &&
            ((ready_buf->head + 1) & ready_buf->mask) != ready_buf->tail) {

        /* if we are blocked waiting on a packet, skip it */
        if (order_buf->pkts[order_buf->head] == NULL) {
            order_buf->head = (order_buf->head + 1) & order_buf->mask;
            order_head_adv++;
        }

        /* Move all ready pkts that fit to the ready_buf */
        while (order_buf->pkts[order_buf->head] != NULL) {
            ready_buf->pkts[ready_buf->head] =
                    order_buf->pkts[order_buf->head];

            order_buf->pkts[order_buf->head] = NULL;
            order_head_adv++;

            order_buf->head = (order_buf->head + 1) & order_buf->mask;

            if (((ready_buf->head + 1) & ready_buf->mask) == ready_buf->tail)
                break;

            ready_buf->head = (ready_buf->head + 1) & ready_buf->mask;
        }
    }

    b->min_seqn += order_head_adv;
    /* Return the number of positions the order_buf head has moved */
    return order_head_adv;
}

int
mlvpn_reorder_insert(struct mlvpn_reorder_buffer *b, mlvpn_pkt_t *pkt)
{
    uint64_t offset;
    uint32_t position;
    struct cir_buffer *order_buf = &b->order_buf;

    if (!b->is_initialized) {
        b->min_seqn = pkt->seq;
        b->is_initialized = 1;
        log_debug("reorder", "initial sequence: %"PRIu64"", pkt->seq);
    }

    /*
     * calculate the offset from the head pointer we need to go.
     * The subtraction takes care of the sequence number wrapping.
     * For example (using 16-bit for brevity):
     *  min_seqn  = 0xFFFD
     *  pkt_seq   = 0x0010
     *  offset    = 0x0010 - 0xFFFD = 0x13
     */
    offset = pkt->seq - b->min_seqn;

    /*
     * action to take depends on offset.
     * offset < buffer->size: the pkt fits within the current window of
     *    sequence numbers we can reorder. EXPECTED CASE.
     * offset > buffer->size: the pkt is outside the current window. There
     *    are a number of cases to consider:
     *    1. The packet sequence is just outside the window, then we need
     *       to see about shifting the head pointer and taking any ready
     *       to return packets out of the ring. If there was a delayed
     *       or dropped packet preventing drains from shifting the window
     *       this case will skip over the dropped packet instead, and any
     *       packets dequeued here will be returned on the next drain call.
     *    2. The packet sequence number is vastly outside our window, taken
     *       here as having offset greater than twice the buffer size. In
     *       this case, the packet is probably an old or late packet that
     *       was previously skipped, so just enqueue the packet for
     *       immediate return on the next drain call, or else return error.
     */
    if (offset < b->order_buf.size) {
        position = (order_buf->head + offset) & order_buf->mask;
        order_buf->pkts[position] = pkt;
    } else if (offset < 2 * b->order_buf.size) {
        if (mlvpn_reorder_fill_overflow(b, offset + 1 - order_buf->size)
                < (offset + 1 - order_buf->size)) {
            /* Put in handling for enqueue straight to output */
            return -1;
        }
        offset = pkt->seq - b->min_seqn;
        position = (order_buf->head + offset) & order_buf->mask;
        order_buf->pkts[position] = pkt;
    } else {
        /* Put in handling for enqueue straight to output */
        log_debug("reorder", "packet sequence out of range");
        return -2;
    }
    return 0;
}

unsigned int
mlvpn_reorder_drain(struct mlvpn_reorder_buffer *b, mlvpn_pkt_t **pkts,
        unsigned max_pkts)
{
    unsigned int drain_cnt = 0;

    struct cir_buffer *order_buf = &b->order_buf,
            *ready_buf = &b->ready_buf;

    /* Try to fetch requested number of pkts from ready buffer */
    while ((drain_cnt < max_pkts) && (ready_buf->tail != ready_buf->head)) {
        pkts[drain_cnt++] = ready_buf->pkts[ready_buf->tail];
        ready_buf->tail = (ready_buf->tail + 1) & ready_buf->mask;
    }

    /*
     * If requested number of buffers not fetched from ready buffer, fetch
     * remaining buffers from order buffer
     */
    while ((drain_cnt < max_pkts) &&
            (order_buf->pkts[order_buf->head] != NULL)) {
        pkts[drain_cnt++] = order_buf->pkts[order_buf->head];
        order_buf->pkts[order_buf->head] = NULL;
        b->min_seqn++;
        order_buf->head = (order_buf->head + 1) & order_buf->mask;
    }
    return drain_cnt;
}
#endif
