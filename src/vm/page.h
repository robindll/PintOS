#ifndef VM_PAGE_HEADER
#define VM_PAGE_HEADER

#include "vm/swap.h"
#include <hash.h>
#include "filesys/off_t.h"


/**
 * Page status
 */
enum page_status {
    ON_FRAME,       // Page already in memory
    ON_SWAP,        // Page swapped out to swap disk
    FROM_FILESYS,   // Loaded from file system or executable
};

/**
 * Supplemental page table (SPT).
 * Each process has one supplemental_page_table
 */
struct supplemental_page_table
{
    struct hash page_map;
};

/**
 * Supplemental page table entry (SPTE)
 */
struct supplemental_page_table_entry
{
    void* virtual_addr;         // Virtual address of page
    void* physical_addr;        // Physical address of frame associated to the virtual address
                                // If the page is not on the frame, this pointer should be NULL
    struct hash_elem elem;      // Hash elements
    enum page_status status;    // Page status
    bool dirty;                 // Dirty bit
    
    // Only valid for status = ON_SWAP
    uint32_t swap_index;        // Stores the swap index if the page is sapped out, only effictive when status = ON_SWAP
    
    // Only valid for status == FROM_FILESYS
    struct file *file;
    int32_t file_offset;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    bool writable;
};

/*
 * Supplemental page table operations
 */

// Create supplemental page table
struct supplemental_page_table* vm_supt_create (void);

// Destroy supplemental page table
void vm_supt_destroy (struct supplemental_page_table *);

// Lookup and return supplemental page table entry for given page
struct supplemental_page_table_entry* vm_supt_lookup (struct supplemental_page_table *supt, void *page);

// Create supplemental page table entry for given page 
bool vm_supt_install_frame (struct supplemental_page_table *supt, void *page, void *frame);

// Create supplemental page table entry a new page specified by the starting address "upage"
bool vm_supt_install_filesys (struct supplemental_page_table *supt, void *page, struct file * file, off_t offset, uint32_t read_bytes, uint32_t zero_bytes, bool writable);

// Mark a page is swapped out to given swap index
bool vm_supt_set_swap (struct supplemental_page_table *supt, void *page, uint32_t swap_index);

// Return whether supplemental page table has entry for given page
bool vm_supt_has_entry (struct supplemental_page_table *supt, void *page);

// Set a page's dirty bit
bool vm_supt_set_dirty (struct supplemental_page_table *supt, void *page, bool);

// Load page back to frame from swap
bool vm_load_page(struct supplemental_page_table *supt, uint32_t *pagedir, void *page);

// Pin given page, prevent the frame associated with given page from swapping out.
void vm_pin_page(struct supplemental_page_table *supt, void *page);

// Unpin given page.
void vm_unpin_page(struct supplemental_page_table *supt, void *page);

#endif