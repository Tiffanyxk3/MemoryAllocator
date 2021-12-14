/**
 * @file
 *
 * Explores memory management at the C runtime level.
 *
 * To use (one specific command):
 * LD_PRELOAD=$(pwd)/allocator.so command
 * ('command' will run with your allocator)
 *
 * To use (all following commands):
 * export LD_PRELOAD=$(pwd)/allocator.so
 * (Everything after this point will use your custom allocator -- be careful!)
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>

#include "allocator.h"
#include "logger.h"

#define ALIGN_SIZE 8

static struct mem_block *g_head = NULL; // Start (head) of our linked list
static struct mem_block *g_tail = NULL; // End (tail) of our linked list

static unsigned long g_allocations = 0; // Allocation counter
static unsigned long g_regions = 0; // Allocation counter

pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for protecting the linked list

/**
 * Given a free block, this function will split it into two pieces and update
 * the linked list.
 *
 * @param block the block to split
 * @param size new size of the first block after the split is complete,
 * including header sizes. The size of the second block will be the original
 * block's size minus this parameter.
 *
 * @return address of the resulting second block (the original address will be
 * unchanged) or NULL if the block cannot be split.
 */
struct mem_block *split_block(struct mem_block *block, size_t size) {
    // check to see if size is too small (not even big enough to store header): return NULL
    if (size < 104) {
        return NULL;
    }
    // check if the block is not freed
    if (block->free == false || block->size - size < sizeof(struct mem_block)) {
        return NULL;
    }

    struct mem_block *second_block = (struct mem_block *) ((void *) block + size);
    second_block->size = block->size - size;
    second_block->free = true;
    second_block->region_id = block->region_id;
    second_block->next = block->next;
    second_block->prev = block;
    block->size = size;

    if (block == g_tail) {// block->next == NULL
        g_tail = second_block;
    }
    else {
        block->next->prev = second_block;
    }
    block->next = second_block;

    return second_block;
}

/**
 * Given a free block, this function attempts to merge it with neighboring
 * blocks --- both the previous and next neighbors --- and update the linked
 * list accordingly.
 *
 * @param block the block to merge
 *
 * @return address of the merged block or NULL if the block cannot be merged.
 */
struct mem_block *merge_block(struct mem_block *block) {
    struct mem_block *merged_block = NULL;

    if (block->prev->free == false && block->next->free == false) {
        return NULL;
    }

    if (block->prev->free && block->prev->region_id == block->region_id) {
        merged_block = block;
        merged_block->size = block->prev->size + block->size;
        if (block->prev == g_head) {
            g_head = merged_block;
        }
        else {
            merged_block->prev->prev->next = merged_block;
            merged_block->prev = block->prev->prev;
        }
    }

    if (block->next->free && block->next->region_id == block->region_id) {
        merged_block = block;
        merged_block->size = block->prev->size + block->size;
        if (block->next == g_tail) {
            g_tail = merged_block;
        }
        else {
            merged_block->next->next->prev = merged_block;
            merged_block->next = block->next->next;
        }
    }
    return merged_block;
}

/**
 * Given a block size (header + data), locate a suitable location using the
 * first fit free space management algorithm.
 *
 * @param size size of the block (header + data)
 */
void *first_fit(size_t size) {
    struct mem_block *current_block = g_head;
    if (current_block == NULL) {
        return NULL;
    }
    while (current_block != NULL) {
        if (current_block->size >= size && current_block->free) {
            return current_block;
        }
        current_block = current_block->next;
    }
    return NULL;
}

/**
 * Given a block size (header + data), locate a suitable location using the
 * worst fit free space management algorithm. If there are ties (i.e., you find
 * multiple worst fit candidates with the same size), use the first candidate
 * found.
 *
 * @param size size of the block (header + data)
 */
void *worst_fit(size_t size) {
    struct mem_block *current_block = g_head;
    struct mem_block *worst_block = g_head;
    if (current_block == NULL) {
        return NULL;
    }

    size_t max = worst_block->size;
    while (current_block != NULL) {
        if (current_block->size >= size && current_block->free) {
            if (current_block->size > max) {
                worst_block = current_block;
                max = worst_block->size;
            }
        }
        current_block = current_block->next;
    }

    if (worst_block == g_head) {
        return NULL;
    }
    return worst_block;
}

/**
 * Given a block size (header + data), locate a suitable location using the
 * best fit free space management algorithm. If there are ties (i.e., you find
 * multiple best fit candidates with the same size), use the first candidate
 * found.
 *
 * @param size size of the block (header + data)
 */
void *best_fit(size_t size) {
    struct mem_block *current_block = g_head;
    struct mem_block *best_block = NULL;
    size_t difference = -1;

    while (current_block != NULL) {
        if (current_block->size >= size && current_block->free && (best_block == NULL 
        || current_block->size < difference)) {
            best_block = current_block;
            difference = best_block->size;
        }
        current_block = current_block->next;
    }
    return best_block;
}

/**
 * Implementation of free space management (FSM) algorithms
 *
 * @param size size of the block (header + data)
 * @return reused block
 */
void *reuse(size_t size) {
    // Uusing free space management (FSM) algorithms, find a block of memory
    // that we can reuse. Return NULL if no suitable block is found.
    char *algo = getenv("ALLOCATOR_ALGORITHM");
    if (algo == NULL) {
        algo = "first_fit";
    }

    void *reused_block = NULL;

    if (strcmp(algo, "first_fit") == 0) {
        reused_block = first_fit(size);
    }
    else if (strcmp(algo, "best_fit") == 0) {
        reused_block = best_fit(size);
    }
    else if (strcmp(algo, "worst_fit") == 0) {
        reused_block = worst_fit(size);
    }

    if (reused_block != NULL) {
        // maybe update its header
        split_block(reused_block, size);
    }
    return reused_block;
}

/**
  * Allocate memory for block name
  *
  * @param size block size
  * @param name block name
  *
  * @return allocated block
*/
void *malloc_name(size_t size, char *name) {
    void *alloc = malloc(size);
    if (alloc == NULL) {
        return NULL;
    }
    struct mem_block *new_block = (struct mem_block*) alloc - 1;
    strcpy(new_block->name, name);
    LOG("Created name block: %s\n", name);
    return alloc;
}

/**
  * Allocate memory for the block
  *
  * @param size block size
  * @return allocated block
*/
void *malloc(size_t size) {
    pthread_mutex_lock(&alloc_mutex);

    size_t total_size = size + sizeof(struct mem_block);
    size_t aligned_size = total_size;
    if (aligned_size % ALIGN_SIZE != 0) {
        aligned_size = aligned_size + ALIGN_SIZE - (total_size % ALIGN_SIZE);
    }
    LOG("Allocation request; size = %zu, total = %zu, aligned = %zu\n", size, total_size, aligned_size);

    char *scrib = getenv("ALLOCATOR_SCRIBBLE");
    int scribbling = -1;
    if (scrib != NULL) {
        scribbling = atoi(scrib);
    }

    struct mem_block *reused_block = reuse(aligned_size);
    if(reused_block != NULL) {
        reused_block->free = false;
        if (scribbling == 1) {
            memset(reused_block + 1, 0xAA, size);
        }

        pthread_mutex_unlock(&alloc_mutex);
        return reused_block + 1;
    }


    int page_size = getpagesize();
    size_t num_pages = aligned_size / page_size;
    if (aligned_size % page_size != 0) {
        num_pages++;
    }
    size_t region_size = num_pages * page_size;
    LOG("New region; size = %zu\n", region_size);

    struct mem_block *block = mmap(
            NULL, // Address (we use NULL to let the kernel decide)
            region_size, // Size of memory block to allocate
            PROT_READ | PROT_WRITE, // Memory protection flags
            MAP_PRIVATE | MAP_ANONYMOUS, // Type of mapping
            -1, // file descriptor
            0); // offset to start at within the file

    if (block == MAP_FAILED) {
        pthread_mutex_unlock(&alloc_mutex);
        perror("mmap");
        return NULL;
    }
    
    snprintf(block->name, 32, "Allocation %lu", g_allocations++);
    block->region_id = g_regions++;

    if (g_head == NULL && g_tail == NULL) {
        block->prev = NULL;
        g_head = block;
        g_tail = block;
    }
    else {
        g_tail->next = block;
        block->prev = g_tail;
        g_tail = block;
    }

    block->next = NULL;
    block->free = true;
    block->size = region_size;
    split_block(block, aligned_size);
    block->free = false;

    if(scribbling == 1){
        memset(block + 1, 0xAA, size);
    }
    pthread_mutex_unlock(&alloc_mutex);

    LOG("New Allocation: Block is at %p; data is at %p.\n",block,block+1);

    return block + 1;
}

/**
  * Free memory
  *
  * @param ptr block to be freed
*/
void free(void *ptr) {
    LOG("Free request; address = %p\n", ptr);
    if (ptr == NULL) {
        return;
    }

    struct mem_block *block = (struct mem_block *) ptr - 1;
    block->free = true;
}

/**
  * Calloc memory for the block
  *
  * @param nmemb size
  * @param size block size
  *
  * @return allocated block
*/
void *calloc(size_t nmemb, size_t size) {
    void *ptr = malloc(nmemb * size);
    if (ptr == NULL) {

    }
    LOG("Initializing memory at address = %p\n", ptr);
    memset(ptr, 0, nmemb * size);
    return ptr;
}

/**
  * Realloc memory for the block
  *
  * @param ptr block to be realloced
  * @param size block size
  *
  * @return allocated block
*/
void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        // If the pointer is NULL, then we simply malloc a new block
        return malloc(size);
    }

    if (size == 0) {
        // Realloc to 0 is often the same as freeing the memory block... But the C standard
        // doesn't require this. We will free the block and return NULL here.
        free(ptr);
        return NULL;
    }

    return NULL;
}

/**
 * print_memory
 *
 * Prints out the current memory state, including both the regions and blocks.
 * Entries are printed in order, so there is an implied link from the topmost
 * entry to the next, and so on.
 */
void print_memory(void) {
    puts("-- Current Memory State --");
    struct mem_block *current_block = g_head;
    struct mem_block *current_region = NULL;
    while (current_block != NULL) {
        bool new = false;
        if (current_region == NULL) {
            new = true;
        }
        else if (current_block->region_id != current_region->region_id) {
            new = true;
        }

        if (new) {
            current_region = current_block;
            printf("[REGION <%lu>] <%p>\n", current_region->region_id, current_region);
        }
        char *state;
        if (current_block->free) {
            state = "FREE";
        }
        else {
            state = "USED";
        }
        printf("  [BLOCK] <%p>-<%p> '%s' <%zu> [%s]\n", current_block, current_block->next, 
            current_block->name, current_block->size, state);

        current_block = current_block->next;
    }
}