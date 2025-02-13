#include "fd_builtin_programs.h"
#include "../fd_acc_mgr.h"
#include "../fd_system_ids.h"
#include "../context/fd_exec_epoch_ctx.h"
#include "../context/fd_exec_slot_ctx.h"

/* BuiltIn programs need "bogus" executable accounts to exist.
   These are loaded and ignored during execution.

   Bogus accounts are marked as "executable", but their data is a
   hardcoded ASCII string. */

/* https://github.com/solana-labs/solana/blob/8f2c8b8388a495d2728909e30460aa40dcc5d733/sdk/src/native_loader.rs#L19 */
void
fd_write_builtin_bogus_account( fd_exec_slot_ctx_t * slot_ctx,
                                uchar const          pubkey[ static 32 ],
                                char const *         data,
                                ulong                sz ) {

  fd_acc_mgr_t *      acc_mgr = slot_ctx->acc_mgr;
  fd_funk_txn_t *     txn     = slot_ctx->funk_txn;
  fd_pubkey_t const * key     = (fd_pubkey_t const *)pubkey;
  FD_BORROWED_ACCOUNT_DECL(rec);

  int err = fd_acc_mgr_modify( acc_mgr, txn, key, 1, sz, rec);
  FD_TEST( !err );

  rec->meta->dlen            = sz;
  rec->meta->info.lamports   = 1UL;
  rec->meta->info.rent_epoch = 0UL;
  rec->meta->info.executable = 1;
  fd_memcpy( rec->meta->info.owner, fd_solana_native_loader_id.key, 32 );
  memcpy( rec->data, data, sz );

  slot_ctx->slot_bank.capitalization++;

  // err = fd_acc_mgr_commit( acc_mgr, rec, slot_ctx );
  FD_TEST( !err );
}

/* https://github.com/solana-labs/solana/blob/8f2c8b8388a495d2728909e30460aa40dcc5d733/runtime/src/inline_spl_token.rs#L74 */
/* TODO: move this somewhere more appropiate */
static void
write_inline_spl_native_mint_program_account( fd_exec_slot_ctx_t * slot_ctx ) {
  // really?! really!?
  fd_epoch_bank_t const * epoch_bank = fd_exec_epoch_ctx_epoch_bank( slot_ctx->epoch_ctx );
  if( epoch_bank->cluster_type != 3)
    return;

  fd_acc_mgr_t *      acc_mgr = slot_ctx->acc_mgr;
  fd_funk_txn_t *     txn     = slot_ctx->funk_txn;
  fd_pubkey_t const * key     = (fd_pubkey_t const *)&fd_solana_spl_native_mint_id;
  FD_BORROWED_ACCOUNT_DECL(rec);

  /* https://github.com/solana-labs/solana/blob/8f2c8b8388a495d2728909e30460aa40dcc5d733/runtime/src/inline_spl_token.rs#L86-L90 */
  static uchar const data[] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  int err = fd_acc_mgr_modify( acc_mgr, txn, key, 1, sizeof(data), rec );
  FD_TEST( !err );

  rec->meta->dlen            = sizeof(data);
  rec->meta->info.lamports   = 1000000000UL;
  rec->meta->info.rent_epoch = 1UL;
  rec->meta->info.executable = 0;
  fd_memcpy( rec->meta->info.owner, fd_solana_spl_token_id.key, 32 );
  memcpy( rec->data, data, sizeof(data) );

  FD_TEST( !err );
}

void fd_builtin_programs_init( fd_exec_slot_ctx_t * slot_ctx ) {
  // https://github.com/anza-xyz/agave/blob/v2.0.1/runtime/src/bank/builtins/mod.rs#L33

  fd_write_builtin_bogus_account( slot_ctx, fd_solana_system_program_id.key,         "system_program",         14UL );
  fd_write_builtin_bogus_account( slot_ctx, fd_solana_vote_program_id.key,           "vote_program",           12UL );

  if( !FD_FEATURE_ACTIVE( slot_ctx, migrate_stake_program_to_core_bpf ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_stake_program_id.key,        "stake_program",          13UL );
  }

  if( !FD_FEATURE_ACTIVE( slot_ctx, migrate_config_program_to_core_bpf ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_config_program_id.key,       "config_program",         14UL );
  }

  if( FD_FEATURE_ACTIVE( slot_ctx, enable_program_runtime_v2_and_loader_v4 ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_bpf_loader_v4_program_id.key,   "loader_v4",             9UL );
  }

  if( !FD_FEATURE_ACTIVE( slot_ctx, migrate_address_lookup_table_program_to_core_bpf ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_address_lookup_table_program_id.key, "address_lookup_table_program",          28UL );
  }

  fd_write_builtin_bogus_account( slot_ctx, fd_solana_bpf_loader_deprecated_program_id.key,  "solana_bpf_loader_deprecated_program",  36UL );

  fd_write_builtin_bogus_account( slot_ctx, fd_solana_bpf_loader_program_id.key,             "solana_bpf_loader_program",             25UL );
  fd_write_builtin_bogus_account( slot_ctx, fd_solana_bpf_loader_upgradeable_program_id.key, "solana_bpf_loader_upgradeable_program", 37UL );

  fd_write_builtin_bogus_account( slot_ctx, fd_solana_compute_budget_program_id.key, "compute_budget_program", 22UL );

  //TODO: remove when no longer necessary
  if( FD_FEATURE_ACTIVE( slot_ctx, zk_token_sdk_enabled ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_zk_token_proof_program_id.key, "zk_token_proof_program", 22UL );
  }

  if( FD_FEATURE_ACTIVE( slot_ctx, zk_elgamal_proof_program_enabled ) ) {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_zk_elgamal_proof_program_id.key, "zk_elgamal_proof_program", 24UL );
  }

  /* Precompiles have empty account data */
  if (slot_ctx->epoch_ctx->epoch_bank.cluster_version[0] < 2) {
    char data[1] = {1};
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_keccak_secp_256k_program_id.key, data, 1 );
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_ed25519_sig_verify_program_id.key, data, 1 );
    if (FD_FEATURE_ACTIVE( slot_ctx, enable_secp256r1_precompile ))
      fd_write_builtin_bogus_account( slot_ctx, fd_solana_secp256r1_program_id.key, data, 1 );
  } else {
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_keccak_secp_256k_program_id.key, "", 0 );
    fd_write_builtin_bogus_account( slot_ctx, fd_solana_ed25519_sig_verify_program_id.key, "", 0 );
    if (FD_FEATURE_ACTIVE( slot_ctx, enable_secp256r1_precompile ))
      fd_write_builtin_bogus_account( slot_ctx, fd_solana_secp256r1_program_id.key, "", 0 );
  }

  /* Inline SPL token mint program ("inlined to avoid an external dependency on the spl-token crate") */
  write_inline_spl_native_mint_program_account( slot_ctx );
}
