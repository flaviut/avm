#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "avm.h"
#include "avm_util.h"
#include "avm_def.h"

#ifdef AVM_EXECUTABLE

int main(int argc, char **args)
{
  FILE* fin = stdin;
  if(argc == 2){
    fin = fopen(argv[1], "r");
    if(!fin){
      fprintf(stderr, "Unable to open file: %s\n", argv[1]);
      return 1;
    }
  }
  size_t bytes_read;
  char *opc = read_file(stdin, &bytes_read);
  if (opc == NULL) {
    fprintf(stderr, "unable to read input");
    return 1;
  }

  avm_int *memory;
  char* error;
  size_t memlen;
  if(avm_parse(opc, &memory, &error, &memlen)) {
    fprintf(stderr, "parse error: %s\n", error);
    my_free(opc);
    my_free(memory);
    my_free(error);
    return 1;
  }

  AVM_Context ctx;
  int retcode = avm_init(&ctx, (void *) memory, memlen);
  my_free(opc);
  my_free(memory);
  if (retcode) {
    fprintf(stderr, "failed to initialize vm\n");
    return 1;
  }

#ifdef AVM_DEBUG
  char *result;
  if (avm_stringify_count(&ctx, 0, (avm_size_t) memlen, &result)) {
    printf("err: %s\n", ctx.error);
  }
  printf("════ code listing ════");
  printf("%s\n", result);
  printf("══════════════════════\n\n");
  my_free(result);
  my_free(ctx.error);
#endif

  avm_int eval_prog_ret = 0;
  if (avm_eval(&ctx, &eval_prog_ret)) {
    printf("err: %s\n", ctx.error);
    avm_free(&ctx);
    return 1;
  }

  avm_free(&ctx);
  return (int) eval_prog_ret;
}

#endif  /* AVM_EXECUTABLE */

/**
 * Returns 0 unless there has been an error.
 *
 * Error information can be obtained from `ctx` unless it's
 * an allocation error, in which case `ctx` will be NULL
 */
int avm_init(AVM_Context *ctx, const avm_int *initial_mem, size_t oplen)
{
  static const avm_size_t INITIAL_MEMORY_OVERHEAD = 1 << 12;
  static const size_t INITIAL_CALLSTACK_SIZE = 256;

  ctx->error = NULL;

  assert((oplen * sizeof(AVM_Operation)) / sizeof(avm_int) < AVM_SIZE_MAX / 2);
  size_t memory_size = oplen  + INITIAL_MEMORY_OVERHEAD;

  // allocate enough memory for opcodes and some slack besides
  ctx->memory_size = (avm_size_t) memory_size;
  ctx->memory = my_calloc(ctx->memory_size * sizeof(avm_int), 1);
  if (ctx->memory == NULL) {
    return avm__error(ctx, "unable to allocate heap (%d avm_int)", memory_size);
  }
  memcpy(ctx->memory, initial_mem, oplen * sizeof(AVM_Operation));

  ctx->stack_size = 0;
  ctx->stack_cap = INITIAL_MEMORY_OVERHEAD;
  // malloc used because stack semantics guarantee
  // uninitialized data cannot be read
  ctx->stack = my_malloc(ctx->stack_cap * sizeof(avm_int));
  if (ctx->stack == NULL) {
    return avm__error(ctx, "unable to allocate stack (%d bytes)", ctx->stack_cap);
  }

  ctx->call_stack_size = 0;
  ctx->call_stack_cap = INITIAL_CALLSTACK_SIZE;
  ctx->call_stack = my_malloc(INITIAL_CALLSTACK_SIZE * sizeof(AVM_Stack_Frame));
  if (ctx->call_stack == NULL) {
    return avm__error(ctx, "unable to allocate call stack", ctx->call_stack_cap);
  }

  ctx->ins = 0;

  return 0;
}

void avm_free(AVM_Context *ctx)
{
  my_free(ctx->error);
  my_free(ctx->memory);
  my_free(ctx->stack);
  my_free(ctx->call_stack);
}


void avm_heap_get(AVM_Context *ctx, avm_int *data, avm_size_t loc)
{
  if (loc >= ctx->memory_size) { // memory never written, it's 0.
    *data = 0;
    return;
  }

  *data = ctx->memory[loc];
}

int avm_heap_set(AVM_Context *ctx, avm_int data, avm_size_t loc)
{
  if (data == 0 && loc >= ctx->memory_size) {
    // This is beyond the bounds of the current memory. Since the memory
    // defaults to zero, pretend it's been written
    return 0;
  }

  if (loc >= ctx->memory_size) {
    avm_size_t new_size = 1;
    while (new_size < loc && new_size != 0) {
      new_size *= 2;
    }

    if (new_size == 0) {
      return avm__error(ctx, "internal error, tried to resize memory to index at"
                        " %u, but memory size integer wrapped.", loc);
    }

    ctx->memory = my_crealloc(ctx->memory, ctx->memory_size * sizeof(avm_int),
                                new_size * sizeof(avm_int));
    if (ctx->memory == NULL) {
      return avm__error(ctx, "unable to allocate more memory (%d avm_ints)",
                        new_size);
    }
    ctx->memory_size = new_size;
  }

  ctx->memory[loc] = data;
  return 0;
}

int avm_stack_push(AVM_Context *ctx, avm_int data)
{
  if (ctx->stack_size == AVM_SIZE_MAX) {
    // incrementing it now would jump to 0
    return avm__error(ctx, "Stack overflow");
  }

  ctx->stack_size += 1;

  if (ctx->stack_cap <= ctx->stack_size) {
    avm_size_t new_cap = (avm_size_t) min(ctx->stack_cap * 2, AVM_SIZE_MAX);
    ctx->stack = my_realloc(ctx->stack, new_cap * sizeof(avm_int));
    if (ctx->stack == NULL) {
      return avm__error(ctx, "unable to increase stack size (%d bytes)", new_cap);
    }

    ctx->stack_cap = new_cap;
  }

  assert(ctx->stack_size != 0);  // if it was 0, it'd cause overflow
  ctx->stack[ctx->stack_size - 1] = data;

  return 0;
}

int avm_stack_pop(AVM_Context *ctx, avm_int *data)
{
  if (ctx->stack_size == 0) {
    return avm__error(ctx, "unable to pop item off stack: stack underrun");
  }

  *data = ctx->stack[--ctx->stack_size];
  return 0;
}

int avm_stack_peak(AVM_Context *ctx, avm_int *data)
{
  if (ctx->stack_size == 0) {
    return avm__error(ctx, "unable to read item off stack: stack underrun");
  }

  *data = ctx->stack[ctx->stack_size - 1];
  return 0;
}
