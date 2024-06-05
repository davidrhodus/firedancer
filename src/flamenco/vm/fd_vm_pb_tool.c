#include "fd_vm_private.h"

#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include "../runtime/tests/fd_exec_test.pb.h"
#include "../nanopb/pb_encode.h"

static uchar *
read_input_file( char const * input_path, ulong * _input_sz ) {
  if( FD_UNLIKELY( !_input_sz ) ) FD_LOG_ERR(( "input_sz cannot be NULL" ));

  /* Open file */

  FILE * input_file = fopen( input_path, "r" );
  if( FD_UNLIKELY( !input_file ) )
    FD_LOG_ERR(( "fopen(\"%s\") failed (%i-%s)", input_path, errno, fd_io_strerror( errno ) ));

  struct stat input_stat;
  if( FD_UNLIKELY( 0!=fstat( fileno( input_file ), &input_stat ) ) )
    FD_LOG_ERR(( "fstat() failed (%i-%s)", errno, fd_io_strerror( errno ) ));
  if( FD_UNLIKELY( !S_ISREG( input_stat.st_mode ) ) )
    FD_LOG_ERR(( "File \"%s\" not a regular file", input_path ));

  /* Allocate file buffer */

  ulong input_sz  = (ulong)input_stat.st_size;
  void * input_buf = malloc( input_sz );
  if( FD_UNLIKELY( !input_buf ) )
    FD_LOG_ERR(( "malloc(%#lx) failed (%i-%s)", input_sz, errno, fd_io_strerror( errno ) ));

  /* Read input */

  if( FD_UNLIKELY( fread( input_buf, input_sz, 1UL, input_file )!=1UL ) )
    FD_LOG_ERR(( "fread() failed (%i-%s)", errno, fd_io_strerror( errno ) ));
  FD_TEST( 0==fclose( input_file ) );

  *_input_sz = input_sz;

  return input_buf;
}

void
bin2vmctx( char const * program_file, char const * output_file ) {
  ulong elf_sz = 0UL;
  uchar * _bin = read_input_file( program_file, &elf_sz );

  /* Extract ELF info */
  fd_sbpf_elf_info_t elf_info;
  FD_TEST( fd_sbpf_elf_peek( &elf_info, _bin, elf_sz ) );

  /* Allocate rodata */
  void * rodata = malloc( elf_info.rodata_footprint );
  FD_TEST( rodata );

  /* Allocate program buffer. */
  fd_sbpf_program_t * prog = fd_sbpf_program_new( aligned_alloc( fd_sbpf_program_align(), fd_sbpf_program_footprint( &elf_info) ), &elf_info , rodata );
  FD_TEST( prog );


  /* Allocate syscalls */
  fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_new(
      aligned_alloc( fd_sbpf_syscalls_align(), fd_sbpf_syscalls_footprint() ) );
  FD_TEST( syscalls );

  fd_vm_syscall_register_all( syscalls );

  /* Load program */
  if ( 0 != fd_sbpf_program_load( prog, _bin, elf_sz, syscalls ) ) {
    FD_LOG_ERR(( "Failed to load program: %s", fd_sbpf_strerror() ));
  }

  /* Create VM ctx */

  fd_exec_test_vm_context_t vm_ctx = FD_EXEC_TEST_VM_CONTEXT_INIT_ZERO;
  
  vm_ctx.rodata = malloc( PB_BYTES_ARRAY_T_ALLOCSIZE( prog->rodata_sz ) );

  vm_ctx.rodata->size = (pb_size_t) prog->rodata_sz;
  memcpy( vm_ctx.rodata->bytes, prog->rodata, prog->rodata_sz );

  vm_ctx.rodata_text_section_length = prog->text_cnt * 8UL;
  vm_ctx.rodata_text_section_offset = prog->text_off;

  /* Sigh.. dumping a protobuf message to file is quite convoluted */
  size_t pb_size = 0;
  pb_get_encoded_size( &pb_size, &fd_exec_test_vm_context_t_msg, &vm_ctx );
  pb_byte_t * pb_buf = malloc( pb_size );
  
  pb_ostream_t ostream = pb_ostream_from_buffer( pb_buf, pb_size );
  int encode_ok = pb_encode( &ostream, &fd_exec_test_vm_context_t_msg, &vm_ctx );
  if( !encode_ok ) {
    FD_LOG_ERR(( "Failed to encode vm_ctx" ));
  }

  free(_bin);
  free(rodata);
  free(prog);
  free(syscalls);
  free(vm_ctx.rodata);

  /* ... dump to file */
  FILE * output_fd = fopen( output_file, "wb" );
  if( FD_UNLIKELY( !output_fd ) )
    FD_LOG_ERR(( "fopen(\"%s\") failed (%i-%s)", output_file, errno, fd_io_strerror( errno ) ));
  
  if( FD_UNLIKELY( fwrite( pb_buf, 1UL , ostream.bytes_written, output_fd )!=pb_size ) )
    FD_LOG_ERR(( "fwrite() failed (%i-%s)", errno, fd_io_strerror( errno ) ));

  free(pb_buf);
}

int
main (int argc, char **argv) {
  fd_boot(&argc, &argv);


  char const * cmd = fd_env_strip_cmdline_cstr( &argc, &argv, "--cmd", NULL, NULL );
  if( FD_UNLIKELY( !cmd ) ) FD_LOG_ERR(( "Please specify a command" ));

  if( !strcmp( cmd, "bin2vmctx" ) ){
    char const * program_file = fd_env_strip_cmdline_cstr( &argc, &argv, "--program-file", NULL, NULL );
    char const * output_file  = fd_env_strip_cmdline_cstr( &argc, &argv, "--output-file",  NULL, NULL );

    if( FD_UNLIKELY( !program_file ) ) FD_LOG_ERR(( "Please specify a --program-file" ));
    if( FD_UNLIKELY( !output_file  ) ) FD_LOG_ERR(( "Please specify a --output-file"  ));

    bin2vmctx( program_file, output_file );
  }

  fd_halt();
  return 0;
}
