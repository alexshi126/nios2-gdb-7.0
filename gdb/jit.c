/* Handle JIT code generation in the inferior for GDB, the GNU Debugger.

   Copyright (C) 2009
   Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include "jit.h"
#include "breakpoint.h"
#include "gdbcore.h"
#include "observer.h"
#include "objfiles.h"
#include "symfile.h"
#include "symtab.h"
#include "target.h"
#include "gdb_stat.h"

static const struct objfile_data *jit_objfile_data;

static const char *const jit_break_name = "__jit_debug_register_code";

static const char *const jit_descriptor_name = "__jit_debug_descriptor";

/* This is the address of the JIT descriptor in the inferior.  */

static CORE_ADDR jit_descriptor_addr = 0;

/* This is a boolean indicating whether we're currently registering code.  This
   is used to avoid re-entering the registration code.  We want to check for
   new JITed every time a new object file is loaded, but we want to avoid
   checking for new code while we're registering object files for JITed code.
   Therefore, we flip this variable to 1 before registering new object files,
   and set it to 0 before returning.  */

static int registering_code = 0;

/* Helper cleanup function to clear an integer flag like the one above.  */

static void
clear_int (void *int_addr)
{
  *((int *) int_addr) = 0;
}

struct target_buffer
{
  CORE_ADDR base;
  size_t size;
};

/* Openning the file is a no-op.  */

static void *
mem_bfd_iovec_open (struct bfd *abfd, void *open_closure)
{
  return open_closure;
}

/* Closing the file is just freeing the base/size pair on our side.  */

static int
mem_bfd_iovec_close (struct bfd *abfd, void *stream)
{
  xfree (stream);
  return 1;
}

/* For reading the file, we just need to pass through to target_read_memory and
   fix up the arguments and return values.  */

static file_ptr
mem_bfd_iovec_pread (struct bfd *abfd, void *stream, void *buf,
                     file_ptr nbytes, file_ptr offset)
{
  int err;
  struct target_buffer *buffer = (struct target_buffer *) stream;

  /* If this read will read all of the file, limit it to just the rest.  */
  if (offset + nbytes > buffer->size)
    nbytes = buffer->size - offset;

  /* If there are no more bytes left, we've reached EOF.  */
  if (nbytes == 0)
    return 0;

  err = target_read_memory (buffer->base + offset, (gdb_byte *) buf, nbytes);
  if (err)
    return -1;

  return nbytes;
}

/* For statting the file, we only support the st_size attribute.  */

static int
mem_bfd_iovec_stat (struct bfd *abfd, void *stream, struct stat *sb)
{
  struct target_buffer *buffer = (struct target_buffer*) stream;

  sb->st_size = buffer->size;
  return 0;
}

/* Open a BFD from the target's memory.  */

static struct bfd *
bfd_open_from_target_memory (CORE_ADDR addr, size_t size, char *target)
{
  const char *filename = xstrdup ("<in-memory>");
  struct target_buffer *buffer = xmalloc (sizeof (struct target_buffer));

  buffer->base = addr;
  buffer->size = size;
  return bfd_openr_iovec (filename, target,
                          mem_bfd_iovec_open,
                          buffer,
                          mem_bfd_iovec_pread,
                          mem_bfd_iovec_close,
                          mem_bfd_iovec_stat);
}

/* Helper function for reading the global JIT descriptor from remote memory.  */

static void
jit_read_descriptor (struct gdbarch *gdbarch,
		     struct jit_descriptor *descriptor)
{
  int err;
  struct type *ptr_type;
  int ptr_size;
  int desc_size;
  gdb_byte *desc_buf;
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  /* Figure out how big the descriptor is on the remote and how to read it.  */
  ptr_type = builtin_type (gdbarch)->builtin_data_ptr;
  ptr_size = TYPE_LENGTH (ptr_type);
  desc_size = 8 + 2 * ptr_size;  /* Two 32-bit ints and two pointers.  */
  desc_buf = alloca (desc_size);

  /* Read the descriptor.  */
  err = target_read_memory (jit_descriptor_addr, desc_buf, desc_size);
  if (err)
    error (_("Unable to read JIT descriptor from remote memory!"));

  /* Fix the endianness to match the host.  */
  descriptor->version = extract_unsigned_integer (&desc_buf[0], 4, byte_order);
  descriptor->action_flag =
      extract_unsigned_integer (&desc_buf[4], 4, byte_order);
  descriptor->relevant_entry = extract_typed_address (&desc_buf[8], ptr_type);
  descriptor->first_entry =
      extract_typed_address (&desc_buf[8 + ptr_size], ptr_type);
}

/* Helper function for reading a JITed code entry from remote memory.  */

static void
jit_read_code_entry (struct gdbarch *gdbarch,
		     CORE_ADDR code_addr, struct jit_code_entry *code_entry)
{
  int err;
  struct type *ptr_type;
  int ptr_size;
  int entry_size;
  gdb_byte *entry_buf;
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  /* Figure out how big the entry is on the remote and how to read it.  */
  ptr_type = builtin_type (gdbarch)->builtin_data_ptr;
  ptr_size = TYPE_LENGTH (ptr_type);
  entry_size = 3 * ptr_size + 8;  /* Three pointers and one 64-bit int.  */
  entry_buf = alloca (entry_size);

  /* Read the entry.  */
  err = target_read_memory (code_addr, entry_buf, entry_size);
  if (err)
    error (_("Unable to read JIT code entry from remote memory!"));

  /* Fix the endianness to match the host.  */
  ptr_type = builtin_type (gdbarch)->builtin_data_ptr;
  code_entry->next_entry = extract_typed_address (&entry_buf[0], ptr_type);
  code_entry->prev_entry =
      extract_typed_address (&entry_buf[ptr_size], ptr_type);
  code_entry->symfile_addr =
      extract_typed_address (&entry_buf[2 * ptr_size], ptr_type);
  code_entry->symfile_size =
      extract_unsigned_integer (&entry_buf[3 * ptr_size], 8, byte_order);
}

/* This function registers code associated with a JIT code entry.  It uses the
   pointer and size pair in the entry to read the symbol file from the remote
   and then calls symbol_file_add_from_local_memory to add it as though it were
   a symbol file added by the user.  */

static void
jit_register_code (struct gdbarch *gdbarch,
		   CORE_ADDR entry_addr, struct jit_code_entry *code_entry)
{
  bfd *nbfd;
  struct section_addr_info *sai;
  struct bfd_section *sec;
  struct objfile *objfile;
  struct cleanup *old_cleanups, *my_cleanups;
  int i;
  const struct bfd_arch_info *b;
  CORE_ADDR *entry_addr_ptr;

  nbfd = bfd_open_from_target_memory (code_entry->symfile_addr,
                                      code_entry->symfile_size, gnutarget);
  old_cleanups = make_cleanup_bfd_close (nbfd);

  /* Check the format.  NOTE: This initializes important data that GDB uses!
     We would segfault later without this line.  */
  if (!bfd_check_format (nbfd, bfd_object))
    {
      printf_unfiltered (_("\
JITed symbol file is not an object file, ignoring it.\n"));
      do_cleanups (old_cleanups);
      return;
    }

  /* Check bfd arch.  */
  b = gdbarch_bfd_arch_info (gdbarch);
  if (b->compatible (b, bfd_get_arch_info (nbfd)) != b)
    warning (_("JITed object file architecture %s is not compatible "
               "with target architecture %s."), bfd_get_arch_info
             (nbfd)->printable_name, b->printable_name);

  /* Read the section address information out of the symbol file.  Since the
     file is generated by the JIT at runtime, it should all of the absolute
     addresses that we care about.  */
  sai = alloc_section_addr_info (bfd_count_sections (nbfd));
  make_cleanup_free_section_addr_info (sai);
  i = 0;
  for (sec = nbfd->sections; sec != NULL; sec = sec->next)
    if ((bfd_get_section_flags (nbfd, sec) & (SEC_ALLOC|SEC_LOAD)) != 0)
      {
        /* We assume that these virtual addresses are absolute, and do not
           treat them as offsets.  */
        sai->other[i].addr = bfd_get_section_vma (nbfd, sec);
        sai->other[i].name = (char *) bfd_get_section_name (nbfd, sec);
        sai->other[i].sectindex = sec->index;
        ++i;
      }

  /* Raise this flag while we register code so we won't trigger any
     re-registration.  */
  registering_code = 1;
  my_cleanups = make_cleanup (clear_int, &registering_code);

  /* This call takes ownership of sai.  */
  objfile = symbol_file_add_from_bfd (nbfd, 0, sai, OBJF_SHARED);

  /* Clear the registering_code flag.  */
  do_cleanups (my_cleanups);

  /* Remember a mapping from entry_addr to objfile.  */
  entry_addr_ptr = xmalloc (sizeof (CORE_ADDR));
  *entry_addr_ptr = entry_addr;
  set_objfile_data (objfile, jit_objfile_data, entry_addr_ptr);

  discard_cleanups (old_cleanups);
}

/* This function unregisters JITed code and frees the corresponding objfile.  */

static void
jit_unregister_code (struct objfile *objfile)
{
  free_objfile (objfile);
}

/* Look up the objfile with this code entry address.  */

static struct objfile *
jit_find_objf_with_entry_addr (CORE_ADDR entry_addr)
{
  struct objfile *objf;
  CORE_ADDR *objf_entry_addr;

  ALL_OBJFILES (objf)
    {
      objf_entry_addr = (CORE_ADDR *) objfile_data (objf, jit_objfile_data);
      if (objf_entry_addr != NULL && *objf_entry_addr == entry_addr)
        return objf;
    }
  return NULL;
}

/* (Re-)Initialize the jit breakpoint handler, and register any already
   created translations.  */

static void
jit_inferior_init (struct gdbarch *gdbarch)
{
  struct minimal_symbol *reg_symbol;
  struct minimal_symbol *desc_symbol;
  CORE_ADDR reg_addr;
  struct jit_descriptor descriptor;
  struct jit_code_entry cur_entry;
  CORE_ADDR cur_entry_addr;
  struct cleanup *old_cleanups;

  /* When we register code, GDB resets its breakpoints in case symbols have
     changed.  That in turn calls this handler, which makes us look for new
     code again.  To avoid being re-entered, we check this flag.  */
  if (registering_code)
    return;

  /* Lookup the registration symbol.  If it is missing, then we assume we are
     not attached to a JIT.  */
  reg_symbol = lookup_minimal_symbol (jit_break_name, NULL, NULL);
  if (reg_symbol == NULL)
    return;
  reg_addr = SYMBOL_VALUE_ADDRESS (reg_symbol);
  if (reg_addr == 0)
    return;

  /* Lookup the descriptor symbol and cache the addr.  If it is missing, we
     assume we are not attached to a JIT and return early.  */
  desc_symbol = lookup_minimal_symbol (jit_descriptor_name, NULL, NULL);
  if (desc_symbol == NULL)
    return;
  jit_descriptor_addr = SYMBOL_VALUE_ADDRESS (desc_symbol);
  if (jit_descriptor_addr == 0)
    return;

  /* Read the descriptor so we can check the version number and load any already
     JITed functions.  */
  jit_read_descriptor (gdbarch, &descriptor);

  /* Check that the version number agrees with that we support.  */
  if (descriptor.version != 1)
    error (_("Unsupported JIT protocol version in descriptor!"));

  /* Put a breakpoint in the registration symbol.  */
  create_jit_event_breakpoint (gdbarch, reg_addr);

  /* If we've attached to a running program, we need to check the descriptor to
     register any functions that were already generated.  */
  for (cur_entry_addr = descriptor.first_entry;
       cur_entry_addr != 0;
       cur_entry_addr = cur_entry.next_entry)
    {
      jit_read_code_entry (gdbarch, cur_entry_addr, &cur_entry);

      /* This hook may be called many times during setup, so make sure we don't
         add the same symbol file twice.  */
      if (jit_find_objf_with_entry_addr (cur_entry_addr) != NULL)
        continue;

      jit_register_code (gdbarch, cur_entry_addr, &cur_entry);
    }
}

/* Exported routine to call when an inferior has been created.  */

void
jit_inferior_created_hook (void)
{
  jit_inferior_init (target_gdbarch);
}

/* Exported routine to call to re-set the jit breakpoints,
   e.g. when a program is rerun.  */

void
jit_breakpoint_re_set (void)
{
  jit_inferior_init (target_gdbarch);
}

/* Wrapper to match the observer function pointer prototype.  */

static void
jit_inferior_created_observer (struct target_ops *objfile, int from_tty)
{
  jit_inferior_init (target_gdbarch);
}

/* This function cleans up any code entries left over when the inferior exits.
   We get left over code when the inferior exits without unregistering its code,
   for example when it crashes.  */

static void
jit_inferior_exit_hook (int pid)
{
  struct objfile *objf;
  struct objfile *temp;

  /* We need to reset the descriptor addr so that next time we load up the
     inferior we look for it again.  */
  jit_descriptor_addr = 0;

  ALL_OBJFILES_SAFE (objf, temp)
    if (objfile_data (objf, jit_objfile_data) != NULL)
      jit_unregister_code (objf);
}

void
jit_event_handler (struct gdbarch *gdbarch)
{
  struct jit_descriptor descriptor;
  struct jit_code_entry code_entry;
  CORE_ADDR entry_addr;
  struct objfile *objf;

  /* Read the descriptor from remote memory.  */
  jit_read_descriptor (gdbarch, &descriptor);
  entry_addr = descriptor.relevant_entry;

  /* Do the corresponding action. */
  switch (descriptor.action_flag)
    {
    case JIT_NOACTION:
      break;
    case JIT_REGISTER:
      jit_read_code_entry (gdbarch, entry_addr, &code_entry);
      jit_register_code (gdbarch, entry_addr, &code_entry);
      break;
    case JIT_UNREGISTER:
      objf = jit_find_objf_with_entry_addr (entry_addr);
      if (objf == NULL)
	printf_unfiltered (_("Unable to find JITed code entry at address: %s\n"),
			   paddress (gdbarch, entry_addr));
      else
        jit_unregister_code (objf);

      break;
    default:
      error (_("Unknown action_flag value in JIT descriptor!"));
      break;
    }
}

/* Provide a prototype to silence -Wmissing-prototypes.  */

extern void _initialize_jit (void);

void
_initialize_jit (void)
{
  observer_attach_inferior_created (jit_inferior_created_observer);
  observer_attach_inferior_exit (jit_inferior_exit_hook);
  jit_objfile_data = register_objfile_data ();
}
