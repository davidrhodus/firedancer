#ifndef HEADER_fd_src_vm_fd_instr_h
#define HEADER_fd_src_vm_fd_instr_h

#include "../util/fd_util.h"

struct fd_vm_sbpf_opcode_any {
  uchar op_class  : 3;
  uchar _unknown  : 5;
};
typedef struct fd_vm_sbpf_opcode_any fd_vm_sbpf_opcode_any_t;

struct fd_vm_sbpf_opcode_normal {
  uchar op_class  : 3;
  uchar op_src    : 1;
  uchar op_mode   : 4;
};
typedef struct fd_vm_sbpf_opcode_normal fd_vm_sbpf_opcode_normal_t;

struct fd_vm_sbpf_opcode_mem {
  uchar op_class       : 3;
  uchar op_size        : 2;
  uchar op_addr_mode   : 3;
};
typedef struct fd_vm_sbpf_opcode_mem fd_vm_sbpf_opcode_mem_t;

union fd_vm_sbpf_opcode {
  uchar raw;
  fd_vm_sbpf_opcode_any_t any;
  fd_vm_sbpf_opcode_normal_t normal;
  fd_vm_sbpf_opcode_mem_t mem;
};
typedef union fd_vm_sbpf_opcode fd_vm_sbpf_opcode_t;

struct fd_vm_sbpf_instr {
  fd_vm_sbpf_opcode_t opcode;
  uchar dst_reg : 4;
  uchar src_reg : 4;
  short offset;
  uint imm;
};
typedef struct fd_vm_sbpf_instr fd_vm_sbpf_instr_t;

#endif /* HEADER_fd_src_vm_fd_instr_h */
