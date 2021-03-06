#include <stddef.h>
#include <stdint.h>

#include <CL/cl.h>

struct _cl_context {
  unsigned refCount;
  void (CL_CALLBACK *pfn_notify)(const char *errinfo,
               const void *private_info, size_t cb,
               void *user_data);
  void *user_data;
};

struct _cl_program {
  unsigned refCount;
  char *source;
  size_t sourceSize;
  uintptr_t module;

  unsigned *workDim;
  size_t *globalWorkOffset, *globalWorkSize, *numGroups;
  __attribute__((address_space(4))) size_t *ids;

  __attribute__((address_space(4))) uint64_t *wgBarrierWlist;
  unsigned *wgBarrierSize;
};

struct _cl_mem {
  unsigned refCount;
  void *data;
  uint8_t ownsData;
  size_t size;
};

union _cl_intern_arg_data {
  uint8_t i8;
  uint16_t i16;
  uint32_t i32;
  uint64_t i64;
  float f32;
  double f64;
  struct _cl_mem mem;
  size_t local_size;
};

struct _cl_kernel {
  unsigned refCount;
  struct _cl_program *program;
  void (*function)();
  union _cl_intern_arg_data args[16];
};

struct _cl_command_queue {
  unsigned refCount;
  struct _cl_context *context;
  struct _cl_event *event;
};

struct _cl_event {
  unsigned refCount;
  pthread_t *threads;
  size_t threadCount;
  struct _cl_event *nextEvent;
};

cl_event kcl_create_pthread_event(pthread_t *threads, size_t threadCount);
void kcl_add_event_to_queue(cl_command_queue queue, cl_event event);

typedef int8_t cl_intern_arg_type;

#define CL_INTERN_ARG_TYPE_I8        0
#define CL_INTERN_ARG_TYPE_I16       1
#define CL_INTERN_ARG_TYPE_I32       2
#define CL_INTERN_ARG_TYPE_I64       3
#define CL_INTERN_ARG_TYPE_F32       4
#define CL_INTERN_ARG_TYPE_F64       5
#define CL_INTERN_ARG_TYPE_MEM       6
#define CL_INTERN_ARG_TYPE_LOCAL_MEM 7
