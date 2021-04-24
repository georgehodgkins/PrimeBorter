#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <immintrin.h>
#include <time.h>

void beginTx() {assert(_xbegin() == UINT_MAX);}
void commitTx() {_xend();}

int txCounter = 0;

void beginTxAndCount () {
	beginTx();
	++txCounter;
}

void commitTxAndUncount () {
	commitTx();
	--txCounter;
}

struct ll_node_s {
	void* pt; // pointer to stored element
	struct ll_node_s *next; // pointer to next element in chain
};
typedef struct ll_node_s ll_node_t;

struct llfifo_s {
	ll_node_t *head; // points to next element to dequeue
	ll_node_t *tail; // points to last element enqueued (NULL if queue is empty)
	ll_node_t *free_tail; // points to end of free node chain (attached to main chain)
	int32_t init_cap; // initial capacity, used for memory management
	int32_t length; // current length of fifo
};
typedef struct llfifo_s llfifo_t;

llfifo_t* llfifo_create (int capacity) { 
	if (capacity < 0) return NULL;

	// allocate everything we need (fifo struct + reqd node count) in one chunk
	assert(sizeof(llfifo_t) == 2*sizeof(ll_node_t)
			&& "You messed with the alignment! Everything is broken now");
	uint64_t* inalloc = calloc(capacity + 2, sizeof(ll_node_t));
	if (inalloc == NULL) return NULL;
	// first 32 bytes are fifo struct
	llfifo_t* this = (llfifo_t*) inalloc;

	if (capacity > 0) {
		// the rest is nodes
		ll_node_t* nodes = (ll_node_t*) &inalloc[4];
		this->head = nodes;
		this->tail = NULL;
			
		// link together nodes
		for (int i = 0; i < capacity-1; ++i) {
			nodes[i].next = &nodes[i+1];
		}

		this->init_cap = capacity;
		this->free_tail = &nodes[capacity-1];
	}

	return this;
}

int llfifo_enqueue(llfifo_t* fifo, void* element) {
	beginTx();

	if (!element) { 
		commitTx();
		return -1;
	}

	if (fifo->head == NULL) { // fifo created with a capacity of zero
		fifo->head = calloc(1, sizeof(ll_node_t));
		fifo->free_tail = fifo->head;
	}

	if (fifo->tail == NULL) { // queue is empty
		assert(fifo->length == 0);
		fifo->head->pt = element;
		fifo->tail = fifo->head;
	} else {
		if (fifo->tail->next == NULL) { // need new node
			assert(fifo->tail == fifo->free_tail);
			fifo->tail->next = calloc(sizeof(ll_node_t), 1);
			if (fifo->tail->next == NULL) { 
				commitTx();
				return -1;
			}
			fifo->free_tail = fifo->tail->next;
		}

		fifo->tail = fifo->tail->next;
		assert(fifo->tail->pt == NULL);
		fifo->tail->pt = element;
	}

	commitTx();
	return ++fifo->length;
}

void* llfifo_dequeue(llfifo_t* fifo) {
	beginTxAndCount();

	if (fifo->head == NULL || fifo->head->pt == NULL) {
		commitTxAndUncount();
		return NULL;
	}
	
	ll_node_t* node = fifo->head;
	if (node == fifo->tail) { // last element being removed, leave head where it is
		assert(fifo->length == 1);
		fifo->tail = NULL; // indicates queue is empty
	} else { // pop head off and recycle its node

		assert(fifo->tail != NULL);
		fifo->head = node->next;
		node->next = NULL;

		// put the disused node at the end of the free chain
		assert(fifo->free_tail->next == NULL);
		fifo->free_tail->next = node;
		fifo->free_tail = node;

	}

	// get element
	void* element = node->pt;
	node->pt = NULL;

	// update length
	assert(fifo->length > 0);
	--fifo->length;

	commitTxAndUncount();
	return element;
}

int llfifo_length(llfifo_t *fifo) {
	return fifo->length;
}

int llfifo_capacity(llfifo_t *fifo) {
	
	if (fifo->head == NULL) return 0;
	
	ll_node_t* seek = fifo->head;
	int count = 0;

	while (seek) {
		++count;
		seek = seek->next;
	}

	return count;
}

void llfifo_destroy(llfifo_t *fifo) {
	// find bounds of original block allocation
	ll_node_t* block_front = (ll_node_t*) fifo;
	ll_node_t* block_back = block_front + fifo->init_cap + 2;
	
	ll_node_t* seek = fifo->head;

	while (seek) {
		ll_node_t* next = seek->next;
		// free singly allocd nodes
		if (seek > block_back || seek < block_front) free (seek);
		seek = next;
	}

	// free block allocd at queue creation
	free(fifo);
}

// this number must be divisible by 4 for tests to work properly
#define TEST_SIZE 1024

void test_llfifo () {

	// fill test array
	static int test_set[TEST_SIZE];
	for (size_t i = 0; i < TEST_SIZE; ++i)
		test_set[i] = rand();

	// alloc fifo
	llfifo_t* the_llfifo = llfifo_create(TEST_SIZE/2);
	
	// basic creation time checks
	assert(the_llfifo != NULL);
	assert(llfifo_enqueue(the_llfifo, NULL) == -1);
	assert(llfifo_dequeue(the_llfifo) == NULL);
	assert(llfifo_capacity(the_llfifo) == TEST_SIZE/2);
	assert(llfifo_length(the_llfifo) == 0);

	// fill fifo up to its initial cap
	beginTx();
	int len;	
	for (len = 0; len < TEST_SIZE/2; ++len) {
		assert(llfifo_length(the_llfifo) == len);
		assert(llfifo_capacity(the_llfifo) == TEST_SIZE/2);

		llfifo_enqueue(the_llfifo, &test_set[len]);
	}
	commitTx();

	// add some more elements to make it resize
	for (; len < 3*TEST_SIZE/4; ++len) {
		llfifo_enqueue(the_llfifo, &test_set[len]);

		assert(llfifo_length(the_llfifo) == len+1);
		if (llfifo_capacity(the_llfifo) != len+1) {
			printf("\nllfifo misreports capacity! Is %d, should be %d.\n",
					llfifo_capacity(the_llfifo), len+1);
			abort();
		}
	}

	int peak_cap = len;

	// dequeue the first half
	for (size_t dq = 0; dq < TEST_SIZE/2; ++dq) {
		beginTxAndCount();
		int* test = (int*) llfifo_dequeue(the_llfifo);
		assert(test != NULL);
		assert(*test == test_set[dq]);
		--len;
		assert(llfifo_length(the_llfifo) == len);
		assert(llfifo_capacity(the_llfifo) == peak_cap);
		commitTxAndUncount();
	}

	// alternate dequeues and enqueues
	for (size_t nq = 3*TEST_SIZE/4; nq < TEST_SIZE; ++nq) {
		int* test = (int*) llfifo_dequeue(the_llfifo);
		assert(test != NULL);
		assert(*test == test_set[nq - TEST_SIZE/4]);
		assert(llfifo_length(the_llfifo) == len-1);
		if (llfifo_capacity(the_llfifo) != peak_cap) {
			printf("\nllfifo misreports capacity! Is %d, should be %d.\n",
					llfifo_capacity(the_llfifo), peak_cap);
			abort();
		}

		int rlen = llfifo_enqueue(the_llfifo, &test_set[nq]);
		assert(rlen == len);
		assert(llfifo_capacity(the_llfifo) == peak_cap);
	}

	// dequeue the rest
	for (size_t dq = 3*TEST_SIZE/4; dq < TEST_SIZE; ++dq) {
		int* test = (int*) llfifo_dequeue(the_llfifo);
		assert(test != NULL);
		assert(*test == test_set[dq]);
		--len;
		assert(llfifo_length(the_llfifo) == len);
		if (llfifo_capacity(the_llfifo) != peak_cap) {
			printf("\nllfifo misreports capacity! Is %d, should be %d.\n",
					llfifo_capacity(the_llfifo), peak_cap);
			abort();
		}
	}

	assert(llfifo_length(the_llfifo) == 0);
	assert(llfifo_capacity(the_llfifo) == peak_cap);
	assert(llfifo_dequeue(the_llfifo) == NULL);

	// destroy the fifo
	llfifo_destroy(the_llfifo);
}

int main () {
	// seed RNG
	srand(time(NULL));
	// do tests
	printf("Testing llfifo...");
	fflush(stdout);
	test_llfifo();
	printf("done. All tests passed!\n");

	return 0;
}
