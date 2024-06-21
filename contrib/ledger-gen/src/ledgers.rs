use {
    solana_sdk::{
        signature::{Keypair, Signer},
        commitment_config::{CommitmentConfig},
        nonce::{State as NonceState, state::Versions as NonceVersions},
        system_instruction,
        system_program,
        message::Message,
        transaction::Transaction,
    },
    solana_client::{
        rpc_client::{RpcClient},
    },
    std::{
        sync::Arc,
    },
    solana_rpc_client_nonce_utils::{get_account_with_commitment, nonblocking},
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

pub fn deploy_invoke_diff_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    println!("Program Id: {:?}", program_account.pubkey());
}

pub fn upgrade_invoke_same_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);
    let buffer_account_upgrade = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Upgrade Program
    let upgrade_program_instructions = programs::upgrade_program_instructions(&payer, &buffer_account_upgrade, &program_account);
    let transaction = utils::create_message_and_sign(&upgrade_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Upgraded Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    println!("Program Id: {:?}", program_account.pubkey());
}

pub fn upgrade_invoke_diff_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);
    let buffer_account_upgrade = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Upgrade Program
    let upgrade_program_instructions = programs::upgrade_program_instructions(&payer, &buffer_account_upgrade, &program_account);
    let transaction = utils::create_message_and_sign(&upgrade_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Upgraded Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    println!("Program Id: {:?}", program_account.pubkey());
}

pub fn deploy_close_same_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    // Close Program
    let close_program_instructions = programs::close_program_instructions(&payer, &program_account);
    let transaction = utils::create_message_and_sign(&close_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());
}

pub fn deploy_close_diff_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Close Program
    let close_program_instructions = programs::close_program_instructions(&payer, &program_account);
    let transaction = utils::create_message_and_sign(&close_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());
}

pub fn close_invoke_same_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Close Program
    let close_program_instructions = programs::close_program_instructions(&payer, &program_account);
    let transaction = utils::create_message_and_sign(&close_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());
}

pub fn close_invoke_diff_slot(client: &RpcClient, arc_client: &Arc<RpcClient>, payer: &Keypair, program_data: &Vec<u8>, account_data: &Vec<u8>) {
    let buffer_account_deploy = programs::set_up_buffer_account(&arc_client, &payer, &program_data);

    utils::wait_atleast_n_slots(&client, 1);

    // Deploy Program
    let (program_account, deploy_program_instructions) = programs::deploy_program_instructions(&client, &payer, None, &buffer_account_deploy, program_data.len());
    let transaction = utils::create_message_and_sign(&deploy_program_instructions, &payer, vec![&payer, &program_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Deployed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 1);

    // Close Program
    let close_program_instructions = programs::close_program_instructions(&payer, &program_account);
    let transaction = utils::create_message_and_sign(&close_program_instructions, &payer, vec![&payer], client.get_latest_blockhash().unwrap());
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Closed Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    utils::wait_atleast_n_slots(&client, 2);

    // Invoke Program
    let (run_account, invoke_program_instructions) = programs::invoke_program_instructions(&client, &payer, &program_account, &account_data);
    let transaction = utils::create_message_and_sign(&invoke_program_instructions, &payer, vec![&payer, &run_account], client.get_latest_blockhash().unwrap());
    let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    println!("Invoked Program Signature: {:?} - Slot: {:?}", transaction.signatures[0], client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());
}

pub fn create_nonce_account(client: &RpcClient, payer: &Keypair) {
    let blockhash = client.get_latest_blockhash().unwrap();
    let (nonce_account, create_nonce_instructions) = programs::create_nonce_account_instructions(None, &payer, 2000000);
    let transaction = utils::create_message_and_sign(&create_nonce_instructions, &payer, vec![&payer, &nonce_account], blockhash);
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Created Nonce Account: {:?} - Slot: {:?}", nonce_account.pubkey(), client.get_slot_with_commitment(CommitmentConfig::processed()).unwrap());

    let nonce_blockhash = match get_account_with_commitment(client, &nonce_account.pubkey(), CommitmentConfig::processed())
        .and_then(|ref a| nonblocking::state_from_account(a)).unwrap()
    {
        NonceState::Initialized(ref data) => data.blockhash(),
        _ => panic!("Nonce Account not Initialized"),
    };
    println!("Nonce Blockhash: {:?}", nonce_blockhash);

    let new_account = Keypair::new();

    let minimum_balance = client
        .get_minimum_balance_for_rent_exemption(NonceState::size())
        .unwrap();

    let open_account_instruction = system_instruction::create_account(
        &payer.pubkey(),
        &new_account.pubkey(),
        minimum_balance,
        0,
        &system_program::id(),
    );

    let message = Message::new_with_nonce(
        vec![open_account_instruction],
        Some(&payer.pubkey()),
        &nonce_account.pubkey(),
        &payer.pubkey(),
    );
    let mut transaction = Transaction::new_unsigned(message);
    let _ = transaction.try_sign(&vec![&payer, &new_account], nonce_blockhash).unwrap();
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();
    println!("Opened Account: {:?}", new_account.pubkey());

    utils::wait_atleast_n_slots(&client, 2);

    // will fail if we don't update the nonce blockhash
    let nonce_blockhash = match get_account_with_commitment(client, &nonce_account.pubkey(), CommitmentConfig::processed())
        .and_then(|ref a| nonblocking::state_from_account(a)).unwrap()
    {
        NonceState::Initialized(ref data) => data.blockhash(),
        _ => panic!("Nonce Account not Initialized"),
    };
    println!("Nonce Blockhash: {:?}", nonce_blockhash);

    let new_account = Keypair::new();

    let minimum_balance = client
        .get_minimum_balance_for_rent_exemption(NonceState::size())
        .unwrap();

    let open_account_instruction = system_instruction::create_account(
        &payer.pubkey(),
        &new_account.pubkey(),
        minimum_balance,
        0,
        &system_program::id(),
    );

    let message = Message::new_with_nonce(
        vec![open_account_instruction],
        Some(&payer.pubkey()),
        &nonce_account.pubkey(),
        &payer.pubkey(),
    );
    let mut transaction = Transaction::new_unsigned(message);
    let _ = transaction.try_sign(&vec![&payer, &new_account], nonce_blockhash).unwrap();
    println!("Transaction: {:?}", transaction);
    utils::wait_atleast_n_slots(&client, 2);
    // let _ = client.send_transaction_with_config(&transaction, *utils::SKIP_PREFLIGHT_CONFIG).unwrap();
    let _ = client.send_and_confirm_transaction(&transaction).unwrap();

    utils::wait_atleast_n_slots(&client, 2);

    let nonce_blockhash = match get_account_with_commitment(client, &nonce_account.pubkey(), CommitmentConfig::processed())
        .and_then(|ref a| nonblocking::state_from_account(a)).unwrap()
    {
        NonceState::Initialized(ref data) => data.blockhash(),
        _ => panic!("Nonce Account not Initialized"),
    };
    println!("Nonce Blockhash: {:?}", nonce_blockhash);
}
