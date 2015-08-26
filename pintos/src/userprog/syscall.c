#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
// OUR CODE HERE
#include "filesys/file.h"
#include "devices/input.h"

#include "filesys/filesys.h"
#include "threads/malloc.h"

static void syscall_handler (struct intr_frame *);
// OUR CODE HERE
static int wait (tid_t pid); 
static int write (int fd, const void *buffer, unsigned size);
static int exec (const char *cmd_line);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned length);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
void verify_user_ptr (const void* ptr);
void verify_args(uint32_t *ptr, int argc);
void* user_to_kernel (void *vaddr);
struct file_wrapper *fd_to_file_wrapper (int fd);

static unsigned curr_fd;

static struct lock fd_lock;
static struct lock file_lock;

struct file_wrapper
  {
    unsigned fd;
    struct file *file;
    struct list_elem thread_elem;
  };

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&file_lock);
  lock_init(&fd_lock);
  curr_fd = 2;
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t* args = ((uint32_t*) f->esp);
  // OUR CODE HERE
  verify_user_ptr(args);
  switch (args[0]) {
    
    case SYS_EXIT: {
      verify_args(args, 1);
      f->eax = args[1];
      exit(args[1]);
      break;
    }

    case SYS_NULL: {
      verify_args(args, 1);
      f->eax = args[1] + 1;
      break;
    }

    case SYS_WRITE: {
      verify_args(args, 3);
      lock_acquire(&file_lock);
      f->eax = write(args[1], user_to_kernel((void *) args[2]), args[3]);
      lock_release(&file_lock);
      break;
    }
    
    case SYS_HALT: {
      shutdown_power_off();
      break;
    }

    case SYS_WAIT: {
      verify_args(args, 1);
      f->eax = wait((tid_t) args[1]);
      break;
    }

    case SYS_EXEC: {
      verify_args(args, 1);
      f->eax = exec(user_to_kernel((void *) args[1]));
      break;
    }

    case SYS_CREATE: {
      verify_args(args, 2);
      f->eax = create(user_to_kernel((void *) args[1]), args[2]);
      break;
    }

    case SYS_REMOVE: {
      verify_args(args, 1);
      f->eax = remove(user_to_kernel((void *) args[1]));
      break;
    }

    case SYS_OPEN: {
      verify_args(args, 1);
      f->eax = open(user_to_kernel((void *) args[1]));
      break;
    }

    case SYS_FILESIZE: {
      verify_args(args, 1);
      f->eax = filesize(args[1]);
      break;
    }

    case SYS_READ: {
      verify_args(args, 3);
      f->eax = read(args[1], user_to_kernel((void *) args[2]), args[3]);
      break;
    }

    case SYS_SEEK: {
      verify_args(args, 2);
      seek(args[1], args[2]);
      break;
    }

    case SYS_TELL: {
      verify_args(args, 1);
      f->eax = tell(args[1]);
      break;
    }

    case SYS_CLOSE: {
      verify_args(args, 1);
      close(args[1]);
      break;
    }
  }
}


/* Syscall handler when syscall exit is invoked. */
void exit (int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  struct thread *curr = thread_current();
  curr->exec_status->exit_code = status;
  
  struct list_elem *e;
  while (!list_empty(&curr->file_wrappers)) {
    e = list_begin(&curr->file_wrappers);
    close(list_entry(e, struct file_wrapper, thread_elem)->fd);
  }
  thread_exit();
}

/* Method for when we want to invoke a syscall wait. */ 
static int wait (tid_t pid) {
  return process_wait(pid);
}

/* Method for when we invoke a syscall exec. */
static int exec (const char *cmd_line) {
  lock_acquire(&file_lock);
  int status = process_execute(cmd_line);
  lock_release(&file_lock);
  return status;
}

/* Syscall handler for when a syscall write is invoked. */
static int write (int fd, const void *buffer, unsigned size) {
  int bytes_written; 
  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    bytes_written = size;
  } else if (fd == STDIN_FILENO) {
    exit(-1);
  } else {
    struct file_wrapper *curr = fd_to_file_wrapper(fd);
    if (!curr) {
      exit(-1);
    } else {
      struct file *file = curr->file;
      lock_acquire(&file_lock);
      bytes_written = file_write(file, buffer, size);
      lock_release(&file_lock);
    }
  }
  return bytes_written;
}


/* Syscall handler for when a syscall create is invoked. */
static bool create (const char *file, unsigned initial_size) { // DONE
  return filesys_create(file, initial_size);
}

/* Syscall handler for when a syscall remove is invoked. */
static bool remove (const char *file) {
  lock_acquire(&file_lock);
  bool success = filesys_remove(file);
  lock_release(&file_lock);
  return success;
}

/* Syscall handler for when a syscall open is invoked. */
static int open (const char *file_) { // DONE
  lock_acquire(&file_lock);
  struct file *file = filesys_open(file_);
  lock_release(&file_lock);
  if (!file)
    return -1;
  struct file_wrapper *f = (struct file_wrapper *) malloc(sizeof(struct file_wrapper));
  f->file = file;
  list_push_back(&thread_current()->file_wrappers, &f->thread_elem);
  lock_acquire(&fd_lock);
  f->fd = curr_fd++;
  lock_release(&fd_lock);
  return f->fd;
}

/* Syscall handler for when a syscall filesize is invoked. */
static int filesize (int fd) {
  struct file_wrapper *curr = fd_to_file_wrapper(fd);
  if (!curr)
    exit(-1);
  lock_acquire(&file_lock);
  int size = file_length (curr->file);
  lock_release(&file_lock);
  return size;
}

/* Syscall handler for when a syscall read is invoked. */
static int read (int fd, void *buffer, unsigned length) {
  int size = -1;
  if (fd == STDIN_FILENO) {
    uint8_t *buffer_copy = (uint8_t *) buffer;
    unsigned i;
    for (i = 0; i < length; i++)
      buffer_copy[i] = input_getc();
    size = length;
  } else if (fd == STDOUT_FILENO) {
    size = -1;
  } else {
    struct file_wrapper *curr = fd_to_file_wrapper(fd);
    if (!curr)
      exit(-1);
    lock_acquire(&file_lock);
    size = file_read(curr->file, buffer, length);
    lock_release(&file_lock);
  }
  return size;
}

/* Syscall handler for when a syscall seek is invoked. */
static void seek (int fd, unsigned position) { // DONE
  struct file_wrapper *curr = fd_to_file_wrapper(fd);
  if (!curr)
    exit(-1);
  lock_acquire(&file_lock);
  file_seek (curr->file, position);
  lock_release(&file_lock);
}

/* Syscall handler for when a syscall tell is invoked. */
static unsigned tell (int fd) {
  struct file_wrapper *curr = fd_to_file_wrapper(fd);
  if (!curr)
    exit(-1);
  lock_acquire(&file_lock);
  unsigned position = file_tell(curr->file);
  lock_release(&file_lock);
  return position;
}

/* Syscall handler for when a syscall close is invoked. */
static void close (int fd) {
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
    exit(-1);
  }
  struct file_wrapper *curr = fd_to_file_wrapper(fd);
  if (!curr)
    exit(-1);
  lock_acquire(&file_lock);
  list_remove(&curr->thread_elem);
  file_close(curr->file);
  free(curr);
  lock_release(&file_lock);
}

/* Checks to see if the ptr is valid or not. A pointer is valid if and only if
   it is not a null pointer, if it points to a mapped portion of virtual memory,
   and it lies below PHYS_BASE.

   If @ptr is invalid, we syscall exit with error code.  */
void verify_user_ptr (const void* ptr) {
  if (ptr == NULL || !is_user_vaddr(ptr) || pagedir_get_page(thread_current()->pagedir, ptr) == NULL)
    exit(-1);
}

/* Checks if all arguments, of length ARGC, have a valid address starting
   from PTR + 1. */
void verify_args(uint32_t *ptr, int argc) {
  int i;
  for (i = 0; i <= argc; i++)
    verify_user_ptr((const void *) (ptr + i));
}

/* convert the user space pointer into kernel one */
void* user_to_kernel (void *ptr) {
  verify_user_ptr((const void *) ptr); // verify pointer you're converting is valid also.
  void* kernel_p = pagedir_get_page(thread_current()->pagedir, (const void *) ptr);
  if (!kernel_p)
    exit(-1);
  return kernel_p;
}

/* Converts the given file descriptor to the associated file_wrapper found on
   this thread's list of file_wrappers. Returns NULL if no such file_wrapper
   is found. */
struct file_wrapper *
fd_to_file_wrapper (int fd_) {
  unsigned fd = (unsigned) fd_;
  struct list_elem *e;
  for (e = list_begin(&thread_current()->file_wrappers); e != list_end(&thread_current()->file_wrappers);
       e = list_next(e)) {
    struct file_wrapper *curr = list_entry(e, struct file_wrapper, thread_elem);
    if (fd == curr->fd) {
      return curr;
    }
  }
  return NULL;
}