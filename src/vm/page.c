#include <hash.h>
#include <string.h>

#include "lib/kernel/hash.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"

// Utility functions used by hash table
static unsigned spte_hash_func(const struct hash_elem* elem, void* aux);
static bool     spte_less_func(const struct hash_elem* elem1, const struct hash_elem* elem2, void* aux);
static void     spte_destroy_func(const struct hash_elem* elem, void *aux);


/**
 *  Create supplemental page table
 */
struct supplemental_page_table* vm_supt_create (void)
{
    // Allocate memory for supplemental page table
    struct supplemental_page_table *supt =
        (struct supplemental_page_table*) malloc(sizeof(struct supplemental_page_table));    

    // Initialize page map in supplemental page table
    hash_init (&supt->page_map, spte_hash_func, spte_less_func, NULL);
}


/**
 * Destroy supplemental page table
 */
void vm_supt_destroy (struct supplemental_page_table *supt)
{
    ASSERT (supt != NULL);

    hash_destroy (&supt->page_map, spte_destroy_func);
    free (supt);
}


/**
 * Lookup and return supplemental page table entry for given page
 * Return NULL if no such entry is found
 */
struct supplemental_page_table_entry* vm_supt_lookup (struct supplemental_page_table *supt, void *page)
{
    // Create temp spte for looking up the hash table
    struct supplemental_page_table_entry spte_temp;
    spte_temp.virtual_addr = page;

    struct hash_elem *elem = hash_find (&supt->page_map, &spte_temp.elem);
    
    if (elem == NULL) {
        // Didn't find the entry
        return NULL;
    }

    return hash_entry(elem, struct supplemental_page_table_entry, elem);
}


/**
 * Create supplemental page table entry for given page
 * The page should already on the frame.
 * Return true if successful, otherwise return false.
 */
bool vm_supt_install_frame (struct supplemental_page_table *supt, void *page, void *frame)
{
    struct supplemental_page_table_entry *spte =
        (struct supplemental_page_table_entry*) malloc(sizeof(struct supplemental_page_table_entry));
    
    spte->virtual_addr = page;
    spte->physical_addr = frame;
    spte->status = ON_FRAME;
    spte->swap_index = NO_SAWP_INDEX;

    // Insert new supplemental page table entry into page table
    struct hash_elem *prev_elem = hash_insert (&supt->page_map, &spte->elem);
    if (prev_elem == NULL) {
        // successfully inserted into supplemental page table
        return true;
    } else {
        // Insertion failed, there is already an entry.
        free (spte);
        return false;
    }
}


/**
 * Mark a page is swapped out to given swap index
 */
bool vm_supt_set_swap (struct supplemental_page_table *supt, void *page, uint32_t swap_index)
{
    struct supplemental_page_table_entry* spte = vm_supt_lookup (supt, page);
    
    if (spte == NULL) {
        // Didn't find supplemental page table entry for given page
        return false;
    }

    spte->status = ON_SWAP;
    spte->physical_addr = NULL;
    spte->swap_index = swap_index;

    return true;
}


/**
 * Return whether supplemental page table has entry for given page
 */
bool vm_supt_has_entry (struct supplemental_page_table *supt, void *page)
{
    return vm_supt_lookup (supt, page) != NULL;
}

/**
 * Load page back to frame from swap
 */
bool vm_load_page(struct supplemental_page_table *supt, uint32_t *pagedir, void *page)
{
    // Check whether memory reference is valid
    struct supplemental_page_table_entry *spte = vm_supt_lookup (supt, page);
    
    if (spte == NULL) {
        // No supplemental page table entry for given page
        return false;
    }

    // If page not already on a frame, obtain a frame to store the page
    if (spte->status == ON_FRAME) {
        // Page already on a frame
        return true;
    }

    void* frame = vm_frame_allocate (PAL_USER, page);

    if (frame == NULL) {
        // Failed to allocate new frame
        return false;
    }

    // Fetch data into frame
    vm_swap_in (spte->swap_index, frame);

    bool writable = true;

    // Point the page table entry for the faulting virtual address to physical address
    if (!pagedir_set_page (pagedir, page, frame, writable)) {
        // Didn't find page in page table
        vm_frame_free (frame);
        return false;
    }

    // Save physical address to supplemental page table and update its status
    spte->physical_addr = frame;
    spte->status = ON_FRAME;

    pagedir_set_dirty (pagedir, frame, false);

    // Unpin frame
    vm_frame_unpin (frame);

    return true;
}