use {
    solana_sdk::{
        signature::{Keypair, Signer},
        commitment_config::{CommitmentConfig}
    },
    solana_client::{
        rpc_client::{RpcClient},
    },
    std::{
        sync::Arc,
    },
};

use crate::programs;
use crate::utils;

pub fn example_ledger(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    // Set Up Buffer Accounts
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);
    let buffer_account_upgrade = programs::set_up_buffer_account(&arc_client, &payer, &program_data);
    let buffer_account_redeploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);
    
    // Upgrade Program
    let upgrade_program_instructions = programs::upgrade_program_instructions(&payer, &buffer_account_upgrade, &program_account);
    let transaction = utils::create_message_and_sign(&upgrade_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Upgraded Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());


    utils::wait_atleast_n_slots(&client, 1);

    // Close Program
    let close_program_instructions = programs::close_program_instructions(&payer, &program_account);
    let transaction = utils::create_message_and_sign(&close_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Redeploy Program Failure
    let upgrade_program_instructions = programs::upgrade_program_instructions(&payer, &buffer_account_redeploy, &program_account);
    let transaction = utils::create_message_and_sign(&upgrade_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG);
    println!("Tried Upgrading on Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Redeploy Program Failure
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, Some(program_account), &buffer_account_redeploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG);
    println!("Tried Deploying on Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());
}

pub fn deploy_invoke_same_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    println!("Program Id: {:?}", program_account.pubkey());
}
