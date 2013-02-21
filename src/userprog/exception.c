#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "lib/kernel/list.h"
#include "lib/string.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include "vm/swap.h"

/* Number of page faults processed. */
static long long page_fault_cnt;
extern struct swap_table swap_table;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
void _exit (int);

//TODO: write this in design doc:                                   
#define STACK_BOTTOM  (PHYS_BASE - 8*1024*1024)

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
       *Kernel code shouldn't throw exceptions.  (Page faults
       * may cause kernel exceptions--but they shouldn't arrive
       * here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Load data from file into a page */
static void
load_page_from_file (struct suppl_pte *s_pte)
{
  

  uint8_t *kpage = palloc_get_page(PAL_USER, s_pte->upage);
  if (kpage == NULL)
    _exit(-1);

  /* Load this page. */
  if (file_read_at ( s_pte->file, kpage,
                     s_pte->bytes_read,
                     s_pte->offset)
      != (int) s_pte->bytes_read)
  {
    palloc_free_page (kpage);
    _exit(-1);
  }

  memset (kpage + s_pte->bytes_read, 0, PGSIZE - s_pte->bytes_read);
  /* Add the page to the process's address space. */
  if (!install_page (s_pte->upage, kpage, file_is_writable (s_pte->file)))
  {
    palloc_free_page (kpage);
    _exit(-1);
  }
}

static void 
load_page_from_swap( uint32_t *pte, void *fault_page)
{
    
    if (! (*pte & PTE_U ))
    {
      //printf("load_page_from_swap: not user pte! exit. \n");
      _exit(-1);

    }
    uint8_t *kpage = palloc_get_page(PAL_USER, fault_page);
    if (kpage == NULL)
    {
      //printf("load_page_from_swap: cannot palloc! exit. \n");
      _exit(-1);
    }

   size_t swap_frame_no = *pte & PTE_ADDR;
   /* TODO:load stack is treated separately, since only accessing esp-4 and esp -16 is legal. */
   /* TODO: however, for David's method of growing stack, swap_frame_no is exactly the point 
    * when we should trigger stack growth, but need to make sure only esp-4 and esp -16 is legal */
   if (swap_frame_no == 0 )
   {
     //printf("load_page_from_swap: empty pte! exit. \n");
      _exit(-1);
   }
   /* TODO: disk read API has no return value indicating success or not. (Song) */
   swap_read ( &swap_table, swap_frame_no, kpage);  
   swap_free ( &swap_table, swap_frame_no);
   
   /* Add the page to the process's address space. */
   /* TODO: for higher efficiency, use this:  *pte | = vtop (kpage) | PTE_P | PTE_W | PTE_U;*/
   /* TODO: make sure that data loaded from swap is indeed writable (Song) */
   if (!install_page (fault_page, kpage, 1) )
   {
     //printf("load_page_from_swap can't install page! \n");
     palloc_free_page (kpage);
     _exit(-1);
   }
   //printf("load_page_from_swap success! \n");
   
}

static void 
stack_growth( void *fault_page)  
{
  uint8_t *kpage = palloc_get_page(PAL_USER | PAL_ZERO, fault_page);
  if (kpage == NULL)
    _exit(-1);

  memset (kpage, 0, PGSIZE);

  if (!install_page (fault_page, kpage, 1) )
  {
    palloc_free_page (kpage);
    _exit(-1);
  }

}


/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* If fault in kernel execpt in system calls, kill the kernel */
  if (!user && !thread_current()->in_syscall)
  {
    /* To implement virtual memory, delete the rest of the function
       body, and replace it with code that brings in the page to
       which fault_addr refers. */
    printf ("Page fault at %p: %s error %s page in %s context.\n",
            fault_addr,
            not_present ? "not present" : "rights violation",
            write ? "writing" : "reading",
            user ? "user" : "kernel");
    kill (f);
  }
  else 
  /* if page fault in the user program or syscall, should search current thread's
      suppl_page table (a hash table), use the rounded down fault address as key,
      to get the info about where to get the page */
  {
     if ( ! is_user_vaddr(fault_addr))
        _exit(-1); 
     struct hash_elem *e;
     struct hash *h = &thread_current ()->suppl_pt;
     struct suppl_pte temp;
     uint32_t *pte;
     struct suppl_pte *s_pte;
     void *fault_page = pg_round_down (fault_addr);
     

     //printf("page fault address = %lx\n", fault_addr);
     /* find an empty page, fill it with the source indicated by s_ptr,
         map the faulted page to the new allocated frame */
     pte = lookup_page (thread_current()->pagedir, fault_page, false);
     if (pte == NULL)
       _exit(-1);     
     /* Case 1. Stack growth*/
     if (( fault_addr == f->esp - 4 || fault_addr == f->esp - 32 ) && fault_addr > STACK_BOTTOM)
     {
       stack_growth(fault_page);
       return;
     }
     /* TODO: How to handle zero page?*/
    
     /* Case 2. In the swap block*/
     if (! (*pte & PTE_M) )  
     {
       //printf("case2\n");
       load_page_from_swap(pte, fault_page);
       return;
     }

     /* Case 3. In the memory mapped file */
     if (*pte & PTE_M)                            
     {
       temp.upage =  (uint8_t *) fault_page;
       e = hash_find (h, &temp.elem_hash);
       if ( e == NULL )
         _exit(-1);

       s_pte = hash_entry (e, struct suppl_pte, elem_hash);
       ASSERT (s_pte->upage == fault_page);
       load_page_from_file (s_pte);
       /* load finish, delete this supplementary page table entry from hash table */
       /* TODO: do we keep or delete this s_pte after loading it? Song*/
       /*lock_acquire(&thread_current()->spt_lock);
       hash_delete(h,  &s_pte->elem_hash);
       lock_release(&thread_current()->spt_lock); 
       */
     }
     
  }

}

