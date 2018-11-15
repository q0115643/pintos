#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"

#define STACK_LIMIT 8 * 1024 * 1024

#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

//#define DEBUG

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
/* Help functions. */
void exit_if_user_access_in_kernel(void *fault_addr, bool user);
void bring_esp_from_thread_struct(bool user, bool not_present, struct intr_frame *f);
void write_on_nonwritable_page(void *fault_addr, bool not_present, bool write);
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
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
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
#ifdef DEBUG
  printf("page_fault(): 진입\n");
#endif
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */
  bool success = false;
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

#ifdef DEBUG
  printf ("Page fault at %p: %s error %s page in %s context.\n",
          fault_addr,
          not_present ? "not present" : "rights violation",
          write ? "writing" : "reading",
          user ? "user" : "kernel");
#endif

  /*
   *  유저 프로세스가 kernel에 접근 => exit(-1)
   */
  exit_if_user_access_in_kernel(fault_addr, user);
  /* 
   *  있는 페이지에 접근했는데 non-writable에 write하려해서 터짐
   */
  write_on_nonwritable_page(fault_addr, not_present, write);
  /*
   *  kernel에서 page fault로 넘어올때 stack pointer를 thread->esp로 복구
   */
  bring_esp_from_thread_struct(user, not_present, f);
  /*
   *  user 주소에서 fault, page가 없음.
   */
  if(is_user_vaddr(fault_addr) && not_present){
    struct page *page = ptable_lookup(fault_addr);
    if(page)
    {
      if(!page->loaded)
      {
        if(page->swaped)
        {
          success = page_load_swap(page);
        }
        else
        {
          success = page_load_file(page);
        }
      }
    }
    else
    { //stack growing 스택은 높은 주소에서 낮은 주소로 자람.
      if(fault_addr >= f->esp - 32 && fault_addr >= PHYS_BASE - STACK_LIMIT){ // PUSHA signal이 permission 받으러온거임.
        uint8_t *stack_page_addr = pg_round_down(fault_addr);
        while(stack_page_addr < ((uint8_t *) PHYS_BASE) - PGSIZE)
        {
          if(ptable_lookup(stack_page_addr))
          {
            stack_page_addr += PGSIZE;
            continue;
          }
          uint8_t *tmp_kpage;
          tmp_kpage = frame_alloc(PAL_USER | PAL_ZERO);
          if (tmp_kpage != NULL)
          {
            success = install_page (stack_page_addr, tmp_kpage, true);
            if(success)
            {
              struct page *s_page = malloc(sizeof(struct page));
              s_page->upage = stack_page_addr;
              s_page->writable = true;
              s_page->loaded = true;
              s_page->file = NULL;
              struct frame *tmp_frame = frame_get_from_addr(tmp_kpage);
              tmp_frame->alloc_page = s_page;
              if(!ptable_insert(s_page))
              {
                success = false;
              }
            }
          }
          stack_page_addr += PGSIZE;
        }
      }
      else
      {
        system_exit(-1);
      }
    }
  }

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
  if(!success)
  {
    printf ("Page fault at %p: %s error %s page in %s context.\n",
            fault_addr,
            not_present ? "not present" : "rights violation",
            write ? "writing" : "reading",
            user ? "user" : "kernel");
    kill (f);
  }
}

/* Help functions. */
void
exit_if_user_access_in_kernel(void *fault_addr, bool user)
{
  if(is_kernel_vaddr(fault_addr) && user){
    system_exit(-1);
  }
}

void
bring_esp_from_thread_struct(bool user, bool not_present, struct intr_frame *f)
{
  if(!user && not_present && f->esp <= PHYS_BASE - STACK_LIMIT)
  { // kernel모드에서 page_fault가 뜨면 f->esp가 이상한 값으로 오는 수가 있음. 이상한 값이면 kernel로의 전환에서 저장한 esp 소환
    f->esp = thread_current()->esp;
  }
}

void
write_on_nonwritable_page(void *fault_addr, bool not_present, bool write)
{
  if(is_user_vaddr(fault_addr) && !not_present && write){
    struct page *page = ptable_lookup(fault_addr);
    if(!page->writable){
      system_exit(-1);
    }
  }
}


