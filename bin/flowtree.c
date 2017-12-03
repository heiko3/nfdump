/*
 *  Copyright (c) 2016, Peter Haag
 *  Copyright (c) 2014, Peter Haag
 *  Copyright (c) 2011, Peter Haag
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without 
 *  modification, are permitted provided that the following conditions are met:
 *  
 *   * Redistributions of source code must retain the above copyright notice, 
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice, 
 *     this list of conditions and the following disclaimer in the documentation 
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be 
 *     used to endorse or promote products derived from this software without 
 *     specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 *  POSSIBILITY OF SUCH DAMAGE.
 *  
 */

#ifdef HAVE_CONFIG_H 
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>

#include "rbtree.h"
#include "nffile.h"
#include "bookkeeper.h"
#include "nfxstat.h"
#include "collector.h"
#include "netflow_pcap.h"
#include "util.h"
#include "flowtree.h"

static void spin_lock(int *p);
static void spin_unlock(int volatile *p);

static void spin_lock(int *p) {
    while(!__sync_bool_compare_and_swap(p, 0, 1));
}

static void spin_unlock(int volatile *p) {
    __asm volatile (""); // acts as a memory barrier.
    *p = 0;
}

#define GetTreeLock(a)		spin_lock(&((a)->list_lock))
#define ReleaseTreeLock(a)	spin_unlock(&((a)->list_lock))


static int FlowNodeCMP(struct FlowNode *e1, struct FlowNode *e2);

// Insert the IP RB tree code here
RB_GENERATE(FlowTree, FlowNode, entry, FlowNodeCMP);

// Flow Cache to store all nodes
#define FLOWELEMENTNUM 1024 * 1024
//#define FLOWELEMENTNUM 128
static struct FlowNode *FlowElementCache;

// free list 
static struct FlowNode *FlowNode_FreeList;
static pthread_mutex_t m_FreeList = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c_FreeList = PTHREAD_COND_INITIALIZER;
static uint32_t	CacheOverflow;
static uint32_t	Allocated;

// Flow tree
static FlowTree_t *FlowTree;
static int NumFlows;

// Simple unprotected list
typedef struct FlowNode_list_s {
	struct FlowNode *list;
	struct FlowNode *tail;
	uint32_t	size;
} Linked_list_t;

/* Free list handling functions */
// Get next free node from free list
struct FlowNode *New_Node(void) {
struct FlowNode *node;

#ifdef USE_MALLOC
	node = malloc(sizeof(struct FlowNode));
	memset((void *)node, 'A', sizeof(struct FlowNode));
	node->left = NULL;
	node->right = NULL;
	node->data = NULL;
	node->memflag = NODE_IN_USE;
	dbg_printf("New node: %llx\n", (unsigned long long)node);
	return node;
#endif 

	pthread_mutex_lock(&m_FreeList);
    while ( FlowNode_FreeList == NULL ) {
		CacheOverflow++;
		LogError("Free list exhausted: %u, Flows: %u - sleep", Allocated, NumFlows);
        pthread_cond_wait(&c_FreeList, &m_FreeList);
	}

	node = FlowNode_FreeList;
	if ( node == NULL ) {
		// should never happen , as we were waiting for a free node
		LogError("*** Software ERROR *** New_Node() unexpected error in %s line %d: %s: %u\n", 
			__FILE__, __LINE__, "Node list exhausted", NumFlows);
		pthread_mutex_unlock(&m_FreeList);
		return NULL;
	}
	if ( node->memflag != NODE_FREE ) {
		LogError("*** Software ERROR *** New_Node() unexpected error in %s line %d: %s\n", 
			__FILE__, __LINE__, "Tried to allocate a non free Node");
		abort();
	}

	FlowNode_FreeList = node->right;
	Allocated++;
	pthread_mutex_unlock(&m_FreeList);

	node->left 	  = NULL;
	node->right	  = NULL;
	node->memflag = NODE_IN_USE;

	return node;

} // End of New_Node

// return node into free list
void Free_Node(struct FlowNode *node) {

	if ( node->memflag == NODE_FREE ) {
		LogError("Free_Node() Fatal: Tried to free an already freed Node");
		abort();
	}

	if ( node->memflag != NODE_IN_USE ) {
		LogError("Free_Node() Fatal: Tried to free a Node not in use");
		abort();
	}

	if ( node->data ) {
		free(node->data);
		node->data = NULL;
	}

	dbg_assert(node->left == NULL);
	dbg_assert(node->right == NULL);

#ifdef USE_MALLOC
	dbg_printf("Free node: %llx\n", (unsigned long long)node);
	node->memflag = NODE_FREE;
	memset((void *)node, 'B', sizeof(struct FlowNode));
	free(node);
	return;
#endif

	memset((void *)node, 0, sizeof(struct FlowNode));

	pthread_mutex_lock(&m_FreeList);
	node->right = FlowNode_FreeList;
	node->left  = NULL;
	node->memflag = NODE_FREE;
	FlowNode_FreeList = node;
	Allocated--;
	pthread_mutex_unlock(&m_FreeList);
	if ( CacheOverflow ) {
		CacheOverflow = 0;
		pthread_cond_signal(&c_FreeList);
	}

} // End of Free_Node

/* safety check - this must never become 0 - otherwise the cache is too small */
uint32_t CacheCheck(void) {
	return FLOWELEMENTNUM - NumFlows;
} // End of CacheCheck

/* flow tree functions */
int Init_FlowTree(uint32_t CacheSize) {
int i;

	FlowTree = malloc(sizeof(FlowTree_t));
	if ( !FlowTree ) {
		LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno) );
		return 0;
	}
	RB_INIT(FlowTree);

	if ( CacheSize == 0 )
		CacheSize = FLOWELEMENTNUM;
	FlowElementCache = calloc(CacheSize, sizeof(struct FlowNode));
	if ( !FlowElementCache ) {
		LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno) );
		free(FlowTree);
		FlowTree = NULL;
		return 0;
	}

	// init free list
	FlowNode_FreeList 		   = FlowElementCache;
	FlowNode_FreeList->left    = NULL;
	FlowNode_FreeList->right   = &FlowElementCache[1];
	FlowNode_FreeList->memflag = NODE_FREE;
	for (i=1; i < (CacheSize-1); i++ ) {
		FlowElementCache[i].memflag = NODE_FREE;
		FlowElementCache[i].left  = &FlowElementCache[i-1];
		FlowElementCache[i].right = &FlowElementCache[i+1];
	}
	FlowElementCache[i].left  	= &FlowElementCache[i-1];
	FlowElementCache[i].right 	= NULL;
	FlowElementCache[i].memflag = NODE_FREE;

	CacheOverflow = 0;
	Allocated 	  = 0;
	NumFlows 	  = 0;

	return 1;
} // End of Init_FlowTree

void Dispose_FlowTree(void) {
struct FlowNode *node, *nxt;

	// Dump all incomplete flows to the file
	for (node = RB_MIN(FlowTree, FlowTree); node != NULL; node = nxt) {
		nxt = RB_NEXT(FlowTree, FlowTree, node);
		RB_REMOVE(FlowTree, FlowTree, node);
		if ( node->data ) 
			free(node->data);
	}
	free(FlowElementCache);
	FlowElementCache 	 = NULL;
	FlowNode_FreeList 	 = NULL;
	CacheOverflow = 0;

} // End of Dispose_FlowTree

static int FlowNodeCMP(struct FlowNode *e1, struct FlowNode *e2) {
uint64_t    *a = e1->src_addr.v6;
uint64_t    *b = e2->src_addr.v6;
int i;

#define CMPLEN (offsetof(struct FlowNode, _ENDKEY_) - offsetof(struct FlowNode, src_addr))

	i = memcmp((void *)a, (void *)b, CMPLEN );
	return i; 
 
} // End of FlowNodeCMP

struct FlowNode *Lookup_Node(struct FlowNode *node) {
	return RB_FIND(FlowTree, FlowTree, node);
} // End of Lookup_FlowTree

struct FlowNode *Insert_Node(struct FlowNode *node) {
struct FlowNode *n;

dbg_assert(node->left == NULL);
dbg_assert(node->right == NULL);

	// return RB_INSERT(FlowTree, FlowTree, node);
	n = RB_INSERT(FlowTree, FlowTree, node);
	if ( n ) { // existing node
		return n;
	} else {
		NumFlows++;
		return NULL;
	}
} // End of Insert_Node

void Remove_Node(struct FlowNode *node) {
struct FlowNode *rev_node;

#ifdef DEVEL
	assert(node->memflag == NODE_IN_USE);
	if ( NumFlows == 0 ) {
		LogError("Remove_Node() Fatal Tried to remove a Node from empty tree");
		return;
	}
#endif

	rev_node = node->rev_node;
	if ( rev_node ) {
		// unlink rev node on both nodes
		dbg_assert(rev_node->rev_node == node);
		rev_node->rev_node = NULL;
		node->rev_node	   = NULL;
	}
	RB_REMOVE(FlowTree, FlowTree, node);
	Free_Node(node);
	NumFlows--;

} // End of Remove_Node

int Link_RevNode(struct FlowNode *node) {
struct FlowNode lookup_node, *rev_node;

    dbg_printf("Link node: ");
    dbg_assert(node->rev_node == NULL);
    lookup_node.src_addr = node->dst_addr;
    lookup_node.dst_addr = node->src_addr;
    lookup_node.src_port = node->dst_port;
    lookup_node.dst_port = node->src_port;
    lookup_node.version  = node->version;
    lookup_node.proto    = node->proto;
    rev_node = Lookup_Node(&lookup_node);
    if ( rev_node ) { 
        dbg_printf("Found revnode ");
		// rev node must not be linked already - otherwise there is an inconsistency
		dbg_assert(node->rev_node == NULL);
        if (node->rev_node == NULL ) {
			// link both nodes
            node->rev_node = rev_node;
            rev_node->rev_node = node;
            dbg_printf(" - linked\n");
        } else {
            dbg_printf("Rev-node != NULL skip linking - inconsitency\n");
            LogError("Rev-node != NULL skip linking - inconsitency\n");
        }
		return 1;
    } else {
        dbg_printf("no revnode node\n");
		return 0;
    }

	/* not reached */

} // End of Link_RevNode

uint32_t Flush_FlowTree(FlowSource_t *fs) {
struct FlowNode *node, *nxt;
uint32_t n = NumFlows;

	// Dump all incomplete flows to the file
	for (node = RB_MIN(FlowTree, FlowTree); node != NULL; node = nxt) {
		StorePcapFlow(fs, node);
		nxt = RB_NEXT(FlowTree, FlowTree, node);
#ifdef DEVEL
if ( node->left || node->right ) {
	assert(node->proto == 17);
	 node->left = node->right = NULL;
}
#endif
		Remove_Node(node);
	}

#ifdef DEVEL
	if ( NumFlows != 0 )
		LogError("### Flush_FlowTree() remaining flows: %u\n", NumFlows);
#endif

	return n;

} // End of Flush_FlowTree

int AddNodeData(struct FlowNode *node, uint32_t seq, void *payload, uint32_t size) {

	return 0;
} // End of AddNodeData

/* Node list functions */
NodeList_t *NewNodeList(void) {
NodeList_t *NodeList;

	NodeList = (NodeList_t *)malloc(sizeof(NodeList_t));
	if ( !NodeList ) {
		LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno) );
		return NULL;
	}
	NodeList->list 		= NULL;
	NodeList->last 		= NULL;
	NodeList->length	= 0;
	NodeList->list_lock	= 0;
	NodeList->waiting	= 0;
	NodeList->waits		= 0;
	pthread_mutex_init(&NodeList->m_list, NULL);
	pthread_cond_init(&NodeList->c_list, NULL);

	return NodeList;

} // End of NewNodeList

void DisposeNodeList(NodeList_t *NodeList) {

	if ( !NodeList )
		return;

	if ( NodeList->length ) {
		LogError("Try to free non empty NodeList");
		return;
	}
 	free(NodeList);

} // End of DisposeNodeList

#ifdef DEVEL
void ListCheck(NodeList_t *NodeList);
void ListCheck(NodeList_t *NodeList) {
uint32_t len = 0, mem = 0, proto;
static uint32_t loops = 0;
struct FlowNode *node, *n;
	
//	DumpList(NodeList);
	loops++;
	node = NodeList->list;
	while (node) {
		len++;
		if ( node == NodeList->last ) {
			mem = len;
		}
		if ( node->memflag != NODE_IN_USE ) {
			printf("mem flag error : len: %u, last: %u, Nodelist: %u, loops: %u\n", len, mem, NodeList->length, loops);
		}
		if ( node->right == NULL ) {
			proto = node->proto;
			n = node;
		}

		node=node->right;
	}
	if ( len != NodeList->length) {
		printf("Len miss match: len: %u, last: %u, proto: %u, Nodelist: %u, loops: %u, allocated: %u, overlow: %u node: %llx\n", len, mem, proto, NodeList->length, loops, Allocated, CacheOverflow, (long long unsigned)n);
		assert(len==NodeList->length);
	} else {
printf("Len: %u ok last: %u, proto: %u in loop %u, allocated: %u, overlow: %u\n", 
len, mem, proto, loops, Allocated, CacheOverflow);
	}
}
#endif

void Push_Node(NodeList_t *NodeList, struct FlowNode *node) {

	GetTreeLock(NodeList);
	// pthread_mutex_lock(&NodeList->m_list);
	if ( NodeList->length == 0 ) {
		// empty list
		NodeList->list = node;
		node->left = NULL;
		node->right = NULL;
	} else {
		NodeList->last->right = node;
		node->left = NodeList->last;
		node->right = NULL;
	}
	NodeList->last = node;
	NodeList->length++;
#ifdef DEVEL
	printf("pushed node 0x%llx proto: %u, length: %u first: %llx, last: %llx\n", 
		(unsigned long long)node, node->proto, NodeList->length, (unsigned long long)NodeList->list, (unsigned long long)NodeList->last);
	ListCheck(NodeList);
#endif
	if ( NodeList->waiting ) {
		pthread_cond_signal(&NodeList->c_list);
	}
	ReleaseTreeLock(NodeList);
 	// pthread_mutex_unlock(&NodeList->m_list);
	// pthread_cond_signal(&NodeList->c_list);

} // End of Push_Node

struct FlowNode *Pop_Node(NodeList_t *NodeList, int *done) {
struct FlowNode *node;

	GetTreeLock(NodeList);
    while ( NodeList->length == 0 && !*done ) {
		pthread_mutex_lock(&NodeList->m_list);
		NodeList->waiting = 1;
		NodeList->waits++;
		ReleaseTreeLock(NodeList);
		// sleep ad wait
        pthread_cond_wait(&NodeList->c_list, &NodeList->m_list);

		// wake up
		GetTreeLock(NodeList);
		NodeList->waiting = 0;
		pthread_mutex_unlock(&NodeList->m_list);
	}

	if ( NodeList->length == 0 && *done ) {
		ReleaseTreeLock(NodeList);
		dbg_printf("Pop_Node done\n");
		return NULL;
	}

	if ( NodeList->list == NULL ) { 
		// should never happen - list is supposed to have at least one item
		ReleaseTreeLock(NodeList);
		LogError("Unexpected empty FlowNode_ProcessList");
		return NULL;
	}

	node = NodeList->list;
	NodeList->list = node->right;
	if ( NodeList->list ) 
		NodeList->list->left = NULL;
	else 
		NodeList->last = NULL;

	node->left = NULL;
	node->right = NULL;

	NodeList->length--;
#ifdef DEVEL
	printf("popped node 0x%llx proto: %u, length: %u first: %llx, last: %llx\n", 
		(unsigned long long)node, node->proto, NodeList->length, (unsigned long long)NodeList->list, (unsigned long long)NodeList->last);

	ListCheck(NodeList);
#endif
	ReleaseTreeLock(NodeList);

	return node;
} // End of Pop_Node

#ifdef DEVEL
void DumpList(NodeList_t *NodeList) {
struct FlowNode *node;

	printf("FlowNode_ProcessList: 0x%llx, length: %u\n", 
		(unsigned long long)NodeList->list, NodeList->length);
	node = NodeList->list;
	while ( node ) {
		printf("node: 0x%llx\n", (unsigned long long)node);
		printf("  ->left: 0x%llx\n", (unsigned long long)node->left);
		printf("  ->right: 0x%llx\n", (unsigned long long)node->right);
		node = node->right;
	}
	printf("tail: 0x%llx\n\n", (unsigned long long)NodeList->last);
} // End of DumpList
#endif

void DumpNodeStat(void) {
	LogInfo("Nodes in use: %u, Flows: %u CacheOverflow: %u", Allocated, NumFlows, CacheOverflow);
} // End of DumpNodeStat

/*
int main(int argc, char **argv) {

	
	return 0;
} // End of main
*/
