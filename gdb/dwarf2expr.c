/* DWARF 2 Expression Evaluator.

   Copyright (C) 2001, 2002, 2003, 2005, 2007, 2008, 2009
   Free Software Foundation, Inc.

   Contributed by Daniel Berlin (dan@dberlin.org)

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
#include "symtab.h"
#include "gdbtypes.h"
#include "value.h"
#include "gdbcore.h"
#include "dwarf2.h"
#include "dwarf2expr.h"
#include "gdb_assert.h"

/* Local prototypes.  */

static void execute_stack_op (struct dwarf_expr_context *,
			      gdb_byte *, gdb_byte *);
static struct type *unsigned_address_type (struct gdbarch *, int);

/* Create a new context for the expression evaluator.  */

struct dwarf_expr_context *
new_dwarf_expr_context (void)
{
  struct dwarf_expr_context *retval;
  retval = xcalloc (1, sizeof (struct dwarf_expr_context));
  retval->stack_len = 0;
  retval->stack_allocated = 10;
  retval->stack = xmalloc (retval->stack_allocated * sizeof (CORE_ADDR));
  retval->num_pieces = 0;
  retval->pieces = 0;
  retval->max_recursion_depth = 0x100;
  return retval;
}

/* Release the memory allocated to CTX.  */

void
free_dwarf_expr_context (struct dwarf_expr_context *ctx)
{
  xfree (ctx->stack);
  xfree (ctx->pieces);
  xfree (ctx);
}

/* Helper for make_cleanup_free_dwarf_expr_context.  */

static void
free_dwarf_expr_context_cleanup (void *arg)
{
  free_dwarf_expr_context (arg);
}

/* Return a cleanup that calls free_dwarf_expr_context.  */

struct cleanup *
make_cleanup_free_dwarf_expr_context (struct dwarf_expr_context *ctx)
{
  return make_cleanup (free_dwarf_expr_context_cleanup, ctx);
}

/* Expand the memory allocated to CTX's stack to contain at least
   NEED more elements than are currently used.  */

static void
dwarf_expr_grow_stack (struct dwarf_expr_context *ctx, size_t need)
{
  if (ctx->stack_len + need > ctx->stack_allocated)
    {
      size_t newlen = ctx->stack_len + need + 10;
      ctx->stack = xrealloc (ctx->stack,
			     newlen * sizeof (struct dwarf_stack_value));
      ctx->stack_allocated = newlen;
    }
}

/* Push VALUE onto CTX's stack.  */

void
dwarf_expr_push (struct dwarf_expr_context *ctx, CORE_ADDR value,
		 int in_stack_memory)
{
  struct dwarf_stack_value *v;

  dwarf_expr_grow_stack (ctx, 1);
  v = &ctx->stack[ctx->stack_len++];
  v->value = value;
  v->in_stack_memory = in_stack_memory;
}

/* Pop the top item off of CTX's stack.  */

void
dwarf_expr_pop (struct dwarf_expr_context *ctx)
{
  if (ctx->stack_len <= 0)
    error (_("dwarf expression stack underflow"));
  ctx->stack_len--;
}

/* Retrieve the N'th item on CTX's stack.  */

CORE_ADDR
dwarf_expr_fetch (struct dwarf_expr_context *ctx, int n)
{
  if (ctx->stack_len <= n)
     error (_("Asked for position %d of stack, stack only has %d elements on it."),
	    n, ctx->stack_len);
  return ctx->stack[ctx->stack_len - (1 + n)].value;

}

/* Retrieve the in_stack_memory flag of the N'th item on CTX's stack.  */

int
dwarf_expr_fetch_in_stack_memory (struct dwarf_expr_context *ctx, int n)
{
  if (ctx->stack_len <= n)
     error (_("Asked for position %d of stack, stack only has %d elements on it."),
	    n, ctx->stack_len);
  return ctx->stack[ctx->stack_len - (1 + n)].in_stack_memory;

}

/* Add a new piece to CTX's piece list.  */
static void
add_piece (struct dwarf_expr_context *ctx, ULONGEST size)
{
  struct dwarf_expr_piece *p;

  ctx->num_pieces++;

  if (ctx->pieces)
    ctx->pieces = xrealloc (ctx->pieces,
                            (ctx->num_pieces
                             * sizeof (struct dwarf_expr_piece)));
  else
    ctx->pieces = xmalloc (ctx->num_pieces
                           * sizeof (struct dwarf_expr_piece));

  p = &ctx->pieces[ctx->num_pieces - 1];
  p->location = ctx->location;
  p->size = size;
  if (p->location == DWARF_VALUE_LITERAL)
    {
      p->v.literal.data = ctx->data;
      p->v.literal.length = ctx->len;
    }
  else
    {
      p->v.expr.value = dwarf_expr_fetch (ctx, 0);
      p->v.expr.in_stack_memory = dwarf_expr_fetch_in_stack_memory (ctx, 0);
    }
}

/* Evaluate the expression at ADDR (LEN bytes long) using the context
   CTX.  */

void
dwarf_expr_eval (struct dwarf_expr_context *ctx, gdb_byte *addr, size_t len)
{
  int old_recursion_depth = ctx->recursion_depth;

  execute_stack_op (ctx, addr, addr + len);

  /* CTX RECURSION_DEPTH becomes invalid if an exception was thrown here.  */

  gdb_assert (ctx->recursion_depth == old_recursion_depth);
}

/* Decode the unsigned LEB128 constant at BUF into the variable pointed to
   by R, and return the new value of BUF.  Verify that it doesn't extend
   past BUF_END.  */

gdb_byte *
read_uleb128 (gdb_byte *buf, gdb_byte *buf_end, ULONGEST * r)
{
  unsigned shift = 0;
  ULONGEST result = 0;
  gdb_byte byte;

  while (1)
    {
      if (buf >= buf_end)
	error (_("read_uleb128: Corrupted DWARF expression."));

      byte = *buf++;
      result |= (byte & 0x7f) << shift;
      if ((byte & 0x80) == 0)
	break;
      shift += 7;
    }
  *r = result;
  return buf;
}

/* Decode the signed LEB128 constant at BUF into the variable pointed to
   by R, and return the new value of BUF.  Verify that it doesn't extend
   past BUF_END.  */

gdb_byte *
read_sleb128 (gdb_byte *buf, gdb_byte *buf_end, LONGEST * r)
{
  unsigned shift = 0;
  LONGEST result = 0;
  gdb_byte byte;

  while (1)
    {
      if (buf >= buf_end)
	error (_("read_sleb128: Corrupted DWARF expression."));

      byte = *buf++;
      result |= (byte & 0x7f) << shift;
      shift += 7;
      if ((byte & 0x80) == 0)
	break;
    }
  if (shift < (sizeof (*r) * 8) && (byte & 0x40) != 0)
    result |= -(1 << shift);

  *r = result;
  return buf;
}

/* Read an address of size ADDR_SIZE from BUF, and verify that it
   doesn't extend past BUF_END.  */

CORE_ADDR
dwarf2_read_address (struct gdbarch *gdbarch, gdb_byte *buf,
		     gdb_byte *buf_end, int addr_size)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR result;

  if (buf_end - buf < addr_size)
    error (_("dwarf2_read_address: Corrupted DWARF expression."));

  /* For most architectures, calling extract_unsigned_integer() alone
     is sufficient for extracting an address.  However, some
     architectures (e.g. MIPS) use signed addresses and using
     extract_unsigned_integer() will not produce a correct
     result.  Make sure we invoke gdbarch_integer_to_address()
     for those architectures which require it.

     The use of `unsigned_address_type' in the code below refers to
     the type of buf and has no bearing on the signedness of the
     address being returned.  */

  if (gdbarch_integer_to_address_p (gdbarch))
    return gdbarch_integer_to_address
	     (gdbarch, unsigned_address_type (gdbarch, addr_size), buf);

  return extract_unsigned_integer (buf, addr_size, byte_order);
}

/* Return the type of an address of size ADDR_SIZE,
   for unsigned arithmetic.  */

static struct type *
unsigned_address_type (struct gdbarch *gdbarch, int addr_size)
{
  switch (addr_size)
    {
    case 2:
      return builtin_type (gdbarch)->builtin_uint16;
    case 4:
      return builtin_type (gdbarch)->builtin_uint32;
    case 8:
      return builtin_type (gdbarch)->builtin_uint64;
    default:
      internal_error (__FILE__, __LINE__,
		      _("Unsupported address size.\n"));
    }
}

/* Return the type of an address of size ADDR_SIZE,
   for signed arithmetic.  */

static struct type *
signed_address_type (struct gdbarch *gdbarch, int addr_size)
{
  switch (addr_size)
    {
    case 2:
      return builtin_type (gdbarch)->builtin_int16;
    case 4:
      return builtin_type (gdbarch)->builtin_int32;
    case 8:
      return builtin_type (gdbarch)->builtin_int64;
    default:
      internal_error (__FILE__, __LINE__,
		      _("Unsupported address size.\n"));
    }
}


/* Check that the current operator is either at the end of an
   expression, or that it is followed by a composition operator.  */

static void
require_composition (gdb_byte *op_ptr, gdb_byte *op_end, const char *op_name)
{
  /* It seems like DW_OP_GNU_uninit should be handled here.  However,
     it doesn't seem to make sense for DW_OP_*_value, and it was not
     checked at the other place that this function is called.  */
  if (op_ptr != op_end && *op_ptr != DW_OP_piece && *op_ptr != DW_OP_bit_piece)
    error (_("DWARF-2 expression error: `%s' operations must be "
	     "used either alone or in conjuction with DW_OP_piece "
	     "or DW_OP_bit_piece."),
	   op_name);
}

/* The engine for the expression evaluator.  Using the context in CTX,
   evaluate the expression between OP_PTR and OP_END.  */

static void
execute_stack_op (struct dwarf_expr_context *ctx,
		  gdb_byte *op_ptr, gdb_byte *op_end)
{
  enum bfd_endian byte_order = gdbarch_byte_order (ctx->gdbarch);
  ctx->location = DWARF_VALUE_MEMORY;
  ctx->initialized = 1;  /* Default is initialized.  */

  if (ctx->recursion_depth > ctx->max_recursion_depth)
    error (_("DWARF-2 expression error: Loop detected (%d)."),
	   ctx->recursion_depth);
  ctx->recursion_depth++;

  while (op_ptr < op_end)
    {
      enum dwarf_location_atom op = *op_ptr++;
      CORE_ADDR result;
      /* Assume the value is not in stack memory.
	 Code that knows otherwise sets this to 1.
	 Some arithmetic on stack addresses can probably be assumed to still
	 be a stack address, but we skip this complication for now.
	 This is just an optimization, so it's always ok to punt
	 and leave this as 0.  */
      int in_stack_memory = 0;
      ULONGEST uoffset, reg;
      LONGEST offset;

      switch (op)
	{
	case DW_OP_lit0:
	case DW_OP_lit1:
	case DW_OP_lit2:
	case DW_OP_lit3:
	case DW_OP_lit4:
	case DW_OP_lit5:
	case DW_OP_lit6:
	case DW_OP_lit7:
	case DW_OP_lit8:
	case DW_OP_lit9:
	case DW_OP_lit10:
	case DW_OP_lit11:
	case DW_OP_lit12:
	case DW_OP_lit13:
	case DW_OP_lit14:
	case DW_OP_lit15:
	case DW_OP_lit16:
	case DW_OP_lit17:
	case DW_OP_lit18:
	case DW_OP_lit19:
	case DW_OP_lit20:
	case DW_OP_lit21:
	case DW_OP_lit22:
	case DW_OP_lit23:
	case DW_OP_lit24:
	case DW_OP_lit25:
	case DW_OP_lit26:
	case DW_OP_lit27:
	case DW_OP_lit28:
	case DW_OP_lit29:
	case DW_OP_lit30:
	case DW_OP_lit31:
	  result = op - DW_OP_lit0;
	  break;

	case DW_OP_addr:
	  result = dwarf2_read_address (ctx->gdbarch,
					op_ptr, op_end, ctx->addr_size);
	  op_ptr += ctx->addr_size;
	  break;

	case DW_OP_const1u:
	  result = extract_unsigned_integer (op_ptr, 1, byte_order);
	  op_ptr += 1;
	  break;
	case DW_OP_const1s:
	  result = extract_signed_integer (op_ptr, 1, byte_order);
	  op_ptr += 1;
	  break;
	case DW_OP_const2u:
	  result = extract_unsigned_integer (op_ptr, 2, byte_order);
	  op_ptr += 2;
	  break;
	case DW_OP_const2s:
	  result = extract_signed_integer (op_ptr, 2, byte_order);
	  op_ptr += 2;
	  break;
	case DW_OP_const4u:
	  result = extract_unsigned_integer (op_ptr, 4, byte_order);
	  op_ptr += 4;
	  break;
	case DW_OP_const4s:
	  result = extract_signed_integer (op_ptr, 4, byte_order);
	  op_ptr += 4;
	  break;
	case DW_OP_const8u:
	  result = extract_unsigned_integer (op_ptr, 8, byte_order);
	  op_ptr += 8;
	  break;
	case DW_OP_const8s:
	  result = extract_signed_integer (op_ptr, 8, byte_order);
	  op_ptr += 8;
	  break;
	case DW_OP_constu:
	  op_ptr = read_uleb128 (op_ptr, op_end, &uoffset);
	  result = uoffset;
	  break;
	case DW_OP_consts:
	  op_ptr = read_sleb128 (op_ptr, op_end, &offset);
	  result = offset;
	  break;

	/* The DW_OP_reg operations are required to occur alone in
	   location expressions.  */
	case DW_OP_reg0:
	case DW_OP_reg1:
	case DW_OP_reg2:
	case DW_OP_reg3:
	case DW_OP_reg4:
	case DW_OP_reg5:
	case DW_OP_reg6:
	case DW_OP_reg7:
	case DW_OP_reg8:
	case DW_OP_reg9:
	case DW_OP_reg10:
	case DW_OP_reg11:
	case DW_OP_reg12:
	case DW_OP_reg13:
	case DW_OP_reg14:
	case DW_OP_reg15:
	case DW_OP_reg16:
	case DW_OP_reg17:
	case DW_OP_reg18:
	case DW_OP_reg19:
	case DW_OP_reg20:
	case DW_OP_reg21:
	case DW_OP_reg22:
	case DW_OP_reg23:
	case DW_OP_reg24:
	case DW_OP_reg25:
	case DW_OP_reg26:
	case DW_OP_reg27:
	case DW_OP_reg28:
	case DW_OP_reg29:
	case DW_OP_reg30:
	case DW_OP_reg31:
	  if (op_ptr != op_end 
	      && *op_ptr != DW_OP_piece
	      && *op_ptr != DW_OP_GNU_uninit)
	    error (_("DWARF-2 expression error: DW_OP_reg operations must be "
		   "used either alone or in conjuction with DW_OP_piece."));

	  result = op - DW_OP_reg0;
	  ctx->location = DWARF_VALUE_REGISTER;
	  break;

	case DW_OP_regx:
	  op_ptr = read_uleb128 (op_ptr, op_end, &reg);
	  require_composition (op_ptr, op_end, "DW_OP_regx");

	  result = reg;
	  ctx->location = DWARF_VALUE_REGISTER;
	  break;

	case DW_OP_implicit_value:
	  {
	    ULONGEST len;
	    op_ptr = read_uleb128 (op_ptr, op_end, &len);
	    if (op_ptr + len > op_end)
	      error (_("DW_OP_implicit_value: too few bytes available."));
	    ctx->len = len;
	    ctx->data = op_ptr;
	    ctx->location = DWARF_VALUE_LITERAL;
	    op_ptr += len;
	    require_composition (op_ptr, op_end, "DW_OP_implicit_value");
	  }
	  goto no_push;

	case DW_OP_stack_value:
	  ctx->location = DWARF_VALUE_STACK;
	  require_composition (op_ptr, op_end, "DW_OP_stack_value");
	  goto no_push;

	case DW_OP_breg0:
	case DW_OP_breg1:
	case DW_OP_breg2:
	case DW_OP_breg3:
	case DW_OP_breg4:
	case DW_OP_breg5:
	case DW_OP_breg6:
	case DW_OP_breg7:
	case DW_OP_breg8:
	case DW_OP_breg9:
	case DW_OP_breg10:
	case DW_OP_breg11:
	case DW_OP_breg12:
	case DW_OP_breg13:
	case DW_OP_breg14:
	case DW_OP_breg15:
	case DW_OP_breg16:
	case DW_OP_breg17:
	case DW_OP_breg18:
	case DW_OP_breg19:
	case DW_OP_breg20:
	case DW_OP_breg21:
	case DW_OP_breg22:
	case DW_OP_breg23:
	case DW_OP_breg24:
	case DW_OP_breg25:
	case DW_OP_breg26:
	case DW_OP_breg27:
	case DW_OP_breg28:
	case DW_OP_breg29:
	case DW_OP_breg30:
	case DW_OP_breg31:
	  {
	    op_ptr = read_sleb128 (op_ptr, op_end, &offset);
	    result = (ctx->read_reg) (ctx->baton, op - DW_OP_breg0);
	    result += offset;
	  }
	  break;
	case DW_OP_bregx:
	  {
	    op_ptr = read_uleb128 (op_ptr, op_end, &reg);
	    op_ptr = read_sleb128 (op_ptr, op_end, &offset);
	    result = (ctx->read_reg) (ctx->baton, reg);
	    result += offset;
	  }
	  break;
	case DW_OP_fbreg:
	  {
	    gdb_byte *datastart;
	    size_t datalen;
	    unsigned int before_stack_len;

	    op_ptr = read_sleb128 (op_ptr, op_end, &offset);
	    /* Rather than create a whole new context, we simply
	       record the stack length before execution, then reset it
	       afterwards, effectively erasing whatever the recursive
	       call put there.  */
	    before_stack_len = ctx->stack_len;
	    /* FIXME: cagney/2003-03-26: This code should be using
               get_frame_base_address(), and then implement a dwarf2
               specific this_base method.  */
	    (ctx->get_frame_base) (ctx->baton, &datastart, &datalen);
	    dwarf_expr_eval (ctx, datastart, datalen);
	    if (ctx->location == DWARF_VALUE_LITERAL
		|| ctx->location == DWARF_VALUE_STACK)
	      error (_("Not implemented: computing frame base using explicit value operator"));
	    result = dwarf_expr_fetch (ctx, 0);
	    if (ctx->location == DWARF_VALUE_REGISTER)
	      result = (ctx->read_reg) (ctx->baton, result);
	    result = result + offset;
	    in_stack_memory = 1;
	    ctx->stack_len = before_stack_len;
	    ctx->location = DWARF_VALUE_MEMORY;
	  }
	  break;

	case DW_OP_dup:
	  result = dwarf_expr_fetch (ctx, 0);
	  in_stack_memory = dwarf_expr_fetch_in_stack_memory (ctx, 0);
	  break;

	case DW_OP_drop:
	  dwarf_expr_pop (ctx);
	  goto no_push;

	case DW_OP_pick:
	  offset = *op_ptr++;
	  result = dwarf_expr_fetch (ctx, offset);
	  in_stack_memory = dwarf_expr_fetch_in_stack_memory (ctx, offset);
	  break;
	  
	case DW_OP_swap:
	  {
	    struct dwarf_stack_value t1, t2;

	    if (ctx->stack_len < 2)
	       error (_("Not enough elements for DW_OP_swap. Need 2, have %d."),
		      ctx->stack_len);
	    t1 = ctx->stack[ctx->stack_len - 1];
	    t2 = ctx->stack[ctx->stack_len - 2];
	    ctx->stack[ctx->stack_len - 1] = t2;
	    ctx->stack[ctx->stack_len - 2] = t1;
	    goto no_push;
	  }

	case DW_OP_over:
	  result = dwarf_expr_fetch (ctx, 1);
	  in_stack_memory = dwarf_expr_fetch_in_stack_memory (ctx, 1);
	  break;

	case DW_OP_rot:
	  {
	    struct dwarf_stack_value t1, t2, t3;

	    if (ctx->stack_len < 3)
	       error (_("Not enough elements for DW_OP_rot. Need 3, have %d."),
		      ctx->stack_len);
	    t1 = ctx->stack[ctx->stack_len - 1];
	    t2 = ctx->stack[ctx->stack_len - 2];
	    t3 = ctx->stack[ctx->stack_len - 3];
	    ctx->stack[ctx->stack_len - 1] = t2;
	    ctx->stack[ctx->stack_len - 2] = t3;
	    ctx->stack[ctx->stack_len - 3] = t1;
	    goto no_push;
	  }

	case DW_OP_deref:
	case DW_OP_deref_size:
	case DW_OP_abs:
	case DW_OP_neg:
	case DW_OP_not:
	case DW_OP_plus_uconst:
	  /* Unary operations.  */
	  result = dwarf_expr_fetch (ctx, 0);
	  dwarf_expr_pop (ctx);

	  switch (op)
	    {
	    case DW_OP_deref:
	      {
		gdb_byte *buf = alloca (ctx->addr_size);
		(ctx->read_mem) (ctx->baton, buf, result, ctx->addr_size);
		result = dwarf2_read_address (ctx->gdbarch,
					      buf, buf + ctx->addr_size,
					      ctx->addr_size);
	      }
	      break;

	    case DW_OP_deref_size:
	      {
		int addr_size = *op_ptr++;
		gdb_byte *buf = alloca (addr_size);
		(ctx->read_mem) (ctx->baton, buf, result, addr_size);
		result = dwarf2_read_address (ctx->gdbarch,
					      buf, buf + addr_size,
					      addr_size);
	      }
	      break;

	    case DW_OP_abs:
	      if ((signed int) result < 0)
		result = -result;
	      break;
	    case DW_OP_neg:
	      result = -result;
	      break;
	    case DW_OP_not:
	      result = ~result;
	      break;
	    case DW_OP_plus_uconst:
	      op_ptr = read_uleb128 (op_ptr, op_end, &reg);
	      result += reg;
	      break;
	    }
	  break;

	case DW_OP_and:
	case DW_OP_div:
	case DW_OP_minus:
	case DW_OP_mod:
	case DW_OP_mul:
	case DW_OP_or:
	case DW_OP_plus:
	case DW_OP_shl:
	case DW_OP_shr:
	case DW_OP_shra:
	case DW_OP_xor:
	case DW_OP_le:
	case DW_OP_ge:
	case DW_OP_eq:
	case DW_OP_lt:
	case DW_OP_gt:
	case DW_OP_ne:
	  {
	    /* Binary operations.  Use the value engine to do computations in
	       the right width.  */
	    CORE_ADDR first, second;
	    enum exp_opcode binop;
	    struct value *val1, *val2;
	    struct type *stype, *utype;

	    second = dwarf_expr_fetch (ctx, 0);
	    dwarf_expr_pop (ctx);

	    first = dwarf_expr_fetch (ctx, 0);
	    dwarf_expr_pop (ctx);

	    utype = unsigned_address_type (ctx->gdbarch, ctx->addr_size);
	    stype = signed_address_type (ctx->gdbarch, ctx->addr_size);
	    val1 = value_from_longest (utype, first);
	    val2 = value_from_longest (utype, second);

	    switch (op)
	      {
	      case DW_OP_and:
		binop = BINOP_BITWISE_AND;
		break;
	      case DW_OP_div:
		binop = BINOP_DIV;
                break;
	      case DW_OP_minus:
		binop = BINOP_SUB;
		break;
	      case DW_OP_mod:
		binop = BINOP_MOD;
		break;
	      case DW_OP_mul:
		binop = BINOP_MUL;
		break;
	      case DW_OP_or:
		binop = BINOP_BITWISE_IOR;
		break;
	      case DW_OP_plus:
		binop = BINOP_ADD;
		break;
	      case DW_OP_shl:
		binop = BINOP_LSH;
		break;
	      case DW_OP_shr:
		binop = BINOP_RSH;
                break;
	      case DW_OP_shra:
		binop = BINOP_RSH;
		val1 = value_from_longest (stype, first);
		break;
	      case DW_OP_xor:
		binop = BINOP_BITWISE_XOR;
		break;
	      case DW_OP_le:
		binop = BINOP_LEQ;
		break;
	      case DW_OP_ge:
		binop = BINOP_GEQ;
		break;
	      case DW_OP_eq:
		binop = BINOP_EQUAL;
		break;
	      case DW_OP_lt:
		binop = BINOP_LESS;
		break;
	      case DW_OP_gt:
		binop = BINOP_GTR;
		break;
	      case DW_OP_ne:
		binop = BINOP_NOTEQUAL;
		break;
	      default:
		internal_error (__FILE__, __LINE__,
				_("Can't be reached."));
	      }
	    result = value_as_long (value_binop (val1, val2, binop));
	  }
	  break;

	case DW_OP_call_frame_cfa:
	  result = (ctx->get_frame_cfa) (ctx->baton);
	  in_stack_memory = 1;
	  break;

	case DW_OP_GNU_push_tls_address:
	  /* Variable is at a constant offset in the thread-local
	  storage block into the objfile for the current thread and
	  the dynamic linker module containing this expression. Here
	  we return returns the offset from that base.  The top of the
	  stack has the offset from the beginning of the thread
	  control block at which the variable is located.  Nothing
	  should follow this operator, so the top of stack would be
	  returned.  */
	  result = dwarf_expr_fetch (ctx, 0);
	  dwarf_expr_pop (ctx);
	  result = (ctx->get_tls_address) (ctx->baton, result);
	  break;

	case DW_OP_skip:
	  offset = extract_signed_integer (op_ptr, 2, byte_order);
	  op_ptr += 2;
	  op_ptr += offset;
	  goto no_push;

	case DW_OP_bra:
	  offset = extract_signed_integer (op_ptr, 2, byte_order);
	  op_ptr += 2;
	  if (dwarf_expr_fetch (ctx, 0) != 0)
	    op_ptr += offset;
	  dwarf_expr_pop (ctx);
	  goto no_push;

	case DW_OP_nop:
	  goto no_push;

        case DW_OP_piece:
          {
            ULONGEST size;

            /* Record the piece.  */
            op_ptr = read_uleb128 (op_ptr, op_end, &size);
	    add_piece (ctx, size);

            /* Pop off the address/regnum, and reset the location
	       type.  */
	    if (ctx->location != DWARF_VALUE_LITERAL)
	      dwarf_expr_pop (ctx);
            ctx->location = DWARF_VALUE_MEMORY;
          }
          goto no_push;

	case DW_OP_GNU_uninit:
	  if (op_ptr != op_end)
	    error (_("DWARF-2 expression error: DW_OP_GNU_uninit must always "
		   "be the very last op."));

	  ctx->initialized = 0;
	  goto no_push;

	default:
	  error (_("Unhandled dwarf expression opcode 0x%x"), op);
	}

      /* Most things push a result value.  */
      dwarf_expr_push (ctx, result, in_stack_memory);
    no_push:;
    }

  ctx->recursion_depth--;
  gdb_assert (ctx->recursion_depth >= 0);
}
