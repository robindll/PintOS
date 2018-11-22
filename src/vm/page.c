#include <hash.h>
#include <string.h>

#include "lib/debug.h"
#include "lib/kernel/hash.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "filesys/file.h"


// Utility functions used by hash table
static unsigned spte_hash_func(const struct hash_elem* elem, void* aux);
static bool     spte_less_func(const struct hash_elem* elem1, const struct hash_elem* elem2, void* aux);
static void     spte_destroy_func(struct hash_elem* elem, void *aux);

// Helper functions
static bool     vm_load_page_from_filesys(struct supplemental_page_table_entry *spte, void *kpage);


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
    spte_temp.upage = page;

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
    
    spte->upage = page;
    spte->kpage = frame;
    spte->status = ON_FRAME;
    spte->dirty = false;
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
 * Create supplemental page table entry a new page specified by the starting address "upage"
 */
bool vm_supt_install_filesys (struct supplemental_page_table *supt, void *page, struct file * file, int32_t offset, uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    struct supplemental_page_table_entry *spte =
        (struct supplemental_page_table_entry*) malloc(sizeof(struct supplemental_page_table_entry));

    spte->upage = page;
    spte->kpage = NULL;
    spte->status = FROM_FILESYS;
    spte->dirty = false;
    spte->file = file;
    spte->file_offset = offset;
    spte->read_bytes = read_bytes;
    spte->zero_bytes = zero_bytes;
    spte->writable = writable;

    struct hash_elem *prev_elem;
    prev_elem = hash_insert (&supt->page_map, &spte->elem);
    if (prev_elem == NULL) return true;

    // Reaching here means there is already an entry for given upage.
    PANIC("Duplicated supplemental page table entry found for filesys-page");
    return false;
}


/**
 * Create supplemental page table entry for a new zero-ed page.
 */
bool vm_supt_install_zeropage (struct supplemental_page_table *supt, void *page)
{
    struct supplemental_page_table_entry* spte = (struct supplemental_page_table_entry*) malloc(sizeof(struct supplemental_page_table_entry));
    ASSERT (spte != NULL);

    spte->upage = page;
    spte->kpage = NULL;
    spte->status = ALL_ZERO;
    spte->dirty = false;

    struct hash_elem* prev_elem;
    prev_elem = hash_insert (&supt->page_map, &spte->elem);
    if (prev_elem == NULL) return true;

    // There is already an entry
    PANIC ("Duplicated supplemental entry found for zero page");
    return false;
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
    spte->kpage = NULL;
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


/** Set dirty status for a given page. */
bool vm_supt_set_dirty (struct supplemental_page_table *supt, void *page, bool value)
{
    struct supplemental_page_table_entry *spte = vm_supt_lookup(supt, page);
    if (spte == NULL) PANIC("Set dirty - the request page doesn't exist in supplemental page table.");

    spte->dirty = value;
    return true;
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
    bool writable = true;
    switch (spte->status) {
        case ALL_ZERO:
            memset (frame, 0, PGSIZE);
            break;

        case ON_FRAME:
            // Data already on the frame, do nothing
            break;

        case ON_SWAP:
            // Data is on swap, load the data back from swap
            vm_swap_in (spte->swap_index, frame);
            break;
        
        case FROM_FILESYS:
            // Data was loaded from file, now we just need to 
            // reload it from file.
            if (vm_load_page_from_filesys (spte, frame) == false) {
                vm_frame_free (frame);
                return false;
            }

            writable = spte->writable;
            break;

        default:
            PANIC ("Invalid page status");
    }

    // Point the page table entry for the faulting virtual address to physical address
    if (!pagedir_set_page (pagedir, page, frame, writable)) {
        // Didn't find page in page table
        vm_frame_free (frame);
        return false;
    }

    // Save physical address to supplemental page table and update its status
    spte->kpage = frame;
    spte->status = ON_FRAME;

    pagedir_set_dirty (pagedir, frame, false);

    // Unpin frame
    vm_frame_unpin (frame);

    return true;
}


/**
 * Pin given page, prevent the frame associated with given page from swapping out.
 */
void vm_pin_page(struct supplemental_page_table *supt, void *page)
{
    struct supplemental_page_table_entry *spte = vm_supt_lookup (supt, page);
    if (spte == NULL) {
        // ignore. stack may be grown.
        return;
    }

    ASSERT (spte->status == ON_FRAME);
    vm_frame_pin (spte->kpage);
}


/**
 * Unpin given page.
 */
void vm_unpin_page(struct supplemental_page_table *supt, void *page)
{
    struct supplemental_page_table_entry *spte = vm_supt_lookup (supt, page);
    if (spte == NULL) PANIC ("Request page does not exist");

    if (spte->status == ON_FRAME) {
        vm_frame_unpin (spte->kpage);
    }
}


/**
 * Hash table helper functions, use upage as key.
 */

// Return element's hash key
static unsigned spte_hash_func(const struct hash_elem* elem, void* aux UNUSED)
{
  struct supplemental_page_table_entry *entry = hash_entry(elem, struct supplemental_page_table_entry, elem);

  return hash_int((int) entry->upage);
}

// Return whether elem a < b
static bool spte_less_func(const struct hash_elem* a, const struct hash_elem* b, void *aux UNUSED)
{
  struct supplemental_page_table_entry *a_entry = hash_entry(a, struct supplemental_page_table_entry, elem);
  struct supplemental_page_table_entry *b_entry = hash_entry(b, struct supplemental_page_table_entry, elem);
  
  return a_entry->upage < b_entry->upage;
}

// Destroy element
static void spte_destroy_func(struct hash_elem* elem, void* aux UNUSED)
{
  struct supplemental_page_table_entry *entry = hash_entry(elem, struct supplemental_page_table_entry, elem);

  // Clean up the associated frame
  if (entry->kpage != NULL) {
    ASSERT (entry->status == ON_FRAME);
    vm_frame_remove_entry (entry->kpage);
  }
  else if(entry->status == ON_SWAP) {
    vm_swap_free (entry->swap_index);
  }

  // Clean up SPTE entry.
  free (entry);
}



/**
 * Helper function : load page from file system
 */
static bool vm_load_page_from_filesys(struct supplemental_page_table_entry* spte, void* frame)
{
  file_seek (spte->file, spte->file_offset);

  // read bytes from the file
  int bytes_read = file_read (spte->file, frame, spte->read_bytes);
  if(bytes_read != (int)spte->read_bytes)
    return false;

  // remain bytes are just zero
  ASSERT (spte->read_bytes + spte->zero_bytes == PGSIZE);
  memset ((char*)frame + bytes_read, 0, spte->zero_bytes);
  return true;
}

