#include <hash.h>
#include <list.h>
#include <stdio.h>

#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"


// Global lock for ensuring atomic frame operation
static struct lock frame_lock;

// Map storing mappings from physical address to frame table entry
struct frame_map
{
    struct hash map;
};

// frame table
static struct frame_map frame_table;

// A circular list of frames for the clock eviction algorithm.
static struct list frame_eviction_candidates;
static struct list_elem* frame_ptr;

/**
 * Helper functions for hash table operations.
 */
static unsigned frame_hash_func(const struct hash_elem *elem, void *aux);
static bool     frame_less_func(const struct hash_elem *, const struct hash_elem *, void *aux);

/**
 * Frame Table Entry
 * Stores frame's virtual/physical addresses and metadata.
 */
struct frame_table_entry
{
    void* kpage;               // Kernal page, in PintOS kernel page address 0xABCDEF is mapped to the same physical addres (0xABCDEF)
    void* upage;               // User page address (virtual address)
    struct thread* thread;     // The thread associated with this frame
    bool pinned;               // Indicate whether this frame is allowed to be evicted.
                               // When pinned == true, this frame is not allowed to be evicted.

    struct hash_elem helem;    // see ::frame_map->map 
    struct list_elem lelem;    // see ::frame_list
};


/**
 * Helper functions to perform concrete frame operations
 */
static void frame_free_internal (void *kpage, bool free_page);
static struct frame_table_entry* frame_next_clockwise(void);
static struct frame_table_entry* frame_pick_one_to_evict(uint32_t* pagedir);
static void* frame_evict_and_allocate (enum palloc_flags flags);
static void frame_set_pinned (void* kpage, bool isPinned);


/**
 * initialize frame table and related resources.
 */
void frame_init ()
{
    // Initializ global lock.
    lock_init (&frame_lock);

    // Initializ hash table.
    hash_init (&frame_table.map, frame_hash_func, frame_less_func, NULL);
    
    // Initialize circular frame list.
    list_init (&frame_eviction_candidates);
    
    // Initialize there is no frame entry in frame list, so frame_ptr set to null.
    frame_ptr = NULL;
}


/**
 * Allocate a frame with given flags for given page,
 * Return kernel virtual address associated with given page.
 * Function is thread-safe.
 */
void* frame_allocate (enum palloc_flags flags, void *upage)
{
    lock_acquire (&frame_lock);
    
    // Obtain a page from user pool.
    void *frame_page = palloc_get_page (PAL_USER | flags);
    if (frame_page == NULL) {
        // page allocation failed. Evict frame and allocate a new frame.
        frame_page = frame_evict_and_allocate(flags);
    }

    // Create frame table entry
    struct frame_table_entry* frame = malloc(sizeof(struct frame_table_entry));
    if (frame == NULL) {
        // frame table entry allocation failed.
        // Panic the kernel.
        lock_release (&frame_lock);

        PANIC ("Cannot create frame table entry -- not enough memory");
        
        return NULL;
    }

    frame->upage = upage;
    frame->kpage = frame_page;
    frame->thread = thread_current ();
    frame->pinned = true;           // Do not allow this frame to be evicted until frame table is fully updated.

    // insert into frame table and frame list
    hash_insert (&frame_table.map, &frame->helem);
    list_push_back (&frame_eviction_candidates, &frame->lelem);

    lock_release (&frame_lock);

    return frame_page;
}


/**
 * Remove frame table entry for given kernel page and free memory used by the frame.
 */
void frame_free (void *kpage)
{
    lock_acquire (&frame_lock);
    frame_free_internal (kpage, true);
    lock_release (&frame_lock);
}


/**
 * Remove frame table entry from frame table without freeing memory the frame uses.
 */
void frame_remove_entry (void *kpage)
{
    lock_acquire (&frame_lock);
    frame_free_internal (kpage, false);
    lock_release (&frame_lock);
}


// Unpin a kernal page
void frame_unpin (void* kpage)
{
    frame_set_pinned (kpage, false);
}


// Pin a kernel page.
void frame_pin (void* kpage)
{
    frame_set_pinned (kpage, true);
}


/** ======================================================
 *  Helper functions to perform concrete frame operations
 *  ======================================================
 */

/**
 * Deallocates memory used by a frame.
 * This function MST be called with frame_lock held.
 */
void frame_free_internal (void *kpage, bool deallocate_frame)
{
    ASSERT (lock_held_by_current_thread(&frame_lock) == true);
    ASSERT (is_kernel_vaddr(kpage));
    ASSERT (pg_ofs (kpage) == 0);       // Kernel address should be aligned to page boundary.

    // Lookup frame table entry from frame table
    struct frame_table_entry temp;
    temp.kpage = kpage;

    struct hash_elem* elem = hash_find (&frame_table.map, &(temp.helem));
    if (elem == NULL) {
        PANIC ("The page to be freed is not stored in the frame table");
    }

    struct frame_table_entry* frame;
    frame = hash_entry(elem, struct frame_table_entry, helem);

    // Remove the frame table entry from frame table and frame list.
    hash_delete (&frame_table.map, &frame->helem);
    list_remove (&frame->lelem);

    // Free memory used by the kernal frame if needed.
    if (deallocate_frame) {
#ifdef MY_DEBUG
        printf("[DEBUG][frame_free_internal] Deallocating kpage 0x%x\n", (unsigned int)kpage);
#endif
        palloc_free_page(kpage);
    }

    // Free memory used by frame table entry.
    free(frame);
}


/**
 * Get next frame in frame list.
 */
struct frame_table_entry* frame_next_clockwise (void)
{
    if (list_empty(&frame_eviction_candidates))
        PANIC("Frame table is empty, which is impossible - there must be some leaks somewhere");

    if (frame_ptr == NULL || frame_ptr == list_end (&frame_eviction_candidates))
        frame_ptr = list_begin (&frame_eviction_candidates);
    else
        frame_ptr = list_next (frame_ptr);

    struct frame_table_entry *frame = list_entry (frame_ptr, struct frame_table_entry, lelem);
    
    return frame;
}


/**
 * Pick a frame to be evicted using clock algorithm.
 */
struct frame_table_entry* frame_pick_one_to_evict (uint32_t* pagedir)
{
    size_t n = hash_size (&frame_table.map);
    if (n == 0)
        PANIC("Frame table is empty, which is impossible - there must be leaks somewhere");

    size_t it;
    for (it = 0; it <= n + n; ++ it) // prevent infinite loop. 2n iterations is enough
    {
        struct frame_table_entry *frame = frame_next_clockwise();
    
        // if pinned, continue.
        if (frame->pinned) continue;
    
        // if referenced, give it a second chance.
        else if (pagedir_is_accessed (pagedir, frame->upage)) {
            pagedir_set_accessed (pagedir, frame->upage, false);
            continue;
        }

        // Found the candidate to be evicted : unreferenced since its last chance
        return frame;
    }

    PANIC ("Cannot evict any frame -- Not enough memory!\n");
}


/**
 * Evict a frame and allocate a frame from user pool.
 * Return physical address of newly allocated frame.
 * This function MUST be called with frame_lock held.
 */
static void* frame_evict_and_allocate (enum palloc_flags flags)
{
    // 1. Pick a page and swap it out.
    struct frame_table_entry *evicted_frame = frame_pick_one_to_evict( thread_current()->pagedir );
    ASSERT (evicted_frame != NULL && evicted_frame->thread != NULL);

    // 2. clear the page mapping, and replace it with swap
    ASSERT (evicted_frame->thread->pagedir != (void*)0xcccccccc);
    pagedir_clear_page (evicted_frame->thread->pagedir, evicted_frame->upage);

    // 3. Gather dirty bit from kernel page and user page for the page being swapped out.
    bool is_dirty = false;
    is_dirty = is_dirty || pagedir_is_dirty (evicted_frame->thread->pagedir, evicted_frame->upage);
    is_dirty = is_dirty || pagedir_is_dirty (evicted_frame->thread->pagedir, evicted_frame->kpage);

    // 4. Swap out frame, update supplemental page table end free physical memory used by evicted frame. 
    uint32_t swap_idx = swap_out (evicted_frame->kpage);
    supt_pt_set_swap (evicted_frame->thread->supt, evicted_frame->upage, swap_idx);
    supt_pt_set_dirty (evicted_frame->thread->supt, evicted_frame->upage, is_dirty);

#ifdef MY_DEBUG
        printf("[DEBUG][frame_evict_and_allocate] Swap out page 0x%x\n", (unsigned int)evicted_frame->kpage);
#endif

    frame_free_internal (evicted_frame->kpage, true);  // evicted_frame is also invalidated

    // 5. Now allocate frame from user pool again, should be allocated successfully.
    void* frame_page = palloc_get_page (PAL_USER | flags);
    ASSERT (frame_page != NULL); 

    return frame_page;
}


/**
 * Pin/Unpin a frame.
 */
static void frame_set_pinned (void* kpage, bool isPinned)
{
    lock_acquire (&frame_lock);

    // Lookup frame entry to be pinned/unpinned.
    struct frame_table_entry temp;
    temp.kpage = kpage;
    struct hash_elem *elem = hash_find (&frame_table.map, &(temp.helem));
    if (elem == NULL) {
        PANIC ("The frame to be pinned/unpinned does not exist");
    }

    struct frame_table_entry *frame = hash_entry (elem, struct frame_table_entry, helem);
    frame->pinned = isPinned;

    lock_release (&frame_lock);
}


/* =============================================================
 * Implementation of helper functions for hash table operations
 * =============================================================
 */

// Get hash for an element, using elem->kpage as key
static unsigned frame_hash_func(const struct hash_elem* elem, void* aux UNUSED)
{
    struct frame_table_entry* entry = hash_entry (elem, struct frame_table_entry, helem);
    
    return hash_bytes (&entry->kpage, sizeof entry->kpage);
}


// Compare two elements' hash : whether hash(a) < hash(b)
static bool frame_less_func(const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED)
{
  struct frame_table_entry* a_entry = hash_entry (a, struct frame_table_entry, helem);
  struct frame_table_entry* b_entry = hash_entry (b, struct frame_table_entry, helem);
  return a_entry->kpage < b_entry->kpage;
}
