#ifndef VM_FRAME_HEADER
#define VM_FRAME_HEADER

#include <hash.h>

#include "lib/kernel/hash.h"
#include "threads/synch.h"
#include "threads/palloc.h"

/**
 * initialize frame table and related resources.
 */
void frame_init (void);

/**
 * Allocate a frame with given flags for given page,
 * Return physical address associated with given page.
 * Function is thread-safe.
 */
void* frame_allocate (enum palloc_flags flags, void *upage);

/**
 * Remove frame table entry for given kernel page and free memory used by the frame.
 */
void frame_free (void*);

/**
 * Remove frame table entry from frame table without freeing memory the frame uses.
 */
void frame_remove_entry (void*);

/** Unpin a kernal page */
void frame_unpin (void* kpage);

/** Unpin a kernal page */
void frame_pin (void* kpage);

#endif