// Copyright 2019 Shift Cryptosecurity AG
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "eth_sign_msg.h"
#include "eth_common.h"
#include "eth_sighash.h"
#include "eth_verify.h"
#include <sha3.h>

#include "eth.h"
#include <hardfault.h>
#include <keystore.h>
#include <util.h>
#include <workflow/confirm.h>

app_eth_sign_error_t app_eth_sign_msg(
    const ETHSignMessageRequest* request,
    ETHSignResponse* response)
{
    if (request->msg.size > 1024) {
        return APP_ETH_SIGN_ERR_INVALID_INPUT;
    }
    // Only support main net for now. Otherwise a user could be tricked into signing something for
    // main net even if they believe they are signing for testnet.
    if (request->coin != ETHCoin_ETH) {
        return APP_ETH_SIGN_ERR_INVALID_INPUT;
    }

    // Let user verify that it is signing for the expected address
    char address[APP_ETH_ADDRESS_HEX_LEN] = {0};
    if (!app_eth_address(
            request->coin,
            ETHPubRequest_OutputType_ADDRESS,
            request->keypath,
            request->keypath_count,
            address,
            sizeof(address))) {
        return APP_ETH_SIGN_ERR_INVALID_INPUT;
    }
    {
        confirm_params_t params = {
            .title = "Your\naddress",
            .body = address,
            .scrollable = true,
        };
        if (!workflow_confirm(&params)) {
            return APP_ETH_SIGN_ERR_USER_ABORT;
        }
    }

    const char msg_header[] =
        "\x19"
        "Ethereum Signed Message:\n";
    // sizeof(msg_header) includes null terminator
    // the maximum length of the signed data is 1024, therefore 4 bytes might be needed as length
    // prefix.
    char msg[sizeof(msg_header) - 1 + sizeof(request->msg.bytes) + 4] = {0};

    // payload_offset is also the length of the fixed header + payload size prefix
    size_t payload_offset = snprintf(msg, sizeof(msg), "%s%d", msg_header, request->msg.size);
    memcpy(&msg[payload_offset], request->msg.bytes, request->msg.size);

    bool all_ascii = true;
    for (size_t i = 0; i < request->msg.size; ++i) {
        if (request->msg.bytes[i] < 20 || request->msg.bytes[i] > 127) {
            all_ascii = false;
        }
    }

    // In case the msg is very long (more than 67 characters) we break it up and print the initial
    // and the last bytes with triple dots in between. The resulting string will be 32 characters
    // from the start and 32 characters from the end.
    char body[64 + 3 + 1] = {0};
    if (all_ascii && request->msg.size > 67) {
        snprintf(
            body,
            sizeof(body),
            "%.32s...%.32s",
            request->msg.bytes,
            &request->msg.bytes[request->msg.size - 32]);
    } else if (!all_ascii && request->msg.size > 33) {
        util_uint8_to_hex(request->msg.bytes, 16, body);
        memset(&body[32], '.', 3);
        util_uint8_to_hex(&request->msg.bytes[request->msg.size - 16], 16, &body[35]);
    } else if (all_ascii) {
        // request->msg.bytes is not null terminated. So print exactly the right amount of bytes.
        // We have also verified that it doesn't contain any null characters earlier.
        snprintf(body, sizeof(body), "%.*s", request->msg.size, request->msg.bytes);
    } else {
        util_uint8_to_hex(request->msg.bytes, request->msg.size, body);
    }

    const char* title;
    if (all_ascii) {
        title = "Sign\nETH Message";
    } else {
        title = "Sign\nETH Message (hex)";
    }

    {
        confirm_params_t params = {
            .title = title,
            .body = body,
            .scrollable = true,
        };
        if (!workflow_confirm(&params)) {
            return APP_ETH_SIGN_ERR_USER_ABORT;
        }
    }

    // Calculate the hash
    uint8_t sighash[32];
    sha3_ctx ctx;
    rhash_sha3_256_init(&ctx);
    rhash_sha3_update(&ctx, (const unsigned char*)msg, payload_offset + request->msg.size);
    rhash_keccak_final(&ctx, sighash);

    // Sign the hash and return the signature, with last byte set to recid.
    // check assumption
    if (sizeof(response->signature) != 65) {
        Abort("unexpected signature size");
    }
    int recid;
    if (!keystore_secp256k1_sign(
            request->keypath, request->keypath_count, sighash, response->signature, &recid)) {
        return APP_ETH_SIGN_ERR_UNKNOWN;
    }
    if (recid > 0xFF) {
        Abort("unexpected recid");
    }
    response->signature[64] = (uint8_t)recid;

    return APP_ETH_SIGN_OK;
}
